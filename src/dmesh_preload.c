/*
 * dmesh_preload.c — LD_PRELOAD socket-compatibility shim over the DPUmesh
 * native API (dmesh.h). Lets an UNMODIFIED, dynamically-linked POSIX socket
 * application run over DPUmesh: selected TCP connects/listens are transparently
 * backed by dmesh connections; every other fd passes through to the kernel.
 *
 * LAYERING (see design/CORE.md §6): the native dmesh_* API stays the optimized product;
 * this shim is a compatibility layer that deliberately re-buys the per-conn-fd
 * readiness model (measured ~half the native ceiling) in exchange for POSIX
 * transparency. Nothing here is on the native hot path.
 *
 * ACTIVATION (env; everything else is untouched kernel TCP):
 *   DMESH_PRELOAD_LISTEN=<port>          listen() on this TCP port becomes the
 *                                        dmesh service listener
 *   DMESH_PRELOAD_SVC=<svc>              service_id this process advertises
 *                                        (required with LISTEN; absent = pure client)
 *   DMESH_PRELOAD_REGISTRY=<file>        control-plane registry, lines "ClusterIP:port
 *                                        svc": connect() to a listed ClusterIP:port is
 *                                        routed to that service — the Envoy xDS/EDS
 *                                        equivalent (a static file for P0, controller-fed
 *                                        later). Keyed on IP:port, so same-port services
 *                                        on distinct ClusterIPs resolve apart.
 *   DMESH_PRELOAD_MAP=<port>=<svc>[,..]  legacy port-only rule (any IP on that port →
 *                                        dmesh); superseded by REGISTRY's ClusterIP key
 *   DMESH_PRELOAD_DEBUG=1                stderr diagnostics
 *
 * FD REALIZATION — the design keystone: when a socket becomes dmesh-backed, the
 * shim creates a PRIVATE eventfd and dup2()s it over the app's fd number. The
 * app's fd is then a real kernel fd (same open file description as the private
 * one), so epoll/poll/select/close/dup ALL work natively — none of them are
 * interposed. The dispatcher asserts readability by writing the private eventfd;
 * the read path drains it at EAGAIN edges.
 *
 * ORDERING — a socket promises byte-stream total order on a connection. The DPU
 * is conn-STICKY by default, but a service can opt into per-message LB
 * (DPUMESH_LB_PER_REQUEST_SVC), and its replies would then interleave across
 * backends. Every shim client conn is therefore dmesh_pin_route()'d unconditionally:
 * all its messages (and its FIN) carry one route-affinity group, pinning the conn to
 * the backend picked for its first message WHATEVER the service's LB policy —
 * connection-level LB, exactly what a TCP proxy gives you.
 *
 * THREAD MODEL — design/API.md contract: dmesh_accept/dmesh_next_ready are single-
 * consumer. The shim's dispatcher thread is that consumer; it also EXCLUSIVELY
 * runs dmesh_destroy_qp (app close() only queues), so a ready-list pop can never
 * race a conn free. App threads touch only their own conns (per-entry mutex).
 *
 * LOST-WAKEUP DISCIPLINE — the ready list re-arms only on an inbox empty→
 * non-empty edge, so: (a) every successful read RE-ASSERTS the eventfd (over-
 * asserting is safe: one spurious wake, then the EAGAIN path drains); (b) the
 * EAGAIN path drains the eventfd and RETRIES once, closing the window where a
 * signal lands between the failed read and the drain. Signals can be delayed
 * one hop but never lost.
 *
 * KNOWN LIMITS (v1, documented): no fork-shared sockets (DOCA is not fork-
 * safe), half-close is approximate (a FIN tears the upstream down — replies
 * after shutdown(SHUT_WR) are undeliverable), most SO_* options are accepted
 * no-ops, AF_INET SOCK_STREAM only, one dmesh listener per process. BYPASSES:
 * Go binaries (raw syscalls) and stdio FILE* wrappers over a socket (glibc
 * stdio calls its internal __read/__write, not the PLT) never enter the shim.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dmesh_core.h"    /* sibling façade: the shim sits on the CORE, not on the
                            * native API — it needs conn internals (rx_slot cursor,
                            * peer_closed) and the internal lifecycle (accept /
                            * next_ready / send_fin) that <dpumesh/dmesh.h> does not
                            * expose. Pulls in <dpumesh/dmesh.h> for the handles. */

/* ================= real libc entry points (lazy dlsym) ================= */

