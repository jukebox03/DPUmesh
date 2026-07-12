/*
 * bench_dpumesh.c — RPC benchmark CLIENT for DPUmesh (bench-dpumesh pod).
 *
 * A closed-loop request/response load generator over the DPUmesh byte-stream
 * façade (dmesh.h). It measures the standard RPC microbenchmark quantities:
 *
 *   - latency  : p50 / p95 / p99 / avg / min / max  (concurrency = 1 ping-pong)
 *   - bandwidth: goodput in Gb/s over the request bytes (large messages)
 *   - rate     : small-RPC rate in Mrps and its scaling with client threads
 *
 * Methodology:
 *   * CLOSED-LOOP, fixed concurrency window: each client thread keeps
 *     `concurrency` requests outstanding on ONE connection. There is NO target
 *     rate and NO pacing — offered load is whatever the closed loop sustains.
 *   * Greeter SayHello semantics: a `req_size`-byte request, a fixed small
 *     `reply_size`-byte reply (default 8). Bandwidth counts REQUEST bytes.
 *   * Warmup: the first `warmup` completions (default 1000) are excluded; the
 *     measurement window starts at the warmup boundary (`warmup_end`).
 *   * Multi-thread: `threads` client threads, each its own connection; the
 *     aggregate rate is the sum of per-thread rates, latency percentiles come
 *     from the merged histogram.
 *
 * DPUmesh mapping notes:
 *   * One dmesh connection per client thread, dmesh_pin_route()'d so the whole
 *     conn stays on the ONE backend => replies arrive in send order (positional
 *     FIFO correlation, cross-checked against the echoed seq).
 *   * Requests/replies are framed (bench.h) so a >8 KB request that the transport
 *     auto-chunks across slots is reassembled by the server before it replies —
 *     so the measured latency includes the full request transfer.
 *
 * Control protocol (TCP :9092, one line):
 *   RUN <req_size> <reply_size> <concurrency> <duration_s> <warmup> <threads>
 *        -> OK mrps=.. gbps=.. p50=.. p95=.. p99=.. avg=.. min=.. max=..
 *              rcnt=.. fail=.. conc=.. threads=.. reqsz=.. repsz=.. durs=..
 *      (all latencies in microseconds; key=value fields)
 *   PING -> PONG
 *
 * env: BENCH_DST_POD_ID (dst service id, default 11);
 *      BENCH_RX_SCRATCH (RX drain buffer bytes).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <dpumesh/dmesh.h>
#include "bench.h"

#define CTRL_PORT          9092
#define MAX_THREADS        64
#define WORKER_STACK_BYTES (256 * 1024)
#define DEFAULT_RX_SCRATCH (64 * 1024)
#define STOP_GRACE_SEC     15         /* watchdog kill margin past the run duration */

static dmesh_channel_t *g_s          = NULL;   /* shared, thread-safe façade endpoint */
static int              g_dst_service = 11;    /* backend service to address (Model B) */
static size_t           g_rx_scratch  = DEFAULT_RX_SCRATCH;

/* ------------------------------------------------------------ per-thread run */
typedef struct {
    /* config (shared, read-only during the run) */
    int          req_size;
    int          reply_size;
    int          W;            /* concurrency window (outstanding per thread) */
    long         warmup;       /* completions excluded from measurement */
    double       duration;     /* run length in seconds */
    double       start_at;     /* shared barrier: all threads begin at this time */
    atomic_int  *stop;         /* watchdog / abort flag */

    /* per-thread transport + pipeline state */
    dmesh_conn_t    *c;
    double          *start_ts;  /* ring[W]: issue time of each outstanding request */
    uint32_t         next_seq;  /* next seq to assign on issue */
    uint32_t         exp_seq;   /* next seq expected to complete (in-order FIFO) */
    long             outstanding;
    long             credits;   /* completions observed this drain -> requests to reissue */
    bench_reframer_t reframer;  /* reply-stream reframer */

    /* per-thread results */
    long         rcnt;         /* total completions incl. warmup */
    long         fail;         /* seq-mismatch / corrupt completions */
    double       warmup_end;   /* timestamp of the warmup boundary */
    double       dura;         /* measured window length (s) */
    bench_hist_t hist;         /* post-warmup latencies (us) */
    int          broken;       /* conn/alloc failure -> excluded from aggregate */
} worker_t;

/* Fire once per fully-received reply frame: correlate to the oldest outstanding
 * request (FIFO — single pinned backend => send order), record its latency. */
static void on_reply(uint32_t seq, uint32_t plen, uint32_t aux, void *user) {
    (void)plen; (void)aux;
    worker_t *w = (worker_t *)user;
    double now = bench_now_sec();
    double t0  = w->start_ts[w->exp_seq % (uint32_t)w->W];
    if (seq != w->exp_seq) w->fail++;            /* reorder/corruption guard */
    w->exp_seq++;
    w->outstanding--;
    if (w->rcnt >= w->warmup)
        bench_hist_record(&w->hist, (now - t0) * 1e6);
    w->rcnt++;
    if (w->rcnt == w->warmup) w->warmup_end = now;   /* measurement window opens */
    w->credits++;
}

