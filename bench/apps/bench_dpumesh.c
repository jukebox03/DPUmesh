/*
 * bench_dpumesh.c — RPC benchmark CLIENT for DPUmesh (bench-dpumesh pod).
 *
 * A closed-loop request/response load generator over the DPUmesh native API
 * (dmesh.h). It measures the standard RPC microbenchmark quantities:
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
 *   * One dmesh connection per client thread. Replies are correlated BY SEQ, never by
 *     arrival order: whether they keep send order depends on the service's codec, which
 *     is DPU-side config, not something this client knows. `reorder` counts how many
 *     arrived out of order and `dist` tallies which backend served each — together they
 *     show the routing granularity:
 *       no codec (passthru) -> conn pinned to one backend -> reorder=0, dist on one pod
 *       codec (_L7_SVC/_FRAME_SVC) -> every message load-balanced -> reorder>0, dist spread
 *   * Requests/replies are framed (bench.h) so a >8 KB request that the transport
 *     auto-chunks across slots is reassembled by the server before it replies —
 *     so the measured latency includes the full request transfer.
 *   * ONE CQ PER THREAD (dmesh_create_cq), each with that thread's own conn on it.
 *     A CQ is single-consumer, so a thread polls only its own CQ, every completion
 *     it sees is its own, and threads share no RX state, no lock and no dispatch.
 *     Measured worth vs all threads on one CQ under a mutex: ~+5% Mrps and ~-17%
 *     p50 at 8 threads (32B/8B, conc=4/thread). NOT more: the DPU pipeline is the
 *     ceiling here, and one thread at conc=64 already saturates it — so the THREAD
 *     sweep only scales while per-thread concurrency stays well under that.
 *   * dmesh_alloc never blocks and caps at post_max, so a request frame is carved
 *     into <= post_max posts and an SQ-full frame parks and resumes on the next
 *     loop pass; the payload is constant filler written straight into the TX ring.
 *
 * Control protocol (TCP :9092, one line):
 *   RUN <req_size> <reply_size> <concurrency> <duration_s> <warmup> <threads> [reconn]
 *        -> OK mrps=.. gbps=.. p50=.. p95=.. p99=.. avg=.. min=.. max=..
 *              rcnt=.. fail=.. conc=.. threads=.. reqsz=.. repsz=.. durs=..
 *              reconns=.. reconn_us=.. grabs=.. rets=.. recyc=.. waits=.. pads=..
 *      (all latencies in microseconds; key=value fields)
 *      reconn: CONNECTION-CHURN mode — every `reconn` completions a thread drains
 *      its window to 0 outstanding, then dmesh_destroy_qp()+dmesh_create_qp()s a fresh
 *      conn (0 = never, default). reconns = total reconnects, reconn_us = mean
 *      wall cost of one close+connect+pin. grabs/rets/recyc/waits/pads = the
 *      elastic TX block-pool event deltas over the run (dmesh_tx_stats_t).
 *   PING -> PONG
 *
 * Addresses the backend by k8s Service NAME ("echo-dpumesh"), resolved through the
 * registry — no service id is typed here.
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
#define CQ_BATCH           64
#define STOP_GRACE_SEC     15         /* watchdog kill margin past the run duration */
#define REQ_FILL           42
#define BENCH_MAX_BACKENDS 32         /* pod_id space we tally replies over (>= MAX_PODS) */
#define MAX_STREAMS        32         /* inbound streams per conn == backends of the service */
#define INFLIGHT_RING      1024       /* direct-map slots for outstanding requests, by seq.
                                       * MUST exceed the concurrency window by a wide margin:
                                       * replies complete OUT OF ORDER on a codec'd service, so
                                       * the span from the oldest outstanding seq to the newest
                                       * issued one is NOT bounded by the window — a lagging
                                       * request holds its slot while newer ones cycle. Each
                                       * slot stores its seq, so a collision is DETECTED (fail)
                                       * rather than silently mis-timed. */

static dmesh_channel_t *g_s           = NULL;   /* shared, thread-safe channel */
static const char      *g_dst_service = "echo-dpumesh";  /* backend service NAME to address */
static uint32_t         g_post_max    = 0;      /* max bytes one dmesh_alloc/post can carry */