#define REAL_DECL(name, ret, ...) \
    static ret (*real_##name)(__VA_ARGS__); \
    static void resolve_##name(void) { real_##name = dlsym(RTLD_NEXT, #name); }

REAL_DECL(socket,      int,     int, int, int)
REAL_DECL(connect,     int,     int, const struct sockaddr *, socklen_t)
REAL_DECL(bind,        int,     int, const struct sockaddr *, socklen_t)
REAL_DECL(listen,      int,     int, int)
REAL_DECL(accept,      int,     int, struct sockaddr *, socklen_t *)
REAL_DECL(accept4,     int,     int, struct sockaddr *, socklen_t *, int)
REAL_DECL(read,        ssize_t, int, void *, size_t)
REAL_DECL(write,       ssize_t, int, const void *, size_t)
REAL_DECL(recv,        ssize_t, int, void *, size_t, int)
REAL_DECL(send,        ssize_t, int, const void *, size_t, int)
REAL_DECL(recvfrom,    ssize_t, int, void *, size_t, int, struct sockaddr *, socklen_t *)
REAL_DECL(sendto,      ssize_t, int, const void *, size_t, int, const struct sockaddr *, socklen_t)
REAL_DECL(recvmsg,     ssize_t, int, struct msghdr *, int)
REAL_DECL(sendmsg,     ssize_t, int, const struct msghdr *, int)
REAL_DECL(readv,       ssize_t, int, const struct iovec *, int)
REAL_DECL(writev,      ssize_t, int, const struct iovec *, int)
REAL_DECL(close,       int,     int)
REAL_DECL(shutdown,    int,     int, int)
REAL_DECL(fcntl,       int,     int, int, ...)
REAL_DECL(ioctl,       int,     int, unsigned long, ...)
REAL_DECL(getsockopt,  int,     int, int, int, void *, socklen_t *)
REAL_DECL(setsockopt,  int,     int, int, int, const void *, socklen_t)
REAL_DECL(getsockname, int,     int, struct sockaddr *, socklen_t *)
REAL_DECL(getpeername, int,     int, struct sockaddr *, socklen_t *)
REAL_DECL(dup,         int,     int)
REAL_DECL(dup2,        int,     int, int)
REAL_DECL(dup3,        int,     int, int, int)
REAL_DECL(sendfile,    ssize_t, int, int, off_t *, size_t)
/* LFS aliases: may be absent from libc (dlsym NULL) → fall back to the plain form. */
REAL_DECL(fcntl64,     int,     int, int, ...)
REAL_DECL(sendfile64,  ssize_t, int, int, off_t *, size_t)

/* variadic real fcntl/ioctl need the va-form; glibc's are (int, int/ulong, arg) */
static pthread_once_t g_resolve_once = PTHREAD_ONCE_INIT;
static void resolve_all(void) {
    resolve_socket(); resolve_connect(); resolve_bind(); resolve_listen();
    resolve_accept(); resolve_accept4(); resolve_read(); resolve_write();
    resolve_recv(); resolve_send(); resolve_recvfrom(); resolve_sendto();
    resolve_recvmsg(); resolve_sendmsg(); resolve_readv(); resolve_writev();
    resolve_close(); resolve_shutdown(); resolve_fcntl(); resolve_ioctl();
    resolve_getsockopt(); resolve_setsockopt(); resolve_getsockname();
    resolve_getpeername(); resolve_dup(); resolve_dup2(); resolve_dup3();
    resolve_sendfile(); resolve_fcntl64(); resolve_sendfile64();
}
#define ENSURE_REAL() pthread_once(&g_resolve_once, resolve_all)

/* ============================ configuration ============================ */

#define PRELOAD_MAX_MAP 64

static int      g_debug;
static int      g_listen_port = -1;      /* DMESH_PRELOAD_LISTEN */
static int      g_svc         = -12345;  /* DMESH_PRELOAD_SVC (sentinel = unset) */

/* connect() routing table: (dst IP:port) -> service_id. ONE lookup, two sources:
 * DMESH_PRELOAD_REGISTRY (a control-plane-fed file — the Envoy xDS/EDS equivalent)
 * gives exact ClusterIP:port -> svc; legacy DMESH_PRELOAD_MAP=port=svc gives a
 * port-only rule (addr 0 = any IP). Exact (addr,port) beats a wildcard, so keying
 * on the ClusterIP lets same-port services on distinct IPs resolve apart. */
struct route_ent { uint32_t addr; uint16_t port; int svc; };   /* addr net-order; 0 = any IP */
static int              g_route_n;
static struct route_ent g_route[PRELOAD_MAX_MAP];

#define DBG(...) do { if (g_debug) { fprintf(stderr, "[dmesh_preload] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void route_add(uint32_t addr, uint16_t port, int svc) {
    if (g_route_n < PRELOAD_MAX_MAP)
        g_route[g_route_n++] = (struct route_ent){ addr, port, svc };
}

/* Resolve a connect() dst. Exact IP:port wins; a wildcard (addr 0, from MAP) matches
 * any IP on that port; -1 = not meshed (the connect stays kernel TCP). */
static int route_lookup(uint32_t addr, uint16_t port) {
    int wild = -1;
    for (int i = 0; i < g_route_n; i++) {
        if (g_route[i].port != port) continue;
        if (g_route[i].addr == addr) return g_route[i].svc;
        if (g_route[i].addr == 0)    wild = g_route[i].svc;
    }
    return wild;
}

/* Load the registry file: lines "IP:port svc" (blank / non-IP lines — e.g. '#'
 * comments — fail the parse and skip). P0 stand-in for the dpumesh-controller
 * feed; P1 swaps the file for a live source behind the SAME table. */
static void route_load_registry(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { DBG("registry %s: %s", path, strerror(errno)); return; }
    char ln[256], ip[64]; int port, svc;
    while (fgets(ln, sizeof ln, f)) {
        struct in_addr a;
        if (sscanf(ln, " %63[^:]:%d %d", ip, &port, &svc) == 3 &&
            inet_pton(AF_INET, ip, &a) == 1)
            route_add(a.s_addr, (uint16_t)port, svc);
    }
    fclose(f);
}

__attribute__((constructor))
static void preload_ctor(void) {
    const char *e;
    g_debug = (e = getenv("DMESH_PRELOAD_DEBUG")) && atoi(e);
    if ((e = getenv("DMESH_PRELOAD_LISTEN"))) g_listen_port = atoi(e);
    if ((e = getenv("DMESH_PRELOAD_SVC")))    g_svc = atoi(e);
    if ((e = getenv("DMESH_PRELOAD_MAP"))) {
        char buf[512]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char *save = NULL, *tok = strtok_r(buf, ",", &save);
             tok; tok = strtok_r(NULL, ",", &save)) {
            char *eq = strchr(tok, '=');
            if (eq) { *eq = 0; route_add(0, (uint16_t)atoi(tok), atoi(eq + 1)); }
        }
    }
    if ((e = getenv("DMESH_PRELOAD_REGISTRY"))) route_load_registry(e);
    DBG("ctor: listen=%d svc=%d routes=%d", g_listen_port, g_svc, g_route_n);
}

/* ============================== fd table =============================== */

#define PRELOAD_MAX_FDS 65536

typedef struct pfd {
    dmesh_qp_t *conn;        /* NULL for the listener entry */
    int  efd;                  /* PRIVATE eventfd (CLOEXEC); app fds are kernel dups */
    int  listener;
    int  nonblock;
    int  wr_closed;
    int  rd_closed;
    int  refs;                 /* app fd aliases (dup); private efd NOT counted */
    int  closing;              /* queued for dispatcher dmesh_destroy_qp */
    long rcv_timeout_ms;       /* SO_RCVTIMEO; 0 = block forever */
    uint16_t lport;            /* synthesized getsockname port */
    uint16_t pport;            /* getpeername port */
    uint32_t paddr;            /* getpeername IP (net-order); 0 = synthesize loopback.
                                * A CLIENT stores the real dialed ClusterIP here; a
                                * SERVER/accepted conn keeps 0 (real client id is P1). */
    pthread_mutex_t mu;
    struct pfd *q_next;        /* accept- / close-queue linkage */
} pfd_t;

