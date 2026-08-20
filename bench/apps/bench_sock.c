/* Matched-C POSIX RPC benchmark client for Envoy and DPUmesh preload.
 *
 * RUN uses a closed fixed-concurrency window. OPEN schedules constant or Poisson
 * arrivals and measures latency from scheduled send time. bench.h defines the
 * request/reply framing. Results include rate, goodput, latency percentiles,
 * failures, drops, histogram overflow, and reordering. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <math.h>
#include <fcntl.h>
#include <netdb.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "bench.h"
#include "bench_result.h"
#include "bench_selftest.h"

#define CTRL_PORT_DEFAULT  9092
#define MAX_THREADS        64
#define WORKER_STACK_BYTES (256 * 1024)
#define STOP_GRACE_SEC     15
#define DRAIN_GRACE_SEC    1.0
#define REQ_FILL           42
#define RD_CHUNK           (64 * 1024)
#define INFLIGHT_RING      (1u << 16)    /* outstanding slots, indexed by seq % RING */
#define OPEN_CAP           (INFLIGHT_RING / 2)  /* max outstanding in open loop before drops */

enum { MODE_CLOSED = 0, MODE_OPEN = 1 };
enum { ARR_CONST = 0, ARR_POISSON = 1 };

static char g_host[256] = "127.0.0.1";
static int  g_port      = 9100;

/* ------------------------------------------------------------ tx byte queue
 * Unsent request bytes for one connection: append whole frames, drain with
 * non-blocking write(). Compacts to the front when the head runs on. Bounded — a
 * full queue is backpressure (the server is not draining), which stops new issues
 * (closed) or records a drop (open) rather than growing without limit. */
typedef struct { uint8_t *buf; size_t cap, head, len; } txq_t;

static int txq_init(txq_t *q, size_t cap) {
    q->buf = (uint8_t *)malloc(cap); q->cap = cap; q->head = q->len = 0;
    return q->buf ? 0 : -1;
}
static void txq_free(txq_t *q) { free(q->buf); q->buf = NULL; }
static int txq_room(txq_t *q, size_t need) {
    if (q->head + q->len + need > q->cap) {           /* would run off the end */
        if (q->len) memmove(q->buf, q->buf + q->head, q->len);
        q->head = 0;
    }
    return q->len + need <= q->cap;
}
static void txq_append(txq_t *q, const uint8_t *b, size_t n) {
    memcpy(q->buf + q->head + q->len, b, n); q->len += n;
}
/* Returns 0 ok (maybe partial), -1 hard error. `*blocked` set if the kernel buffer
 * is full (EAGAIN) with bytes still queued — the caller then waits for EPOLLOUT. */
static int txq_flush(txq_t *q, int fd, int *blocked) {
    *blocked = 0;
    while (q->len > 0) {
        ssize_t w = write(fd, q->buf + q->head, q->len);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { *blocked = 1; return 0; }
            return -1;
        }
        q->head += (size_t)w; q->len -= (size_t)w;
    }
    q->head = 0;
    return 0;
}

/* ------------------------------------------------------------ per-thread run */
typedef struct {
    int          req_size, reply_size;
    int          mode;         /* MODE_CLOSED | MODE_OPEN */
    int          W;            /* closed: concurrency window */
    double       rate;         /* open: this thread's offered RPS */
    int          arrival;      /* open: ARR_CONST | ARR_POISSON */
    long         warmup;
    double       duration, start_at;
    atomic_int  *stop;

    int          fd;
    txq_t        tx;
    double      *start_ts;     /* per outstanding: SCHEDULED time (open) or issue time (closed) */
    uint8_t     *live;
    uint32_t    *slot_seq;
    uint32_t     next_seq;
    long         outstanding;
    long         pending;
    int          draining;
    double       sched_next;   /* open: next scheduled send time */
    uint64_t     prng;         /* open/poisson: per-thread xorshift state */

    long         rcnt, fail, reorder, drops;
    uint32_t     prev_seq;
    double       warmup_end, dura, now_cache;
    bench_hist_t hist;
    atomic_int   broken;
} worker_t;

static double prng_exp_gap(worker_t *w, double rate) {   /* ~ Exp(rate): -ln(U)/rate */
    w->prng ^= w->prng << 13; w->prng ^= w->prng >> 7; w->prng ^= w->prng << 17;
    double u = ((double)(w->prng >> 11) + 1.0) / 9007199254740993.0;  /* (0,1) */
    return -log(u) / rate;
}