/* ------------------------------------------------------------ per-thread run */
typedef struct {
    /* config (shared, read-only during the run) */
    int          req_size;
    int          reply_size;
    int          W;            /* concurrency window (outstanding per thread) */
    long         warmup;       /* completions excluded from measurement */
    double       duration;     /* run length in seconds */
    double       start_at;     /* shared barrier: all threads begin at this time */
    long         reconn;       /* completions per conn before close+reconnect (0 = never) */
    atomic_int  *stop;         /* watchdog / abort flag */

    /* per-thread transport + pipeline state. ALL of it — issue side and reply side
     * alike — is owned by this one thread: the conn lives on this thread's own CQ, so
     * only this thread can ever poll its completions. No cross-thread state, hence no
     * lock and no atomics below. */
    dmesh_cq_t      *cq;        /* this thread's CQ (single-consumer, polled here only) */
    dmesh_qp_t    *c;
    /* Outstanding requests, direct-mapped by seq % INFLIGHT_RING (see above). */
    double          *start_ts;  /* issue time */
    uint32_t        *slot_seq;  /* which seq owns the slot (collision check) */
    uint8_t         *live;      /* 1 = awaiting its reply */
    uint32_t         next_seq;  /* next seq to assign on issue */
    uint32_t         prev_seq;  /* seq of the previous completion — only to count reordering */
    uint32_t         tx_done;   /* bytes of the in-flight request frame already posted */
    int              tx_active; /* a frame is mid-carve (must finish before the next) */
    long             outstanding;
    long             credits;   /* completions observed -> requests to reissue */
    /* One reframer PER INBOUND STREAM (wc.stream). A reply longer than msg_max arrives
     * as several completions, and on a per-message-routed service the backends reply
     * concurrently, so another peer's bytes can land between two of them. Reassembling
     * them all through one reframer would splice the streams together and desync. */
    struct { uint16_t id; int used; bench_reframer_t rf; } rs[MAX_STREAMS];

    /* per-thread results */
    long         rcnt;         /* total completions incl. warmup */
    long         since_conn;   /* completions on the CURRENT conn (churn trigger) */
    long         reconns;      /* reconnects performed */
    double       reconn_sec;   /* wall time inside close+connect+pin (sums) */
    long         fail;         /* completions that named a seq we never had outstanding */
    long         reorder;      /* replies that arrived out of send order. 0 on a pinned
                                * (no-codec) service; >0 is the visible proof that a
                                * codec'd service load-balances per message. */
    long         dist[BENCH_MAX_BACKENDS];  /* replies per backend pod_id — which pod served */
    double       warmup_end;   /* timestamp of the warmup boundary */
    double       dura;         /* measured window length (s) */
    bench_hist_t hist;         /* post-warmup latencies (us) */
    atomic_int   broken;       /* conn/alloc failure -> excluded from aggregate */
} worker_t;

/* Find (or start) this stream's reframer. Linear over MAX_STREAMS: a conn has at most
 * one stream per backend of the service, so the scan is a handful of compares and needs
 * no hashing. NULL = more distinct peers than MAX_STREAMS. */
static bench_reframer_t *stream_rf(worker_t *w, uint16_t id) {
    int free_i = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (w->rs[i].used && w->rs[i].id == id) return &w->rs[i].rf;
        if (!w->rs[i].used && free_i < 0) free_i = i;
    }
    if (free_i < 0) return NULL;
    w->rs[free_i].used = 1;
    w->rs[free_i].id   = id;
    bench_reframe_reset(&w->rs[free_i].rf);
    return &w->rs[free_i].rf;
}

static void streams_reset(worker_t *w) {
    for (int i = 0; i < MAX_STREAMS; i++) w->rs[i].used = 0;
}

/* Fire once per fully-received reply frame. Correlate BY SEQ, not by arrival order: a
 * codec'd service load-balances every message, so replies from different backends
 * interleave and out-of-order is normal, not an error. `aux` carries the backend's
 * pod_id. Runs on w's own thread: the completion came off w's own CQ. */
static void on_reply(uint32_t seq, uint32_t plen, uint32_t aux, void *user) {
    (void)plen;
    worker_t *w = (worker_t *)user;
    double now = bench_now_sec();
    uint32_t idx = seq % INFLIGHT_RING;
    if (!w->live[idx] || w->slot_seq[idx] != seq) { w->fail++; return; }  /* not in flight */
    w->live[idx] = 0;
    double t0 = w->start_ts[idx];
    if (seq != w->prev_seq + 1) w->reorder++;   /* arrival order != send order */
    w->prev_seq = seq;
    if (aux < BENCH_MAX_BACKENDS) w->dist[aux]++;
    w->outstanding--;
    if (w->rcnt >= w->warmup)
        bench_hist_record(&w->hist, (now - t0) * 1e6);
    w->rcnt++;
    w->since_conn++;
    if (w->rcnt == w->warmup) w->warmup_end = now;   /* measurement window opens */
    w->credits++;
}