static pfd_t *g_fds[PRELOAD_MAX_FDS];
static pthread_mutex_t g_tbl_mu = PTHREAD_MUTEX_INITIALIZER;

static pfd_t *pfd_get(int fd) {
    if (fd < 0 || fd >= PRELOAD_MAX_FDS) return NULL;
    return g_fds[fd];                    /* torn-read safe: pointer store/load */
}

static pfd_t *pfd_new(dmesh_qp_t *c) {
    pfd_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->conn = c;
    e->efd  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->efd < 0) { free(e); return NULL; }
    pthread_mutex_init(&e->mu, NULL);
    return e;
}

static void efd_signal(pfd_t *e) {
    uint64_t one = 1;
    ssize_t r = real_write(e->efd, &one, sizeof one);
    (void)r;                             /* counter overflow (impossible here) only */
}

static void efd_drain(pfd_t *e) {
    uint64_t v;
    while (real_read(e->efd, &v, sizeof v) > 0) {}
}

/* ===================== channel + dispatcher thread ===================== */

static dmesh_channel_t *g_ch;
static dmesh_cq_t *g_cq;                 /* the ONE CQ: the dispatcher is the single
                                          * consumer for every shim conn (see THREAD
                                          * MODEL), so one is exactly right here. */
static int  g_wake_fd = -1;              /* wakes the dispatcher for the close queue */
static pfd_t *g_listener;                /* the (single) dmesh listener entry */
static int  g_listener_closed;           /* a listener existed and was closed: inbound
                                          * conns can never be accepted → close them at
                                          * wrap instead of queueing forever. Distinct
                                          * from "not listening YET" (NULL + flag 0),
                                          * where pre-listen conns legitimately queue. */
static pthread_mutex_t g_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pfd_t *g_accept_head, *g_accept_tail;   /* dispatcher → accept() */
static pfd_t *g_close_head;                    /* close() → dispatcher */

#define FREE_GRACE 256                          /* delayed-free ring (see reap) */
static pfd_t *g_grace[FREE_GRACE];
static int g_grace_i;

static void accept_q_push(pfd_t *e) {
    pthread_mutex_lock(&g_q_mu);
    e->q_next = NULL;
    if (g_accept_tail) g_accept_tail->q_next = e; else g_accept_head = e;
    g_accept_tail = e;
    pthread_mutex_unlock(&g_q_mu);
}

static pfd_t *accept_q_pop(void) {
    pthread_mutex_lock(&g_q_mu);
    pfd_t *e = g_accept_head;
    if (e) {
        g_accept_head = e->q_next;
        if (!g_accept_head) g_accept_tail = NULL;
        e->q_next = NULL;
    }
    pthread_mutex_unlock(&g_q_mu);
    return e;
}

static void close_q_push(pfd_t *e) {
    pthread_mutex_lock(&g_q_mu);
    e->q_next = g_close_head;
    g_close_head = e;
    pthread_mutex_unlock(&g_q_mu);
    uint64_t one = 1;
    ssize_t r = real_write(g_wake_fd, &one, sizeof one);
    (void)r;
}

static void *dispatcher_main(void *arg) {
    (void)arg;
    int ch_fd = dmesh_cq_fd(g_cq);
    struct pollfd pfds[2] = {
        { .fd = ch_fd,     .events = POLLIN },
        { .fd = g_wake_fd, .events = POLLIN },
    };
    DBG("dispatcher up (ch_fd=%d wake_fd=%d)", ch_fd, g_wake_fd);
    for (;;) {
        (void)poll(pfds, 2, -1);

        if (pfds[0].revents & POLLIN) {
            uint64_t v;
            while (real_read(ch_fd, &v, sizeof v) > 0) {}

            /* New inbound connections → wrap + queue + signal the listener. */
            dmesh_qp_t *c;
            for (;;) {
                errno = 0;
                c = dmesh_accept(g_cq);
                if (!c) {
                    /* EAGAIN = drained. ENOMEM = dmesh_accept silently DROPPED a
                     * pending first message (alloc failure or a duplicate/reused-uP
                     * accept) — the peer will hang waiting; make it visible, and keep
                     * draining (one queue entry was consumed; more may be pending). */
                    if (errno == ENOMEM) {
                        fprintf(stderr, "[dmesh_preload] accept DROPPED a pending conn "
                                        "(ENOMEM/dup-uP)\n");
                        continue;
                    }
                    break;
                }
                if (g_listener == NULL && g_listener_closed) {
                    dmesh_destroy_qp(c);              /* listener gone for good → FIN back */
                    continue;
                }
                pfd_t *e = pfd_new(c);
                if (!e) { dmesh_destroy_qp(c); continue; }
                e->pport = c->remote_port;
                e->lport = c->local_port;
                c->user_data = e;
                /* dmesh_accept returns the conn HOLDING its first message (plus
                 * any coalesced pipelined ones) — that delivery predates this
                 * entry, so the ready list will never re-edge for it. Assert
                 * readability up front or an epoll app never reads it. */
                efd_signal(e);
                accept_q_push(e);
                pfd_t *l = g_listener;
                if (l) efd_signal(l);
                DBG("accepted conn (peer pod=%d port=%u)", c->remote_pod, c->remote_port);
            }

            /* Conns with inbound → assert their eventfd. user_data is set by
             * THIS thread (accept above) or before first send (connect), so a
             * ready conn always carries its entry. */
            while ((c = dmesh_next_ready(g_cq)) != NULL) {
                pfd_t *e = (pfd_t *)c->user_data;
                if (e) efd_signal(e);
            }
        }

        if (pfds[1].revents & POLLIN) {
            uint64_t v;
            while (real_read(g_wake_fd, &v, sizeof v) > 0) {}
            for (;;) {                    /* reap the close queue */
                pthread_mutex_lock(&g_q_mu);
                pfd_t *e = g_close_head;
                if (e) g_close_head = e->q_next;
                pthread_mutex_unlock(&g_q_mu);
                if (!e) break;
                if (e->listener) {
                    /* Nobody can accept the queued conns anymore — close them (FIN).
                     * Pushed to the close queue we are draining, so they are reaped
                     * in this very loop. Runs AFTER any racing accept-wrap above
                     * (same thread), so no conn can slip into the queue behind us. */
                    pfd_t *p;
                    while ((p = accept_q_pop()) != NULL) close_q_push(p);
                }
                pthread_mutex_lock(&e->mu);      /* serialize vs in-flight read/write */
                dmesh_qp_t *c2 = e->conn;
                e->conn = NULL;
                int fin_sent = e->wr_closed;     /* shutdown(SHUT_WR) already shipped a FIN */
                pthread_mutex_unlock(&e->mu);
                if (c2) {
                    /* Suppress dmesh_destroy_qp's FIN when shutdown already sent one — a
                     * second FIN is harmless on the DPU (teardown fan-out finds no
                     * upstream) but wastes a TX slot + ACK round trip. peer_closed
                     * only gates the FIN; RX credits/port are still reclaimed. */
                    if (fin_sent) c2->peer_closed = 1;
                    dmesh_destroy_qp(c2);              /* single-thread vs next_ready: safe */
                }
                real_close(e->efd);               /* immediately unblocks poll()ers */
                /* GRACE-DELAYED free: a thread blocked in read() on another
                 * thread's close() wakes from the efd close (POLLNVAL) and may
                 * still touch the entry (mu, conn==NULL → EOF). Deferring the
                 * free by a 256-reap ring makes that window safe in practice. */
                pfd_t *old = g_grace[g_grace_i];
                g_grace[g_grace_i] = e;
                g_grace_i = (g_grace_i + 1) % FREE_GRACE;
                if (old) { pthread_mutex_destroy(&old->mu); free(old); }
            }
        }
    }
    return NULL;
}

