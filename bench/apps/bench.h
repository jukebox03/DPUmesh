/* Shared Greeter frame helpers. A 16-byte little-endian header contains magic,
 * sequence, payload length, and auxiliary reply length, followed by payload. */
#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define BENCH_REQ_MAGIC 0x62526571u   /* "bReq" */
#define BENCH_REP_MAGIC 0x62526570u   /* "bRep" */
#define BENCH_HDR_LEN   16u

/* ------------------------------------------------------------ time helpers */
static inline double bench_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------ frame header */
static inline void bench_put_hdr(uint8_t *h, uint32_t magic, uint32_t seq,
                                 uint32_t payload_len, uint32_t aux) {
    memcpy(h + 0,  &magic,       4);
    memcpy(h + 4,  &seq,         4);
    memcpy(h + 8,  &payload_len, 4);
    memcpy(h + 12, &aux,         4);
}

/* Stream reframer. Feed arbitrary chunks from one stream; the callback receives
 * each complete frame's sequence, payload length, and auxiliary value. */
typedef void (*bench_frame_cb)(uint32_t seq, uint32_t payload_len, uint32_t aux, void *user);

typedef struct {
    uint8_t  hdr[BENCH_HDR_LEN];
    uint32_t hdr_got;      /* header bytes accumulated (0..16) */
    uint32_t skip;         /* payload bytes still to consume for the current frame */
    uint32_t seq, plen, aux;
    int      in_payload;   /* header parsed; consuming its payload */
} bench_reframer_t;

static inline void bench_reframe_reset(bench_reframer_t *rf) {
    memset(rf, 0, sizeof(*rf));
}

/* Returns 0, or -1 on DESYNC: the bytes where a frame must begin do not carry
 * `want_magic` (BENCH_REQ_MAGIC on a server, BENCH_REP_MAGIC on a client). Desync is
 * unrecoverable — the boundary is lost, so every later length is read out of whatever
 * happens to sit there and the stream never resyncs. Kill the conn; do not scan for a
 * new boundary. Without this check that failure is SILENT: the reframer keeps counting
 * garbage lengths, no frame ever completes, and the conn simply stops. */
static inline int bench_reframe_feed(bench_reframer_t *rf, const uint8_t *buf, size_t len,
                                     uint32_t want_magic, bench_frame_cb cb, void *user) {
    while (len > 0) {
        if (!rf->in_payload) {
            uint32_t need = BENCH_HDR_LEN - rf->hdr_got;
            uint32_t take = (len < need) ? (uint32_t)len : need;
            memcpy(rf->hdr + rf->hdr_got, buf, take);
            rf->hdr_got += take; buf += take; len -= take;
            if (rf->hdr_got == BENCH_HDR_LEN) {
                uint32_t magic;
                memcpy(&magic, rf->hdr, 4);
                if (magic != want_magic) return -1;
                memcpy(&rf->seq,  rf->hdr + 4, 4);
                memcpy(&rf->plen, rf->hdr + 8, 4);
                memcpy(&rf->aux,  rf->hdr + 12, 4);
                rf->hdr_got = 0;
                rf->skip = rf->plen;
                if (rf->plen == 0) {
                    cb(rf->seq, rf->plen, rf->aux, user);   /* empty-payload frame */
                } else {
                    rf->in_payload = 1;
                }
            }
        } else {
            uint32_t take = (len < rf->skip) ? (uint32_t)len : rf->skip;
            buf += take; len -= take; rf->skip -= take;
            if (rf->skip == 0) {
                rf->in_payload = 0;
                cb(rf->seq, rf->plen, rf->aux, user);       /* whole frame done */
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------ latency histogram
 * Fixed-memory histogram: 1 us buckets up to ~1.05 s, plus an overflow bucket and
 * exact min/max/sum. Per-thread instances are merged at the end; percentiles are
 * exact to 1 us over the covered range. */
#define BENCH_HIST_BUCKETS (1u << 20)     /* 0..1048575 us */

typedef struct {
    uint32_t *b;          /* count per 1-us bucket */
    uint64_t  overflow;   /* samples >= BENCH_HIST_BUCKETS us */
    uint64_t  count;
    double    sum_us;
    double    min_us, max_us;
} bench_hist_t;

static inline int bench_hist_init(bench_hist_t *h) {
    h->b = (uint32_t *)calloc(BENCH_HIST_BUCKETS, sizeof(uint32_t));
    h->overflow = h->count = 0;
    h->sum_us = 0.0; h->min_us = 1e30; h->max_us = 0.0;
    return h->b ? 0 : -1;
}
static inline void bench_hist_free(bench_hist_t *h) { free(h->b); h->b = NULL; }

static inline void bench_hist_record(bench_hist_t *h, double us) {
    if (us < 0) us = 0;
    uint32_t idx = (us >= (double)BENCH_HIST_BUCKETS) ? BENCH_HIST_BUCKETS
                                                      : (uint32_t)us;
    if (idx >= BENCH_HIST_BUCKETS) h->overflow++;
    else                           h->b[idx]++;
    h->count++;
    h->sum_us += us;
    if (us < h->min_us) h->min_us = us;
    if (us > h->max_us) h->max_us = us;
}

static inline void bench_hist_merge(bench_hist_t *dst, const bench_hist_t *src) {
    for (uint32_t i = 0; i < BENCH_HIST_BUCKETS; i++) dst->b[i] += src->b[i];
    dst->overflow += src->overflow;
    dst->count    += src->count;
    dst->sum_us   += src->sum_us;
    if (src->count) {
        if (src->min_us < dst->min_us) dst->min_us = src->min_us;
        if (src->max_us > dst->max_us) dst->max_us = src->max_us;
    }
}

/* p in [0,100]. Returns microseconds. */
static inline double bench_hist_pct(const bench_hist_t *h, double p) {
    if (h->count == 0) return 0.0;
    uint64_t rank = (uint64_t)(p / 100.0 * (double)(h->count - 1));
    uint64_t cum = 0;
    for (uint32_t i = 0; i < BENCH_HIST_BUCKETS; i++) {
        cum += h->b[i];
        if (cum > rank) return (double)i;
    }
    return h->max_us;   /* rank falls in the overflow tail */
}
static inline double bench_hist_avg(const bench_hist_t *h) {
    return h->count ? h->sum_us / (double)h->count : 0.0;
}
static inline double bench_hist_min(const bench_hist_t *h) { return h->count ? h->min_us : 0.0; }
static inline double bench_hist_max(const bench_hist_t *h) { return h->count ? h->max_us : 0.0; }

#endif /* BENCH_H */
