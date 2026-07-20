/* Native self-routing validator.
 *
 * One thread and CQ drive client and echo roles for the same registered service.
 * The test verifies DPU service resolution and client/upstream port demultiplexing.
 * Command: RUN <count> <size> [zero-copy]. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <dpumesh/dmesh.h>

/* Send a best-effort control reply; failures are ignored. */
static void ctl_reply(int fd, const char *s, size_t n) { (void)!write(fd, s, n); }


#define CTRL_PORT   9092
#define CQ_BATCH    16
#define MAX_SERVERS 4096

static dmesh_channel_t *g_s      = NULL;
static dmesh_cq_t      *g_cq     = NULL;  /* the one CQ both roles are polled from */
static const char      *g_service = NULL; /* own service NAME (self-routing: connect to ourselves) */
static int              g_msgmax  = 0;    /* max bytes per RECV == max bytes per echo post */
static long             g_served  = 0;    /* requests the echo side replied to */

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
/* 2us idle backoff, taken only when a pump found NO work: the CQ is the one wake
 * source, and SQ space frees silently (there are no send completions), so both
 * kinds of wait re-poll rather than sleep on the fd. */
static void idle_wait(void) { struct timespec t = {0, 2000}; nanosleep(&t, NULL); }

/* Per-server-conn echo backlog (dmesh_qp_t::user_data). Empty in steady state —
 * a request is echoed straight out of the RX mmap into the TX ring. It only fills
 * if dmesh_alloc reports EAGAIN (the conn's SQ is at its in-flight ceiling): the
 * RX buffer is transport memory that must be released promptly, so the bytes are
 * parked here in arrival order and a later pump ships them. */
typedef struct { uint8_t *q; size_t cap, len; int dead; } sstate_t;

/* Everything pump() touches: the echo side's conns plus the client's reply
 * reassembly (a reply larger than one slot lands as several RECVs). */
typedef struct {
    dmesh_qp_t *cl;                    /* the one client conn (reopened on fault) */
    uint8_t      *rb;                    /* reply reassembly buffer (`size` bytes) */
    uint32_t      size, got;             /* expected / landed reply bytes */
    int           bad, eof;              /* over-long reply / peer FIN */
    dmesh_qp_t *servers[MAX_SERVERS];
    int           nserv;
} rstate_t;

/* ---- server side: echo every looped-back request verbatim ---- */

static int sq_push(sstate_t *ss, const uint8_t *b, uint32_t n) {
    if (ss->len + n > ss->cap) {
        size_t cap = ss->cap ? ss->cap : 65536;
        while (cap < ss->len + n) cap *= 2;
        uint8_t *q = realloc(ss->q, cap);
        if (!q) return -1;
        ss->q = q; ss->cap = cap;
    }
    memcpy(ss->q + ss->len, b, n);
    ss->len += n;
    return 0;
}

/* Ship as much of the backlog as the SQ will take. Returns messages posted; a
 * permanent fault (non-EAGAIN alloc / descriptor enqueue fault) flags the conn. */
static int sq_drain(dmesh_qp_t *c) {
    sstate_t *ss = (sstate_t *)c->user_data;
    int posted = 0;
    while (ss && !ss->dead && ss->len) {
        uint32_t n = ss->len > (size_t)g_msgmax ? (uint32_t)g_msgmax : (uint32_t)ss->len;
        uint8_t *b = (uint8_t *)dmesh_alloc(c, n);
        if (!b) { if (errno != EAGAIN) ss->dead = 1; break; }
        memcpy(b, ss->q, n);
        if (dmesh_post_send(c, b, n, 0, 0) != 0) { ss->dead = 1; break; }
        ss->len -= n; memmove(ss->q, ss->q + n, ss->len);
        g_served++; posted++;
    }
    if (posted && dmesh_flush(c) != 0) ss->dead = 1;
    return posted;
}

/* Echo one inbound request. A fault only FLAGS the conn: it must stay alive until
 * the whole wc[] batch is dispatched, since later entries may still name it. */
static void srv_recv(dmesh_qp_t *c, const dmesh_wc_t *w) {
    sstate_t *ss = (sstate_t *)c->user_data;
    if (!ss || ss->dead) return;                    /* untracked / already faulted */
    if (ss->len == 0) {                             /* fast path: RX mmap -> TX ring */
        uint8_t *b = (uint8_t *)dmesh_alloc(c, w->len);
        if (b) {
            memcpy(b, w->buf, w->len);
            if (dmesh_post_send(c, b, w->len, 0, 0) != 0 ||
                dmesh_flush(c) != 0) ss->dead = 1;
            else g_served++;
            return;
        }
        if (errno != EAGAIN) { ss->dead = 1; return; }   /* EINVAL: conn not established */
    }
    if (sq_push(ss, w->buf, w->len) != 0) ss->dead = 1;  /* SQ full → owe the bytes */
}

