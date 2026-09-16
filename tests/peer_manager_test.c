/* Two nodes in one process: loopback TCP control sessions, the real pair
 * transport on fake verbs lanes wired to each other, and a fake hardware
 * adapter the test completes by hand. Registration, placement and policy are
 * fixture answers; nothing here claims hardware protection or Kubernetes
 * authorization. The clock is manual so leases expire on demand. */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doca/peer_manager.h"

#define WORKERS 2u
#define SLOTS 2u
#define NS 1000000000ull
#define STEP (5ull * 1000000ull)
#define INBOX 8u

static uint64_t g_now = NS;
static uint64_t now_cb(void *v) { (void)v; return g_now; }

struct node;
struct fconn {
    struct node *n; unsigned worker, slot; int used;
    uint8_t inbox[INBOX][PEER_WIRE_MSG_MAX]; size_t len[INBOX]; unsigned head, count;
};
struct fdriver { struct node *n; unsigned worker; struct fconn conn[SLOTS]; int freed; };
struct fhw {
    struct peer_crypto_request queue[64]; unsigned count;
    struct peer_crypto_completion done[64]; unsigned ndone;
    int backpressure, fail_action, fail_status;
    unsigned seen[16]; uint64_t last_epoch[16];
};
struct node {
    unsigned index; const char *name, *pod, *peer_name, *peer_pod;
    uint8_t seed[32], key[32], boot[16];
    const struct peer_wire_ops *tcp; void *tcp_ctx; uint16_t port;
    struct peer_manager *m;
    struct peer_pair_transport *rt[WORKERS];
    struct dmesh_peer_table table[WORKERS];
    struct fdriver driver[WORKERS];
    struct fhw hw;
    int registered, allow, paused;
    struct dmesh_peer_registration reg;
};
static struct node nodes[2];
static struct node *other(const struct node *n) { return &nodes[1 - n->index]; }

/* ---- fixture answers ---------------------------------------------------- */
static int binding(void *v, const char *name, const uint8_t **key, uint32_t *ip, uint16_t *port)
{
    struct node *n = v;
    if (strcmp(name, n->peer_name)) return 0;
    *key = other(n)->key; *ip = htonl(INADDR_LOOPBACK); *port = other(n)->port; return 1;
}
static int by_key(void *v, const uint8_t key[32], char name[DMESH_K8S_NAME_MAX])
{
    struct node *n = v;
    if (memcmp(key, other(n)->key, 32)) return 0;
    strcpy(name, n->peer_name); return 1;
}
static int placed(void *v, const char *uid, const char *name)
{
    (void)v;
    return (!strcmp(uid, "pod-a") && !strcmp(name, "node-a")) || (!strcmp(uid, "pod-b") && !strcmp(name, "node-b"));
}
static int registration(void *v, const char *uid, struct dmesh_peer_registration *out)
{
    struct node *n = v;
    if (!n->registered || strcmp(uid, n->pod)) return 0;
    *out = n->reg; return 1;
}
static int authorize(void *v, const struct dmesh_peer_pair *p, const struct dmesh_peer_stream_open *in)
{ struct node *n = v; return n->allow && in->dst_port == 8080 && !strcmp(p->local_uid, n->pod); }
static const struct dmesh_peer_ops TABLE_OPS = {.node_binding = binding, .pod_on_node = placed,
    .local_registration = registration, .pair_authorize = authorize, .now_ns = now_cb};
static const struct peer_manager_ops MANAGER_OPS = {.node_binding = binding, .node_by_key = by_key};

