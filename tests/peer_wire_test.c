/* One suite, both carriers.
 *
 * `peer_wire_ops` is the whole contract the layers above were written against,
 * so the assertions here are written against the contract and run twice: once
 * on the TCP carrier, which is always available, and once on the RDMA carrier
 * when this machine has a device an address resolves to. A machine without one
 * reports that arm as skipped rather than failing — the contract it would have
 * checked is the same one the TCP arm just checked. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../doca/peer_wire.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define INBOX_MAX   16
#define PUMP_MS     8000

struct side {
    const struct peer_wire_ops *ops;
    void    *ctx;
    uint16_t port;
    void    *inbox[INBOX_MAX];
    int      inbox_n;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void breathe(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000 };
    nanosleep(&ts, NULL);
}

static void pump_side(struct side *s)
{
    void *acc[INBOX_MAX];
    int n = 0;
    s->ops->progress(s->ctx, acc, INBOX_MAX, &n);
    for (int i = 0; i < n; i++) {
        assert(s->inbox_n < INBOX_MAX);
        s->inbox[s->inbox_n++] = acc[i];
    }
}

static void pump(struct side *a, struct side *b)
{
    pump_side(a);
    pump_side(b);
}

static void *inbox_pop(struct side *s)
{
    if (s->inbox_n == 0)
        return NULL;
    void *c = s->inbox[0];
    for (int i = 1; i < s->inbox_n; i++)
        s->inbox[i - 1] = s->inbox[i];
    s->inbox_n--;
    return c;
}

/* ---- payloads ----------------------------------------------------------- */

static void fill(uint8_t *buf, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 8));
}

static void expect_payload(const uint8_t *buf, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; i++)
        assert(buf[i] == (uint8_t)(seed * 31u + i * 7u + (i >> 8)));
}

/* ---- connection setup --------------------------------------------------- */

static void connect_pair(struct side *a, struct side *b, uint32_t ip_be,
                         void **out_a, void **out_b, const char *what)
{
    void *ca = NULL;
    assert(a->ops->connect(a->ctx, ip_be, b->port, &ca) == 0);
    assert(ca != NULL);

    void *cb = NULL;
    uint64_t deadline = now_ms() + PUMP_MS;
    while (now_ms() < deadline) {
        pump(a, b);
        if (!cb)
            cb = inbox_pop(b);
        assert(!a->ops->faulted(ca));
        if (cb && a->ops->established(ca) && b->ops->established(cb))
            break;
        breathe();
    }
    if (!cb || !a->ops->established(ca) || !b->ops->established(cb)) {
        fprintf(stderr, "%s: connection never established\n", what);
        abort();
    }
    *out_a = ca;
    *out_b = cb;
}

/* Move one message and wait for it, pumping both sides. */
static void send_one(struct side *from, void *cf, const void *buf, size_t len,
                     const char *what)
{
    uint64_t deadline = now_ms() + PUMP_MS;
    for (;;) {
        int rc = from->ops->send_msg(cf, buf, len);
        assert(rc >= 0);
        if (rc == 1)
            return;
        if (now_ms() > deadline) {
            fprintf(stderr, "%s: send never accepted\n", what);
            abort();
        }
        breathe();
    }
}

static size_t recv_one(struct side *a, struct side *b, void *cr, uint8_t *buf,
                       size_t cap, const char *what)
{
    uint64_t deadline = now_ms() + PUMP_MS;
    for (;;) {
        pump(a, b);
        long n = a->ops->recv_msg(cr, buf, cap);
        assert(n >= 0);
        if (n > 0)
            return (size_t)n;
        if (now_ms() > deadline) {
            fprintf(stderr, "%s: message never arrived\n", what);
            abort();
        }
        breathe();
    }
}

/* ---- the suite ---------------------------------------------------------- */

/* A message each way, which is the whole of what the layer above needs on a
 * fresh connection: its handshake is a message the initiator sends and a
 * message the responder answers with. */
static void t_roundtrip(struct side *a, struct side *b, uint32_t ip_be)
{
    void *ca = NULL, *cb = NULL;
    connect_pair(a, b, ip_be, &ca, &cb, "roundtrip");

    uint8_t out[256], in[512];
    fill(out, sizeof(out), 1);
    send_one(a, ca, out, sizeof(out), "roundtrip a->b");
    assert(recv_one(b, a, cb, in, sizeof(in), "roundtrip a->b") == sizeof(out));
    expect_payload(in, sizeof(out), 1);

    fill(out, 100, 2);
    send_one(b, cb, out, 100, "roundtrip b->a");
    assert(recv_one(a, b, ca, in, sizeof(in), "roundtrip b->a") == 100);
    expect_payload(in, 100, 2);

    a->ops->close(ca);
    b->ops->close(cb);
}

/* The largest message the contract admits, byte for byte. This is the bound
 * the layer above sizes its own buffers against, so a carrier that quietly
 * truncated here would corrupt a stream rather than fault it. */