/* Drain THIS thread's CQ. Every completion on it belongs to this worker's conn — no
 * dispatch, no lock, no sharing with the other threads. Returns the count harvested. */
static int cq_pump(worker_t *w) {
    dmesh_wc_t wc[CQ_BATCH];
    int got = 0, n;
    while ((n = dmesh_poll_cq(w->cq, wc, CQ_BATCH)) > 0) {  /* drain to 0 (edge-triggered rule) */
        for (int i = 0; i < n; i++) {
            if (wc[i].opcode == DMESH_WC_RECV) {
                bench_reframer_t *rf = stream_rf(w, wc[i].stream);
                if (!rf) w->fail++;           /* more peers than MAX_STREAMS */
                else if (bench_reframe_feed(rf, wc[i].buf, wc[i].len,
                                            BENCH_REP_MAGIC, on_reply, w) < 0) {
                    /* Unrecoverable: this stream's boundary is lost. Fail loudly instead
                     * of stalling forever on a reframer that will never complete a frame. */
                    fprintf(stderr, "[bench] reply stream %u desync\n", wc[i].stream);
                    w->fail++;
                    atomic_store(&w->broken, 1);
                }
                dmesh_wc_release(g_s, &wc[i]);
            } else if (wc[i].opcode == DMESH_WC_RECV_FIN) {
                atomic_store(&w->broken, 1);              /* backend FIN — abort this thread */
            }
        }
        got += n;
    }
    return got;
}

/* Ship one request frame: [hdr | payload], carved into <= post_max posts (all but
 * the last defer the doorbell, so the frame still ships in ONE doorbell). The
 * payload is constant filler, so it is written straight into the TX ring — no
 * staging buffer. start_ts is stamped when the frame STARTS, so the closed-loop
 * latency includes any SQ queueing, as it did when the write blocked.
 * Returns 1 = frame posted, 0 = SQ full (resume from tx_done), -1 = hard fault. */
static int issue(worker_t *w) {
    uint32_t total = BENCH_HDR_LEN + (uint32_t)w->req_size;
    if (!w->tx_active) {
        w->start_ts[w->next_seq % INFLIGHT_RING] = bench_now_sec();
        w->tx_active = 1;
        w->tx_done   = 0;
    }
    while (w->tx_done < total) {
        uint32_t want = total - w->tx_done;
        if (want > g_post_max) want = g_post_max;
        uint8_t *b = (uint8_t *)dmesh_alloc(w->c, want);
        if (!b) return (errno == EAGAIN) ? 0 : -1;
        uint32_t off = 0;
        if (w->tx_done == 0) {
            bench_put_hdr(b, BENCH_REQ_MAGIC, w->next_seq, (uint32_t)w->req_size,
                          (uint32_t)w->reply_size);
            off = BENCH_HDR_LEN;
        }
        memset(b + off, REQ_FILL, want - off);
        unsigned fl = (w->tx_done + want < total) ? DMESH_SEND_MORE : 0;
        if (dmesh_post_send(w->c, b, want, 0, fl) != 0) return -1;
        w->tx_done += want;
    }
    w->tx_active = 0;
    uint32_t si = w->next_seq % INFLIGHT_RING;
    w->slot_seq[si] = w->next_seq;
    w->live[si] = 1;                            /* in flight until its reply lands */
    w->next_seq++;
    w->outstanding++;
    return 1;
}

