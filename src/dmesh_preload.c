/* LD_PRELOAD POSIX-socket facade. Registry-matched TCP endpoints use DPUmesh; all
 * other descriptors remain kernel-backed. A dispatcher translates native
 * events into socket readiness. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
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

#include "dmesh_core.h"    /* The data/event plane uses the public native API.
                            * This in-tree adapter still needs narrow control-plane
                            * hooks for ClusterIP resolution, numeric QP open, and
                            * POSIX shutdown's transport FIN. */

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

static int g_debug;

#define DBG(...) do { if (g_debug) { fprintf(stderr, "[dmesh_preload] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* Identity ($DPUMESH_SERVICE) and routing (ClusterIP:port → svc) both come from the
 * shared registry (src/dmesh_resolve.c) — the shim types no integer. connect() keys
 * on IP:port (dmesh_resolve_addr); listen() converts the port named by $DPUMESH_PORT
 * (dmesh_config_listen_port). The registry is loaded once here at ctor. */
#ifndef DMESH_PRELOAD_TEST
__attribute__((constructor))
static void preload_ctor(void) {
    const char *e;
    g_debug = (e = getenv("DMESH_PRELOAD_DEBUG")) && atoi(e);
    int n = dmesh_config_load(NULL);   /* $DPUMESH_CONFIG or /etc/dpumesh/registry */
    DBG("ctor: listen_port=%d service='%s' registry_entries=%d",
        dmesh_config_listen_port(), getenv("DPUMESH_SERVICE") ? getenv("DPUMESH_SERVICE") : "(none)", n);
}
#endif

/* ============================== fd table =============================== */

#define PRELOAD_MAX_FDS 65536

typedef struct preload_rx {
    dmesh_event_t event;              /* owns one native RX credit until fully consumed */
    uint32_t pos;               /* POSIX partial-read cursor within event.buf */
    struct preload_rx *next;
} preload_rx_t;

typedef struct pfd {
    dmesh_qp_t *conn;        /* NULL for the listener entry */
    int  efd;                  /* PRIVATE app side of a nonblocking socketpair */
    int  sigfd;                /* dispatcher side: RX signal + TX gate drain */
    int  listener;
    int  nonblock;
    int  wr_closed;
    int  rd_closed;
    int  peer_closed;
    int  io_error;              /* sticky transport/RX protocol error */
    int  tx_blocked;            /* efd send buffer filled: kernel reports !POLLOUT */
    int  refs;                 /* app fd aliases (dup); private efd NOT counted */
    int  active_ops;           /* wrappers that acquired this entry from g_fds */
    int  retired;              /* dispatcher closed resources; last op frees it */
    int  closing;              /* queued for dispatcher dmesh_destroy_qp */
    long rcv_timeout_ms;       /* SO_RCVTIMEO; 0 = block forever */
    long snd_timeout_ms;       /* SO_SNDTIMEO; 0 = block forever */
    uint16_t lport;            /* synthesized getsockname port */
    uint16_t pport;            /* getpeername port */
    uint32_t paddr;            /* getpeername IP (net-order); 0 = synthesize loopback.
                                * A CLIENT stores the real dialed ClusterIP here; a
                                * SERVER/accepted conn keeps 0 (real client id is P1). */
    pthread_mutex_t mu;
    pthread_mutex_t tx_mu;      /* serialize bytes from concurrent POSIX send calls */
    preload_rx_t *rx_head, *rx_tail;
    struct pfd *q_next;        /* accept- / close-queue linkage */
} pfd_t;

static pfd_t *g_fds[PRELOAD_MAX_FDS];
static pthread_mutex_t g_tbl_mu = PTHREAD_MUTEX_INITIALIZER;

static pfd_t *pfd_get(int fd) {
    if (fd < 0 || fd >= PRELOAD_MAX_FDS) return NULL;
    pthread_mutex_lock(&g_tbl_mu);
    pfd_t *e = g_fds[fd];
    if (e) e->active_ops++;
    pthread_mutex_unlock(&g_tbl_mu);
    return e;
}