/* Correlate complete replies by sequence. Open-loop latency uses scheduled time.
 * The receive stamp is taken per reply so both clients time identically. */
static void on_reply(uint32_t seq, uint32_t plen, uint32_t aux, void *user) {
    (void)plen; (void)aux;
    worker_t *w = (worker_t *)user;
    double now = bench_now_sec();
    uint32_t idx = seq % INFLIGHT_RING;
    if (!w->live[idx] || w->slot_seq[idx] != seq) { w->fail++; return; }
    w->live[idx] = 0;
    w->outstanding--;
    if (w->draining) return;
    double t0 = w->start_ts[idx];
    if ((int32_t)(seq - w->prev_seq) <= 0) w->reorder++;   /* arrival went backwards */
    w->prev_seq = seq;
    if (w->rcnt >= w->warmup) bench_hist_record(&w->hist, (now - t0) * 1e6);
    w->rcnt++;
    if (w->rcnt == w->warmup) w->warmup_end = now;
}

/* Drain replies until EAGAIN. Returns -1 if the peer closed / errored. */
static int reap(worker_t *w, bench_reframer_t *rf, uint8_t *rd) {
    for (;;) {
        ssize_t n = read(w->fd, rd, RD_CHUNK);
        if (n > 0) {
            if (bench_reframe_feed(rf, rd, (size_t)n, BENCH_REP_MAGIC, on_reply, w) < 0) {
                fprintf(stderr, "[bench_sock] reply desync\n");
                return -1;
            }
            continue;
        }
        if (n == 0) return -1;                         /* peer closed */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* Build one request frame [hdr | filler] into the tx queue. Caller has ensured the
 * slot is free and the queue has room. Stamps start_ts with `sched` (the intended
 * send time in open loop; the actual time in closed loop). */
/* Shared read-only request filler, initialised exactly once across all worker
 * threads via pthread_once — a plain `static int ready` guard is a data race. */
static uint8_t g_req_fill[RD_CHUNK];
static pthread_once_t g_req_fill_once = PTHREAD_ONCE_INIT;
static void init_req_fill(void) { memset(g_req_fill, REQ_FILL, sizeof g_req_fill); }

static void enqueue_request(worker_t *w, double sched) {
    uint8_t hdr[BENCH_HDR_LEN];
    bench_put_hdr(hdr, BENCH_REQ_MAGIC, w->next_seq, (uint32_t)w->req_size,
                  (uint32_t)w->reply_size);
    txq_append(&w->tx, hdr, BENCH_HDR_LEN);
    if (w->req_size > 0) {
        pthread_once(&g_req_fill_once, init_req_fill);
        uint32_t left = (uint32_t)w->req_size;
        while (left > 0) { uint32_t n = left < RD_CHUNK ? left : RD_CHUNK;
                           txq_append(&w->tx, g_req_fill, n); left -= n; }
    }
    uint32_t si = w->next_seq % INFLIGHT_RING;
    w->start_ts[si] = sched; w->slot_seq[si] = w->next_seq; w->live[si] = 1;
    w->next_seq++; w->outstanding++;
}

static int connect_target(void) {
    struct addrinfo hints, *res = NULL, *rp; char ports[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(ports, sizeof ports, "%d", g_port);
    if (getaddrinfo(g_host, ports, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static void *worker_fn(void *arg) {
    worker_t *w = (worker_t *)arg;
    double start = 0.0, end = 0.0;
    uint8_t *rd = (uint8_t *)malloc(RD_CHUNK);
    bench_reframer_t rf; bench_reframe_reset(&rf);
    if (bench_hist_init(&w->hist) < 0 || !rd) { atomic_store(&w->broken, 1); free(rd); return NULL; }
    w->start_ts = (double *)calloc(INFLIGHT_RING, sizeof(double));
    w->live     = (uint8_t *)calloc(INFLIGHT_RING, 1);
    w->slot_seq = (uint32_t *)calloc(INFLIGHT_RING, sizeof(uint32_t));
    /* tx queue holds UNSENT frames. Size it to the whole closed-loop window so the
     * issue loop can actually keep W requests in flight (a queue that fits only a
     * couple of frames silently caps effective concurrency); bound it for very large
     * messages, where flush-when-full below keeps the window filling anyway. */
    size_t frame = (size_t)BENCH_HDR_LEN + (size_t)w->req_size;
    size_t txcap = frame * (size_t)(w->W > 0 ? (w->W + 2) : 66);
    if (txcap > (8u << 20)) txcap = 8u << 20;
    if (txcap < frame + 8192) txcap = frame + 8192;
    if (txq_init(&w->tx, txcap) < 0 ||
        !w->start_ts || !w->live || !w->slot_seq) { atomic_store(&w->broken, 1); goto done; }
    w->prev_seq = UINT32_MAX;

    w->fd = connect_target();
    if (w->fd < 0) { atomic_store(&w->broken, 1); goto done; }

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = w->fd };
    epoll_ctl(ep, EPOLL_CTL_ADD, w->fd, &ev);
    uint32_t cur_events = EPOLLIN;

    while (bench_now_sec() < w->start_at) {
        if (atomic_load(w->stop)) { atomic_store(&w->broken, 1); goto done_ep; }
        struct timespec ts = {0, 50000}; nanosleep(&ts, NULL);
    }
    start = bench_now_sec();
    w->warmup_end = start;
    w->sched_next = start;
    int tx_blocked = 0;
    end = start;

    while (!atomic_load(&w->broken) && !atomic_load(w->stop)) {
        w->now_cache = bench_now_sec();
        if (w->now_cache - start > w->duration) break;

        if (reap(w, &rf, rd) < 0) { atomic_store(&w->broken, 1); break; }
        w->now_cache = bench_now_sec();

        /* Issue side. */
        if (w->mode == MODE_CLOSED) {
            while (w->outstanding < w->W) {
                if (!txq_room(&w->tx, BENCH_HDR_LEN + w->req_size)) {
                    /* queue full mid-fill: flush to the socket and keep going, so the
                     * window fills to W. A blocked kernel buffer is real backpressure. */
                    if (txq_flush(&w->tx, w->fd, &tx_blocked) < 0) { atomic_store(&w->broken, 1); break; }
                    if (tx_blocked) break;
                }
                enqueue_request(w, w->now_cache);
            }
        } else {
            while (w->now_cache >= w->sched_next) {
                double sched = w->sched_next;
                double gap = (w->arrival == ARR_POISSON) ? prng_exp_gap(w, w->rate)
                                                         : 1.0 / w->rate;
                w->sched_next += gap;
                if (w->outstanding >= OPEN_CAP ||
                    !txq_room(&w->tx, BENCH_HDR_LEN + w->req_size)) { w->drops++; continue; }
                enqueue_request(w, sched);
            }
        }

        if (txq_flush(&w->tx, w->fd, &tx_blocked) < 0) { atomic_store(&w->broken, 1); break; }

        /* Arm EPOLLOUT only while the kernel buffer is full with bytes pending. */
        uint32_t want = EPOLLIN | (tx_blocked && w->tx.len ? EPOLLOUT : 0);
        if (want != cur_events) {
            ev.events = want; epoll_ctl(ep, EPOLL_CTL_MOD, w->fd, &ev); cur_events = want;
        }

        /* Sleep until the socket is ready — or, in open loop, until the next
         * scheduled send. A short cap bounds how long a quiet loop parks so the
         * duration check stays responsive. */
        struct epoll_event out[1];
        if (w->mode == MODE_OPEN) {
            bench_epoll_wait_until(ep, out, w->sched_next);
        } else {
            int timeout_ms = (w->outstanding > 0 || w->tx.len) ? 20 : 1;
            epoll_wait(ep, out, 1, timeout_ms);
        }
    }
    end = bench_now_sec();
    w->pending = w->outstanding;

    /* Finish requests accepted before the measurement boundary. This leaves the
     * peer with no unread request or blocked reply when close sends FIN. */
    w->draining = 1;
    double drain_deadline = end + DRAIN_GRACE_SEC;
    while ((w->tx.len > 0 || w->outstanding > 0) &&
           bench_now_sec() < drain_deadline && !atomic_load(w->stop)) {
        if (reap(w, &rf, rd) < 0) break;
        if (txq_flush(&w->tx, w->fd, &tx_blocked) < 0) break;

        uint32_t want = EPOLLIN | (tx_blocked && w->tx.len ? EPOLLOUT : 0);
        if (want != cur_events) {
            ev.events = want;
            epoll_ctl(ep, EPOLL_CTL_MOD, w->fd, &ev);
            cur_events = want;
        }
        if (w->tx.len == 0 && w->outstanding == 0) break;

        double left = drain_deadline - bench_now_sec();
        int timeout_ms = left > 0.020 ? 20 : (left > 0 ? (int)(left * 1000.0) : 0);
        epoll_wait(ep, &ev, 1, timeout_ms);
    }
    if (w->tx.len > 0 || w->outstanding > 0) {
        fprintf(stderr,
                "[bench_sock] drain timeout: queued=%zu outstanding=%ld\n",
                w->tx.len, w->outstanding);
        atomic_store(&w->broken, 1);
    }
done_ep:
    close(ep);
done:
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    w->dura = (end > start && w->rcnt > w->warmup) ? (end - w->warmup_end) : 0.0;
    free(rd); txq_free(&w->tx);
    free(w->start_ts); free(w->live); free(w->slot_seq);
    return NULL;
}

/* ------------------------------------------------------------ watchdog */
typedef struct { atomic_int *stop, *early; double deadline_sec; } wd_t;
static void *watchdog_fn(void *arg) {
    wd_t *a = (wd_t *)arg; double t0 = bench_now_sec();
    while (bench_now_sec() - t0 < a->deadline_sec) {
        if (atomic_load(a->early)) return NULL;
        struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL);
    }
    atomic_store(a->stop, 1); return NULL;
}

/* ------------------------------------------------------------ one benchmark run */
static void run_bench(int conn_fd, int mode, int req_size, int reply_size, int conc,
                      double duration, long warmup, int threads, double rate, int arrival) {
    char reply[1024];
    typedef int (*preload_stats_fn)(unsigned long long out[7]);
    preload_stats_fn read_preload_stats =
        (preload_stats_fn)dlsym(RTLD_DEFAULT, "dmesh_preload_tx_stats");
    unsigned long long tx0[7] = {0}, tx1[7] = {0};
    int have_tx0 = read_preload_stats && read_preload_stats(tx0) == 0;
    if (req_size < 0 || reply_size < 1 || duration <= 0 || threads < 1 ||
        (mode == MODE_CLOSED && conc < 1) || (mode == MODE_OPEN && rate <= 0)) {
        const char *e = "ERR invalid args\n"; if (write(conn_fd, e, strlen(e)) < 0) {} return;
    }
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    if (warmup < 0) warmup = 0;
    if (mode == MODE_OPEN && rate * duration / (double)threads <= (double)warmup)
        fprintf(stderr, "[bench_sock] WARNING: ~%.0f arrivals/thread <= warmup=%ld; "
                        "measurement window may be empty\n",
                rate * duration / (double)threads, warmup);

    char load[32];
    if (mode == MODE_OPEN) snprintf(load, sizeof load, "rate=%.0f", rate);
    else                   snprintf(load, sizeof load, "conc=%d", conc);
    fprintf(stderr, "[bench_sock] %s req=%d reply=%d %s dur=%.1fs warmup=%ld threads=%d target=%s:%d\n",
            mode == MODE_OPEN ? "OPEN" : "RUN", req_size, reply_size, load,
            duration, warmup, threads, g_host, g_port);

    worker_t  *w   = (worker_t *)calloc((size_t)threads, sizeof(worker_t));
    pthread_t *tid = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));
    if (!w || !tid) { free(w); free(tid); if (write(conn_fd, "ERR oom\n", 8) < 0) {} return; }

    atomic_int stop = 0;
    double start_at = bench_now_sec() + 0.1;
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);
    for (int i = 0; i < threads; i++) {
        w[i].req_size = req_size; w[i].reply_size = reply_size; w[i].mode = mode;
        w[i].W = conc; w[i].rate = rate / (double)threads; w[i].arrival = arrival;
        w[i].warmup = warmup; w[i].duration = duration; w[i].start_at = start_at;
        w[i].stop = &stop; w[i].prng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)(i + 1) * 0x100000001b3ULL);
        if (pthread_create(&tid[i], &attr, worker_fn, &w[i]) != 0) { atomic_store(&w[i].broken, 1); tid[i] = 0; }
    }
    pthread_attr_destroy(&attr);

    atomic_int early = 0;
    wd_t wa = { .stop = &stop, .early = &early, .deadline_sec = duration + STOP_GRACE_SEC };
    pthread_t wd; pthread_create(&wd, NULL, watchdog_fn, &wa);
    for (int i = 0; i < threads; i++) if (tid[i]) pthread_join(tid[i], NULL);
    atomic_store(&early, 1); pthread_join(wd, NULL);

    bench_hist_t agg; bench_hist_init(&agg);
    double mrps = 0.0, request_gbps = 0.0, response_gbps = 0.0;
    double request_frame = (double)BENCH_HDR_LEN + (double)req_size;
    double response_frame = (double)BENCH_HDR_LEN + (double)reply_size;
    long total_ok = 0, total_fail = 0, total_reorder = 0, total_drops = 0;
    long total_scheduled = 0, total_pending = 0;
    int worker_fail = 0;
    for (int i = 0; i < threads; i++) {
        long measured = w[i].rcnt - w[i].warmup; if (measured < 0) measured = 0;
        total_fail += w[i].fail; total_reorder += w[i].reorder; total_drops += w[i].drops;
        if (atomic_load(&w[i].broken)) { worker_fail++; total_fail++; }
        total_scheduled += (long)w[i].next_seq + w[i].drops;
        total_pending += w[i].pending;
        if (!atomic_load(&w[i].broken) && w[i].dura > 1e-9 && measured > 0) {
            mrps += (double)measured / w[i].dura * 1e-6;
            double rps = (double)measured / w[i].dura;
            request_gbps += 8e-9 * rps * request_frame;
            response_gbps += 8e-9 * rps * response_frame;
            total_ok += measured;
            bench_hist_merge(&agg, &w[i].hist);
        }
        bench_hist_free(&w[i].hist);
    }
    double p50 = bench_hist_pct(&agg, 50.0), p95 = bench_hist_pct(&agg, 95.0);
    double p99 = bench_hist_pct(&agg, 99.0), p999 = bench_hist_pct(&agg, 99.9);
    double p9999 = bench_hist_pct(&agg, 99.99);
    double avg = bench_hist_avg(&agg), mn = bench_hist_min(&agg), mx = bench_hist_max(&agg);
    uint64_t overflow = agg.overflow;
    bench_hist_free(&agg);

    double offered_mrps = (mode == MODE_OPEN) ? rate * 1e-6 : mrps;
    double gbps = request_gbps + response_gbps;
    int have_tx1 = have_tx0 && read_preload_stats(tx1) == 0;
    char tx_stats[192];
    if (have_tx1) {
        snprintf(tx_stats, sizeof tx_stats,
                 "grabs=%llu rets=%llu recyc=%llu waits=%llu pads=%llu "
                 "wwin=%llu wpool=%llu",
                 tx1[0] - tx0[0], tx1[1] - tx0[1], tx1[2] - tx0[2],
                 tx1[3] - tx0[3], tx1[4] - tx0[4],
                 tx1[5] - tx0[5], tx1[6] - tx0[6]);
    } else {
        snprintf(tx_stats, sizeof tx_stats,
                 "grabs=NA rets=NA recyc=NA waits=NA pads=NA "
                 "wwin=NA wpool=NA");
    }
    int n = snprintf(reply, sizeof reply,
        "%s mrps=%.6f gbps=%.4f req_gbps=%.4f resp_gbps=%.4f "
        "p50=%.2f p95=%.2f p99=%.2f p999=%.2f p9999=%.2f "
        "avg=%.2f min=%.2f max=%.2f rcnt=%ld scheduled=%ld pending=%ld fail=%ld "
        "conc=%d threads=%d reqsz=%d repsz=%d reqframe=%u respframe=%u "
        "durs=%.3f offered_mrps=%.6f drops=%ld overflow=%llu worker_fail=%d reorder=%ld "
        "mode=%s arr=%s %s\n",
        bench_result_status(total_ok, total_fail, worker_fail),
        mrps, gbps, request_gbps, response_gbps,
        p50, p95, p99, p999, p9999, avg, mn, mx,
        total_ok, total_scheduled, total_pending, total_fail,
        conc, threads, req_size, reply_size,
        BENCH_HDR_LEN + (uint32_t)req_size,
        BENCH_HDR_LEN + (uint32_t)reply_size, duration,
        offered_mrps, total_drops, (unsigned long long)overflow, worker_fail, total_reorder,
        mode == MODE_OPEN ? "open" : "closed",
        arrival == ARR_POISSON ? "poisson" : "const", tx_stats);
    if (write(conn_fd, reply, (size_t)n) < 0) {}
    fprintf(stderr, "[bench_sock] DONE %s", reply);
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
        sscanf(buf, "%*s %d %d %d %lf %ld %d", &req, &rep, &conc, &dur, &warm, &threads);
        run_bench(fd, MODE_CLOSED, req, rep, conc, dur, warm, threads, 0.0, ARR_CONST);
        close(fd); return;
    }
    if (sscanf(buf, "%15s", cmd) == 1 && strcmp(cmd, "OPEN") == 0) {
        int req = 32, rep = 8, threads = 1; double dur = 10.0, rate = 100000.0; long warm = 1000;
        char arr[16] = "const";
        sscanf(buf, "%*s %d %d %d %lf %ld %lf %15s", &req, &rep, &threads, &dur, &warm, &rate, arr);
        int arrival = (strcmp(arr, "poisson") == 0) ? ARR_POISSON : ARR_CONST;
        run_bench(fd, MODE_OPEN, req, rep, 0, dur, warm, threads, rate, arrival);
        close(fd); return;
    }
    if (sscanf(buf, "%15s", cmd) == 1 && strcmp(cmd, "SELFTEST") == 0) {
        int payload, threads;
        double duration, rate;
        char arr[16];
        char reply[512];
        int fields = sscanf(buf, "%*s %d %d %lf %lf %15s",
                            &payload, &threads, &duration, &rate, arr);
        int arrival = fields == 5 && strcmp(arr, "poisson") == 0
                    ? ARR_POISSON : ARR_CONST;
        if (fields != 5 || (strcmp(arr, "const") != 0 &&
                            strcmp(arr, "poisson") != 0) ||
            threads > MAX_THREADS ||
            bench_run_selftest(reply, sizeof reply, payload, threads, duration,
                               rate, arrival) < 0) {
            snprintf(reply, sizeof reply, "ERR invalid SELFTEST args\n");
        }
        if (write(fd, reply, strlen(reply)) < 0) {}
        close(fd); return;
    }
    const char *u = "ERR use: RUN <req> <reply> <conc> <dur> <warmup> <threads> | "
                    "OPEN <req> <reply> <threads> <dur> <warmup> <rate> [const|poisson] | "
                    "SELFTEST <payload> <threads> <dur> <rate> <const|poisson> | PING\n";
    if (write(fd, u, strlen(u)) < 0) {}
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