static pthread_mutex_t g_ch_mu = PTHREAD_MUTEX_INITIALIZER;

/* Create the channel + dispatcher. Called under g_ch_mu; leaves g_ch NULL on
 * failure so a later attempt RETRIES (a register timeout — e.g. the DPU came
 * up after us, or its pod table was momentarily full — must not latch a
 * long-lived process into permanent failure). */
static void channel_init(void) {
    /* Advertise DMESH_PRELOAD_SVC if set (server or mixed process); else a pure
     * client. One channel per process, created on first mapped socket op. */
    int svc = (g_svc != -12345) ? g_svc : DMESH_SVC_NONE;
    dmesh_channel_t *ch = dmesh_create_channel(svc);
    if (!ch) { DBG("dmesh_create_channel(%d) FAILED (will retry)", svc); return; }
    dmesh_cq_t *cq = dmesh_create_cq(ch);
    if (!cq || dmesh_cq_fd(cq) < 0) {   /* no readiness fd → dispatcher would be deaf */
        dmesh_destroy_cq(cq);
        dmesh_destroy_channel(ch);
        DBG("dmesh cq unavailable (will retry)");
        return;
    }
    if (g_wake_fd < 0)                  /* kept across a failed attempt (no re-create leak) */
        g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_t t;
    g_ch = ch; g_cq = cq;               /* dispatcher reads both */
    if (g_wake_fd < 0 || pthread_create(&t, NULL, dispatcher_main, NULL) != 0) {
        g_ch = NULL; g_cq = NULL;
        dmesh_destroy_cq(cq);
        dmesh_destroy_channel(ch);
        DBG("dispatcher start FAILED");
        return;
    }
    pthread_detach(t);
    DBG("channel up: svc=%d pod=%d msg_max=%d", svc, dmesh_pod_id(ch), dmesh_msg_max(ch));
}

static int ensure_channel(void) {
    pthread_mutex_lock(&g_ch_mu);
    if (!g_ch) channel_init();
    int ok = (g_ch != NULL);
    pthread_mutex_unlock(&g_ch_mu);
    return ok ? 0 : -1;
}

/* Occupy the app's fd number with a kernel dup of the private eventfd. */
static int install_fd(int fd, pfd_t *e) {
    if (fd < 0 || fd >= PRELOAD_MAX_FDS) { errno = EMFILE; return -1; }  /* g_fds bound */
    if (real_dup2(e->efd, fd) < 0) return -1;    /* closes the old TCP socket */
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = e;
    e->refs++;
    pthread_mutex_unlock(&g_tbl_mu);
    return 0;
}

/* ====================== blocking-wait helper ====================== */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Wait until the entry's eventfd is readable. timeout_ms 0 = forever.
 * Returns 0 on ready, -1 with errno=EAGAIN on timeout. */
static int wait_ready(pfd_t *e, long timeout_ms) {
    struct pollfd p = { .fd = e->efd, .events = POLLIN };
    int r = poll(&p, 1, timeout_ms > 0 ? (int)timeout_ms : -1);
    if (r > 0) return 0;
    errno = EAGAIN;
    return -1;
}

/* ========================= data-path helpers ==========================
 * The POSIX byte-stream semantics (stream_read / stream_write) are defined below,
 * next to the send path that also uses them; forward-declared here for the recv
 * path. They are the shim's own — the transport does not carry them. */
static ssize_t stream_read(dmesh_qp_t *c, void *buf, size_t len);
static ssize_t stream_write(dmesh_qp_t *c, const void *buf, size_t len);

/* One receive attempt honoring the lost-wakeup discipline. Returns >0 bytes,
 * 0 EOF, or -1/EAGAIN (never blocks). Caller holds e->mu. */
static ssize_t rx_once(pfd_t *e, void *buf, size_t len, int peek) {
    dmesh_qp_t *c = e->conn;
    if (!c || e->rd_closed) return 0;

    if (peek) {
        if (c->rx_slot < 0) {                     /* load the next message, consume 0 */
            char dummy;
            ssize_t l = stream_read(c, &dummy, 0);
            if (l < 0) return -1;                 /* EAGAIN */
            if (c->peer_closed) return 0;         /* the load hit the FIN */
        }
        size_t avail = c->rx_len - c->rx_pos;
        size_t n = len < avail ? len : avail;
        if (n) memcpy(buf, c->rx_buf + c->rx_pos, n);
        if (n) efd_signal(e);                     /* data still pending → stay readable */
        return (ssize_t)n;
    }

    ssize_t n = stream_read(c, buf, len);
    if (n > 0) {
        efd_signal(e);        /* re-assert: more may be pending; over-assert is safe */
        return n;
    }
    if (n == 0) return 0;                          /* EOF (peer FIN) */

    /* EAGAIN: drain the eventfd, then retry ONCE — closes the race where the
     * dispatcher signaled between the failed read and the drain. */
    efd_drain(e);
    n = stream_read(c, buf, len);
    if (n > 0) { efd_signal(e); return n; }
    if (n == 0) return 0;
    errno = EAGAIN;
    return -1;
}