/* Ship one request frame: [hdr | payload]. dmesh_write busy-spins on byte-ring
 * backpressure (so large requests block until credited — that queueing IS part
 * of the closed-loop latency), then flush ships it. Returns 0 on hard fault. */
static int issue(worker_t *w, const uint8_t *payload) {
    uint8_t hdr[BENCH_HDR_LEN];
    uint32_t seq = w->next_seq;
    bench_put_hdr(hdr, BENCH_REQ_MAGIC, seq, (uint32_t)w->req_size, (uint32_t)w->reply_size);
    w->start_ts[seq % (uint32_t)w->W] = bench_now_sec();
    if (dmesh_write(w->c, hdr, BENCH_HDR_LEN) < 0) return 0;
    if (w->req_size > 0 && dmesh_write(w->c, payload, (size_t)w->req_size) < 0) return 0;
    if (dmesh_flush(w->c) < 0) return 0;
    w->next_seq++;
    w->outstanding++;
    return 1;
}

static void *worker_fn(void *arg) {
    worker_t *w = (worker_t *)arg;
    bench_reframe_reset(&w->reframer);

    if (bench_hist_init(&w->hist) < 0) { w->broken = 1; return NULL; }
    w->start_ts = (double *)calloc((size_t)w->W, sizeof(double));
    uint8_t *payload = (uint8_t *)malloc(w->req_size ? (size_t)w->req_size : 1);
    uint8_t *rxbuf   = (uint8_t *)malloc(g_rx_scratch);
    if (!w->start_ts || !payload || !rxbuf) { w->broken = 1; goto done; }
    memset(payload, 42, w->req_size ? (size_t)w->req_size : 1);

    w->c = dmesh_connect(g_s, g_dst_service);
    if (!w->c) { w->broken = 1; goto done; }
    dmesh_pin_route(w->c);                       /* socket-order on the one backend */

    /* barrier: all threads start together */
    while (bench_now_sec() < w->start_at) {
        if (atomic_load(w->stop)) { w->broken = 1; goto done; }
        struct timespec ts = {0, 50000}; nanosleep(&ts, NULL);
    }
    double start = bench_now_sec();
    w->warmup_end = start;                        /* fallback if warmup never reached */

    for (int i = 0; i < w->W; i++)                /* prime the window */
        if (!issue(w, payload)) { w->broken = 1; break; }

    while (!w->broken && !atomic_load(w->stop)) {
        if (bench_now_sec() - start > w->duration) break;

        int did = 0;
        ssize_t n;
        while ((n = dmesh_read(w->c, rxbuf, g_rx_scratch)) > 0) {
            bench_reframe_feed(&w->reframer, rxbuf, (size_t)n, on_reply, w);
            did = 1;
        }
        if (n == 0) { w->broken = 1; break; }     /* backend FIN — abort this thread */

        while (w->credits > 0) {                  /* keep the window full */
            if (bench_now_sec() - start > w->duration) { w->credits = 0; break; }
            if (!issue(w, payload)) { w->broken = 1; break; }
            w->credits--; did = 1;
        }
        if (!did) { struct timespec ts = {0, 2000}; nanosleep(&ts, NULL); }  /* 2us */
    }

    double end = bench_now_sec();
    w->dura = (w->rcnt > w->warmup) ? (end - w->warmup_end) : 0.0;

done:
    if (w->c) dmesh_close(w->c);
    free(payload); free(rxbuf);
    return NULL;
}

/* ------------------------------------------------------------ watchdog */
typedef struct { atomic_int *stop, *early; double deadline_sec; } wd_t;
static void *watchdog_fn(void *arg) {
    wd_t *a = (wd_t *)arg;
    double t0 = bench_now_sec();
    while (bench_now_sec() - t0 < a->deadline_sec) {
        if (atomic_load(a->early)) return NULL;
        struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL);   /* 100ms */
    }
    atomic_store(a->stop, 1);
    return NULL;
}