/* ---- fake verbs lanes --------------------------------------------------- */
static uint32_t lane_qpn(const struct node *n, unsigned worker, unsigned slot)
{ return 1000u * (n->index + 1) + 10u * worker + slot + 1; }
static int prepare(void *v, void **out, struct peer_verbs_endpoint *e)
{
    struct fdriver *d = v;
    for (unsigned s = 0; s < SLOTS; s++) {
        struct fconn *c = &d->conn[s];
        if (c->used) continue;
        memset(c, 0, sizeof(*c)); c->n = d->n; c->worker = d->worker; c->slot = s; c->used = 1;
        *e = (struct peer_verbs_endpoint){.qpn = lane_qpn(d->n, d->worker, s), .psn = 7, .mtu = 1024};
        e->gid[15] = (uint8_t)(d->n->index + 1); e->gid[14] = (uint8_t)(d->worker + 1);
        *out = c; return 0;
    }
    return -1;
}
static int activate(void *v, const struct peer_verbs_endpoint *remote, int hw)
{
    struct fconn *c = v;
    assert(hw == 1 && c->used && remote->mtu == 1024);
    assert(remote->qpn / 1000 == other(c->n)->index + 1 && (remote->qpn / 10) % 100 == c->worker);
    return 0;
}
static int drained(void *v) { (void)v; return 1; }
static int close_lane(void *v) { struct fconn *c = v; assert(c->used); memset(c, 0, sizeof(*c)); return 0; }
static struct fconn *peer_of(struct fconn *c) { return &other(c->n)->driver[c->worker].conn[c->slot]; }
static int wire_send(void *v, const void *b, size_t n)
{
    struct fconn *c = v, *p = peer_of(c);
    assert(c->used && n && n <= PEER_WIRE_MSG_MAX);
    if (!p->used || p->count == INBOX) return 0;
    unsigned i = (p->head + p->count) % INBOX;
    memcpy(p->inbox[i], b, n); p->len[i] = n; p->count++; return 1;
}
static long wire_recv(void *v, void *b, size_t cap)
{
    struct fconn *c = v;
    if (!c->used || !c->count) return 0;
    size_t n = c->len[c->head];
    assert(n <= cap);
    memcpy(b, c->inbox[c->head], n); c->head = (c->head + 1) % INBOX; c->count--; return (long)n;
}
static int wire_progress(void *v, void **a, int cap, int *count) { (void)v; (void)a; (void)cap; *count = 0; return 0; }
static int wire_faulted(void *v) { (void)v; return 0; }
static int wire_epfd(void *v) { (void)v; return -1; }
static void wire_close(void *v) { (void)v; assert(0); }
static void wire_free(void *v) { ((struct fdriver *)v)->freed++; }
static const struct peer_wire_ops WIRE = {.progress = wire_progress, .faulted = wire_faulted, .send_msg = wire_send,
    .recv_msg = wire_recv, .epfd = wire_epfd, .close = wire_close, .ctx_free = wire_free};

/* ---- fake hardware ------------------------------------------------------ */
static int hw_submit(void *v, const struct peer_crypto_request *r)
{
    struct fhw *h = v;
    if (h->backpressure) return 0;
    assert(h->count < 64);
    h->queue[h->count++] = *r; return 1;
}
static int hw_poll(void *v, struct peer_crypto_completion *out)
{
    struct fhw *h = v;
    if (!h->ndone) return 0;
    *out = h->done[0]; memmove(h->done, h->done + 1, --h->ndone * sizeof(*h->done)); return 1;
}
static int hw_fd(void *v) { (void)v; return -1; }
static void hw_step(struct node *n)
{
    struct fhw *h = &n->hw;
    for (unsigned i = 0; i < h->count; i++) {
        const struct peer_crypto_request *r = &h->queue[i];
        int status = 0;
        if (h->fail_action && (int)r->action == h->fail_action) { status = h->fail_status; h->fail_action = 0; }
        if (r->action == PEER_CRYPTO_INSTALL_RX || r->action == PEER_CRYPTO_INSTALL_TX)
            assert(peer_sec_nonzero(r->material, 20));
        h->seen[r->action]++; h->last_epoch[r->action] = r->binding.epoch;
        assert(h->ndone < 64);
        h->done[h->ndone++] = (struct peer_crypto_completion){.token = r->token, .operation = r->operation,
            .action = r->action, .epoch = r->binding.epoch, .status = status};
        memcpy(h->done[h->ndone-1].session, r->binding.session, 16);
        memcpy(h->done[h->ndone-1].association, r->binding.association, 16);
    }
    h->count = 0;
}