static ssize_t shim_recv(pfd_t *e, void *buf, size_t len, int flags) {
    if (e->listener) { errno = ENOTCONN; return -1; }
    int peek    = flags & MSG_PEEK;
    int waitall = flags & MSG_WAITALL;
    int block   = !(e->nonblock || (flags & MSG_DONTWAIT));
    /* SO_RCVTIMEO caps the WHOLE call, not each wait — partial wakes must not
     * restart the clock. 0 = block forever. */
    long deadline = (block && e->rcv_timeout_ms > 0) ? now_ms() + e->rcv_timeout_ms : 0;
    size_t got = 0;

    for (;;) {
        pthread_mutex_lock(&e->mu);
        ssize_t n = rx_once(e, (char *)buf + got, len - got, peek);
        pthread_mutex_unlock(&e->mu);

        if (n > 0) {
            got += (size_t)n;
            if (peek || !waitall || got >= len) return (ssize_t)got;
            continue;                              /* MSG_WAITALL: keep collecting */
        }
        if (n == 0) return (ssize_t)got;           /* EOF: return what we have */
        if (got > 0 && !waitall) return (ssize_t)got;
        if (!block) { errno = EAGAIN; return -1; }
        long left = 0;                             /* 0 = forever */
        if (deadline) {
            left = deadline - now_ms();
            if (left <= 0) left = -1;              /* already expired */
        }
        if (left < 0 || wait_ready(e, left) < 0) {
            if (got > 0) return (ssize_t)got;
            errno = EAGAIN;                        /* SO_RCVTIMEO expiry */
            return -1;
        }
    }
}

/* Gather-send: ALL iovs accumulate into ONE message (dmesh_write buffers and
 * auto-chunks any length; a pinned conn keeps chunks on its backend), shipped by
 * a single flush — so writev(header, body) costs one message, not two. */

/* ===== POSIX byte-stream semantics — the shim's, and ONLY the shim's =====
 *
 * These two are the price of the socket contract, so they live here rather than in
 * the transport. read(2) MUST copy into caller memory and MUST support consuming a
 * message partially; write(2) MUST accept any length. The native API
 * (<dpumesh/dmesh.h>) owes neither and is zero-copy on both sides because of it.
 * They were public API (dmesh_read/dmesh_write) until the consolidation, which is
 * exactly what forced the native path to pay for POSIX. */

/* read(): copy up to `len` bytes of the NEXT inbound message into buf. One message is
 * atomic (<= slot_size); the conn's rx_pos cursor lets a caller consume it across
 * several reads. When fully consumed the RX credit is freed and the next read fetches
 * a new message. >0 bytes, 0 = EOF (peer FIN; sticky), -1 = EAGAIN. */
static ssize_t stream_read(dmesh_qp_t *c, void *buf, size_t len) {
    if (c->peer_closed) return 0;             /* EOF is sticky once the FIN arrived */
    if (c->rx_slot < 0) {                     /* no message loaded → fetch the next */
        sw_descriptor_t d;
        if (!dpumesh_conn_recv(c->ep->ctx, c->local_port, &d)) { errno = EAGAIN; return -1; }
        if (d.body_len == 0) {                /* FIN marker → EOF: reclaim landing, latch */
            dpumesh_rx_free(c->ep->ctx, d.body_buf_slot);
            c->peer_closed = 1;
            return 0;
        }
        c->rx_slot = d.body_buf_slot;
        c->rx_buf  = dpumesh_rx_buf(c->ep->ctx, d.body_buf_slot);
        c->rx_len  = d.body_len;
        c->rx_pos  = 0;
        /* Model B: a CLIENT does NOT learn/pin a peer — it keeps addressing its
         * service (dst_pod=BLANK) and the DPU owns the upstream. A SERVER conn already
         * learned its peer (client_pod, uP) at accept. So no learn-on-read here. */
    }
    size_t avail = c->rx_len - c->rx_pos;
    size_t n = (len < avail) ? len : avail;
    if (n && c->rx_buf) memcpy(buf, c->rx_buf + c->rx_pos, n);
    c->rx_pos += (uint32_t)n;
    if (c->rx_pos >= c->rx_len) {             /* consumed → free credit, next read fetches */
        dpumesh_rx_free(c->ep->ctx, c->rx_slot);
        c->rx_slot = -1; c->rx_buf = NULL; c->rx_len = 0; c->rx_pos = 0;
    }
    return (ssize_t)n;
}

/* write(): copy `len` bytes into the conn's TX ring (shipped by dmesh_flush). Any
 * length — carved across as many <= post_max reserves as it takes, which is the one
 * thing dmesh_alloc cannot do and a POSIX write must.
 *
 * BLOCKS under backpressure, which is CORRECT here: every shim conn is a blocking
 * socket as far as this path is concerned, and a blocking write(2) blocks until done.
 * The wait used to live inside dpumesh_tx_reserve (a nanosleep ladder in the core hot
 * path); it now sits here, in the one caller whose contract actually asks for it.
 *
 * KNOWN GAP (design/API_REDESIGN.md §6): O_NONBLOCK is not honored on this path — it
 * blocks instead of returning EAGAIN. Fixing that needs honest EPOLLOUT, which the
 * eventfd fd-realization cannot express (an eventfd is always writable), so an app
 * would livelock on epoll→write→EAGAIN. Gated on grow_waits actually being non-zero.
 * Returns len, or -1 on a permanent conn error (never EAGAIN). */
static ssize_t stream_write(dmesh_qp_t *c, const void *buf, size_t len) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t cap = (uint32_t)c->ep->block_size;   /* one reserve <= block_size (contiguous) */
    size_t done = 0;
    struct timespec backoff = {0, 1000};
    while (done < len) {
        uint32_t chunk = (len - done > cap) ? cap : (uint32_t)(len - done);
        uint8_t *dst = dpumesh_tx_reserve(ctx, c->local_port, chunk);
        if (!dst) {
            if (errno != EAGAIN)                  /* EINVAL: conn gone → permanent */
                return done ? (ssize_t)done : -1;
            nanosleep(&backoff, NULL);            /* SQ full: the conn's TX_ACKs free it */
            if (backoff.tv_nsec < 50000) backoff.tv_nsec *= 2;
            continue;
        }
        backoff.tv_nsec = 1000;
        memcpy(dst, p + done, chunk);
        dpumesh_tx_commit(ctx, c->local_port, chunk);   /* append to the send stream */
        done += chunk;
    }
    return (ssize_t)len;
}