static void pfd_storage_free(pfd_t *e) {
    pthread_mutex_destroy(&e->tx_mu);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

static void pfd_put(pfd_t *e) {
    if (!e) return;
    int free_now = 0;
    pthread_mutex_lock(&g_tbl_mu);
    if (--e->active_ops == 0 && e->retired == 1) {
        e->retired = 2;                  /* this thread owns the final free */
        free_now = 1;
    }
    pthread_mutex_unlock(&g_tbl_mu);
    if (free_now) pfd_storage_free(e);
}

static void pfd_retire(pfd_t *e) {
    int free_now = 0;
    pthread_mutex_lock(&g_tbl_mu);
    e->retired = 1;
    if (e->active_ops == 0) {
        e->retired = 2;
        free_now = 1;
    }
    pthread_mutex_unlock(&g_tbl_mu);
    if (free_now) pfd_storage_free(e);
}

static pfd_t *pfd_new(dmesh_qp_t *c) {
    pfd_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0) {
        free(e);
        return NULL;
    }
    e->conn = c;
    e->efd = sv[0];
    e->sigfd = sv[1];
    /* Keeping the gate small bounds the one-time work needed to suppress POLLOUT
     * after native EAGAIN. Linux may double this value; fd_block_tx drains whatever
     * the kernel actually accepted rather than relying on the requested size. */
    int sndbuf = 1024;
    (void)real_setsockopt(e->efd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    (void)real_setsockopt(e->sigfd, SOL_SOCKET, SO_RCVBUF, &sndbuf, sizeof(sndbuf));
    pthread_mutex_init(&e->mu, NULL);
    pthread_mutex_init(&e->tx_mu, NULL);
    return e;
}

static void efd_signal(pfd_t *e) {
    const uint8_t one = 1;
    ssize_t r = real_write(e->sigfd, &one, sizeof(one));
    (void)r;       /* EAGAIN means an older unread token already keeps fd readable. */
}

static void efd_drain(pfd_t *e) {
    uint8_t buf[256];
    while (real_read(e->efd, buf, sizeof(buf)) > 0) {}
}

/* eventfd is permanently writable and therefore cannot represent POSIX EPOLLOUT.
 * The app-visible end of this socketpair can: fill its otherwise-unused outbound
 * kernel buffer after native EAGAIN, then drain the peer when TX_READY arrives. RX
 * notification travels in the opposite socketpair direction and remains independent. */
static int fd_block_tx_locked(pfd_t *e) {
    if (e->tx_blocked) return 0;
    static const uint8_t fill[65536];
    size_t filled = 0;
    for (;;) {
        ssize_t n = real_write(e->efd, fill, sizeof(fill));
        if (n > 0) { filled += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        e->io_error = errno ? errno : EIO;
        return -1;
    }
    e->tx_blocked = 1;
    DBG("TX gate blocked after %zu kernel bytes (%s)", filled, strerror(errno));
    return 0;                               /* EAGAIN = honest !POLLOUT */
}

static void fd_unblock_tx_locked(pfd_t *e) {
    if (!e->tx_blocked) return;
    uint8_t buf[4096];
    while (real_read(e->sigfd, buf, sizeof(buf)) > 0) {}
    e->tx_blocked = 0;                      /* draining creates the POLLOUT edge */
}

/* ===================== channel + dispatcher thread ===================== */

static dmesh_channel_t *g_ch;
static dmesh_eq_t *g_eq;                 /* the ONE EQ: the dispatcher is the single
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

static void release_rx_list(preload_rx_t *head) {
    while (head) {
        preload_rx_t *next = head->next;
        dmesh_release_rx_buffer(g_ch, &head->event);
        free(head);
        head = next;
    }
}

/* Transfer one native RECV event into the POSIX fd's pull queue. The RX mmap
 * remains zero-copy up to read()/recv(); its credit is released only after the app
 * consumes the final byte. A socket facade supports one pinned L4 stream, so fail
 * closed instead of silently concatenating replies from different DPU streams. */
static int pfd_queue_rx(pfd_t *e, const dmesh_event_t *event) {
    preload_rx_t *rx = calloc(1, sizeof(*rx));
    if (!rx) {
        dmesh_event_t drop = *event;
        dmesh_release_rx_buffer(g_ch, &drop);
        pthread_mutex_lock(&e->mu);
        e->io_error = ENOMEM;
        efd_signal(e);
        pthread_mutex_unlock(&e->mu);
        return -1;
    }
    rx->event = *event;

    pthread_mutex_lock(&e->mu);
    if (e->peer_closed && !e->io_error) {
        e->io_error = EPROTO;               /* data after FIN violates stream order */
        efd_signal(e);
    }
    if (e->io_error || !e->conn) {
        pthread_mutex_unlock(&e->mu);
        dmesh_release_rx_buffer(g_ch, &rx->event);
        free(rx);
        return -1;
    }
    if (e->rx_tail) e->rx_tail->next = rx; else e->rx_head = rx;
    e->rx_tail = rx;
    efd_signal(e);
    pthread_mutex_unlock(&e->mu);
    return 0;
}

static int pfd_rx_fin(pfd_t *e, const dmesh_event_t *event) {
    (void)event;
    pthread_mutex_lock(&e->mu);
    e->peer_closed = 1;
    efd_signal(e);                              /* EOF stays poll-readable */
    pthread_mutex_unlock(&e->mu);
    return 0;
}

static void pfd_tx_ready(pfd_t *e) {
    pthread_mutex_lock(&e->mu);
    fd_unblock_tx_locked(e);                    /* creates the app fd's POLLOUT edge */
    pthread_mutex_unlock(&e->mu);
}

static void defer_qp_once(dmesh_qp_t **qps, int *nqps, int cap, dmesh_qp_t *qp) {
    if (!qp) return;
    for (int i = 0; i < *nqps; i++) if (qps[i] == qp) return;
    if (*nqps < cap) qps[(*nqps)++] = qp;
}

/* One EQ consumer for every native event type. QP destruction is deferred until
 * the complete returned batch has been inspected because later entries may name the
 * same QP. */
static void dispatcher_drain_eq(void) {
    dmesh_event_t events[64];
    for (;;) {
        int n = dmesh_poll_eq(g_eq, events, (int)(sizeof(events) / sizeof(events[0])));
        if (n == 0) return;
        if (n < 0) {
            DBG("dmesh_poll_eq failed: %s", strerror(errno));
            return;
        }
        dmesh_qp_t *deferred[64];
        int ndeferred = 0;

        for (int i = 0; i < n; i++) {
            dmesh_qp_t *c = events[i].qp;
            pfd_t *e = c ? (pfd_t *)c->user_data : NULL;
            switch (events[i].type) {
            case DMESH_EVENT_CONN_REQ:
                if (!c) break;
                if (g_listener == NULL && g_listener_closed) {
                    defer_qp_once(deferred, &ndeferred, 64, c);
                    break;
                }
                e = pfd_new(c);
                if (!e) {
                    defer_qp_once(deferred, &ndeferred, 64, c);
                    break;
                }
                e->pport = c->remote_port;
                e->lport = c->local_port;
                c->user_data = e;
                accept_q_push(e);
                if (g_listener) efd_signal(g_listener);
                DBG("accepted conn (peer pod=%d port=%u)", c->remote_pod,
                    c->remote_port);
                break;

            case DMESH_EVENT_RECV:
                if (!e || pfd_queue_rx(e, &events[i]) != 0) {
                    if (!e) dmesh_release_rx_buffer(g_ch, &events[i]);
                    if (e) defer_qp_once(deferred, &ndeferred, 64, c);
                }
                break;

            case DMESH_EVENT_RECV_FIN:
                if (e && pfd_rx_fin(e, &events[i]) != 0)
                    defer_qp_once(deferred, &ndeferred, 64, c);
                break;

            case DMESH_EVENT_TX_READY:
                if (e) pfd_tx_ready(e);
                break;

            default:
                dmesh_release_rx_buffer(g_ch, &events[i]);
                if (e) {
                    pthread_mutex_lock(&e->mu);
                    e->io_error = EPROTO;
                    efd_signal(e);
                    pthread_mutex_unlock(&e->mu);
                    defer_qp_once(deferred, &ndeferred, 64, c);
                }
                break;
            }
        }

        for (int i = 0; i < ndeferred; i++) {
            dmesh_qp_t *c = deferred[i];
            pfd_t *e = c ? (pfd_t *)c->user_data : NULL;
            if (e) {
                pthread_mutex_lock(&e->mu);
                if (e->conn == c) e->conn = NULL;
                if (!e->io_error) e->io_error = ECONNRESET;
                fd_unblock_tx_locked(e);
                efd_signal(e);
                pthread_mutex_unlock(&e->mu);
            }
            dmesh_abort_qp(c);
        }
    }
}

static void *dispatcher_main(void *arg) {
    (void)arg;
    int eq_fd = dmesh_eq_fd(g_eq);
    struct pollfd pfds[2] = {
        { .fd = eq_fd,     .events = POLLIN },
        { .fd = g_wake_fd, .events = POLLIN },
    };
    DBG("dispatcher up (eq_fd=%d wake_fd=%d)", eq_fd, g_wake_fd);
    for (;;) {
        (void)poll(pfds, 2, -1);

        if (pfds[0].revents & POLLIN) {
            uint64_t v;
            while (real_read(eq_fd, &v, sizeof v) > 0) {}
            dispatcher_drain_eq();
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
                preload_rx_t *rx = e->rx_head;
                e->rx_head = e->rx_tail = NULL;
                fd_unblock_tx_locked(e);
                pthread_mutex_unlock(&e->mu);
                release_rx_list(rx);
                if (c2)
                    dmesh_destroy_qp(c2);              /* single-thread vs EQ polling: safe.
                                                        * A shutdown(SHUT_WR) FIN already sent
                                                        * latched fin_sent, so this sends no
                                                        * second one. */
                real_close(e->sigfd);             /* HUP immediately unblocks poll()ers */
                real_close(e->efd);
                /* A blocking wrapper may still be waking from POLLNVAL. The entry
                 * is freed exactly when its final active-operation reference drops. */
                pfd_retire(e);
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
    /* Identity is injected: dmesh_create_channel() resolves $DPUMESH_SERVICE via the
     * registry (a server/mixed process) or opens a pure client if unset. One channel
     * per process, created on first mapped socket op. */
    dmesh_channel_t *ch = dmesh_create_channel();
    if (!ch) { DBG("dmesh_create_channel() FAILED (will retry)"); return; }
    dmesh_eq_t *eq = dmesh_create_eq(ch);
    if (!eq || dmesh_eq_fd(eq) < 0) {   /* no readiness fd → dispatcher would be deaf */
        dmesh_destroy_eq(eq);
        dmesh_destroy_channel(ch);
        DBG("dmesh eq unavailable (will retry)");
        return;
    }
    if (g_wake_fd < 0)                  /* kept across a failed attempt (no re-create leak) */
        g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_t t;
    g_ch = ch; g_eq = eq;               /* dispatcher reads both */
    if (g_wake_fd < 0 || pthread_create(&t, NULL, dispatcher_main, NULL) != 0) {
        g_ch = NULL; g_eq = NULL;
        dmesh_destroy_eq(eq);
        dmesh_destroy_channel(ch);
        DBG("dispatcher start FAILED");
        return;
    }
    pthread_detach(t);
    DBG("channel up: service='%s' pod=%d msg_max=%d",
        getenv("DPUMESH_SERVICE") ? getenv("DPUMESH_SERVICE") : "(none)",
        dmesh_pod_id(ch), dmesh_msg_max(ch));
}

static int ensure_channel(void) {
    pthread_mutex_lock(&g_ch_mu);
    if (!g_ch) channel_init();
    int ok = (g_ch != NULL);
    pthread_mutex_unlock(&g_ch_mu);
    return ok ? 0 : -1;
}

/* Occupy the app's fd number with a kernel dup of the private socketpair end. */
static int install_fd(int fd, pfd_t *e) {
    if (fd < 0 || fd >= PRELOAD_MAX_FDS) { errno = EMFILE; return -1; }  /* g_fds bound */
    int fd_flags = real_fcntl(fd, F_GETFD);
    if (fd_flags < 0) return -1;
    int rc = (fd_flags & FD_CLOEXEC)
           ? real_dup3(e->efd, fd, O_CLOEXEC)
           : real_dup2(e->efd, fd);
    if (rc < 0) return -1;                       /* replaces the TCP socket */
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

/* Wait until the entry's socketpair end is readable. timeout_ms 0 = forever.
 * Returns 0 on ready, -1 with errno=EAGAIN on timeout. */
static int wait_ready(pfd_t *e, long timeout_ms) {
    struct pollfd p = { .fd = e->efd, .events = POLLIN };
    int r = poll(&p, 1, timeout_ms > 0 ? (int)timeout_ms : -1);
    if (r > 0 && (p.revents & POLLIN)) return 0;
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        errno = ECONNRESET;
        return -1;
    }
    errno = EAGAIN;
    return -1;
}

static int wait_writable(pfd_t *e, long timeout_ms) {
    struct pollfd p = { .fd = e->efd, .events = POLLOUT };
    int r = poll(&p, 1, timeout_ms > 0 ? (int)timeout_ms : -1);
    if (r > 0 && (p.revents & POLLOUT)) return 0;
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        errno = EPIPE;
        return -1;
    }
    errno = EAGAIN;
    return -1;
}

/* ========================= data-path helpers ==========================
 * Native RECV events remain queued on each pfd until POSIX read consumes them;
 * this preserves the single required RX copy without exposing transport fragments. */

/* One receive attempt honoring the lost-wakeup discipline. Returns >0 bytes, 0 EOF,
 * or -1/EAGAIN (never blocks). Caller holds e->mu. */
static ssize_t rx_once(pfd_t *e, void *buf, size_t len, int peek) {
    if (e->rd_closed) return 0;
    preload_rx_t *rx = e->rx_head;
    if (!rx) {
        if (e->io_error || !e->conn) {
            errno = e->io_error ? e->io_error : ECONNRESET;
            return -1;
        }
        if (e->peer_closed) return 0;
        /* Drain while holding e->mu. A dispatcher that was waiting on the mutex
         * queues+signals only after we unlock, closing the empty/drain race. */
        efd_drain(e);
        errno = EAGAIN;
        return -1;
    }

    size_t avail = (size_t)rx->event.len - rx->pos;
    size_t n = len < avail ? len : avail;
    if (n) memcpy(buf, rx->event.buf + rx->pos, n);
    if (!peek) {
        rx->pos += (uint32_t)n;
        if (rx->pos == rx->event.len) {
            e->rx_head = rx->next;
            if (!e->rx_head) e->rx_tail = NULL;
            dmesh_release_rx_buffer(g_ch, &rx->event);
            free(rx);
        }
    }
    if (peek || e->rx_head || e->peer_closed || e->io_error) efd_signal(e);
    if (n > 0) return (ssize_t)n;
    errno = EAGAIN;
    return -1;
}

static ssize_t shim_recv(pfd_t *e, void *buf, size_t len, int flags) {
    if (e->listener) { errno = ENOTCONN; return -1; }
    if (len == 0) return 0;
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
        int saved_errno = errno;
        if (saved_errno != EAGAIN)
            return got ? (ssize_t)got : -1;
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

/* Copy as much as native admission currently accepts. Caller holds e->mu. A return
 * shorter than len with errno=EAGAIN means native alloc armed one TX_READY and this
 * function made the app fd honestly non-writable. */
static ssize_t stream_write_locked(pfd_t *e, const void *buf, size_t len) {
    dmesh_qp_t *c = e->conn;
    if (!c || e->wr_closed || e->io_error) {
        errno = e->io_error ? e->io_error : EPIPE;
        return -1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t cap = (uint32_t)c->ep->block_size;   /* one reserve <= block_size (contiguous) */
    size_t done = 0;
    while (done < len) {
        uint32_t chunk = (len - done > cap) ? cap : (uint32_t)(len - done);
        uint8_t *dst = dmesh_alloc(c, chunk);
        if (!dst) {
            if (errno == EAGAIN) {
                if (done > 0 && dmesh_flush(c) != 0) {
                    e->io_error = errno ? errno : EIO;
                    return (ssize_t)done;
                }
                if (fd_block_tx_locked(e) != 0)
                    return done ? (ssize_t)done : -1;
                errno = EAGAIN;
            }
            return done ? (ssize_t)done : -1;
        }
        /* A successful opportunistic retry cancels a stale native TX_READY. Mirror
         * that cancellation in the kernel readiness fd as well. */
        fd_unblock_tx_locked(e);
        memcpy(dst, p + done, chunk);
        if (dmesh_post_send(c, dst, chunk) != 0) {
            e->io_error = errno ? errno : EIO;
            return done ? (ssize_t)done : -1;
        }
        done += chunk;
    }
    return (ssize_t)len;
}

static ssize_t shim_send_iov(pfd_t *e, const struct iovec *iov, int cnt, int flags) {
    if (e->listener) { errno = ENOTCONN; return -1; }
    if (cnt < 0 || (cnt > 0 && !iov)) { errno = EINVAL; return -1; }
    size_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len > (size_t)SSIZE_MAX - total) {
            errno = EINVAL;
            return -1;
        }
        total += iov[i].iov_len;
    }
    if (total == 0) return 0;

    pthread_mutex_lock(&e->tx_mu);
    pthread_mutex_lock(&e->mu);
    int block = !(e->nonblock || (flags & MSG_DONTWAIT));
    long deadline = (block && e->snd_timeout_ms > 0) ? now_ms() + e->snd_timeout_ms : 0;
    pthread_mutex_unlock(&e->mu);
    size_t sent = 0;
    for (int i = 0; i < cnt; i++) {
        const char *p = (const char *)iov[i].iov_base;
        size_t len = iov[i].iov_len, done = 0;
        while (done < len) {
            size_t remaining = len - done;
            pthread_mutex_lock(&e->mu);
            errno = 0;
            ssize_t w = stream_write_locked(e, p + done, remaining);
            int saved_errno = errno;
            pthread_mutex_unlock(&e->mu);
            if (w > 0) {
                done += (size_t)w;
                sent += (size_t)w;
                if ((size_t)w == remaining) continue;
            }
            if (saved_errno != EAGAIN) {
                pthread_mutex_unlock(&e->tx_mu);
                errno = saved_errno ? saved_errno : ECONNRESET;
                return sent ? (ssize_t)sent : -1;
            }
            if (!block) {
                pthread_mutex_unlock(&e->tx_mu);
                errno = EAGAIN;
                return sent ? (ssize_t)sent : -1;
            }
            long left = 0;
            if (deadline) {
                left = deadline - now_ms();
                if (left <= 0) {
                    pthread_mutex_unlock(&e->tx_mu);
                    errno = EAGAIN;
                    return sent ? (ssize_t)sent : -1;
                }
            }
            if (wait_writable(e, left) != 0) {
                int wait_errno = errno;
                pthread_mutex_unlock(&e->tx_mu);
                errno = wait_errno;
                return sent ? (ssize_t)sent : -1;
            }
        }
    }

    pthread_mutex_lock(&e->mu);
    dmesh_qp_t *c = e->conn;
    int fr = c ? dmesh_flush(c) : -1;
    int saved_errno = errno;
    if (fr < 0) e->io_error = saved_errno ? saved_errno : ECONNRESET;
    pthread_mutex_unlock(&e->mu);
    pthread_mutex_unlock(&e->tx_mu);
    if (fr < 0) { errno = saved_errno ? saved_errno : ECONNRESET; return -1; }
    return (ssize_t)total;
}

static ssize_t shim_send(pfd_t *e, const void *buf, size_t len, int flags) {
    struct iovec iov = { (void *)buf, len };
    return shim_send_iov(e, &iov, 1, flags);
}

/* ========================= socket-call surface ========================= */

int connect(int fd, const struct sockaddr *addr, socklen_t alen) {
    ENSURE_REAL();
    pfd_t *existing = pfd_get(fd);
    if (existing) { pfd_put(existing); errno = EISCONN; return -1; }
    if (addr && addr->sa_family == AF_INET && alen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        int svc = dmesh_resolve_addr(sin->sin_addr.s_addr, ntohs(sin->sin_port));
        if (svc >= 0) {
            /* AF_INET SOCK_STREAM only (design/API.md §7) — a UDP connect() to a mapped
             * port must stay kernel. */
            int so_type = 0; socklen_t tl = sizeof so_type;
            if (real_getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &tl) < 0 ||
                so_type != SOCK_STREAM)
                return real_connect(fd, addr, alen);
            if (ensure_channel() < 0) { errno = ENETUNREACH; return -1; }
            dmesh_qp_t *c = dmesh_qp_open(g_eq, svc);
            if (!c) return -1;                     /* ENOMEM */
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
                dmesh_destroy_qp(c);
                real_close(e->sigfd); real_close(e->efd);
                pthread_mutex_destroy(&e->tx_mu); pthread_mutex_destroy(&e->mu);
                free(e);
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
    int listen_port = dmesh_config_listen_port();   /* $DPUMESH_PORT, -1 = not a server */
    pfd_t *existing = pfd_get(fd);
    if (existing) pfd_put(existing);
    if (listen_port >= 0 && !existing) {
        struct sockaddr_in sin; socklen_t sl = sizeof sin;
        if (real_getsockname(fd, (struct sockaddr *)&sin, &sl) == 0 &&
            sin.sin_family == AF_INET && ntohs(sin.sin_port) == (uint16_t)listen_port) {
            if (ensure_channel() < 0) { errno = EADDRNOTAVAIL; return -1; }
            pfd_t *e = pfd_new(NULL);
            if (!e) { errno = ENOMEM; return -1; }
            e->listener = 1;
            int fl = real_fcntl(fd, F_GETFL);
            e->nonblock = (fl >= 0 && (fl & O_NONBLOCK)) ? 1 : 0;
            e->lport = (uint16_t)listen_port;
            if (install_fd(fd, e) < 0) {
                real_close(e->sigfd); real_close(e->efd);
                pthread_mutex_destroy(&e->tx_mu); pthread_mutex_destroy(&e->mu);
                free(e);
                errno = ENOMEM;
                return -1;
            }
            g_listener = e;
            g_listener_closed = 0;                /* re-listen resumes queueing */
            pthread_mutex_lock(&g_q_mu);          /* conns that arrived pre-listen */
            int pending = g_accept_head != NULL;
            pthread_mutex_unlock(&g_q_mu);
            if (pending) efd_signal(e);
            DBG("listen fd=%d port=%d → dmesh service='%s'", fd, listen_port,
                getenv("DPUMESH_SERVICE") ? getenv("DPUMESH_SERVICE") : "(none)");
            return 0;
        }
    }
    return real_listen(fd, backlog);
}

static int shim_accept(int fd, struct sockaddr *addr, socklen_t *alen, int flags) {
    pfd_t *l = pfd_get(fd);
    if (!l) return -2;                             /* not ours */
    if (!l->listener) { pfd_put(l); errno = EINVAL; return -1; }

    pfd_t *e;
    for (;;) {
        e = accept_q_pop();
        if (e) break;
        efd_drain(l);
        e = accept_q_pop();                        /* close the signal race */
        if (e) break;
        if (l->nonblock || (flags & SOCK_NONBLOCK)) {
            pfd_put(l); errno = EAGAIN; return -1;
        }
        if (wait_ready(l, 0) < 0) { pfd_put(l); return -1; }
    }

    int newfd = real_dup(e->efd);                  /* app-visible fd (clears CLOEXEC) */
    if (newfd < 0) { close_q_push(e); pfd_put(l); return -1; }
    if (newfd >= PRELOAD_MAX_FDS) {
        real_close(newfd); close_q_push(e); pfd_put(l); errno = EMFILE; return -1;
    }
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
    pfd_put(l);
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
    ssize_t r = shim_recv(e, buf, len, 0);
    pfd_put(e);
    return r;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_recv(fd, buf, len, flags);
    ssize_t r = shim_recv(e, buf, len, flags);
    pfd_put(e);
    return r;
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src, socklen_t *slen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_recvfrom(fd, buf, len, flags, src, slen);
    if (src && slen) *slen = 0;
    ssize_t r = shim_recv(e, buf, len, flags);
    pfd_put(e);
    return r;
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
        if (n < 0) { pfd_put(e); return total ? total : -1; }
        total += n;
        if (n < (ssize_t)msg->msg_iov[i].iov_len) break;   /* short read = stop */
    }
    pfd_put(e);
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
        if (n < 0) { pfd_put(e); return total ? total : -1; }
        total += n;
        if (n < (ssize_t)iov[i].iov_len) break;
    }
    pfd_put(e);
    return total;
}

ssize_t write(int fd, const void *buf, size_t len) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_write(fd, buf, len);
    ssize_t r = shim_send(e, buf, len, 0);
    pfd_put(e);
    return r;
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_send(fd, buf, len, flags);
    ssize_t r = shim_send(e, buf, len, flags);
    pfd_put(e);
    return r;
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dst, socklen_t dlen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_sendto(fd, buf, len, flags, dst, dlen);
    ssize_t r = shim_send(e, buf, len, flags);     /* connected: dst ignored */
    pfd_put(e);
    return r;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_sendmsg(fd, msg, flags);
    if (!msg || msg->msg_iovlen > INT_MAX) {
        pfd_put(e); errno = EINVAL; return -1;
    }
    ssize_t r = shim_send_iov(e, msg->msg_iov, (int)msg->msg_iovlen, flags);
    pfd_put(e);
    return r;
}

ssize_t writev(int fd, const struct iovec *iov, int cnt) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_writev(fd, iov, cnt);
    ssize_t r = shim_send_iov(e, iov, cnt, 0);
    pfd_put(e);
    return r;
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
        if (r < 0) { ssize_t out = done ? (ssize_t)done : -1; pfd_put(e); return out; }
        if (r == 0) break;
        ssize_t w = shim_send(e, buf, (size_t)r, 0);
        if (w < 0) { ssize_t out = done ? (ssize_t)done : -1; pfd_put(e); return out; }
        done += (size_t)w;
        if (w < r) {
            if (!offset) {
                off_t unread = (off_t)(r - w);
                if (lseek(in_fd, -unread, SEEK_CUR) < 0 && done == 0) {
                    pfd_put(e);
                    return -1;
                }
            }
            break;
        }
    }
    if (offset) *offset += (off_t)done;
    else if (done) { /* non-offset form consumed in_fd via read: already advanced */ }
    pfd_put(e);
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
    pfd_put(e);
    return 0;
}

