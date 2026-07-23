/* Self-routing byte-stream validator for the DPU L7 codec.
 *
 * One thread and EQ drive client and echo roles. Length-prefixed RPC frames may
 * span or share native posts and are verified byte-for-byte after DPU framing.
 * Command: RUN <count> <payload-size> [frames-per-write]. */
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
#include "dmesh_core.h"   /* dmesh_qp_open + dmesh_resolve_name: this validator's numeric
                           * multi-svc knob addresses by int, below the name surface */
#include "../apps/bench.h"

/* Send a best-effort control reply; failures are ignored. */
static void ctl_reply(int fd, const char *s, size_t n) { (void)!write(fd, s, n); }

#define CTRL_PORT   9092
#define FRAME_HDR   BENCH_HDR_LEN
#define FRAME_MAX   (128u * 1024u)
#define MAX_FPW     64                 /* frames packed per burst cap */
#define EVENT_BATCH    16
#define MAX_SERVERS 4096

static dmesh_channel_t *g_s      = NULL;
static dmesh_eq_t      *g_eq     = NULL;   /* the one EQ both roles are polled from */
static int              g_service = 16;    /* own service id (self-routing default) */
static int              g_msgmax  = 0;     /* max bytes per RECV == max bytes per echo post */
static int              g_postmax = 0;     /* max bytes per dmesh_alloc (one block) */
static long             g_served  = 0;     /* BYTES the echo side sent back */

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
/* 2us idle backoff, taken only when a pump found NO work: the EQ is the one wake
 * source, and SQ space frees silently (there are no send events), so both
 * kinds of wait re-poll rather than sleep on the fd. */
static void idle_wait(void) { struct timespec t = {0, 2000}; nanosleep(&t, NULL); }

/* Deterministic, position-dependent payload byte so a misrouted / truncated /
 * mis-ordered reply is caught byte-exactly. Keyed by (iteration, dest svc). */
static inline uint8_t pat(long iter, uint8_t svc, uint32_t j) {
    return (uint8_t)(iter * 131u + svc * 17u + j);
}

/* Write one request frame into buf. Returns total bytes. */
static uint32_t build_frame(uint8_t *buf, long iter, uint8_t svc, uint32_t size) {
    uint32_t total = FRAME_HDR + size;
    bench_put_hdr(buf, BENCH_REQ_MAGIC, (uint32_t)iter, size, 0);
    for (uint32_t j = 0; j < size; j++)
        buf[FRAME_HDR + j] = pat(iter, svc, j);
    return total;
}

/* Per-server-conn echo backlog (dmesh_qp_t::user_data). Empty in steady state —
 * a chunk is echoed straight out of the RX mmap into the TX ring. It only fills
 * if dmesh_alloc reports EAGAIN (the conn's SQ is at its in-flight ceiling): the
 * RX buffer is transport memory that must be released promptly, so the bytes are
 * parked here in arrival order and a later pump ships them. Echoing is verbatim
 * bytes; the DPU codec owns framing, so re-chunking is legal here. */
typedef struct { uint8_t *q; size_t cap, len; int dead; } sstate_t;

/* Everything pump() touches: the echo side's conns plus the client's reassembly
 * of the one burst in flight (`cur` = the conn it was sent on). */
typedef struct {
    dmesh_qp_t *cur;                   /* client conn of the iteration in flight */
    uint8_t      *rb;                    /* reply reassembly buffer (`size` bytes) */
    size_t        size, got;             /* expected / landed reply bytes */
    int           bad, eof;              /* stray or over-long reply / peer FIN */
    dmesh_qp_t *servers[MAX_SERVERS];
    int           nserv;
} rstate_t;

/* ---- server side: echo every inbound byte ---- */

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
        if (dmesh_post_send(c, b, n) != 0) { ss->dead = 1; break; }
        ss->len -= n; memmove(ss->q, ss->q + n, ss->len);
        g_served += n; posted++;
    }
    if (posted && dmesh_flush(c) != 0) ss->dead = 1;
    return posted;
}

/* Echo one inbound chunk. A fault only FLAGS the conn: it must stay alive until
 * the whole events[] batch is dispatched, since later entries may still name it. */