static void *worker_fn(void *arg) {
    worker_t *w = (worker_t *)arg;
    double end = 0.0;
    streams_reset(w);

    if (bench_hist_init(&w->hist) < 0) { atomic_store(&w->broken, 1); return NULL; }
    w->start_ts = (double *)calloc(INFLIGHT_RING, sizeof(double));
    w->slot_seq = (uint32_t *)calloc(INFLIGHT_RING, sizeof(uint32_t));
    w->live     = (uint8_t *)calloc(INFLIGHT_RING, 1);
    if (!w->start_ts || !w->slot_seq || !w->live) { atomic_store(&w->broken, 1); goto done; }
    w->prev_seq = UINT32_MAX;                   /* so seq 0 counts as in-order */

    /* This thread's OWN CQ, and its conn on it: nothing on this CQ belongs to anyone
     * else, so poll_cq below is contention-free. This is the scaling knob. */
    w->cq = dmesh_create_cq(g_s);
    if (!w->cq) { atomic_store(&w->broken, 1); goto done; }
    w->c = dmesh_create_qp(w->cq, g_dst_service);
    if (!w->c) { atomic_store(&w->broken, 1); goto done; }

    /* barrier: all threads start together */
    while (bench_now_sec() < w->start_at) {
        if (atomic_load(w->stop)) { atomic_store(&w->broken, 1); goto done; }
        struct timespec ts = {0, 50000}; nanosleep(&ts, NULL);
    }
    double start = bench_now_sec();
    w->warmup_end = start;                        /* fallback if warmup never reached */
    w->credits = w->W;                            /* prime the window through the loop below */

    while (!atomic_load(&w->broken) && !atomic_load(w->stop)) {
        if (bench_now_sec() - start > w->duration) break;

        int did = cq_pump(w) > 0;

        /* Connection churn: once `reconn` completions landed on this conn, STOP
         * issuing (the gate below), drain to 0 outstanding, then swap the conn.
         * Reconnect wall time is accumulated so the per-reconnect cost is a
         * direct measurement (not inferred from the rate delta). */
        int churn = (w->reconn > 0 && w->since_conn >= w->reconn);
        if (churn && w->outstanding == 0 && !w->tx_active) {
            double t0 = bench_now_sec();
            dmesh_destroy_qp(w->c);                    /* vq_cur is this CQ's, i.e. ours */
            w->c = dmesh_create_qp(w->cq, g_dst_service);
            if (!w->c) { atomic_store(&w->broken, 1); break; }
            streams_reset(w);                  /* fresh conn → fresh upstreams */
            w->since_conn = 0;
            w->reconn_sec += bench_now_sec() - t0;
            w->reconns++;
            w->credits = w->W;                    /* window re-primed fresh */
            churn = 0;
            did = 1;
        }

        /* Keep the window full … unless draining to churn. A mid-carve frame always
         * finishes first: it holds the conn's byte stream open. */
        while (w->tx_active || (!churn && w->credits > 0)) {
            if (!w->tx_active && bench_now_sec() - start > w->duration) {
                w->credits = 0; break;
            }
            int r = issue(w);
            if (r < 0) { atomic_store(&w->broken, 1); break; }
            if (r == 0) break;                    /* SQ full: resume on the next pass */
            w->credits--;
            did = 1;
        }
        if (!did) { struct timespec ts = {0, 2000}; nanosleep(&ts, NULL); }  /* 2us */
    }
    end = bench_now_sec();

done:
    /* Conn first, then its CQ (a conn outliving its CQ has nowhere to report). */
    if (w->c) { dmesh_destroy_qp(w->c); w->c = NULL; }
    if (w->cq) { dmesh_destroy_cq(w->cq); w->cq = NULL; }
    w->dura = (end > 0.0 && w->rcnt > w->warmup) ? (end - w->warmup_end) : 0.0;
    free(w->start_ts);
    free(w->slot_seq);
    free(w->live);
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
                      double duration, long warmup, int threads, long reconn) {
    char reply[768];
    if (req_size < 0 || reply_size < 1 || concurrency < 1 || duration <= 0 || threads < 1) {
        const char *e = "ERR invalid args\n";
        if (write(conn_fd, e, strlen(e)) < 0) {} return;
    }
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    if (warmup < 0) warmup = 0;
    if (reconn < 0) reconn = 0;

    fprintf(stderr, "[bench] RUN req=%d reply=%d conc=%d dur=%.1fs warmup=%ld threads=%d reconn=%ld dst_svc=%s\n",
            req_size, reply_size, concurrency, duration, warmup, threads, reconn, g_dst_service);

    dmesh_tx_stats_t st0, st1;                    /* elastic-pool event deltas over the run */
    dmesh_get_tx_stats(g_s, &st0);

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
        w[i].reconn     = reconn;
        w[i].stop       = &stop;
        if (pthread_create(&tid[i], &attr, worker_fn, &w[i]) != 0) {
            atomic_store(&w[i].broken, 1); tid[i] = 0;
        }
    }
    pthread_attr_destroy(&attr);

    atomic_int early = 0;
    wd_t wa = { .stop = &stop, .early = &early, .deadline_sec = duration + STOP_GRACE_SEC };
    pthread_t wd; pthread_create(&wd, NULL, watchdog_fn, &wa);

    for (int i = 0; i < threads; i++) if (tid[i]) pthread_join(tid[i], NULL);
    atomic_store(&early, 1); pthread_join(wd, NULL);

    /* aggregate: sum per-thread rates, merge latency histograms */
    bench_hist_t agg; bench_hist_init(&agg);
    double mrps = 0.0, gbps = 0.0, reconn_sec = 0.0;
    long total_ok = 0, total_fail = 0, total_reconns = 0, total_reorder = 0;
    long dist[BENCH_MAX_BACKENDS] = { 0 };
    for (int i = 0; i < threads; i++) {
        long measured = w[i].rcnt - w[i].warmup;
        if (measured < 0) measured = 0;
        total_fail += w[i].fail;
        total_reorder += w[i].reorder;
        for (int b = 0; b < BENCH_MAX_BACKENDS; b++) dist[b] += w[i].dist[b];
        total_reconns += w[i].reconns;
        reconn_sec    += w[i].reconn_sec;
        if (!atomic_load(&w[i].broken) && w[i].dura > 1e-9 && measured > 0) {
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

    dmesh_get_tx_stats(g_s, &st1);
    double reconn_us = (total_reconns > 0) ? reconn_sec * 1e6 / (double)total_reconns : 0.0;

    int n = snprintf(reply, sizeof reply,
        "OK mrps=%.6f gbps=%.4f p50=%.2f p95=%.2f p99=%.2f avg=%.2f min=%.2f max=%.2f "
        "rcnt=%ld fail=%ld conc=%d threads=%d reqsz=%d repsz=%d durs=%.3f "
        "reconns=%ld reconn_us=%.2f grabs=%llu rets=%llu recyc=%llu waits=%llu pads=%llu "
        "reorder=%ld",
        mrps, gbps, p50, p95, p99, avg, mn, mx,
        total_ok, total_fail, concurrency, threads, req_size, reply_size, duration,
        total_reconns, reconn_us,
        st1.pool_grabs - st0.pool_grabs, st1.pool_returns - st0.pool_returns,
        st1.recycle_hits - st0.recycle_hits, st1.grow_waits - st0.grow_waits,
        st1.block_pads - st0.block_pads, total_reorder);
    /* dist=pod:count,... — which backend served each reply. One entry => the conn is
     * pinned (no codec); several => the service load-balances per message. */
    n += snprintf(reply + n, sizeof reply - (size_t)n, " dist=");
    int first = 1;
    for (int b = 0; b < BENCH_MAX_BACKENDS && n < (int)sizeof reply - 24; b++) {
        if (!dist[b]) continue;
        n += snprintf(reply + n, sizeof reply - (size_t)n, "%s%d:%ld",
                      first ? "" : ",", b, dist[b]);
        first = 0;
    }
    if (first) n += snprintf(reply + n, sizeof reply - (size_t)n, "-");
    n += snprintf(reply + n, sizeof reply - (size_t)n, "\n");
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
        int req = 32, rep = 8, conc = 1, threads = 1; double dur = 10.0; long warm = 1000, reconn = 0;
        /* RUN <req_size> <reply_size> <concurrency> <duration> <warmup> <threads> [reconn] */
        sscanf(buf, "%*s %d %d %d %lf %ld %d %ld", &req, &rep, &conc, &dur, &warm, &threads, &reconn);
        run_bench(fd, req, rep, conc, dur, warm, threads, reconn);
        close(fd); return;
    }
    if (write(fd, "ERR use: RUN <req> <reply> <conc> <dur> <warmup> <threads> [reconn] | PING\n", 76) < 0) {}
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
    if (getenv("BENCH_DST_SERVICE")) g_dst_service = getenv("BENCH_DST_SERVICE");

    g_s = dmesh_create_channel();                     /* pure client ($DPUMESH_SERVICE unset) */
    if (!g_s) { fprintf(stderr, "[bench] dmesh_create_channel failed\n"); return 1; }
    g_post_max = (uint32_t)dmesh_post_max(g_s);
    fprintf(stderr, "[bench] ready: pod_id=%d dst_service=%s slot=%d\n",
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