int shutdown(int fd, int how) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_shutdown(fd, how);
    if (e->listener) { pfd_put(e); errno = ENOTCONN; return -1; }
    pthread_mutex_lock(&e->mu);
    if ((how == SHUT_WR || how == SHUT_RDWR) && !e->wr_closed) {
        e->wr_closed = 1;
        /* Approximate half-close: ship buffered bytes, then a FIN. NOTE: the
         * FIN frees the DPU upstream — replies sent after it are undeliverable
         * (the transport has no true half-close; documented limit). */
        if (e->conn) {
            if (dmesh_flush(e->conn) != 0 || dmesh_send_fin(e->conn) != 0) {
                pthread_mutex_unlock(&e->mu);
                pfd_put(e);
                errno = ECONNRESET;
                return -1;
            }
        }
    }
    if (how == SHUT_RD || how == SHUT_RDWR) e->rd_closed = 1;
    pthread_mutex_unlock(&e->mu);
    if (how == SHUT_RD || how == SHUT_RDWR) efd_signal(e);   /* unblock readers → EOF */
    pfd_put(e);
    return 0;
}

/* ------------------------- fd-flag / name calls ------------------------ */

static int fcntl_no_arg(int cmd) {
    switch (cmd) {
    case F_GETFD:
    case F_GETFL:
#ifdef F_GETOWN
    case F_GETOWN:
#endif
#ifdef F_GETSIG
    case F_GETSIG:
#endif
#ifdef F_GETLEASE
    case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
    case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
    case F_GET_SEALS:
#endif
        return 1;
    default:
        return 0;
    }
}