static ssize_t shim_send_iov(pfd_t *e, const struct iovec *iov, int cnt) {
    if (e->listener) { errno = ENOTCONN; return -1; }
    size_t total = 0;
    for (int i = 0; i < cnt; i++) total += iov[i].iov_len;
    if (total == 0) return 0;

    pthread_mutex_lock(&e->mu);
    dmesh_qp_t *c = e->conn;
    if (!c || e->wr_closed) {
        pthread_mutex_unlock(&e->mu);
        errno = EPIPE;
        return -1;
    }
    size_t sent = 0;
    for (int i = 0; i < cnt; i++) {
        const char *p = (const char *)iov[i].iov_base;
        size_t len = iov[i].iov_len, done = 0;
        while (done < len) {
            ssize_t w = stream_write(c, p + done, len - done);
            if (w > 0) { done += (size_t)w; continue; }
            dmesh_flush(c);                        /* best-effort: ship what buffered */
            pthread_mutex_unlock(&e->mu);
            errno = ECONNRESET;
            sent += done;
            return sent ? (ssize_t)sent : -1;
        }
        sent += done;
    }
    int fr = dmesh_flush(c);
    pthread_mutex_unlock(&e->mu);
    if (fr < 0) { errno = ECONNRESET; return -1; }
    return (ssize_t)total;
}

static ssize_t shim_send(pfd_t *e, const void *buf, size_t len, int flags) {
    (void)flags;
    struct iovec iov = { (void *)buf, len };
    return shim_send_iov(e, &iov, 1);
}

/* ========================= socket-call surface ========================= */

int connect(int fd, const struct sockaddr *addr, socklen_t alen) {
    ENSURE_REAL();
    if (pfd_get(fd)) { errno = EISCONN; return -1; }
    if (g_route_n > 0 && addr && addr->sa_family == AF_INET && alen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        int svc = route_lookup(sin->sin_addr.s_addr, ntohs(sin->sin_port));
        if (svc >= 0) {
            /* AF_INET SOCK_STREAM only (design/API.md §7) — a UDP connect() to a mapped
             * port must stay kernel. */
            int so_type = 0; socklen_t tl = sizeof so_type;
            if (real_getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &tl) < 0 ||
                so_type != SOCK_STREAM)
                return real_connect(fd, addr, alen);
            if (ensure_channel() < 0) { errno = ENETUNREACH; return -1; }
            dmesh_qp_t *c = dmesh_create_qp(g_cq, svc);
            if (!c) return -1;                     /* ENOMEM */
            dmesh_pin_route(c);                    /* socket contract: one backend, total order */
            pfd_t *e = pfd_new(c);
            if (!e) { dmesh_destroy_qp(c); errno = ENOMEM; return -1; }
            /* preserve O_NONBLOCK the app may have set on the TCP socket */
            int fl = real_fcntl(fd, F_GETFL);
            e->nonblock = (fl >= 0 && (fl & O_NONBLOCK)) ? 1 : 0;
            e->lport = c->local_port;
            e->paddr = sin->sin_addr.s_addr;       /* real dst → getpeername tells the truth */
            e->pport = ntohs(sin->sin_port);
            c->user_data = e;                      /* before any send → before any ready */
            if (install_fd(fd, e) < 0) {
                dmesh_destroy_qp(c); real_close(e->efd); free(e);
                errno = ENOMEM;
                return -1;
            }
            DBG("connect fd=%d port=%u → svc=%d (pinned)", fd, e->pport, svc);
            return 0;                              /* local: immediate success is legal */
        }
    }
    return real_connect(fd, addr, alen);
}

int bind(int fd, const struct sockaddr *addr, socklen_t alen) {
    ENSURE_REAL();
    /* Pass through — the real bind also reserves the port so a half-configured
     * deployment fails loudly instead of silently double-serving. listen()
     * decides on conversion by re-reading the bound port. */
    return real_bind(fd, addr, alen);
}

int listen(int fd, int backlog) {
    ENSURE_REAL();
    if (g_listen_port >= 0 && g_svc != -12345 && !pfd_get(fd)) {
        struct sockaddr_in sin; socklen_t sl = sizeof sin;
        if (real_getsockname(fd, (struct sockaddr *)&sin, &sl) == 0 &&
            sin.sin_family == AF_INET && ntohs(sin.sin_port) == (uint16_t)g_listen_port) {
            if (ensure_channel() < 0) { errno = EADDRNOTAVAIL; return -1; }
            pfd_t *e = pfd_new(NULL);
            if (!e) { errno = ENOMEM; return -1; }
            e->listener = 1;
            int fl = real_fcntl(fd, F_GETFL);
            e->nonblock = (fl >= 0 && (fl & O_NONBLOCK)) ? 1 : 0;
            e->lport = (uint16_t)g_listen_port;
            if (install_fd(fd, e) < 0) {
                real_close(e->efd); free(e);
                errno = ENOMEM;
                return -1;
            }
            g_listener = e;
            g_listener_closed = 0;                /* re-listen resumes queueing */
            pthread_mutex_lock(&g_q_mu);          /* conns that arrived pre-listen */
            int pending = g_accept_head != NULL;
            pthread_mutex_unlock(&g_q_mu);
            if (pending) efd_signal(e);
            DBG("listen fd=%d port=%d → dmesh svc=%d", fd, g_listen_port, g_svc);
            return 0;
        }
    }
    return real_listen(fd, backlog);
}