/* Drop a server conn: its backlog dies with it (the peer is gone). */
static void srv_retire(rstate_t *st, dmesh_qp_t *c) {
    sstate_t *ss = (sstate_t *)c->user_data;
    if (ss) { free(ss->q); free(ss); c->user_data = NULL; }
    dmesh_destroy_qp(c);
    for (int i = 0; i < st->nserv; i++)
        if (st->servers[i] == c) { st->servers[i] = st->servers[--st->nserv]; break; }
}

/* ---- client side: land the reply ---- */

static void cli_recv(rstate_t *st, const dmesh_wc_t *w) {
    if (w->len > st->size - st->got) { st->bad = 1; return; }   /* more bytes than we sent */
    memcpy(st->rb + st->got, w->buf, w->len);
    st->got += w->len;
}

/* A message wider than one slot is carved into several descriptors, but the conn is
 * STICKY to its first backend for life, so they all land on it in send order and
 * reassemble. Nothing to configure per size. */
static void cli_reconnect(rstate_t *st) {
    dmesh_destroy_qp(st->cl);                            /* safe on NULL (first open) */
    st->cl = dmesh_create_qp(g_cq, g_service);
}

/* ---- the one CQ pump: dispatch by conn role, then retry stalled echoes ---- */
static int pump(rstate_t *st) {
    dmesh_wc_t wc[CQ_BATCH];
    int work = dmesh_poll_cq(g_cq, wc, CQ_BATCH);
    if (work < 0) work = 0;
    for (int i = 0; i < work; i++) {
        dmesh_qp_t *c = wc[i].qp;
        switch (wc[i].opcode) {

        /* NB: nothing here may close a conn — dmesh_destroy_qp frees it, and poll_cq
         * emits CONN_REQ together with that conn's already-landed messages, so a
         * later entry in THIS batch can still name it. Deaths are flagged and
         * collected by the sweep below. */
        case DMESH_WC_CONN_REQ:                     /* the DPU looped a request back */
            c->user_data = st->nserv < MAX_SERVERS ? calloc(1, sizeof(sstate_t)) : NULL;
            if (c->user_data) st->servers[st->nserv++] = c;
            break;                                  /* untracked → never echoes → client times out */

        case DMESH_WC_RECV:
            if (c->role == DMESH_ROLE_SERVER) srv_recv(c, &wc[i]);
            else                              cli_recv(st, &wc[i]);
            dmesh_wc_release(g_s, &wc[i]);
            break;

        case DMESH_WC_RECV_FIN:                     /* our client's FIN → drop the echo conn */
            if (c->role != DMESH_ROLE_SERVER) st->eof = 1;
            else if (c->user_data) ((sstate_t *)c->user_data)->dead = 1;
            break;
        }
    }
    for (int i = 0; i < st->nserv; ) {              /* ship echoes the SQ refused, retire the dead */
        dmesh_qp_t *c = st->servers[i];
        work += sq_drain(c);
        if (((sstate_t *)c->user_data)->dead) { srv_retire(st, c); continue; }
        i++;
    }
    return work;
}

/* Post one request. dmesh_alloc NEVER blocks: on EAGAIN the conn's SQ is at its
 * in-flight ceiling, so keep the loop alive (the echo side shares this thread and
 * its TX_ACKs are what free the ring) and retry. Returns 0, -1 on a fault.
 * zc != 0 → fill transport DMA memory in place; else copy the staged body in. */
static int send_req(rstate_t *st, const uint8_t *body, uint8_t p, uint32_t size, int zc) {
    double tw = now_sec();
    for (;;) {
        uint8_t *b = (uint8_t *)dmesh_alloc(st->cl, size);
        if (b) {
            if (zc) { b[0] = p; b[size / 2] = p; b[size - 1] = p; }
            else    memcpy(b, body, size);
            if (dmesh_post_send(st->cl, b, size, 0, 0) != 0) return -1;
            return dmesh_flush(st->cl);
        }
        if (errno != EAGAIN) return -1;             /* EINVAL is permanent */
        if (!pump(st)) idle_wait();
        if (now_sec() - tw > 5.0) return -1;
    }
}