static int fcntl_common(int fd, int cmd, void *arg, int no_arg,
                        int (*realf)(int, int, ...)) {
    pfd_t *e = pfd_get(fd);
    if (!e) return no_arg ? realf(fd, cmd) : realf(fd, cmd, arg);

    switch (cmd) {
    case F_GETFL: {
        int fl = realf(fd, F_GETFL);
        if (fl >= 0) fl = e->nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        pfd_put(e);
        return fl;
    }
    case F_SETFL:
        pthread_mutex_lock(&e->mu);
        e->nonblock = ((long)(intptr_t)arg & O_NONBLOCK) ? 1 : 0;
        pthread_mutex_unlock(&e->mu);
        pfd_put(e);
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
        pfd_put(e);
        return nf;
    }
    default: {
        int r = no_arg ? realf(fd, cmd) : realf(fd, cmd, arg);
        pfd_put(e);
        return r;
    }
    }
}

int fcntl(int fd, int cmd, ...) {
    ENSURE_REAL();
    int no_arg = fcntl_no_arg(cmd);
    void *arg = NULL;
    if (!no_arg) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, void *);
        va_end(ap);
    }
    return fcntl_common(fd, cmd, arg, no_arg, real_fcntl);
}

int fcntl64(int fd, int cmd, ...) {
    ENSURE_REAL();
    int no_arg = fcntl_no_arg(cmd);
    void *arg = NULL;
    if (!no_arg) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, void *);
        va_end(ap);
    }
    return fcntl_common(fd, cmd, arg, no_arg,
                        real_fcntl64 ? real_fcntl64 : real_fcntl);
}