static void srv_recv(dmesh_qp_t *c, const dmesh_event_t *w) {
    sstate_t *ss = (sstate_t *)c->user_data;
    if (!ss || ss->dead) return;                    /* untracked / already faulted */
    if (ss->len == 0) {                             /* fast path: RX mmap -> TX ring */
        uint8_t *b = (uint8_t *)dmesh_alloc(c, w->len);
        if (b) {
            memcpy(b, w->buf, w->len);
            if (dmesh_post_send(c, b, w->len) != 0 ||
                dmesh_flush(c) != 0) ss->dead = 1;
            else g_served += w->len;
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

/* ---- client side: land the echoed burst ---- */

static void cli_recv(rstate_t *st, dmesh_qp_t *c, const dmesh_event_t *w) {
    /* bytes on an idle conn, or more than we sent → a reply we cannot account for */
    if (c != st->cur || w->len > st->size - st->got) { st->bad = 1; return; }
    memcpy(st->rb + st->got, w->buf, w->len);
    st->got += w->len;
}

static void cli_reconnect(rstate_t *st, dmesh_qp_t **slot, int svc) {
    dmesh_destroy_qp(*slot);                             /* safe on NULL (first open) */
    if (st->cur == *slot) st->cur = NULL;
    *slot = dmesh_qp_open(g_eq, svc);   /* numeric svc → integer entry point */
}

/* ---- the one EQ pump: dispatch by conn role, then retry stalled echoes ---- */
static int pump(rstate_t *st) {
    dmesh_event_t events[EVENT_BATCH];
    int work = dmesh_poll_eq(g_eq, events, EVENT_BATCH);
    if (work < 0) work = 0;
    for (int i = 0; i < work; i++) {
        dmesh_qp_t *c = events[i].qp;
        switch (events[i].type) {

        /* NB: nothing here may close a conn — dmesh_destroy_qp frees it, and poll_eq
         * emits CONN_REQ together with that conn's already-landed messages, so a
         * later entry in THIS batch can still name it. Deaths are flagged and
         * collected by the sweep below. */
        case DMESH_EVENT_CONN_REQ:                     /* the DPU routed a stream to us */
            c->user_data = st->nserv < MAX_SERVERS ? calloc(1, sizeof(sstate_t)) : NULL;
            if (c->user_data) st->servers[st->nserv++] = c;
            break;                                  /* untracked → never echoes → client times out */

        case DMESH_EVENT_RECV:
            if (c->role == DMESH_ROLE_SERVER) srv_recv(c, &events[i]);
            else                              cli_recv(st, c, &events[i]);
            dmesh_release_rx_buffer(g_s, &events[i]);
            break;

        case DMESH_EVENT_RECV_FIN:                     /* our client's FIN → drop the echo conn */
            if (c->role != DMESH_ROLE_SERVER) { if (c == st->cur) st->eof = 1; }
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

/* Ship `burst` bytes as one byte stream. One dmesh_alloc reserves at most one
 * block, so a wider burst goes out as several posts — the codec reframes from the
 * length prefix regardless. dmesh_alloc NEVER blocks: on EAGAIN the conn's SQ is
 * at its in-flight ceiling, so keep the loop alive (the echo side shares this
 * thread and its TX_ACKs are what free the ring) and retry the same chunk.
 * Returns 0, -1 on a fault. */
static int send_burst(rstate_t *st, dmesh_qp_t *c, const uint8_t *sbuf, size_t burst) {
    size_t off = 0;
    double tw = now_sec();
    while (off < burst) {
        size_t left = burst - off;
        uint32_t n = left > (size_t)g_postmax ? (uint32_t)g_postmax : (uint32_t)left;
        uint8_t *b = (uint8_t *)dmesh_alloc(c, n);
        if (!b) {
            if (errno != EAGAIN) return -1;         /* EINVAL is permanent */
            if (off > 0 && dmesh_flush(c) != 0) return -1;
            if (!pump(st)) idle_wait();
            if (now_sec() - tw > 5.0) return -1;
            continue;
        }
        memcpy(b, sbuf + off, n);
        if (dmesh_post_send(c, b, n) != 0) return -1;
        off += n;
    }
    return dmesh_flush(c);
}

/* ---- client side: N round-trips of framed byte-stream to our own service ---- */
static void run_stream(int conn_fd, long N, uint32_t size, int fpw) {
    char reply[192];
    if (N < 1 || fpw < 1 || fpw > MAX_FPW ||
        FRAME_HDR + size > FRAME_MAX) {
        int n = snprintf(reply, sizeof reply,
                         "ERR bad args (size<=%u, fpw<=%d)\n", FRAME_MAX - FRAME_HDR, MAX_FPW);
        ctl_reply(conn_fd, reply, (size_t)n); return;
    }

    uint32_t frame_bytes = FRAME_HDR + size;
    size_t   burst = (size_t)frame_bytes * (size_t)fpw;   /* bytes per round-trip */
    uint8_t *sbuf = malloc(burst), *rbuf = malloc(burst);
    double  *lat  = malloc((size_t)N * sizeof(double));
    if (!sbuf || !rbuf || !lat) { free(sbuf); free(rbuf); free(lat);
        ctl_reply(conn_fd, "ERR oom\n", 8); return; }

    static rstate_t st;
    memset(&st, 0, sizeof st);
    st.rb = rbuf; st.size = burst;

    dmesh_qp_t *c = NULL;
    cli_reconnect(&st, &c, g_service);

    /* Early-abort: a run that cannot round-trip burns
     * the 5 s reply timeout every iteration — N×5 s would wedge for hours. Bail after a
     * burst of consecutive failures so the operator gets fast feedback. */
    const long MAX_CONSEC_FAIL = 40;
    long consec_fail = 0;

    long ok = 0, fail = 0; size_t ns = 0;
    for (long i = 0; i < N; i++) {
        if (consec_fail >= MAX_CONSEC_FAIL) {
            fprintf(stderr, "[stream] ABORT after %ld consecutive failures "
                    "(is service %d enabled in DPUMESH_PROXY_L7_SVC?)\n",
                    consec_fail, g_service);
            break;
        }
        uint8_t svc = (uint8_t)g_service;
        if (!c) { fail++; consec_fail++; continue; }

        /* pack `fpw` frames of this iteration into one byte-stream burst */
        for (int f = 0; f < fpw; f++)
            build_frame(sbuf + (size_t)f * frame_bytes, i, svc, size);

        double t0 = now_sec();
        st.cur = c; st.got = 0; st.bad = 0; st.eof = 0;
        if (send_burst(&st, c, sbuf, burst) != 0) {
            fail++; consec_fail++; cli_reconnect(&st, &c, g_service); continue;
        }

        /* reply is the echoed byte-stream — reassemble exactly `burst` bytes */
        double tw = now_sec(); int timedout = 0;
        while (st.got < burst && !st.bad && !st.eof) {
            if (!pump(&st)) idle_wait();
            if (now_sec() - tw > 5.0) { timedout = 1; break; }
        }
        int bad = timedout || st.bad || st.got != burst || memcmp(sbuf, rbuf, burst) != 0;
        st.cur = NULL;
        if (bad) { fail++; consec_fail++; cli_reconnect(&st, &c, g_service); }
        else     { ok++; consec_fail = 0; lat[ns++] = (now_sec() - t0) * 1e6; }
    }
    if (c) { dmesh_destroy_qp(c); c = NULL; }
    /* let the echo side observe our FIN and retire its conns; drop the stragglers */
    double tw = now_sec();
    while (st.nserv > 0 && now_sec() - tw < 1.0)
        if (!pump(&st)) idle_wait();
    while (st.nserv > 0) srv_retire(&st, st.servers[0]);

    qsort(lat, ns, sizeof(double), cmp_d);
    double p50 = ns ? lat[ns / 2] : 0.0;
    int rn = snprintf(reply, sizeof reply, "OK %ld %ld %ld %.1f\n", ok, fail, g_served, p50);
    ctl_reply(conn_fd, reply, (size_t)rn);
    fprintf(stderr, "[stream] DONE N=%ld size=%u fpw=%d ok=%ld fail=%ld served_bytes=%ld p50=%.1fus\n",
            N, size, fpw, ok, fail, g_served, p50);
    free(sbuf); free(rbuf); free(lat);
}

static void handle_ctrl(int fd) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';

    char cmd[16] = {0};
    long N = 0; uint32_t size = 0; int fpw = 1;
    int m = sscanf(buf, "%15s %ld %u %d", cmd, &N, &size, &fpw);
    if (m < 3 || strcmp(cmd, "RUN") != 0) {
        ctl_reply(fd, "ERR use: RUN <N> <SIZE> [<FPW>]\n", 32);
        close(fd); return;
    }
    if (m < 4) fpw = 1;
    run_stream(fd, N, size, fpw);
    close(fd);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    g_s = dmesh_create_channel();   /* identity from $DPUMESH_SERVICE via the registry */
    if (!g_s) { fprintf(stderr, "[stream] create_channel failed\n"); return 1; }
    /* Own service_id, for the self-default svc list (numeric addressing below). */
    const char *self = getenv("DPUMESH_SERVICE");
    if (self) { int id = dmesh_resolve_name(self); if (id >= 0) g_service = id; }
    g_msgmax  = dmesh_msg_max(g_s);
    g_postmax = dmesh_post_max(g_s);
    g_eq = dmesh_create_eq(g_s);
    if (!g_eq) { fprintf(stderr, "[stream] create_eq failed\n"); return 1; }
    fprintf(stderr, "[stream] ready: pod_id=%d own_service=%d msg_max=%d\n",
            dmesh_pod_id(g_s), g_service, g_msgmax);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(CTRL_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 4) < 0) {
        perror("listen"); return 1;
    }
    fprintf(stderr, "[stream] control LISTEN on :%d\n", CTRL_PORT);
    for (;;) {
        int conn = accept(srv, NULL, NULL);
        if (conn < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        handle_ctrl(conn);
    }
}
