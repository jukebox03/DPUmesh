/*
 * echo_dpumesh.c — Greeter SERVER for the DPUmesh RPC benchmark (echo-dpumesh pod).
 *
 * A Greeter that answers SayHello(request) with a fixed-size reply. It is NOT a
 * symmetric echo — the reply size is chosen by the client (default 8 B),
 * independent of the request size. This asymmetry (big request, tiny reply) is
 * what makes the bandwidth benchmark measure request-side goodput.
 *
 * Over the DPUmesh byte stream (dmesh.h) each request is a framed message
 * (bench.h): [u32 magic|u32 seq|u32 req_len|u32 reply_size] + req_len bytes.
 * A per-connection reframer reassembles a whole request — even a >8 KB request
 * the transport delivered as several <=8 KB chunks — and only THEN emits the
 * reply frame [u32 magic|u32 seq|u32 reply_size|u32 0] + reply_size bytes, with
 * the request's seq echoed so the client can correlate it.
 *
 * Event model: a one-fd epoll server (dmesh_event_fd). The connection ready-list
 * (dmesh_next_ready) is SINGLE-CONSUMER by contract, so the server runs ONE event
 * loop; the transport itself is offloaded to the DPU. Server-thread scaling has no
 * faithful analogue here (the host-side delivery funnels through one PE-named
 * ready-list) — client-thread scaling (bench_dpumesh.c) is the scalable axis.
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
#include <stdatomic.h>
#include <sys/epoll.h>

#include <dpumesh/dmesh.h>
#include "bench.h"

#define MAX_EVENTS   8
#define DRAIN_BUF    65536

/* Per-connection server state: the request reframer. Attached to the conn via
 * dmesh_conn_t::user_data (app-owned), allocated at accept, freed at close. */
typedef struct { bench_reframer_t rf; } greeter_conn_t;

static uint8_t     g_fill[8192];        /* reply payload filler */
static atomic_long g_served = 0;        /* requests answered (all conns) */

/* Emit one reply frame for a completed request. */
static void send_reply(dmesh_conn_t *c, uint32_t seq, uint32_t reply_size) {
    uint8_t hdr[BENCH_HDR_LEN];
    bench_put_hdr(hdr, BENCH_REP_MAGIC, seq, reply_size, 0);
    dmesh_write(c, hdr, BENCH_HDR_LEN);
    uint32_t left = reply_size;
    while (left > 0) {                               /* reply_size may exceed one slot */
        uint32_t chunk = left < sizeof g_fill ? left : (uint32_t)sizeof g_fill;
        dmesh_write(c, g_fill, chunk);
        left -= chunk;
    }
    dmesh_flush(c);                                  /* ship the reply */
}

/* Reframer callback: a whole request arrived -> answer it (SayHello). aux is the
 * client-requested reply_size. */
static void on_request(uint32_t seq, uint32_t req_len, uint32_t reply_size, void *user) {
    (void)req_len;
    send_reply((dmesh_conn_t *)user, seq, reply_size);
    atomic_fetch_add(&g_served, 1);
}

/* Drain a ready conn to EAGAIN, reframing requests + replying. Returns 1 on
 * peer FIN (caller closes + frees the conn state). */
static int greeter_drain(dmesh_conn_t *c) {
    greeter_conn_t *gc = (greeter_conn_t *)c->user_data;
    static __thread uint8_t buf[DRAIN_BUF];
    ssize_t n;
    while ((n = dmesh_read(c, buf, sizeof buf)) > 0)
        bench_reframe_feed(&gc->rf, buf, (size_t)n, on_request, c);
    return n == 0;   /* EOF (FIN) */
}

static greeter_conn_t *attach(dmesh_conn_t *c) {
    greeter_conn_t *gc = (greeter_conn_t *)malloc(sizeof *gc);
    if (!gc) return NULL;
    bench_reframe_reset(&gc->rf);
    c->user_data = gc;
    return gc;
}
static void reclaim(dmesh_conn_t *c) {
    free(c->user_data); c->user_data = NULL;
    dmesh_close(c);
}

int main(void) {
    int service_id = 11;                             /* advertised service (DPU assigns pod_id) */
    if (getenv("BENCH_WORKER_ID")) service_id = atoi(getenv("BENCH_WORKER_ID"));
    memset(g_fill, 43, sizeof g_fill);

    dmesh_channel_t *s = dmesh_create_channel(service_id);
    if (!s) { fprintf(stderr, "[greeter] dmesh_create_channel failed\n"); return 1; }

    int dfd = dmesh_event_fd(s);
    if (dfd < 0) { fprintf(stderr, "[greeter] event_fd unavailable\n"); return 1; }
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = dfd } };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, dfd, &ev) < 0) { perror("epoll_ctl"); return 1; }

    fprintf(stderr, "[greeter] ready (SayHello, single-loop): pod_id=%d service=%d channel_fd=%d\n",
            dmesh_pod_id(s), service_id, dfd);

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        uint64_t cnt; while (read(dfd, &cnt, sizeof cnt) > 0) { }   /* drain level counter */

        dmesh_conn_t *c;
        while ((c = dmesh_accept(s)) != NULL) {       /* new conns (first request in hand) */
            if (!attach(c)) { dmesh_close(c); continue; }
            if (greeter_drain(c)) reclaim(c);
        }
        while ((c = dmesh_next_ready(s)) != NULL)      /* conns with more requests */
            if (greeter_drain(c)) reclaim(c);

        long served = atomic_load(&g_served);
        if (served && (served % 200000) < 64)
            fprintf(stderr, "[greeter] served=%ld\n", served);
    }

    dmesh_destroy_channel(s);
    return 0;
}