int ioctl(int fd, unsigned long req, ...) {
    ENSURE_REAL();
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    pfd_t *e = pfd_get(fd);
    if (!e) return real_ioctl(fd, req, arg);

    if (req == FIONBIO && arg) {
        pthread_mutex_lock(&e->mu);
        e->nonblock = *(int *)arg ? 1 : 0;
        pthread_mutex_unlock(&e->mu);
        pfd_put(e);
        return 0;
    }
    if (req == FIONREAD && arg) {
        size_t available = 0;
        pthread_mutex_lock(&e->mu);
        for (preload_rx_t *rx = e->rx_head; rx; rx = rx->next)
            available += (size_t)rx->event.len - rx->pos;
        pthread_mutex_unlock(&e->mu);
        *(int *)arg = available > INT_MAX ? INT_MAX : (int)available;
        pfd_put(e);
        return 0;
    }
    int r = real_ioctl(fd, req, arg);
    pfd_put(e);
    return r;
}

int setsockopt(int fd, int level, int optname, const void *val, socklen_t len) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_setsockopt(fd, level, optname, val, len);
    if (level == SOL_SOCKET && val && len >= sizeof(struct timeval) &&
        (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)) {
        const struct timeval *tv = (const struct timeval *)val;
        long timeout = tv->tv_sec * 1000L + tv->tv_usec / 1000L;
        pthread_mutex_lock(&e->mu);
        if (optname == SO_RCVTIMEO) e->rcv_timeout_ms = timeout;
        else e->snd_timeout_ms = timeout;
        pthread_mutex_unlock(&e->mu);
    }
    pfd_put(e);
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
            pfd_put(e);
            return 0;
        case SO_TYPE:                               /* apps sanity-check this */
            if (*len >= sizeof(int)) { *(int *)val = SOCK_STREAM; *len = sizeof(int); }
            pfd_put(e);
            return 0;
        case SO_SNDBUF:
        case SO_RCVBUF:                             /* NEVER 0 — apps size buffers by it */
            if (*len >= sizeof(int)) { *(int *)val = 262144; *len = sizeof(int); }
            pfd_put(e);
            return 0;
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {                         /* mirror what setsockopt stored */
            if (*len >= sizeof(struct timeval)) {
                pthread_mutex_lock(&e->mu);
                long timeout = optname == SO_RCVTIMEO ? e->rcv_timeout_ms
                                                       : e->snd_timeout_ms;
                pthread_mutex_unlock(&e->mu);
                struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
                memcpy(val, &tv, sizeof tv);
                *len = sizeof tv;
            }
            pfd_put(e);
            return 0;
        }
        }
    }
    if (val && len && *len >= sizeof(int)) { *(int *)val = 0; *len = sizeof(int); }
    pfd_put(e);
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
    int r = synth_name(0, e->lport, addr, alen);   /* loopback — real pod IP is P1 */
    pfd_put(e);
    return r;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *alen) {
    ENSURE_REAL();
    pfd_t *e = pfd_get(fd);
    if (!e) return real_getpeername(fd, addr, alen);
    if (e->listener) { pfd_put(e); errno = ENOTCONN; return -1; }
    int r = synth_name(e->paddr, e->pport, addr, alen);
    pfd_put(e);
    return r;
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
    pfd_put(e);
}

int dup(int fd) {
    ENSURE_REAL();
    int nf = real_dup(fd);
    if (nf >= 0) track_alias(fd, nf);
    return nf;
}

int dup2(int fd, int nfd) {
    ENSURE_REAL();
    pfd_t *target = pfd_get(nfd);
    if (target) pfd_put(target);
    if (target && nfd != fd) close(nfd);            /* our close semantics on the target */
    int nf = real_dup2(fd, nfd);
    if (nf >= 0 && nf != fd) track_alias(fd, nf);
    return nf;
}

int dup3(int fd, int nfd, int flags) {
    ENSURE_REAL();
    pfd_t *target = pfd_get(nfd);
    if (target) pfd_put(target);
    if (target && nfd != fd) close(nfd);
    int nf = real_dup3(fd, nfd, flags);
    if (nf >= 0 && nf != fd) track_alias(fd, nf);
    return nf;
}