/* Parse BENCH_TARGET = "host:port" (default 127.0.0.1:9100). */
static void parse_target(void) {
    const char *t = getenv("BENCH_TARGET");
    if (!t || !*t) return;
    const char *colon = strrchr(t, ':');
    if (colon) {
        size_t hl = (size_t)(colon - t); if (hl >= sizeof g_host) hl = sizeof g_host - 1;
        memcpy(g_host, t, hl); g_host[hl] = '\0';
        g_port = atoi(colon + 1);
    } else {
        snprintf(g_host, sizeof g_host, "%s", t);
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1) {
        char reply[512];
        int payload, threads, arrival;
        double duration, rate;
        if (argc != 7 || strcmp(argv[1], "--selftest") != 0 ||
            sscanf(argv[2], "%d", &payload) != 1 ||
            sscanf(argv[3], "%d", &threads) != 1 ||
            sscanf(argv[4], "%lf", &duration) != 1 ||
            sscanf(argv[5], "%lf", &rate) != 1 ||
            (strcmp(argv[6], "const") != 0 && strcmp(argv[6], "poisson") != 0) ||
            threads > MAX_THREADS) {
            fprintf(stderr, "usage: %s --selftest <payload> <threads> <dur> "
                            "<rate> <const|poisson>\n", argv[0]);
            return 2;
        }
        arrival = strcmp(argv[6], "poisson") == 0 ? ARR_POISSON : ARR_CONST;
        int rc = bench_run_selftest(reply, sizeof reply, payload, threads,
                                    duration, rate, arrival);
        fputs(reply, stdout);
        return rc == 0 ? 0 : 1;
    }
    parse_target();
    int ctrl_port = getenv("CTRL_PORT") ? atoi(getenv("CTRL_PORT")) : CTRL_PORT_DEFAULT;

    int srv = ctrl_listen(ctrl_port);
    if (srv < 0) return 1;
    fprintf(stderr, "[bench_sock] control LISTEN on :%d, target=%s:%d\n", ctrl_port, g_host, g_port);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        handle_ctrl(c);
    }
}