/* ---- turns ------------------------------------------------------------- */
static uint64_t g_authority = 1;
static void turn(void)
{
    for (unsigned i = 0; i < 2; i++) {
        struct node *n = &nodes[i];
        if (n->paused) continue;
        for (unsigned w = 0; w < WORKERS; w++) assert(peer_pair_transport_ops()->progress(n->rt[w]) >= 0);
        hw_step(n);
        if (g_now % (5 * NS) < STEP) assert(peer_manager_notify_authority(n->m, ++g_authority, g_now + 60 * NS) == 1);
        assert(peer_manager_progress(n->m, g_now) >= 0);
    }
    g_now += STEP;
}
static void settle(int (*pred)(void), unsigned max_turns)
{
    for (unsigned i = 0; i < max_turns; i++) { if (pred()) return; turn(); }
    fprintf(stderr, "settle: condition not reached after %u turns\n", max_turns); abort();
}
static unsigned pairs(struct node *n) { struct peer_manager_stats s; peer_manager_stats(n->m, &s); return s.pairs; }
static int no_pairs(void) { return !pairs(&nodes[0]) && !pairs(&nodes[1]); }
static const struct dmesh_peer_transport *ops(void) { return peer_pair_transport_ops(); }

static struct dmesh_peer_channel *open_stream(struct node *n, unsigned w, uint16_t port)
{
    struct dmesh_peer_stream_open in = {.src_generation = n->reg.generation, .dst_port = port, .source_token = 1};
    strcpy(in.src_pod_uid, n->pod); strcpy(in.dst_pod_uid, n->peer_pod); strcpy(in.src_service_key, "ns/service");
    enum dmesh_peer_refusal why;
    struct dmesh_peer_channel *ch = dmesh_peer_open_stream(&n->table[w], n->peer_name, &in, &why);
    assert(ch && !why && ch->conn);
    return ch;
}
static struct dmesh_peer_channel *g_watch;
static int watch_ready(void)
{ struct dmesh_peer_pair p; return g_watch->conn && ops()->pair_ready(g_watch->conn, &p) == 1; }
static int watch_closed(void) { return g_watch->state == DMESH_PEER_CLOSED && no_pairs(); }
static void expect_open(struct node *n, unsigned w, struct dmesh_peer_channel *ch)
{
    assert(dmesh_peer_channel_progress(&n->table[w], ch, 1) >= 0);
    assert(ch->state == DMESH_PEER_OPEN && ch->pair_scoped);
}
static void exchange(struct dmesh_peer_channel *from, struct dmesh_peer_channel *to, unsigned seed, size_t bytes)
{
    static uint8_t tx[PEER_WIRE_MSG_MAX], rx[PEER_WIRE_MSG_MAX];
    for (size_t i = 0; i < bytes; i++) tx[i] = (uint8_t)(i * 31 + seed);
    assert(ops()->send(from->conn, tx, bytes) == (long)bytes);
    assert(ops()->recv(to->conn, rx, sizeof(rx)) == (long)bytes && !memcmp(tx, rx, bytes));
}
static struct dmesh_peer_channel *accepted(struct node *n, unsigned w)
{
    struct dmesh_peer_channel *ch = dmesh_peer_find_pair(&n->table[w], n->peer_name, n->pod, n->peer_pod);
    assert(ch && ch->conn);
    return ch;
}