static void t_max_message(struct side *a, struct side *b, uint32_t ip_be)
{
    void *ca = NULL, *cb = NULL;
    connect_pair(a, b, ip_be, &ca, &cb, "max message");

    uint8_t *out = malloc(PEER_WIRE_MSG_MAX);
    uint8_t *in = malloc(PEER_WIRE_MSG_MAX);
    assert(out && in);
    fill(out, PEER_WIRE_MSG_MAX, 7);
    send_one(a, ca, out, PEER_WIRE_MSG_MAX, "max message");
    size_t n = recv_one(b, a, cb, in, PEER_WIRE_MSG_MAX, "max message");
    assert(n == PEER_WIRE_MSG_MAX);
    assert(memcmp(in, out, PEER_WIRE_MSG_MAX) == 0);

    free(out);
    free(in);
    a->ops->close(ca);
    b->ops->close(cb);
}

/* Enough messages to wrap every ring in the carrier several times, at sizes
 * that keep the boundaries interesting. Order is the property under test: the
 * layer above encrypts in sequence and cannot survive a reordering. */
#define BURST_N 300

static void t_ordered_burst(struct side *a, struct side *b, uint32_t ip_be)
{
    void *ca = NULL, *cb = NULL;
    connect_pair(a, b, ip_be, &ca, &cb, "ordered burst");

    uint8_t *out = malloc(PEER_WIRE_MSG_MAX);
    uint8_t *in = malloc(PEER_WIRE_MSG_MAX);
    assert(out && in);

    int sent = 0, got = 0;
    uint64_t deadline = now_ms() + PUMP_MS;
    while (got < BURST_N) {
        if (sent < BURST_N) {
            size_t len = 64u + (size_t)(sent % 17) * 3701u;
            fill(out, len, (unsigned)sent);
            int rc = a->ops->send_msg(ca, out, len);
            assert(rc >= 0);
            if (rc == 1)
                sent++;
        }
        pump(a, b);
        for (;;) {
            long n = b->ops->recv_msg(cb, in, PEER_WIRE_MSG_MAX);
            assert(n >= 0);
            if (n == 0)
                break;
            size_t want = 64u + (size_t)(got % 17) * 3701u;
            assert((size_t)n == want);
            expect_payload(in, want, (unsigned)got);
            got++;
            deadline = now_ms() + PUMP_MS;
        }
        if (now_ms() > deadline) {
            fprintf(stderr, "ordered burst: stalled at %d sent / %d received\n",
                    sent, got);
            abort();
        }
    }

    free(out);
    free(in);
    a->ops->close(ca);
    b->ops->close(cb);
}

/* A receiver that never drains. Whatever the carrier's depth, refusal is all
 * or nothing and refusal is not loss: once the receiver starts reading, every
 * message the sender was told it had taken is there, once, in order. */
static void t_backpressure(struct side *a, struct side *b, uint32_t ip_be)
{
    void *ca = NULL, *cb = NULL;
    connect_pair(a, b, ip_be, &ca, &cb, "backpressure");

    uint8_t *out = malloc(PEER_WIRE_MSG_MAX);
    uint8_t *in = malloc(PEER_WIRE_MSG_MAX);
    assert(out && in);

    int accepted = 0;
    const int cap = 64;
    while (accepted < cap) {
        fill(out, 4096, (unsigned)accepted);
        int rc = a->ops->send_msg(ca, out, 4096);
        assert(rc >= 0);
        if (rc == 0)
            break;                   /* the carrier is full; nothing was lost */
        accepted++;
        pump_side(a);                /* completions, but b never reads */
    }
    assert(accepted > 0);

    int got = 0;
    uint64_t deadline = now_ms() + PUMP_MS;
    while (got < accepted) {
        pump(a, b);
        long n = b->ops->recv_msg(cb, in, PEER_WIRE_MSG_MAX);
        assert(n >= 0);
        if (n > 0) {
            assert(n == 4096);
            expect_payload(in, 4096, (unsigned)got);
            got++;
            deadline = now_ms() + PUMP_MS;
            continue;
        }
        if (now_ms() > deadline) {
            fprintf(stderr, "backpressure: %d of %d came back\n", got, accepted);
            abort();
        }
        breathe();
    }
    /* And nothing beyond what was accepted. */
    pump(a, b);
    assert(b->ops->recv_msg(cb, in, PEER_WIRE_MSG_MAX) == 0);

    free(out);
    free(in);
    a->ops->close(ca);
    b->ops->close(cb);
}

/* A closed connection has to reach the other side, because the layer above
 * only reclaims a channel when its carrier reports the fault. */
static void t_close_faults_peer(struct side *a, struct side *b, uint32_t ip_be)
{
    void *ca = NULL, *cb = NULL;
    connect_pair(a, b, ip_be, &ca, &cb, "close");

    uint8_t out[64];
    fill(out, sizeof(out), 3);
    send_one(a, ca, out, sizeof(out), "close");
    uint8_t in[128];
    assert(recv_one(b, a, cb, in, sizeof(in), "close") == sizeof(out));

    a->ops->close(ca);
    uint64_t deadline = now_ms() + PUMP_MS;
    while (now_ms() < deadline) {
        pump_side(b);
        if (b->ops->faulted(cb) || b->ops->recv_msg(cb, in, sizeof(in)) < 0)
            break;
        breathe();
    }
    assert(b->ops->faulted(cb));
    b->ops->close(cb);
}

