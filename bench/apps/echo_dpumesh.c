/* Event-driven native DPUmesh Greeter server.
 *
 * One EQ and epoll loop reframe bench.h requests and return the requested reply
 * size with the same sequence id. EAGAIN parks the reply until TX_READY names the
 * connection. BENCH_WORKER_ID selects the advertised service. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/epoll.h>

#include <dpumesh/dmesh.h>
#include "bench.h"

#define MAX_EVENTS   8
#define EVENT_BATCH     64
#define MAX_CONNS    256
#define REPLY_Q      1024        /* deferred reply frames per conn (>= the client window) */
#define REPLY_FILL   43

static dmesh_channel_t *g_s        = NULL;
static dmesh_eq_t      *g_eq       = NULL;   /* the one event loop's EQ */
static uint32_t         g_post_max = 0;   /* max bytes one dmesh_alloc/post can carry */
static uint32_t         g_pod_id   = 0;   /* our DPU-assigned pod_id, stamped into every
                                           * reply's aux so the client can see WHICH backend
                                           * served it — the only way to observe the LB. */
static long             g_served   = 0;   /* requests answered (all conns; single-loop) */
static long             g_reported = 0;   /* g_served at the last progress print */
static long             g_app_work_us = 0;/* per-request busy-spin (µs) — models the greeter
                                           * doing real work on ITS host core (which the DPU
                                           * offload leaves it). Set at start from APP_WORK_US,
                                           * then live-tunable: write /tmp/app_work_us + SIGHUP,
                                           * so a busy-app sweep needs no pod restart. */
static volatile sig_atomic_t g_reload = 0;
static void on_hup(int s) { (void)s; g_reload = 1; }
static void load_work(void) {
    FILE *f = fopen("/tmp/app_work_us", "r");
    if (f) { long v; if (fscanf(f, "%ld", &v) == 1) g_app_work_us = v; fclose(f); }
    else { const char *e = getenv("APP_WORK_US"); if (e) g_app_work_us = atol(e); }
}

typedef struct { uint32_t seq, size; } reply_t;

/* Per-connection server state: the request reframer plus the replies the conn's SQ
 * has not taken yet. Attached to the conn via dmesh_qp_t::user_data (app-owned),
 * allocated at CONN_REQ, freed at FIN. */
typedef struct {
    bench_reframer_t rf;
    reply_t  q[REPLY_Q];
    int      qh, qn;         /* FIFO head, depth */
    uint32_t done;           /* bytes of q[qh]'s frame already posted */
    uint32_t last_seq;       /* request sequence most recently decoded */
    int      have_seq;
    int      idx;            /* this conn's slot in g_live */
    int      dead;           /* give up: stop replying, and get destroyed by the sweep in
                              * the loop below (dmesh.h forbids destroying mid-batch). */
} greeter_conn_t;

static dmesh_qp_t *g_live[MAX_CONNS];
static int           g_nlive = 0;

/* Post as much of c's reply FIFO as its SQ accepts. A frame is carved into
 * <= post_max chunks (dmesh_alloc reserves one contiguous block); complete transport
 * units publish immediately and the post-batch flush submits the tail. On EAGAIN it
 * returns keeping its exact place — the frame resumes from `done`. */
static void reply_pump(dmesh_qp_t *c) {
    greeter_conn_t *gc = (greeter_conn_t *)c->user_data;
    while (gc->qn > 0 && !gc->dead) {
        uint32_t seq = gc->q[gc->qh].seq, size = gc->q[gc->qh].size;
        uint32_t total = BENCH_HDR_LEN + size;
        while (gc->done < total) {
            uint32_t want = total - gc->done;
            if (want > g_post_max) want = g_post_max;
            uint8_t *b = (uint8_t *)dmesh_alloc(c, want);
            if (!b) { if (errno != EAGAIN) gc->dead = 1; return; }    /* EAGAIN: keep our place */
            uint32_t off = 0;
            if (gc->done == 0) {
                bench_put_hdr(b, BENCH_REP_MAGIC, seq, size, g_pod_id);
                off = BENCH_HDR_LEN;
            }
            memset(b + off, REPLY_FILL, want - off);
            if (dmesh_post_send(c, b, want) != 0) { gc->dead = 1; return; }
            gc->done += want;
        }
        gc->done = 0;
        gc->qh = (gc->qh + 1) % REPLY_Q;
        gc->qn--;
        g_served++;
    }
}

/* Reframer callback: a whole request arrived -> queue its reply (SayHello) and try
 * to ship it. aux is the client-requested reply_size. */
static void on_request(uint32_t seq, uint32_t req_len, uint32_t reply_size, void *user) {
    (void)req_len;
    dmesh_qp_t *c = (dmesh_qp_t *)user;
    greeter_conn_t *gc = (greeter_conn_t *)c->user_data;
    if (gc->dead) return;                              /* reframer emits several per feed */
    if (gc->have_seq && (int32_t)(seq - gc->last_seq) <= 0)
        fprintf(stderr, "[greeter] non-increasing request seq=%u after=%u\n",
                seq, gc->last_seq);
    gc->last_seq = seq;
    gc->have_seq = 1;
    if (g_app_work_us > 0) {                           /* simulate app-work on the host core */
        double t0 = bench_now_sec();
        while ((bench_now_sec() - t0) * 1e6 < (double)g_app_work_us) { }
    }
    if (gc->qn >= REPLY_Q) { gc->dead = 1; return; }   /* client outran its own window */
    gc->q[(gc->qh + gc->qn) % REPLY_Q] = (reply_t){ .seq = seq, .size = reply_size };
    gc->qn++;
    reply_pump(c);
}