/* ---- setup ------------------------------------------------------------- */
static void node_init(struct node *n, unsigned index)
{
    n->index = index;
    n->name = index ? "node-b" : "node-a"; n->pod = index ? "pod-b" : "pod-a";
    n->peer_name = index ? "node-a" : "node-b"; n->peer_pod = index ? "pod-a" : "pod-b";
    n->seed[0] = (uint8_t)(index + 1); n->boot[0] = (uint8_t)(index + 5);
    n->reg.daemon[0] = (uint8_t)(index + 1); n->reg.nonce[31] = (uint8_t)(index + 1);
    n->reg.generation = 3; n->reg.slot = index;
    n->registered = n->allow = 1;
    struct peer_tls_ctx *ctx; char error[128];
    assert(!peer_tls_ctx_new(n->seed, n->name, &ctx, error, sizeof(error)));
    peer_tls_ctx_public_key(ctx, n->key); peer_tls_ctx_free(ctx);
    assert(!peer_wire_tcp_new(htonl(INADDR_LOOPBACK), 0, &n->tcp, &n->tcp_ctx, error, sizeof(error)));
    n->port = peer_wire_tcp_port(n->tcp_ctx);
    for (unsigned w = 0; w < WORKERS; w++) {
        n->driver[w] = (struct fdriver){.n = n, .worker = w};
        struct peer_pair_transport_config cfg = {.worker = w, .connections = SLOTS, .setup_timeout_ns = 30 * NS,
            .now_ns = now_cb, .dma_fenced = dmesh_peer_channel_dma_fenced};
        struct peer_pair_driver d = {.wire = &WIRE, .ctx = &n->driver[w], .prepare = prepare, .activate = activate,
            .send_drained = drained, .close = close_lane};
        assert(!peer_pair_transport_new_driver(&cfg, &d, &n->rt[w]));
        dmesh_peer_table_init(&n->table[w], n->name, n->key, ops(), n->rt[w], &TABLE_OPS, n);
        peer_pair_transport_attach(n->rt[w], &n->table[w]);
    }
}
static void manager_init(struct node *n)
{
    struct peer_manager_config cfg = {.seed = n->seed, .workers = WORKERS, .mtu = 1024,
        .wire = n->tcp, .wire_ctx = n->tcp_ctx,
        .crypto = {.ctx = &n->hw, .submit = hw_submit, .poll = hw_poll, .fd = hw_fd},
        .limits = {.pairs = 8, .lanes = 32, .sas = 32, .setup_timeout_ns = 5 * NS, .overlap_ns = NS},
        .lease_ns = 20 * NS, .control_lease_ns = 5 * NS, .setup_timeout_ns = 5 * NS,
        .now_ns = now_cb, .ops = &MANAGER_OPS, .ops_ctx = n};
    strcpy(cfg.node, n->name); strcpy(cfg.cluster, "test-cluster"); memcpy(cfg.boot, n->boot, 16);
    char error[128];
    assert(!peer_manager_new(&cfg, &n->m, error, sizeof(error)));
    for (unsigned w = 0; w < WORKERS; w++) assert(!peer_manager_attach_worker(n->m, w, n->rt[w]));
    assert(peer_manager_notify_authority(n->m, g_authority, g_now + 60 * NS) == 1);
}

