/* Transport-free arrival scheduler shared by the native and POSIX clients. */
#ifndef BENCH_SELFTEST_H
#define BENCH_SELFTEST_H

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "bench.h"

typedef struct {
    double rate;
    double duration;
    double start_at;
    double elapsed;
    int payload;
    uint64_t prng;
    uint64_t scheduled;
    uint64_t drops;
    uint64_t checksum;
    int arrival;
    int failed;
} bench_selftest_worker_t;

static void bench_selftest_build_frame(bench_selftest_worker_t *worker,
                                       uint8_t *frame) {
    bench_put_hdr(frame, BENCH_REQ_MAGIC, (uint32_t)worker->scheduled,
                  (uint32_t)worker->payload, (uint32_t)worker->payload);
    memset(frame + BENCH_HDR_LEN, 42, (size_t)worker->payload);
    __asm__ __volatile__("" : : "r"(frame) : "memory");
    size_t frame_len = BENCH_HDR_LEN + (size_t)worker->payload;
    worker->checksum ^= frame[worker->scheduled % frame_len];
}

static double bench_selftest_gap(bench_selftest_worker_t *worker) {
    if (worker->arrival == 0)
        return 1.0 / worker->rate;
    worker->prng ^= worker->prng << 13;
    worker->prng ^= worker->prng >> 7;
    worker->prng ^= worker->prng << 17;
    double u = ((double)(worker->prng >> 11) + 1.0) / 9007199254740993.0;
    return -log(u) / worker->rate;
}

static int bench_epoll_wait_until(int epoll_fd, struct epoll_event *event,
                                  double deadline) {
    double left = deadline - bench_now_sec();
    if (left < 0.0) left = 0.0;
    if (left > 0.020) left = 0.020;
    struct timespec timeout = {
        .tv_sec = (time_t)left,
        .tv_nsec = (long)((left - (double)(time_t)left) * 1e9),
    };
    int rc = epoll_pwait2(epoll_fd, event, 1, &timeout, NULL);
    if (rc < 0 && errno == ENOSYS) {
        int timeout_ms = (int)ceil(left * 1e3);
        if (left > 0.0 && timeout_ms < 1) timeout_ms = 1;
        rc = epoll_wait(epoll_fd, event, 1, timeout_ms);
    }
    return rc;
}

static void *bench_selftest_worker(void *arg) {
    bench_selftest_worker_t *worker = (bench_selftest_worker_t *)arg;
    uint8_t *frame = (uint8_t *)malloc(BENCH_HDR_LEN + (size_t)worker->payload);
    if (!frame) {
        worker->failed = 1;
        return NULL;
    }
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        worker->failed = 1;
        free(frame);
        return NULL;
    }

    while (bench_now_sec() < worker->start_at) {
        struct timespec delay = {0, 50000};
        nanosleep(&delay, NULL);
    }
    double start = bench_now_sec();
    double stop_at = start + worker->duration;
    double next = start;

    for (;;) {
        double now = bench_now_sec();
        if (now > stop_at)
            break;
        while (now >= next) {
            bench_selftest_build_frame(worker, frame);
            worker->scheduled++;
            next += bench_selftest_gap(worker);
        }
        struct epoll_event event;
        if (bench_epoll_wait_until(epoll_fd, &event, next) < 0 &&
            errno != EINTR) {
            worker->failed = 1;
            break;
        }
    }
    worker->elapsed = bench_now_sec() - start;

    /* Count timestamps not consumed within the requested window. */
    if (worker->arrival == 0) {
        uint64_t target =
            (uint64_t)ceil(worker->rate * worker->duration - 1e-9);
        if (target > worker->scheduled)
            worker->drops = target - worker->scheduled;
    } else {
        while (next < stop_at) {
            worker->drops++;
            next += bench_selftest_gap(worker);
        }
    }
    close(epoll_fd);
    free(frame);
    return NULL;
}

static int bench_run_selftest(char *reply, size_t reply_size, int payload,
                              int threads, double duration, double rate,
                              int arrival) {
    if (!reply || reply_size == 0 || payload < 0 || threads < 1 ||
        payload > INT32_MAX - (int)BENCH_HDR_LEN ||
        !isfinite(duration) || !isfinite(rate) ||
        duration <= 0.0 || rate <= 0.0 || (arrival != 0 && arrival != 1)) {
        return -1;
    }

    bench_selftest_worker_t *workers =
        (bench_selftest_worker_t *)calloc((size_t)threads, sizeof(*workers));
    pthread_t *tids = (pthread_t *)calloc((size_t)threads, sizeof(*tids));
    if (!workers || !tids) {
        free(workers);
        free(tids);
        return -1;
    }

    double start_at = bench_now_sec() + 0.05;
    int created = 0;
    for (int i = 0; i < threads; i++) {
        workers[i].rate = rate / (double)threads;
        workers[i].duration = duration;
        workers[i].start_at = start_at;
        workers[i].payload = payload;
        workers[i].arrival = arrival;
        workers[i].prng = 0x9e3779b97f4a7c15ULL ^
                          ((uint64_t)(i + 1) * 0x100000001b3ULL);
        if (pthread_create(&tids[i], NULL, bench_selftest_worker, &workers[i]) != 0) {
            workers[i].failed = 1;
            continue;
        }
        created++;
    }

    uint64_t scheduled = 0, drops = 0, checksum = 0;
    double elapsed = 0.0;
    int failed = created != threads;
    for (int i = 0; i < threads; i++) {
        if (tids[i])
            pthread_join(tids[i], NULL);
        scheduled += workers[i].scheduled;
        drops += workers[i].drops;
        checksum ^= workers[i].checksum;
        if (workers[i].elapsed > elapsed)
            elapsed = workers[i].elapsed;
        failed |= workers[i].failed;
    }

    double expected = rate * duration;
    double schedule_ratio = expected > 0.0 ? (double)scheduled / expected : 0.0;
    double drop_ratio = scheduled + drops
                      ? (double)drops / (double)(scheduled + drops) : 0.0;
    snprintf(
        reply, reply_size,
        "%s selftest=1 frame=%u payload=%d threads=%d durs=%.6f elapsed=%.6f "
        "offered_rps=%.3f scheduled=%llu drops=%llu schedule_ratio=%.6f "
        "drop_ratio=%.6f checksum=%llu arr=%s\n",
        failed ? "ERR" : "OK", BENCH_HDR_LEN + (uint32_t)payload,
        payload, threads, duration, elapsed, rate,
        (unsigned long long)scheduled, (unsigned long long)drops,
        schedule_ratio, drop_ratio, (unsigned long long)checksum,
        arrival ? "poisson" : "const"
    );
    free(workers);
    free(tids);
    return failed ? -1 : 0;
}

#endif