/* ---- client side: N round-trips to our OWN service (reusable conn) ---- */
static void run_loopback(int conn_fd, long N, int size, int zc) {
    char reply[160];
    int msgmax = dmesh_msg_max(g_s), postmax = dmesh_post_max(g_s);
    /* zero-copy may span slots (one alloc, delivered as several RECVs); the plain
     * path stays single-slot (<= msgmax). Either way one alloc caps at postmax. */
    if (N < 1 || size < 1 || size > postmax || (!zc && size > msgmax)) {
        int n = snprintf(reply, sizeof reply,
                         "ERR bad args (size<=%d for non-zc, <=%d for zc)\n", msgmax, postmax);
        ctl_reply(conn_fd, reply, (size_t)n); return;
    }
    uint8_t *body = malloc((size_t)size), *rb = malloc((size_t)size);
    double  *lat  = malloc((size_t)N * sizeof(double));
    if (!body || !rb || !lat) { free(body); free(rb); free(lat);
        ctl_reply(conn_fd, "ERR oom\n", 8); return; }

    static rstate_t st;
    memset(&st, 0, sizeof st);
    st.rb = rb; st.size = (uint32_t)size;
    cli_reconnect(&st);

    long ok = 0, fail = 0; size_t ns = 0;
    for (long i = 0; i < N && st.cl; i++) {
        uint8_t p = (uint8_t)('A' + (i & 0xf));
        double t0 = now_sec();
        st.got = 0; st.bad = 0; st.eof = 0;
        if (!zc) { body[0] = p; body[size / 2] = p; body[size - 1] = p; }
        if (send_req(&st, body, p, (uint32_t)size, zc) != 0) {
            fail++; cli_reconnect(&st); continue;
        }
        /* await the reply — reassembled up to `size` bytes (large = several RECVs) */
        double tw = now_sec(); int timedout = 0;
        while (st.got < (uint32_t)size && !st.bad && !st.eof) {
            if (!pump(&st)) idle_wait();
            if (now_sec() - tw > 5.0) { timedout = 1; break; }
        }
        int bad = timedout || st.bad || st.got != (uint32_t)size ||
                  rb[0] != p || rb[size / 2] != p || rb[size - 1] != p;
        if (bad) { fail++; cli_reconnect(&st); }
        else     { ok++; lat[ns++] = (now_sec() - t0) * 1e6; }
    }
    if (st.cl) { dmesh_destroy_qp(st.cl); st.cl = NULL; }
    /* let the echo side observe our FIN and retire its conns; drop the stragglers */
    double tw = now_sec();
    while (st.nserv > 0 && now_sec() - tw < 1.0)
        if (!pump(&st)) idle_wait();
    while (st.nserv > 0) srv_retire(&st, st.servers[0]);

    qsort(lat, ns, sizeof(double), cmp_d);
    double p50 = ns ? lat[ns / 2] : 0.0;
    int rn = snprintf(reply, sizeof reply, "OK %ld %ld %ld %.1f\n", ok, fail, g_served, p50);
    ctl_reply(conn_fd, reply, (size_t)rn);
    fprintf(stderr, "[loopback] DONE N=%ld ok=%ld fail=%ld served=%ld p50=%.1fus\n",
            N, ok, fail, g_served, p50);
    free(body); free(rb); free(lat);
}

static void handle_ctrl(int fd) {
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
    char cmd[16] = {0}; long N = 0; int size = 0, zc = 0;
    if (sscanf(buf, "%15s %ld %d %d", cmd, &N, &size, &zc) >= 1 && strcmp(cmd, "RUN") == 0)
        run_loopback(fd, N, size, zc);   /* 4th arg (optional) = zero-copy flag */
    else
        ctl_reply(fd, "ERR use: RUN <N> <SIZE> [ZC]\n", 29);
    close(fd);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* This node advertises AND connects to its OWN service: identity from
     * $DPUMESH_SERVICE, and we address that same name so the DPU routes back to us. */
    g_service = getenv("DPUMESH_SERVICE");
    if (!g_service) g_service = "loopback-dpumesh";

    g_s = dmesh_create_channel();
    if (!g_s) { fprintf(stderr, "[loopback] create_channel failed\n"); return 1; }
    g_msgmax  = dmesh_msg_max(g_s);
    g_cq = dmesh_create_cq(g_s);
    if (!g_cq) { fprintf(stderr, "[loopback] create_cq failed\n"); return 1; }
    fprintf(stderr, "[loopback] ready: pod_id=%d own_service=%s\n", dmesh_pod_id(g_s), g_service);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(CTRL_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 4) < 0) {
        perror("listen"); return 1;
    }
    fprintf(stderr, "[loopback] control LISTEN on :%d\n", CTRL_PORT);
    for (;;) {
        int conn = accept(srv, NULL, NULL);
        if (conn < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        handle_ctrl(conn);
    }
}
