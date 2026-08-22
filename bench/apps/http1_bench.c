/* Closed-loop HTTP/1.1 client, answering the same control protocol the other
 * bench clients answer, so `bench.sh point http1` needs no harness of its own.
 *
 * This is a functional driver rather than a capacity instrument: it exists so
 * that the protocol-aware path's HTTP/1 branch has traffic to decide about.
 * Its latency figures are reported for shape, and no capacity is quoted from
 * them — the campaign that quotes capacity is the Greeter one.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "bench.h"
#include "bench_result.h"

#define CTRL_PORT_DEFAULT 9092
#define MAX_THREADS 64
#define MAX_CONC    64
#define HEADER_MAX  8192
/* The reply size travels as a request header, so one server serves every point. */
#define REPLY_HEADER_NAME "X-Reply-Size:"

static char g_host[256] = "127.0.0.1";
static int  g_port = 9103;

static void parse_target(void) {
    const char *t = getenv("BENCH_TARGET");
    if (!t) return;
    const char *colon = strrchr(t, ':');
    if (!colon) { snprintf(g_host, sizeof g_host, "%s", t); return; }
    size_t host_len = (size_t)(colon - t);
    if (host_len >= sizeof g_host) host_len = sizeof g_host - 1;
    memcpy(g_host, t, host_len); g_host[host_len] = '\0';
    g_port = atoi(colon + 1);
}

