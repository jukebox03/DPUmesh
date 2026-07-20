/* Blocking Greeter server for direct TCP, Envoy, and preload transports.
 * Each connection has its own thread and reframer. Replies contain the byte count
 * requested in the frame's aux field. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "bench.h"

#define ECHO_PORT_DEFAULT 9100
#define RD_CHUNK          (64 * 1024)
#define OUT_BUF           (128 * 1024)
#define REPLY_FILL        43

static long g_app_work_us = 0;   /* per-request busy-spin (µs) — models the greeter doing real
                                  * work while sharing its core with Envoy. Start from
                                  * APP_WORK_US, then live-tunable: write /tmp/app_work_us +
                                  * SIGHUP, so a busy-app sweep needs no pod restart. */
static volatile sig_atomic_t g_reload = 0;
static void on_hup(int s) { (void)s; g_reload = 1; }
static void load_work(void) {
    FILE *f = fopen("/tmp/app_work_us", "r");
    if (f) { long v; if (fscanf(f, "%ld", &v) == 1) g_app_work_us = v; fclose(f); }
    else { const char *e = getenv("APP_WORK_US"); if (e) g_app_work_us = atol(e); }
}

/* Per-connection reply state, threaded through the reframer callback. Replies are
 * staged into `out` and flushed once per read batch, so a burst of small requests
 * costs one write() rather than one per frame. */
typedef struct {
    int      fd;
    uint8_t *out;      /* staging buffer, OUT_BUF bytes */
    size_t   out_len;
    int      err;      /* a short/failed write kills the conn */
} conn_t;

static int write_all(int fd, const uint8_t *b, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

static void out_flush(conn_t *c) {
    if (c->out_len == 0 || c->err) return;
    if (write_all(c->fd, c->out, c->out_len) < 0) c->err = 1;
    c->out_len = 0;
}

/* Append `n` bytes to the staging buffer, flushing first if they would not fit.
 * A single reply larger than the buffer is written straight through. */
static void out_put(conn_t *c, const uint8_t *b, size_t n) {
    if (c->err) return;
    if (n > OUT_BUF) { out_flush(c); if (write_all(c->fd, b, n) < 0) c->err = 1; return; }
    if (c->out_len + n > OUT_BUF) out_flush(c);
    memcpy(c->out + c->out_len, b, n);
    c->out_len += n;
}

/* Shared read-only reply filler initialized once across connection threads. */
static uint8_t g_reply_fill[RD_CHUNK];
static pthread_once_t g_reply_fill_once = PTHREAD_ONCE_INIT;
static void init_reply_fill(void) { memset(g_reply_fill, REPLY_FILL, sizeof g_reply_fill); }

/* Fires once per WHOLE request frame. Reply frame: [magic|seq|reply_size|0] +
 * reply_size filler bytes. `aux` is the reply_size the client asked for. */
static void on_request(uint32_t seq, uint32_t plen, uint32_t aux, void *user) {
    (void)plen;
    conn_t *c = (conn_t *)user;
    if (g_reload) { g_reload = 0; load_work(); }
    if (g_app_work_us > 0) {                            /* simulate app-work on the host core */
        double t0 = bench_now_sec();
        while ((bench_now_sec() - t0) * 1e6 < (double)g_app_work_us) { }
    }
    uint8_t hdr[BENCH_HDR_LEN];
    bench_put_hdr(hdr, BENCH_REP_MAGIC, seq, aux, 0);
    out_put(c, hdr, BENCH_HDR_LEN);

    pthread_once(&g_reply_fill_once, init_reply_fill);
    uint32_t left = aux;
    while (left > 0) {
        uint32_t n = left < RD_CHUNK ? left : RD_CHUNK;
        out_put(c, g_reply_fill, n);
        left -= n;
    }
}

static void *conn_fn(void *arg) {
    int fd = (int)(intptr_t)arg;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    conn_t c = { .fd = fd, .out = (uint8_t *)malloc(OUT_BUF), .out_len = 0, .err = 0 };
    bench_reframer_t rf; bench_reframe_reset(&rf);
    uint8_t *rd = (uint8_t *)malloc(RD_CHUNK);
    if (!c.out || !rd) { free(c.out); free(rd); close(fd); return NULL; }

    for (;;) {
        { static double lr = 0; double now = bench_now_sec();   /* live app-work: poll the file */
          if (now - lr > 0.5) { lr = now; load_work(); } }
        ssize_t n = read(fd, rd, RD_CHUNK);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        /* want_magic = REQ: a client that desyncs its stream is a hard error, not
         * something to scan past — same rule as the reframer's client side. */
        if (bench_reframe_feed(&rf, rd, (size_t)n, BENCH_REQ_MAGIC, on_request, &c) < 0) {
            fprintf(stderr, "[echo_sock] request desync — closing conn\n");
            break;
        }
        out_flush(&c);                       /* one write() per read batch */
        if (c.err) break;
    }
    free(c.out); free(rd); close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    int port = ECHO_PORT_DEFAULT;
    if (getenv("ECHO_PORT")) port = atoi(getenv("ECHO_PORT"));
    signal(SIGHUP, on_hup); load_work();             /* app-work: env default, live via SIGHUP */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[echo_sock] SayHello LISTEN on :%d\n", port);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        pthread_t t;
        if (pthread_create(&t, NULL, conn_fn, (void *)(intptr_t)c) != 0) { close(c); continue; }
        pthread_detach(t);
    }
}