/* ------------------------------------------------------------ one benchmark run */
static void run_bench(int conn_fd, int req_size, int reply_size, int concurrency,
                      double duration, long warmup, int threads) {
    char reply[512];
    if (req_size < 0 || reply_size < 1 || concurrency < 1 || duration <= 0 || threads < 1) {
        const char *e = "ERR invalid args\n";
        if (write(conn_fd, e, strlen(e)) < 0) {} return;
    }
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    if (warmup < 0) warmup = 0;

    fprintf(stderr, "[bench] RUN req=%d reply=%d conc=%d dur=%.1fs warmup=%ld threads=%d dst_svc=%d\n",
            req_size, reply_size, concurrency, duration, warmup, threads, g_dst_service);

    worker_t  *w   = (worker_t *)calloc((size_t)threads, sizeof(worker_t));
    pthread_t *tid = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));
    if (!w || !tid) { free(w); free(tid); if (write(conn_fd, "ERR oom\n", 8) < 0) {} return; }

    atomic_int stop = 0;
    double start_at = bench_now_sec() + 0.1;

    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);
    for (int i = 0; i < threads; i++) {
        w[i].req_size   = req_size;
        w[i].reply_size = reply_size;
        w[i].W          = concurrency;
        w[i].warmup     = warmup;
        w[i].duration   = duration;
        w[i].start_at   = start_at;
        w[i].stop       = &stop;
        if (pthread_create(&tid[i], &attr, worker_fn, &w[i]) != 0) { w[i].broken = 1; tid[i] = 0; }
    }
    pthread_attr_destroy(&attr);

    atomic_int early = 0;
    wd_t wa = { .stop = &stop, .early = &early, .deadline_sec = duration + STOP_GRACE_SEC };
    pthread_t wd; pthread_create(&wd, NULL, watchdog_fn, &wa);

    for (int i = 0; i < threads; i++) if (tid[i]) pthread_join(tid[i], NULL);
    atomic_store(&early, 1); pthread_join(wd, NULL);

    /* aggregate: sum per-thread rates, merge latency histograms */
    bench_hist_t agg; bench_hist_init(&agg);
    double mrps = 0.0, gbps = 0.0;
    long total_ok = 0, total_fail = 0;
    for (int i = 0; i < threads; i++) {
        long measured = w[i].rcnt - w[i].warmup;
        if (measured < 0) measured = 0;
        total_fail += w[i].fail;
        if (!w[i].broken && w[i].dura > 1e-9 && measured > 0) {
            mrps += (double)measured / w[i].dura * 1e-6;
            gbps += 8e-9 * (double)measured * (double)req_size / w[i].dura;
            total_ok += measured;
            bench_hist_merge(&agg, &w[i].hist);
        }
        bench_hist_free(&w[i].hist);
    }

    double p50 = bench_hist_pct(&agg, 50.0), p95 = bench_hist_pct(&agg, 95.0);
    double p99 = bench_hist_pct(&agg, 99.0);
    double avg = bench_hist_avg(&agg), mn = bench_hist_min(&agg), mx = bench_hist_max(&agg);
    bench_hist_free(&agg);

    int n = snprintf(reply, sizeof reply,
        "OK mrps=%.6f gbps=%.4f p50=%.2f p95=%.2f p99=%.2f avg=%.2f min=%.2f max=%.2f "
        "rcnt=%ld fail=%ld conc=%d threads=%d reqsz=%d repsz=%d durs=%.3f\n",
        mrps, gbps, p50, p95, p99, avg, mn, mx,
        total_ok, total_fail, concurrency, threads, req_size, reply_size, duration);
    if (write(conn_fd, reply, (size_t)n) < 0) {}
    fprintf(stderr, "[bench] DONE %s", reply);
    free(w); free(tid);
}

/* ------------------------------------------------------------ control TCP */
static void handle_ctrl(int fd) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
    char *cr = strchr(buf, '\r'); if (cr) *cr = '\0';

    if (strncmp(buf, "PING", 4) == 0) { if (write(fd, "PONG\n", 5) < 0) {} close(fd); return; }

    char cmd[16] = {0};
    if (sscanf(buf, "%15s", cmd) == 1 && strcmp(cmd, "RUN") == 0) {
        int req = 32, rep = 8, conc = 1, threads = 1; double dur = 10.0; long warm = 1000;
        /* RUN <req_size> <reply_size> <concurrency> <duration> <warmup> <threads> */
        sscanf(buf, "%*s %d %d %d %lf %ld %d", &req, &rep, &conc, &dur, &warm, &threads);
        run_bench(fd, req, rep, conc, dur, warm, threads);
        close(fd); return;
    }
    if (write(fd, "ERR use: RUN <req> <reply> <conc> <dur> <warmup> <threads> | PING\n", 65) < 0) {}
    close(fd);
}

static int ctrl_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 4) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (getenv("BENCH_DST_POD_ID")) g_dst_service = atoi(getenv("BENCH_DST_POD_ID"));
    const char *rxs = getenv("BENCH_RX_SCRATCH");
    if (rxs) g_rx_scratch = (size_t)atol(rxs);
    if (g_rx_scratch < 4096) g_rx_scratch = 4096;

    g_s = dmesh_create_channel(DMESH_SVC_NONE);       /* pure client */
    if (!g_s) { fprintf(stderr, "[bench] dmesh_create_channel failed\n"); return 1; }
    fprintf(stderr, "[bench] ready: pod_id=%d dst_service=%d slot=%d\n",
            dmesh_pod_id(g_s), g_dst_service, dmesh_msg_max(g_s));

    int srv = ctrl_listen(CTRL_PORT);
    if (srv < 0) return 1;
    fprintf(stderr, "[bench] control LISTEN on :%d\n", CTRL_PORT);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        handle_ctrl(c);
    }
}