/* ---- scenarios --------------------------------------------------------- */
static struct dmesh_peer_channel *establish(void)
{
    struct node *a = &nodes[0], *b = &nodes[1];
    unsigned rx = a->hw.seen[PEER_CRYPTO_INSTALL_RX];
    g_watch = open_stream(a, 0, 8080);
    settle(watch_ready, 4000);
    struct dmesh_peer_channel *a0 = g_watch, *b0 = accepted(b, 0);
    expect_open(a, 0, a0); expect_open(b, 0, b0);
    assert(a->hw.seen[PEER_CRYPTO_INSTALL_RX] == rx + 1 && a->hw.seen[PEER_CRYPTO_INSTALL_TX] == rx + 1);
    assert(b->hw.seen[PEER_CRYPTO_INSTALL_RX] == rx + 1 && b->hw.seen[PEER_CRYPTO_INSTALL_TX] == rx + 1);
    assert(pairs(a) == 1 && pairs(b) == 1);
    exchange(a0, b0, 1, 256); exchange(b0, a0, 2, 65536 + 64);
    return a0;
}
static void happy_path_adoption_renewal_rekey(void)
{
    struct node *a = &nodes[0], *b = &nodes[1];
    struct dmesh_peer_channel *a0 = establish(), *b0 = accepted(b, 0);
    /* Every worker holds a lane; the second worker's own stream adopts its
     * lane instead of proposing again. */
    assert(a->driver[1].conn[0].used && b->driver[1].conn[0].used);
    struct dmesh_peer_channel *a1 = open_stream(a, 1, 8080);
    assert(a1->conn == &nodes[0].driver[1].conn[0] || ops()->pair_ready(a1->conn, &(struct dmesh_peer_pair){0}) >= 0);
    g_watch = a1; settle(watch_ready, 100);
    struct dmesh_peer_channel *b1 = accepted(b, 1);
    expect_open(a, 1, a1); expect_open(b, 1, b1);
    exchange(a1, b1, 3, 4096); exchange(b1, a1, 4, 512);
    assert(pairs(a) == 1 && pairs(b) == 1);
    /* Leases: control heartbeat, authority notifications and the acceptor's
     * policy re-query all outlive the 5 s control lease and 20 s pair leases. */
    uint64_t until = g_now + 45 * NS;
    while (g_now < until) { turn(); assert(pairs(a) == 1 && pairs(b) == 1); }
    struct dmesh_peer_pair cert;
    assert(ops()->pair_ready(a0->conn, &cert) == 1 && ops()->pair_admit(a0->conn) == 1);
    assert(ops()->pair_ready(b1->conn, &cert) == 1);
    exchange(a0, b0, 5, 1024);
    /* Rekey: new keys on both sides, admission paused then re-granted, the
     * retiring epoch removed after the overlap. */
    assert(!peer_manager_rekey(a->m, cert.association));
    assert(peer_manager_rekey(a->m, cert.association) == -1);
    until = g_now + 6 * NS;
    while (g_now < until) turn();
    for (unsigned i = 0; i < 2; i++) {
        struct fhw *h = &nodes[i].hw;
        assert(h->seen[PEER_CRYPTO_INSTALL_RX] == 2 && h->seen[PEER_CRYPTO_INSTALL_TX] == 2);
        assert(h->last_epoch[PEER_CRYPTO_INSTALL_RX] == 2 && h->seen[PEER_CRYPTO_REMOVE_OLD] == 1);
        assert(h->seen[PEER_CRYPTO_QUIESCE] == 0 && h->seen[PEER_CRYPTO_BLOCK] == 0);
    }
    assert(ops()->pair_admit(a0->conn) == 1 && ops()->pair_admit(b1->conn) == 1);
    exchange(a0, b0, 6, 2048); exchange(b1, a1, 7, 128);
    /* Teardown by the source Pod's unregistration. */
    a->registered = 0;
    assert(peer_manager_notify_unregister(a->m, "pod-a") == 1);
    g_watch = a0; settle(watch_closed, 4000);
    assert(a1->state == DMESH_PEER_CLOSED && b0->state == DMESH_PEER_CLOSED && b1->state == DMESH_PEER_CLOSED);
    for (unsigned i = 0; i < 2; i++) {
        assert(nodes[i].hw.seen[PEER_CRYPTO_BLOCK] == 1 && nodes[i].hw.seen[PEER_CRYPTO_REMOVE_ALL] == 1);
        for (unsigned w = 0; w < WORKERS; w++) for (unsigned s = 0; s < SLOTS; s++) assert(!nodes[i].driver[w].conn[s].used);
    }
    a->registered = 1;
    memset(&a->hw.seen, 0, sizeof(a->hw.seen)); memset(&b->hw.seen, 0, sizeof(b->hw.seen));
}
static void refusals(void)
{
    struct node *a = &nodes[0], *b = &nodes[1];
    /* Destination policy refuses: no QP, no SA, the source stream is refused. */
    b->allow = 0;
    g_watch = open_stream(a, 0, 8080);
    settle(watch_closed, 4000);
    assert(!a->hw.seen[PEER_CRYPTO_INSTALL_RX] && !b->hw.seen[PEER_CRYPTO_INSTALL_RX] && !b->driver[0].conn[0].used);
    b->allow = 1;
    /* Hardware refuses the receive install on the acceptor: both ends retire. */
    b->hw.fail_action = PEER_CRYPTO_INSTALL_RX; b->hw.fail_status = -1;
    g_watch = open_stream(a, 0, 8080);
    settle(watch_closed, 4000);
    assert(b->hw.seen[PEER_CRYPTO_BLOCK] == 1 && b->hw.seen[PEER_CRYPTO_REMOVE_ALL] == 1 && !b->driver[0].conn[0].used);
    assert(a->hw.seen[PEER_CRYPTO_REMOVE_ALL] == 1 && !a->driver[0].conn[0].used && !a->driver[1].conn[0].used);
    memset(&a->hw.seen, 0, sizeof(a->hw.seen)); memset(&b->hw.seen, 0, sizeof(b->hw.seen));
    /* Adapter backpressure only delays. */
    a->hw.backpressure = 1;
    struct dmesh_peer_channel *a0 = open_stream(a, 0, 8080);
    g_watch = a0;
    for (unsigned i = 0; i < 200; i++) turn();
    assert(!watch_ready() && !a->hw.seen[PEER_CRYPTO_INSTALL_RX]);
    a->hw.backpressure = 0;
    settle(watch_ready, 4000);
    exchange(a0, accepted(b, 0), 8, 300);
    b->registered = 0;
    assert(peer_manager_notify_unregister(b->m, "pod-b") == 1);
    settle(watch_closed, 4000);
    b->registered = 1;
    memset(&a->hw.seen, 0, sizeof(a->hw.seen)); memset(&b->hw.seen, 0, sizeof(b->hw.seen));
}
static int one_pair_each(void) { return pairs(&nodes[0]) == 1 && pairs(&nodes[1]) == 1 && watch_ready(); }
static void simultaneous_proposals(void)
{
    struct node *a = &nodes[0], *b = &nodes[1];
    struct dmesh_peer_channel *a0 = open_stream(a, 0, 8080), *bx = open_stream(b, 0, 8080);
    g_watch = a0;
    settle(one_pair_each, 6000);
    /* node-a is canonical endpoint 0: its proposal stands, node-b's own stream
     * was refused (its channel slot may be re-tenanted by the accepted lane)
     * and a new stream reuses the accepted lane. */
    struct dmesh_peer_channel *b0 = accepted(b, 0);
    assert(bx == b0 || bx->state == DMESH_PEER_CLOSED);
    assert(open_stream(b, 0, 8080) == b0);
    expect_open(a, 0, a0); expect_open(b, 0, b0);
    exchange(a0, b0, 9, 700); exchange(b0, a0, 10, 900);
    a->registered = 0;
    assert(peer_manager_notify_unregister(a->m, "pod-a") == 1);
    settle(watch_closed, 4000);
    a->registered = 1;
}
static void session_loss(void)
{
    struct node *a = &nodes[0], *b = &nodes[1];
    struct dmesh_peer_channel *a0 = establish();
    /* node-b stalls past the control lease: node-a's session and pair expire
     * on their own; node-b retires on its own lease when it resumes. */
    b->paused = 1;
    uint64_t until = g_now + 8 * NS;
    while (g_now < until) turn();
    assert(pairs(a) == 0 && a0->state == DMESH_PEER_CLOSED);
    b->paused = 0;
    settle(no_pairs, 4000);
    /* A fresh session carries the next pair. */
    a0 = establish();
    a->registered = 0;
    assert(peer_manager_notify_unregister(a->m, "pod-a") == 1);
    g_watch = a0; settle(watch_closed, 4000);
    a->registered = 1;
}
static void quarantine(void)
{
    struct node *a = &nodes[0], *b = &nodes[1];
    memset(&a->hw.seen, 0, sizeof(a->hw.seen)); memset(&b->hw.seen, 0, sizeof(b->hw.seen));
    b->hw.fail_action = PEER_CRYPTO_INSTALL_TX; b->hw.fail_status = -2;
    g_watch = open_stream(a, 0, 8080);
    for (unsigned i = 0; i < 3000; i++) turn();
    struct peer_manager_stats sb; peer_manager_stats(b->m, &sb);
    assert(sb.quarantined == 1 && pairs(b) == 1 && pairs(a) == 0);
    assert(b->driver[0].conn[0].used && b->driver[1].conn[0].used); /* lanes held, never reused */
    assert(peer_manager_free(b->m) == -1);
}

int main(void)
{
    node_init(&nodes[0], 0); node_init(&nodes[1], 1);
    manager_init(&nodes[0]); manager_init(&nodes[1]);
    happy_path_adoption_renewal_rekey();
    refusals();
    simultaneous_proposals();
    session_loss();
    for (unsigned w = 0; w < WORKERS; w++) for (unsigned i = 0; i < 2; i++) {
        struct peer_pair_event e;
        while (peer_pair_manager_poll(nodes[i].rt[w], &e) == 1) assert(e.type != PEER_PAIR_OPEN);
    }
    quarantine();
    /* node-a retires cleanly; node-b keeps its quarantined lanes and SA. */
    for (unsigned w = 0; w < WORKERS; w++) {
        assert(!dmesh_peer_table_fini(&nodes[0].table[w]));
        assert(!peer_pair_transport_free(nodes[0].rt[w]) && nodes[0].driver[w].freed == 1);
    }
    assert(!peer_manager_free(nodes[0].m));
    puts("peer_manager_test: PASS (negotiation, lanes on every worker, adoption, leases, rekey, "
         "unregister, refusal, hardware failure, backpressure, simultaneous proposals, session loss, quarantine)");
    return 0;
}