/* Connections come and go, and their slots come back. A carrier that shares
 * one completion queue has work in flight belonging to a connection already
 * torn down, so the slot a later peer is given must not answer for it. */
static void t_slot_reuse(struct side *a, struct side *b, uint32_t ip_be)
{
    uint8_t out[8192], in[8192];
    for (int round = 0; round < 12; round++) {
        void *ca = NULL, *cb = NULL;
        connect_pair(a, b, ip_be, &ca, &cb, "slot reuse");
        fill(out, sizeof(out), (unsigned)round);
        /* Leave sends outstanding on purpose, then tear the pair down. */
        int posted = 0;
        for (int i = 0; i < 4; i++) {
            int rc = a->ops->send_msg(ca, out, sizeof(out));
            assert(rc >= 0);
            if (rc == 0)
                break;
            posted++;
        }
        assert(posted > 0);
        assert(recv_one(b, a, cb, in, sizeof(in), "slot reuse") == sizeof(out));
        expect_payload(in, sizeof(out), (unsigned)round);
        a->ops->close(ca);
        b->ops->close(cb);
        pump(a, b);
    }
}

static void run_suite(const char *name, struct side *a, struct side *b,
                      uint32_t ip_be)
{
    printf("  %s: roundtrip", name);
    fflush(stdout);
    t_roundtrip(a, b, ip_be);
    printf(" max"); fflush(stdout);
    t_max_message(a, b, ip_be);
    printf(" burst"); fflush(stdout);
    t_ordered_burst(a, b, ip_be);
    printf(" backpressure"); fflush(stdout);
    t_backpressure(a, b, ip_be);
    printf(" close"); fflush(stdout);
    t_close_faults_peer(a, b, ip_be);
    printf(" reuse"); fflush(stdout);
    t_slot_reuse(a, b, ip_be);
    printf(" ok\n");
}

/* ---- carriers ----------------------------------------------------------- */

static void run_tcp(void)
{
    struct side a = { 0 }, b = { 0 };
    char err[256];
    uint32_t lo = htonl(INADDR_LOOPBACK);
    assert(peer_wire_tcp_new(lo, 0, &a.ops, &a.ctx, err, sizeof(err)) == 0);
    assert(peer_wire_tcp_new(lo, 0, &b.ops, &b.ctx, err, sizeof(err)) == 0);
    a.port = peer_wire_tcp_port(a.ctx);
    b.port = peer_wire_tcp_port(b.ctx);
    assert(a.port && b.port);
    run_suite("tcp", &a, &b, lo);
    a.ops->ctx_free(a.ctx);
    b.ops->ctx_free(b.ctx);
}

/* The first local address that resolves to an rdma device, found by asking the
 * carrier to bind to each in turn — the same question it would be asked in
 * production, so an address that passes here is one it can actually run on. */
static int find_rdma_addr(uint32_t *ip_be, const struct peer_wire_ops **ops,
                          void **ctx, char *why, size_t why_len)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) {
        snprintf(why, why_len, "getifaddrs: %s", strerror(errno));
        return -1;
    }
    snprintf(why, why_len, "no local address resolves to an rdma device");
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        uint32_t ip = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        if (ip == INADDR_ANY)
            continue;
        char err[256];
        if (peer_wire_rdma_new(ip, 0, ops, ctx, err, sizeof(err)) == 0) {
            *ip_be = ip;
            freeifaddrs(list);
            return 0;
        }
    }
    freeifaddrs(list);
    return -1;
}

static void run_rdma(void)
{
    struct side a = { 0 }, b = { 0 };
    uint32_t ip = 0;
    char why[256];
    if (find_rdma_addr(&ip, &a.ops, &a.ctx, why, sizeof(why)) != 0) {
        printf("  rdma: skipped (%s)\n", why);
        return;
    }
    char err[256];
    if (peer_wire_rdma_new(ip, 0, &b.ops, &b.ctx, err, sizeof(err)) != 0) {
        printf("  rdma: skipped (second endpoint: %s)\n", err);
        a.ops->ctx_free(a.ctx);
        return;
    }
    a.port = peer_wire_rdma_port(a.ctx);
    b.port = peer_wire_rdma_port(b.ctx);
    assert(a.port && b.port);
    char ipstr[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr));
    printf("  rdma: on %s\n", ipstr);
    run_suite("rdma", &a, &b, ip);
    a.ops->ctx_free(a.ctx);
    b.ops->ctx_free(b.ctx);
}

int main(void)
{
    printf("peer_wire_test\n");
    run_tcp();
    run_rdma();
    printf("peer_wire_test: PASS\n");
    return 0;
}