static greeter_conn_t *attach(dmesh_qp_t *c) {
    if (g_nlive >= MAX_CONNS) return NULL;
    greeter_conn_t *gc = (greeter_conn_t *)calloc(1, sizeof *gc);   /* zeroed == reframe_reset */
    if (!gc) return NULL;
    gc->idx = g_nlive;
    g_live[g_nlive++] = c;
    c->user_data = gc;
    return gc;
}

static void reclaim(dmesh_qp_t *c) {
    greeter_conn_t *gc = (greeter_conn_t *)c->user_data;
    if (gc) {
        int last = --g_nlive;
        if (gc->idx != last) {                     /* compact; re-index the moved conn */
            g_live[gc->idx] = g_live[last];
            ((greeter_conn_t *)g_live[gc->idx]->user_data)->idx = gc->idx;
        }
        free(gc);
        c->user_data = NULL;
    }
    dmesh_destroy_qp(c);
}

int main(void) {
    const char *service = getenv("DPUMESH_SERVICE");  /* identity injected via env → registry */
    if (!service) service = "(none)";
    signal(SIGHUP, on_hup); load_work();              /* app-work: env default, live via SIGHUP */

    g_s = dmesh_create_channel();                     /* advertises $DPUMESH_SERVICE (DPU assigns pod_id) */
    if (!g_s) { fprintf(stderr, "[greeter] dmesh_create_channel failed\n"); return 1; }
    g_post_max = (uint32_t)dmesh_post_max(g_s);

    g_eq = dmesh_create_eq(g_s);
    if (!g_eq) { fprintf(stderr, "[greeter] dmesh_create_eq failed\n"); return 1; }
    int dfd = dmesh_eq_fd(g_eq);
    if (dfd < 0) { fprintf(stderr, "[greeter] eq_fd unavailable\n"); return 1; }
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = dfd } };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, dfd, &ev) < 0) { perror("epoll_ctl"); return 1; }

    g_pod_id = (uint32_t)dmesh_pod_id(g_s);
    fprintf(stderr, "[greeter] ready (SayHello, single-loop): pod_id=%d service=%s channel_fd=%d\n",
            dmesh_pod_id(g_s), service, dfd);

    struct epoll_event epoll_events[MAX_EVENTS];
    dmesh_event_t events[EVENT_BATCH];
    for (;;) {
        { static double lr = 0; double now = bench_now_sec();   /* live app-work: poll the file
             * every 0.5s (robust vs SIGHUP, which a libdpumesh thread can absorb). */
          if (now - lr > 0.5) { lr = now; load_work(); } }
        /* EQ fd covers RX, accepts, and armed TX-ready transitions, so parked replies
         * need neither a timer nor a zero-timeout sweep. */
        int nfds = epoll_wait(epfd, epoll_events, MAX_EVENTS, -1);
        if (nfds < 0) { if (errno == EINTR) { if (g_reload) { g_reload = 0; load_work(); } continue; } perror("epoll_wait"); break; }
        if (g_reload) { g_reload = 0; load_work(); }

        uint64_t cnt; while (read(dfd, &cnt, sizeof cnt) > 0) { }   /* drain level counter */

        int n;
        while ((n = dmesh_poll_eq(g_eq, events, EVENT_BATCH)) > 0) {       /* drain to 0 before sleeping */
            for (int i = 0; i < n; i++) {
                dmesh_qp_t *c = events[i].qp;
                greeter_conn_t *gc = (greeter_conn_t *)c->user_data;
                switch (events[i].type) {
                case DMESH_EVENT_CONN_REQ:
                    if (!attach(c)) fprintf(stderr, "[greeter] attach failed (conn idle)\n");
                    break;
                case DMESH_EVENT_RECV:
                    if (gc && !gc->dead &&
                        bench_reframe_feed(&gc->rf, events[i].buf, events[i].len,
                                           BENCH_REQ_MAGIC, on_request, c) < 0) {
                        fprintf(stderr, "[greeter] request stream desync — dropping conn\n");
                        gc->dead = 1;
                    }
                    dmesh_release_rx_buffer(g_s, &events[i]);
                    break;
                case DMESH_EVENT_RECV_FIN:
                    if (gc) gc->dead = 1;
                    break;
                case DMESH_EVENT_TX_READY:
                    if (gc && !gc->dead) reply_pump(c); /* targeted one-shot retry */
                    break;
                }
            }
        }
        /* Post-batch: destroy whatever we gave up on. Backwards, so reclaim's
         * swap-with-last compaction cannot skip an entry. */
        for (int i = g_nlive - 1; i >= 0; i--) {
            if (((greeter_conn_t *)g_live[i]->user_data)->dead) reclaim(g_live[i]);
        }
        /* post_send commits and submits every newly complete transport unit. One
         * explicit flush per event-loop pass forces each connection's trailing tail. */
        for (int i = 0; i < g_nlive; i++)
            if (dmesh_flush(g_live[i]) != 0)
                ((greeter_conn_t *)g_live[i]->user_data)->dead = 1;

        /* Report each 200k milestone once. */
        if (g_served - g_reported >= 200000) {
            g_reported = g_served;
            fprintf(stderr, "[greeter] served=%ld\n", g_served);
        }
    }

    dmesh_destroy_eq(g_eq);
    dmesh_destroy_channel(g_s);
    return 0;
}