static int shim_accept(int fd, struct sockaddr *addr, socklen_t *alen, int flags) {
    pfd_t *l = pfd_get(fd);
    if (!l) return -2;                             /* not ours */
    if (!l->listener) { errno = EINVAL; return -1; }

    pfd_t *e;
    for (;;) {
        e = accept_q_pop();
        if (e) break;
        efd_drain(l);
        e = accept_q_pop();                        /* close the signal race */
        if (e) break;
        if (l->nonblock || (flags & SOCK_NONBLOCK)) { errno = EAGAIN; return -1; }
        if (wait_ready(l, 0) < 0) return -1;
    }

    int newfd = real_dup(e->efd);                  /* app-visible fd (clears CLOEXEC) */
    if (newfd < 0) { close_q_push(e); return -1; }
    if (newfd >= PRELOAD_MAX_FDS) { real_close(newfd); close_q_push(e); errno = EMFILE; return -1; }
    e->nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;
    if (flags & SOCK_CLOEXEC) real_fcntl(newfd, F_SETFD, FD_CLOEXEC);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[newfd] = e;
    e->refs++;
    pthread_mutex_unlock(&g_tbl_mu);

    if (addr && alen && *alen >= (socklen_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin.sin_port = htons(e->pport);
        memcpy(addr, &sin, sizeof sin);
        *alen = sizeof sin;
    }
    DBG("accept → fd=%d (peer port=%u)", newfd, e->pport);
    return newfd;
}

int accept(int fd, struct sockaddr *addr, socklen_t *alen) {
    ENSURE_REAL();
    int r = shim_accept(fd, addr, alen, 0);
    return (r == -2) ? real_accept(fd, addr, alen) : r;
}

int accept4(int fd, struct sockaddr *addr, socklen_t *alen, int flags) {
    ENSURE_REAL();
    int r = shim_accept(fd, addr, alen, flags);
    return (r == -2) ? real_accept4(fd, addr, alen, flags) : r;
}

/* ------------------------------ data calls ----------------------------- */

ssize_t read(int fd, void *buf, size_t len) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_read(fd, buf, len);
    return shim_recv(e, buf, len, 0);
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_recv(fd, buf, len, flags);
    return shim_recv(e, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src, socklen_t *slen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_recvfrom(fd, buf, len, flags, src, slen);
    if (src && slen) *slen = 0;
    return shim_recv(e, buf, len, flags);
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_recvmsg(fd, msg, flags);
    ssize_t total = 0;
    msg->msg_controllen = 0;
    msg->msg_flags = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        ssize_t n = shim_recv(e, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len,
                              flags | (total ? MSG_DONTWAIT : 0));
        if (n < 0) return total ? total : -1;
        total += n;
        if (n < (ssize_t)msg->msg_iov[i].iov_len) break;   /* short read = stop */
    }
    return total;
}

ssize_t readv(int fd, const struct iovec *iov, int cnt) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_readv(fd, iov, cnt);
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t n = shim_recv(e, iov[i].iov_base, iov[i].iov_len,
                              total ? MSG_DONTWAIT : 0);
        if (n < 0) return total ? total : -1;
        total += n;
        if (n < (ssize_t)iov[i].iov_len) break;
    }
    return total;
}

ssize_t write(int fd, const void *buf, size_t len) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_write(fd, buf, len);
    return shim_send(e, buf, len, 0);
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_send(fd, buf, len, flags);
    return shim_send(e, buf, len, flags);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dst, socklen_t dlen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_sendto(fd, buf, len, flags, dst, dlen);
    return shim_send(e, buf, len, flags);          /* connected: dst ignored */
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    ENSURE_REAL();
    (void)flags;
    pfd_t *e = pfd_get(fd);
    if (!e) return real_sendmsg(fd, msg, flags);
    return shim_send_iov(e, msg->msg_iov, (int)msg->msg_iovlen);
}

ssize_t writev(int fd, const struct iovec *iov, int cnt) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_writev(fd, iov, cnt);
    return shim_send_iov(e, iov, cnt);
}

static ssize_t sendfile_common(int out_fd, int in_fd, off_t *offset, size_t count,
                               ssize_t (*realsf)(int, int, off_t *, size_t)) {
    pfd_t *e = pfd_get(out_fd);
    if (!e) return realsf(out_fd, in_fd, offset, count);
    char buf[8192];
    size_t done = 0;
    while (done < count) {
        size_t want = count - done; if (want > sizeof buf) want = sizeof buf;
        ssize_t r = offset ? pread(in_fd, buf, want, *offset + (off_t)done)
                           : real_read(in_fd, buf, want);
        if (r < 0) return done ? (ssize_t)done : -1;
        if (r == 0) break;
        ssize_t w = shim_send(e, buf, (size_t)r, 0);
        if (w < 0) return done ? (ssize_t)done : -1;
        done += (size_t)w;
        if (!offset) continue;
    }
    if (offset) *offset += (off_t)done;
    else if (done) { /* non-offset form consumed in_fd via read: already advanced */ }
    return (ssize_t)done;
}

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    ENSURE_REAL();
    return sendfile_common(out_fd, in_fd, offset, count, real_sendfile);
}

ssize_t sendfile64(int out_fd, int in_fd, off_t *offset, size_t count) {
    ENSURE_REAL();
    return sendfile_common(out_fd, in_fd, offset, count,
                           real_sendfile64 ? real_sendfile64 : real_sendfile);
}

/* --------------------------- lifecycle calls --------------------------- */

int close(int fd) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_close(fd);
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[fd] = NULL;
    int last = (--e->refs == 0);
    pthread_mutex_unlock(&g_tbl_mu);
    real_close(fd);                               /* the kernel dup */
    if (last) {
        if (e == g_listener) {
            g_listener = NULL;
            g_listener_closed = 1;
            /* Orphan the pending accept queue: nobody can accept these conns now.
             * Queue them for dmesh_destroy_qp (FIN) — the dispatcher's reap of `e`
             * re-drains, closing the race with a concurrent accept-wrap. */
            pfd_t *p;
            while ((p = accept_q_pop()) != NULL) close_q_push(p);
        }
        close_q_push(e);                          /* dmesh_destroy_qp runs on the dispatcher */
        DBG("close fd=%d (last ref)", fd);
    }
    return 0;
}

int shutdown(int fd, int how) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_shutdown(fd, how);
    if (e->listener) { errno = ENOTCONN; return -1; }
    pthread_mutex_lock(&e->mu);
    if ((how == SHUT_WR || how == SHUT_RDWR) && !e->wr_closed) {
        e->wr_closed = 1;
        /* Approximate half-close: ship buffered bytes, then a FIN. NOTE: the
         * FIN frees the DPU upstream — replies sent after it are undeliverable
         * (the transport has no true half-close; documented limit). */
        if (e->conn) {
            dmesh_flush(e->conn);
            if (!e->conn->peer_closed) dmesh_send_fin(e->conn);
        }
    }
    if (how == SHUT_RD || how == SHUT_RDWR) e->rd_closed = 1;
    pthread_mutex_unlock(&e->mu);
    if (how == SHUT_RD || how == SHUT_RDWR) efd_signal(e);   /* unblock readers → EOF */
    return 0;
}

