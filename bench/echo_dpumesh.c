/*
 * echo_dpumesh.c — Greeter SERVER for the DPUmesh RPC benchmark (echo-dpumesh pod).
 *
 * A Greeter that answers SayHello(request) with a fixed-size reply. It is NOT a
 * symmetric echo — the reply size is chosen by the client (default 8 B),
 * independent of the request size. This asymmetry (big request, tiny reply) is
 * what makes the bandwidth benchmark measure request-side goodput.
 *
 * Over the DPUmesh native API (dmesh.h) each request is a framed message
 * (bench.h): [u32 magic|u32 seq|u32 req_len|u32 reply_size] + req_len bytes.
 * A per-connection reframer reassembles a whole request — even a >8 KB request
 * the transport delivered as several <=8 KB RECV completions — and only THEN
 * emits the reply frame [u32 magic|u32 seq|u32 reply_size|u32 0] + reply_size
 * bytes, with the request's seq echoed so the client can correlate it.
 *
 * Event model: a one-fd epoll server over ONE CQ (dmesh_cq_fd), so the server runs
 * ONE event loop; the transport itself is offloaded to the DPU. Several CQs would
 * spread the accept queue across server threads, but that is not what this measures —
 * client-thread scaling (bench_dpumesh.c) is the axis under test.
 *
 * Backpressure: dmesh_alloc never blocks, so a reply the conn's SQ cannot take
 * stays on that conn's reply FIFO and is re-posted from the loop, instead of
 * stalling every other conn on the thread the way a blocking write did.
 *
 * env: BENCH_WORKER_ID (advertised service id, default 11).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>

#include <dpumesh/dmesh.h>
#include "bench.h"

#define MAX_EVENTS   8
#define CQ_BATCH     64
#define MAX_CONNS    256
#define REPLY_Q      1024        /* deferred reply frames per conn (>= the client window) */
#define REPLY_FILL   43

static dmesh_channel_t *g_s        = NULL;
static dmesh_cq_t      *g_cq       = NULL;   /* the one event loop's CQ */
static uint32_t         g_post_max = 0;   /* max bytes one dmesh_alloc/post can carry */
static long             g_served   = 0;   /* requests answered (all conns; single-loop) */
static long             g_reported = 0;   /* g_served at the last progress print */

typedef struct { uint32_t seq, size; } reply_t;

/* Per-connection server state: the request reframer plus the replies the conn's SQ
 * has not taken yet. Attached to the conn via dmesh_qp_t::user_data (app-owned),
 * allocated at CONN_REQ, freed at FIN. */
typedef struct {
    bench_reframer_t rf;
    reply_t  q[REPLY_Q];
    int      qh, qn;         /* FIFO head, depth */
    uint32_t done;           /* bytes of q[qh]'s frame already posted */
    int      idx;            /* this conn's slot in g_live */
    int      dead;           /* give up: stop replying, and get destroyed by the sweep in
                              * the loop below (dmesh.h forbids destroying mid-batch). */
} greeter_conn_t;

static dmesh_qp_t *g_live[MAX_CONNS];
static int           g_nlive = 0;