static int dial(void) {
    struct addrinfo hints, *res = NULL;
    char service[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%d", g_port);
    if (getaddrinfo(g_host, service, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) { close(fd); fd = -1; }
    if (fd >= 0) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    freeaddrinfo(res);
    return fd;
}

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* One connection's in-flight exchange: the request is written whole and the
 * response is read to the end of its declared body before the next is sent. */
struct conn {
    int fd;
    char *request;
    size_t request_len;
    char buf[HEADER_MAX];
    size_t held;
    uint8_t *sink;
    size_t sink_cap;
};

static long header_value(const char *headers, const char *name, long fallback) {
    size_t name_len = strlen(name);
    for (const char *line = headers; line && *line; ) {
        if (strncasecmp(line, name, name_len) == 0)
            return strtol(line + name_len, NULL, 10);
        const char *next = strstr(line, "\r\n");
        line = next ? next + 2 : NULL;
    }
    return fallback;
}

/* 0 on a complete 200 response, -1 on any transport or protocol failure. */
static int exchange(struct conn *c) {
    if (write_all(c->fd, c->request, c->request_len) < 0) return -1;
    char *end = NULL;
    while (!(end = memmem(c->buf, c->held, "\r\n\r\n", 4))) {
        if (c->held == sizeof c->buf - 1) return -1;
        ssize_t n = read(c->fd, c->buf + c->held, sizeof c->buf - 1 - c->held);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        c->held += (size_t)n;
    }
    size_t header_len = (size_t)(end - c->buf) + 4;
    char saved = c->buf[header_len - 1];
    c->buf[header_len - 1] = '\0';
    if (strncmp(c->buf, "HTTP/1.1 200", 12) != 0) return -1;
    long content_length = header_value(c->buf, "Content-Length:", -1);
    c->buf[header_len - 1] = saved;
    if (content_length < 0) return -1;

    if ((size_t)content_length > c->sink_cap) {
        uint8_t *grown = realloc(c->sink, (size_t)content_length + 1);
        if (!grown) return -1;
        c->sink = grown; c->sink_cap = (size_t)content_length + 1;
    }
    size_t carried = c->held - header_len;
    size_t take = carried < (size_t)content_length ? carried : (size_t)content_length;
    memmove(c->buf, c->buf + header_len + take, carried - take);
    c->held = carried - take;
    for (size_t got = take; got < (size_t)content_length; ) {
        ssize_t n = read(c->fd, c->sink, (size_t)content_length - got);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    return 0;
}

struct worker {
    int conc, req_size, reply_size;
    double duration;
    long warmup;
    long ok, fail;
    int failed;
    bench_hist_t hist;
};

static void *worker_fn(void *arg) {
    struct worker *w = arg;
    struct conn conns[MAX_CONC];
    memset(conns, 0, sizeof conns);
    char *body = calloc(1, (size_t)w->req_size ? (size_t)w->req_size : 1);
    if (!body) { w->failed = 1; return NULL; }

    for (int i = 0; i < w->conc; i++) {
        conns[i].fd = dial();
        if (conns[i].fd < 0) { w->failed = 1; goto out; }
        size_t cap = 512 + (size_t)w->req_size;
        conns[i].request = malloc(cap);
        if (!conns[i].request) { w->failed = 1; goto out; }
        int head = snprintf(conns[i].request, 512,
                            "POST /echo HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Content-Type: application/octet-stream\r\n"
                            "%s %d\r\n"
                            "Content-Length: %d\r\n\r\n",
                            g_host, REPLY_HEADER_NAME, w->reply_size, w->req_size);
        memcpy(conns[i].request + head, body, (size_t)w->req_size);
        conns[i].request_len = (size_t)head + (size_t)w->req_size;
    }

    for (long i = 0; i < w->warmup; i++)
        if (exchange(&conns[i % w->conc]) < 0) { w->failed = 1; goto out; }

    double deadline = bench_now_sec() + w->duration;
    while (bench_now_sec() < deadline) {
        for (int i = 0; i < w->conc; i++) {
            double t0 = bench_now_sec();
            if (exchange(&conns[i]) < 0) { w->fail++; w->failed = 1; goto out; }
            bench_hist_record(&w->hist, (bench_now_sec() - t0) * 1e6);
            w->ok++;
        }
    }
out:
    for (int i = 0; i < w->conc; i++) {
        if (conns[i].fd >= 0) close(conns[i].fd);
        free(conns[i].request); free(conns[i].sink);
    }
    free(body);
    return NULL;
}

static void run_bench(int conn_fd, int req_size, int reply_size, int conc,
                      double duration, long warmup, int threads) {
    char reply[768];
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;
    if (conc < 1) conc = 1;
    if (conc > MAX_CONC) conc = MAX_CONC;

    struct worker *w = calloc((size_t)threads, sizeof *w);
    pthread_t *tid = calloc((size_t)threads, sizeof *tid);
    if (!w || !tid) { free(w); free(tid); return; }

    for (int i = 0; i < threads; i++) {
        w[i] = (struct worker){ .conc = conc, .req_size = req_size,
                                .reply_size = reply_size, .duration = duration,
                                .warmup = warmup / threads };
        bench_hist_init(&w[i].hist);
    }
    double started = bench_now_sec();
    for (int i = 0; i < threads; i++)
        if (pthread_create(&tid[i], NULL, worker_fn, &w[i]) != 0) w[i].failed = 1;
    for (int i = 0; i < threads; i++) pthread_join(tid[i], NULL);
    double elapsed = bench_now_sec() - started;

    bench_hist_t agg; bench_hist_init(&agg);
    long total_ok = 0, total_fail = 0; int worker_fail = 0;
    for (int i = 0; i < threads; i++) {
        total_ok += w[i].ok; total_fail += w[i].fail;
        worker_fail += w[i].failed ? 1 : 0;
        bench_hist_merge(&agg, &w[i].hist);
        bench_hist_free(&w[i].hist);
    }
    double mrps = elapsed > 0 ? (double)total_ok / elapsed * 1e-6 : 0.0;
    int n = snprintf(reply, sizeof reply,
        "%s mrps=%.6f gbps=0.0000 req_gbps=0.0000 resp_gbps=0.0000 "
        "p50=%.2f p95=%.2f p99=%.2f p999=%.2f p9999=%.2f "
        "avg=%.2f min=%.2f max=%.2f rcnt=%ld scheduled=%ld pending=0 fail=%ld "
        "conc=%d threads=%d reqsz=%d repsz=%d durs=%.3f worker_fail=%d "
        "mode=closed arr=const proto=http1\n",
        bench_result_status(total_ok, total_fail, worker_fail),
        mrps,
        bench_hist_pct(&agg, 50.0), bench_hist_pct(&agg, 95.0),
        bench_hist_pct(&agg, 99.0), bench_hist_pct(&agg, 99.9),
        bench_hist_pct(&agg, 99.99), bench_hist_avg(&agg),
        bench_hist_min(&agg), bench_hist_max(&agg),
        total_ok, total_ok + total_fail, total_fail,
        conc, threads, req_size, reply_size, elapsed, worker_fail);
    bench_hist_free(&agg);
    if (write(conn_fd, reply, (size_t)n) < 0) {}
    fprintf(stderr, "[http1_bench] DONE %s", reply);
    free(w); free(tid);
}

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
        int req = 1024, rep = 8, conc = 1, threads = 1;
        double dur = 10.0; long warm = 100;
        sscanf(buf, "%*s %d %d %d %lf %ld %d", &req, &rep, &conc, &dur, &warm, &threads);
        run_bench(fd, req, rep, conc, dur, warm, threads);
    } else {
        const char *msg = "ERR unknown command\n";
        if (write(fd, msg, strlen(msg)) < 0) {}
    }
    close(fd);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    parse_target();
    int ctrl_port = getenv("CTRL_PORT") ? atoi(getenv("CTRL_PORT")) : CTRL_PORT_DEFAULT;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)ctrl_port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[http1_bench] control LISTEN on :%d, target=%s:%d\n",
            ctrl_port, g_host, g_port);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        handle_ctrl(c);
    }
}