/* ------------------------- fd-flag / name calls ------------------------ */

static int fcntl_common(int fd, int cmd, void *arg, int (*realf)(int, int, ...)) {
    pfd_t *e = pfd_get(fd);
    if (!e) return realf(fd, cmd, arg);

    switch (cmd) {
    case F_GETFL: {
        int fl = realf(fd, F_GETFL, 0);
        if (fl < 0) return fl;
        return e->nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    }
    case F_SETFL:
        e->nonblock = ((long)(intptr_t)arg & O_NONBLOCK) ? 1 : 0;
        return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int nf = realf(fd, cmd, arg);
        if (nf >= 0 && nf < PRELOAD_MAX_FDS) {
            pthread_mutex_lock(&g_tbl_mu);
            g_fds[nf] = e;
            e->refs++;
            pthread_mutex_unlock(&g_tbl_mu);
        }
        return nf;
    }
    default:
        return realf(fd, cmd, arg);
    }
}

int fcntl(int fd, int cmd, ...) {
    ENSURE_REAL();
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return fcntl_common(fd, cmd, arg, real_fcntl);
}

int fcntl64(int fd, int cmd, ...) {
    ENSURE_REAL();
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return fcntl_common(fd, cmd, arg, real_fcntl64 ? real_fcntl64 : real_fcntl);
}

int ioctl(int fd, unsigned long req, ...) {
    ENSURE_REAL();
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    pfd_t *e = pfd_get(fd);
    if (!e) return real_ioctl(fd, req, arg);

    if (req == FIONBIO && arg) { e->nonblock = *(int *)arg ? 1 : 0; return 0; }
    if (req == FIONREAD && arg) {
        int n = 0;
        pthread_mutex_lock(&e->mu);
        if (e->conn && e->conn->rx_slot >= 0)
            n = (int)(e->conn->rx_len - e->conn->rx_pos);
        pthread_mutex_unlock(&e->mu);
        *(int *)arg = n;                            /* best-effort: loaded message only */
        return 0;
    }
    return real_ioctl(fd, req, arg);
}

int setsockopt(int fd, int level, int optname, const void *val, socklen_t len) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_setsockopt(fd, level, optname, val, len);
    if (level == SOL_SOCKET && optname == SO_RCVTIMEO && val && len >= sizeof(struct timeval)) {
        const struct timeval *tv = (const struct timeval *)val;
        e->rcv_timeout_ms = tv->tv_sec * 1000L + tv->tv_usec / 1000L;
    }
    return 0;                                       /* accepted no-op (TCP_NODELAY, …) */
}

int getsockopt(int fd, int level, int optname, void *val, socklen_t *len) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_getsockopt(fd, level, optname, val, len);
    if (level == SOL_SOCKET && val && len) {
        switch (optname) {
        case SO_ERROR:                              /* connect() is local: never pending */
            if (*len >= sizeof(int)) { *(int *)val = 0; *len = sizeof(int); }
            return 0;
        case SO_TYPE:                               /* apps sanity-check this */
            if (*len >= sizeof(int)) { *(int *)val = SOCK_STREAM; *len = sizeof(int); }
            return 0;
        case SO_SNDBUF:
        case SO_RCVBUF:                             /* NEVER 0 — apps size buffers by it */
            if (*len >= sizeof(int)) { *(int *)val = 262144; *len = sizeof(int); }
            return 0;
        case SO_RCVTIMEO:                           /* mirror what setsockopt stored */
            if (*len >= sizeof(struct timeval)) {
                struct timeval tv = { e->rcv_timeout_ms / 1000,
                                      (e->rcv_timeout_ms % 1000) * 1000 };
                memcpy(val, &tv, sizeof tv);
                *len = sizeof tv;
            }
            return 0;
        }
    }
    if (val && len && *len >= sizeof(int)) { *(int *)val = 0; *len = sizeof(int); }
    return 0;                                       /* unknown: report "off/disabled" */
}

/* Build a sockaddr_in view. addr==0 → loopback (a synthesized name); a CLIENT conn
 * passes its real dialed ClusterIP so getpeername is truthful. */
static int synth_name(uint32_t addr, uint16_t port, struct sockaddr *out, socklen_t *alen) {
    if (!out || !alen) { errno = EFAULT; return -1; }
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = addr ? addr : htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);
    socklen_t n = *alen < (socklen_t)sizeof sin ? *alen : (socklen_t)sizeof sin;
    memcpy(out, &sin, n);
    *alen = sizeof sin;
    return 0;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *alen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_getsockname(fd, addr, alen);
    return synth_name(0, e->lport, addr, alen);   /* loopback — real pod IP is P1 */
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *alen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_getpeername(fd, addr, alen);
    if (e->listener) { errno = ENOTCONN; return -1; }
    return synth_name(e->paddr, e->pport, addr, alen);
}

/* ------------------------------- dup calls ----------------------------- */

static void track_alias(int oldfd, int newfd) {
    if (newfd < 0 || newfd >= PRELOAD_MAX_FDS) return;
    pfd_t *e = pfd_get(oldfd);
    if (!e) return;
    pthread_mutex_lock(&g_tbl_mu);
    g_fds[newfd] = e;
    e->refs++;
    pthread_mutex_unlock(&g_tbl_mu);
}

int dup(int fd) {
    ENSURE_REAL();
    int nf = real_dup(fd);
    if (nf >= 0) track_alias(fd, nf);
    return nf;
}

int dup2(int fd, int nfd) {
    ENSURE_REAL();
    if (pfd_get(nfd) && nfd != fd) close(nfd);      /* our close semantics on the target */
    int nf = real_dup2(fd, nfd);
    if (nf >= 0 && nf != fd) track_alias(fd, nf);
    return nf;
}

int dup3(int fd, int nfd, int flags) {
    ENSURE_REAL();
    if (pfd_get(nfd) && nfd != fd) close(nfd);
    int nf = real_dup3(fd, nfd, flags);
    if (nf >= 0 && nf != fd) track_alias(fd, nf);
    return nf;
}