/* Post as much of c's reply FIFO as its SQ accepts. A frame is carved into
 * <= post_max chunks (dmesh_alloc reserves one contiguous block), all but the last
 * deferring the doorbell, so a reply still ships in ONE doorbell. On EAGAIN it
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
                bench_put_hdr(b, BENCH_REP_MAGIC, seq, size, 0);
                off = BENCH_HDR_LEN;
            }
            memset(b + off, REPLY_FILL, want - off);
            unsigned fl = (gc->done + want < total) ? DMESH_SEND_MORE : 0;
            if (dmesh_post_send(c, b, want, 0, fl) != 0) { gc->dead = 1; return; }
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

    g_s = dmesh_create_channel();                     /* advertises $DPUMESH_SERVICE (DPU assigns pod_id) */
    if (!g_s) { fprintf(stderr, "[greeter] dmesh_create_channel failed\n"); return 1; }
    g_post_max = (uint32_t)dmesh_post_max(g_s);

    g_cq = dmesh_create_cq(g_s);
    if (!g_cq) { fprintf(stderr, "[greeter] dmesh_create_cq failed\n"); return 1; }
    int dfd = dmesh_cq_fd(g_cq);
    if (dfd < 0) { fprintf(stderr, "[greeter] cq_fd unavailable\n"); return 1; }
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = dfd } };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, dfd, &ev) < 0) { perror("epoll_ctl"); return 1; }

    fprintf(stderr, "[greeter] ready (SayHello, single-loop): pod_id=%d service=%s channel_fd=%d\n",
            dmesh_pod_id(g_s), service, dfd);

    struct epoll_event events[MAX_EVENTS];
    dmesh_wc_t wc[CQ_BATCH];
    for (;;) {
        /* A reply is parked on SQ space -> poll instead of sleeping: nothing wakes this fd
         * when the SQ drains (dmesh_alloc's EAGAIN carries no completion), so sleeping
         * here would strand the reply.
         * Only LIVE conns reach here: the sweep below destroys the ones we gave up on, so
         * a conn that will never drain its FIFO can no longer pin this timeout at 0 —
         * the 100%-core bug this greeter shipped. A greeter's replies are TINY (repsz
         * default 8 B) so its ring rarely fills; a server with large replies would poll
         * here for real. */
        int pend = 0;
        for (int i = 0; i < g_nlive; i++)
            if (((greeter_conn_t *)g_live[i]->user_data)->qn > 0) { pend = 1; break; }

        int nfds = epoll_wait(epfd, events, MAX_EVENTS, pend ? 0 : -1);
        if (nfds < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        uint64_t cnt; while (read(dfd, &cnt, sizeof cnt) > 0) { }   /* drain level counter */

        int n;
        while ((n = dmesh_poll_cq(g_cq, wc, CQ_BATCH)) > 0) {       /* drain to 0 before sleeping */
            for (int i = 0; i < n; i++) {
                dmesh_qp_t *c = wc[i].qp;
                greeter_conn_t *gc = (greeter_conn_t *)c->user_data;
                switch (wc[i].opcode) {
                case DMESH_WC_CONN_REQ:
                    if (!attach(c)) fprintf(stderr, "[greeter] attach failed (conn idle)\n");
                    break;
                case DMESH_WC_RECV:
                    if (gc && !gc->dead)
                        bench_reframe_feed(&gc->rf, wc[i].buf, wc[i].len, on_request, c);
                    dmesh_wc_release(g_s, &wc[i]);
                    break;
                case DMESH_WC_RECV_FIN:
                    reclaim(c);                      /* FIN is the conn's last completion */
                    break;
                }
            }
        }
        /* Post-batch: re-post replies the SQ refused, then DESTROY whatever we gave up on.
         * This is the deferred-destroy sweep dmesh.h asks for (never destroy mid-batch —
         * poll_cq can hand the same QP back later in wc[]); past the batch it is safe, and
         * destroy_qp clears the CQ's resume cursor itself. Backwards, so reclaim's
         * swap-with-last compaction cannot skip an entry. The FIN reclaim sends is the
         * point: the peer LEARNS instead of waiting forever for a reply that will never
         * come, and the conn leaves g_live so `pend` above can never see it again. */
        for (int i = g_nlive - 1; i >= 0; i--) {
            reply_pump(g_live[i]);                   /* may itself latch dead */
            if (((greeter_conn_t *)g_live[i]->user_data)->dead) reclaim(g_live[i]);
        }

        /* Report every 200k. A modulo test cannot do this: g_served advances a FEW per
         * loop pass, so `(g_served % 200000) < 64` (the old test, widened to survive a
         * batch stepping over the exact multiple) stays true for ~64 consecutive passes
         * and prints ~64 times per milestone — spam that buried the DEAD lines above.
         * A milestone cursor fires exactly once, whatever the step size. */
        if (g_served - g_reported >= 200000) {
            g_reported = g_served;
            fprintf(stderr, "[greeter] served=%ld\n", g_served);
        }
    }

    dmesh_destroy_cq(g_cq);
    dmesh_destroy_channel(g_s);
    return 0;
}
