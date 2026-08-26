/* Two whole peer stacks in one process.
 *
 * Everything below the seam is real: real Ed25519 credentials, a real mutual
 * TLS handshake, a real carrier. Only the node's own state — which Pods the
 * generation places where, which slot a Pod registered into — is faked, and it
 * is faked because that is the part `peer_channel_test.c` already covers.
 *
 * Most of the file runs over loopback TCP, which is what CI has and what
 * bring-up falls back to. One test runs over an in-memory carrier instead,
 * because it needs a send that refuses on demand: the rule it checks is that a
 * frame never enters the TLS session while the previous frame's ciphertext is
 * still waiting, and a real socket cannot be made to block on cue. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../doca/peer_transport.h"
#include "../doca/peer_wire.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE_A   "node-a"
#define NODE_B   "node-b"
#define UID_A    "pod-uid-a"
#define UID_B    "pod-uid-b"
#define EXTENT   (64u * 1024u)

/* ---- the node's own state, faked ---------------------------------------- */

struct binding {
    char     name[DMESH_K8S_NAME_MAX];
    uint8_t  key[32];
    uint32_t ip_be;
    uint16_t port;
    int      bound;
};

struct fake {
    struct binding peer;             /* the one other node in the generation */
    char     src_uid[64];            /* the Pod that node may claim */
    char     local_uid[64];          /* the Pod registered here */
    int32_t  local_slot;
    uint32_t pod_generation;
    uint64_t now;

    uint32_t delivered;
    uint64_t delivered_bytes;
    uint32_t releases;
    uint64_t released_bytes;
    uint32_t poisons;
    uint32_t source_opens;
    uint32_t destination_fins;
    uint32_t resets;
    uint32_t last_source_handle;
    int32_t  last_source_status;
    uint8_t  payload[EXTENT];
    uint32_t payload_len;
    uint32_t seq_seen[8];
    uint8_t  first_seen[8];
    uint32_t seen_count;
    char     last_reset[64];
};

static int fake_node_binding(void *ctx, const char *node, const uint8_t **key,
                             uint32_t *ip_be, uint16_t *port)
{
    struct fake *f = ctx;
    if (!f->peer.bound || strcmp(node, f->peer.name) != 0)
        return 0;
    *key = f->peer.key;
    *ip_be = f->peer.ip_be;
    *port = f->peer.port;
    return 1;
}

static int fake_pod_on_node(void *ctx, const char *pod_uid, const char *node)
{
    struct fake *f = ctx;
    return strcmp(pod_uid, f->src_uid) == 0 && strcmp(node, f->peer.name) == 0;
}

static int32_t fake_local_pod(void *ctx, const char *pod_uid, uint32_t *generation)
{
    struct fake *f = ctx;
    if (strcmp(pod_uid, f->local_uid) != 0)
        return -1;
    if (generation)
        *generation = f->pod_generation;
    return f->local_slot;
}

static uint16_t fake_upstream_for(void *ctx, int32_t slot, uint16_t port)
{
    (void)ctx;
    return (uint16_t)(40000 + slot + port);
}

static uint32_t fake_pod_generation(void *ctx, int32_t slot)
{
    struct fake *f = ctx;
    (void)slot;
    return f->pod_generation;
}

static int fake_deliver(void *ctx, const struct dmesh_peer_handle *handle,
                        const uint8_t *bytes, uint32_t len, uint32_t seq)
{
    struct fake *f = ctx;
    (void)handle;
    f->delivered++;
    f->delivered_bytes += len;
    if (len <= sizeof(f->payload)) {
        memcpy(f->payload, bytes, len);
        f->payload_len = len;
    }
    if (f->seen_count < 8) {
        f->seq_seen[f->seen_count] = seq;
        f->first_seen[f->seen_count] = len ? bytes[0] : 0;
        f->seen_count++;
    }
    return 0;
}

static void fake_release(void *ctx, uint8_t kind, void *cookie, uint32_t bytes)
{
    struct fake *f = ctx;
    (void)kind; (void)cookie;
    f->releases++;
    f->released_bytes += bytes;
}

static void fake_poison(void *ctx, const struct dmesh_peer_handle *handle,
                        const char *why)
{
    struct fake *f = ctx;
    (void)handle; (void)why;
    f->poisons++;
}

static void fake_source_opened(void *ctx, struct dmesh_peer_channel *channel,
                               uint32_t source_token, uint32_t handle,
                               int32_t status)
{
    struct fake *f = ctx;
    (void)channel; (void)source_token;
    f->source_opens++;
    f->last_source_handle = handle;
    f->last_source_status = status;
}

static int fake_source_fin(void *ctx, struct dmesh_peer_channel *channel,
                           uint32_t handle)
{
    struct fake *f = ctx;
    (void)channel;
    return handle == f->last_source_handle;
}

static int fake_source_deliver(void *ctx, struct dmesh_peer_channel *channel,
                               uint32_t handle, const uint8_t *bytes,
                               uint32_t len, uint32_t seq)
{
    struct fake *f = ctx;
    (void)channel; (void)handle; (void)bytes; (void)seq;
    f->delivered++;
    f->delivered_bytes += len;
    return 0;
}

static int fake_destination_fin(void *ctx, const struct dmesh_peer_handle *handle)
{
    struct fake *f = ctx;
    (void)handle;
    f->destination_fins++;
    return 0;
}

static void fake_source_reset(void *ctx, struct dmesh_peer_channel *channel,
                              const char *why)
{
    struct fake *f = ctx;
    (void)channel;
    f->resets++;
    snprintf(f->last_reset, sizeof(f->last_reset), "%s", why ? why : "");
}

static void fake_event(void *ctx, const char *reason)
{
    (void)ctx; (void)reason;
}

static uint64_t fake_now(void *ctx)
{
    struct fake *f = ctx;
    return f->now;
}

static const struct dmesh_peer_ops FAKE_OPS = {
    .node_binding    = fake_node_binding,
    .pod_on_node     = fake_pod_on_node,
    .local_pod       = fake_local_pod,
    .upstream_for    = fake_upstream_for,
    .pod_generation  = fake_pod_generation,
    .deliver         = fake_deliver,
    .source_deliver  = fake_source_deliver,
    .destination_fin = fake_destination_fin,
    .release         = fake_release,
    .poison          = fake_poison,
    .source_opened   = fake_source_opened,
    .source_fin      = fake_source_fin,
    .source_reset    = fake_source_reset,
    .event           = fake_event,
    .now_ns          = fake_now,
};

/* ---- an in-memory carrier whose send can be told to refuse --------------- */

#define STUB_CONNS 8
#define STUB_QUEUE 64

struct stub_ctx;

struct stub_conn {
    struct stub_ctx  *ctx;
    struct stub_conn *peer;
    uint8_t  in_use;
    uint8_t  dead;
    uint8_t *queue[STUB_QUEUE];      /* messages waiting for THIS end to read */
    uint32_t qlen[STUB_QUEUE];
    uint32_t head, tail;
};

struct stub_ctx {
    struct stub_ctx *remote;
    int      block;                  /* refuse every send while set */
    struct stub_conn conns[STUB_CONNS];
    void    *pending[STUB_CONNS];
    int      npending;
};

static struct stub_conn *stub_slot(struct stub_ctx *ctx)
{
    for (int i = 0; i < STUB_CONNS; i++) {
        if (ctx->conns[i].in_use)
            continue;
        memset(&ctx->conns[i], 0, sizeof(ctx->conns[i]));
        ctx->conns[i].ctx = ctx;
        ctx->conns[i].in_use = 1;
        return &ctx->conns[i];
    }
    return NULL;
}

static int stub_connect(void *wctx, uint32_t ip_be, uint16_t port, void **wc)
{
    struct stub_ctx *ctx = wctx;
    (void)ip_be; (void)port;
    *wc = NULL;
    if (!ctx->remote || ctx->remote->npending >= STUB_CONNS)
        return -1;
    struct stub_conn *near = stub_slot(ctx);
    struct stub_conn *far = near ? stub_slot(ctx->remote) : NULL;
    if (!near || !far) {
        if (near)
            near->in_use = 0;
        return -1;
    }
    near->peer = far;
    far->peer = near;
    ctx->remote->pending[ctx->remote->npending++] = far;
    *wc = near;
    return 0;
}

static int stub_progress(void *wctx, void **accepted, int max, int *n_accepted)
{
    struct stub_ctx *ctx = wctx;
    int n = 0;
    if (n_accepted)
        *n_accepted = 0;
    while (accepted && n_accepted && n < max && n < ctx->npending) {
        accepted[n] = ctx->pending[n];
        n++;
    }
    if (n_accepted)
        *n_accepted = n;
    for (int i = n; i < ctx->npending; i++)
        ctx->pending[i - n] = ctx->pending[i];
    ctx->npending -= n;
    return n > 0;
}

static int stub_send_msg(void *wc, const void *buf, size_t len)
{
    struct stub_conn *c = wc;
    if (!c || !c->in_use || c->dead || !c->peer)
        return -1;
    if (c->ctx->block)
        return 0;
    struct stub_conn *dst = c->peer;
    uint32_t next = (dst->tail + 1) % STUB_QUEUE;
    if (next == dst->head)
        return 0;
    uint8_t *copy = malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, buf, len);
    dst->queue[dst->tail] = copy;
    dst->qlen[dst->tail] = (uint32_t)len;
    dst->tail = next;
    return 1;
}

static long stub_recv_msg(void *wc, void *buf, size_t cap)
{
    struct stub_conn *c = wc;
    if (!c || !c->in_use)
        return -1;
    if (c->head == c->tail)
        return c->dead ? -1 : 0;
    uint32_t len = c->qlen[c->head];
    if (len > cap)
        return -1;
    memcpy(buf, c->queue[c->head], len);
    free(c->queue[c->head]);
    c->queue[c->head] = NULL;
    c->head = (c->head + 1) % STUB_QUEUE;
    return (long)len;
}

static int stub_established(void *wc)
{
    struct stub_conn *c = wc;
    return c && c->in_use && !c->dead;
}

static int stub_faulted(void *wc)
{
    struct stub_conn *c = wc;
    return !c || !c->in_use || (c->dead && c->head == c->tail);
}

static void stub_close(void *wc)
{
    struct stub_conn *c = wc;
    if (!c || !c->in_use)
        return;
    while (c->head != c->tail) {
        free(c->queue[c->head]);
        c->queue[c->head] = NULL;
        c->head = (c->head + 1) % STUB_QUEUE;
    }
    if (c->peer) {
        c->peer->dead = 1;
        c->peer->peer = NULL;
    }
    memset(c, 0, sizeof(*c));
}

static int stub_epfd(void *wctx)
{
    (void)wctx;
    return -1;
}

static void stub_ctx_free(void *wctx)
{
    struct stub_ctx *ctx = wctx;
    if (!ctx)
        return;
    for (int i = 0; i < STUB_CONNS; i++)
        stub_close(&ctx->conns[i]);
    if (ctx->remote)
        ctx->remote->remote = NULL;
    free(ctx);
}

static const struct peer_wire_ops STUB_OPS = {
    .connect     = stub_connect,
    .progress    = stub_progress,
    .send_msg    = stub_send_msg,
    .recv_msg    = stub_recv_msg,
    .established = stub_established,
    .faulted     = stub_faulted,
    .close       = stub_close,
    .epfd        = stub_epfd,
    .ctx_free    = stub_ctx_free,
};

/* ---- a node ------------------------------------------------------------- */

struct node {
    char     name[DMESH_K8S_NAME_MAX];
    struct dmesh_peer_table table;
    struct peer_transport_rt *rt;
    struct fake fake;
    uint8_t  pub[32];
    uint16_t port;
};

static void node_start(struct node *n, const char *name, uint8_t seed_byte,
                       const struct peer_wire_ops *ops, void *wctx)
{
    uint8_t seed[32];
    memset(seed, seed_byte, sizeof(seed));
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->fake.local_slot = 3;
    n->fake.pod_generation = 11;
    n->fake.now = 1000;

    struct peer_transport_config config = {
        .node_name = n->name,
        .seed = seed,
        .wire = ops,
        .wire_ctx = wctx,
        .handshake_timeout_ns = 5ull * 1000000000ull,
        .now_ns = fake_now,
        .now_ctx = &n->fake,
    };
    char error[256] = { 0 };
    assert(dmesh_peer_transport_new(&config, &n->rt, error, sizeof(error)) == 0);
    dmesh_peer_transport_public_key(n->rt, n->pub);
    dmesh_peer_table_init(&n->table, n->name, n->pub, dmesh_peer_transport_ops(),
                          n->rt, &FAKE_OPS, &n->fake);
    dmesh_peer_transport_attach(n->rt, &n->table);
}

static void node_start_tcp(struct node *n, const char *name, uint8_t seed_byte)
{
    const struct peer_wire_ops *ops = NULL;
    void *wctx = NULL;
    char error[256] = { 0 };
    assert(peer_wire_tcp_new(htonl(0x7F000001u), 0, &ops, &wctx, error,
                             sizeof(error)) == 0);
    node_start(n, name, seed_byte, ops, wctx);
    n->port = peer_wire_tcp_port(wctx);
    assert(n->port != 0);
}

static void node_stop(struct node *n)
{
    dmesh_peer_table_fini(&n->table);
    dmesh_peer_transport_free(n->rt);
    n->rt = NULL;
}

/* Each node learns the other exactly as a signed generation would state it:
 * the name, the key it binds to that name, and where to reach it. */
static void node_bind(struct node *n, const struct node *peer)
{
    snprintf(n->fake.peer.name, sizeof(n->fake.peer.name), "%s", peer->name);
    memcpy(n->fake.peer.key, peer->pub, 32);
    n->fake.peer.ip_be = htonl(0x7F000001u);
    n->fake.peer.port = peer->port;
    n->fake.peer.bound = 1;
    snprintf(n->fake.src_uid, sizeof(n->fake.src_uid), "%s",
             strcmp(peer->name, NODE_A) == 0 ? UID_A : UID_B);
    snprintf(n->fake.local_uid, sizeof(n->fake.local_uid), "%s",
             strcmp(n->name, NODE_A) == 0 ? UID_A : UID_B);
}

static void node_tick(struct node *n)
{
    dmesh_peer_transport_progress(n->rt);
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *channel = &n->table.channels[i];
        if (channel->in_use && channel->state != DMESH_PEER_CLOSED)
            dmesh_peer_channel_progress(&n->table, channel, 64);
    }
}

static void pump(struct node *a, struct node *b, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        node_tick(a);
        node_tick(b);
    }
}

static struct dmesh_peer_channel *open_channel(struct node *a, struct node *b)
{
    enum dmesh_peer_refusal reason = DMESH_PEER_OK;
    struct dmesh_peer_channel *channel = dmesh_peer_open(&a->table, b->name, &reason);
    assert(channel != NULL);
    assert(reason == DMESH_PEER_OK);
    assert(channel->state == DMESH_PEER_AUTHENTICATING);
    return channel;
}

static uint32_t live_channels(const struct node *n)
{
    uint32_t live = 0;
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++)
        if (n->table.channels[i].in_use &&
            n->table.channels[i].state == DMESH_PEER_OPEN)
            live++;
    return live;
}

static struct dmesh_peer_stream_open make_open(const char *src, const char *dst)
{
    struct dmesh_peer_stream_open open;
    memset(&open, 0, sizeof(open));
    snprintf(open.src_pod_uid, sizeof(open.src_pod_uid), "%s", src);
    snprintf(open.dst_pod_uid, sizeof(open.dst_pod_uid), "%s", dst);
    snprintf(open.src_service_key, sizeof(open.src_service_key), "%s", "test/source");
    open.dst_port = 9091;
    open.source_token = 77;
    open.src_generation = 42;
    return open;
}

/* Runs a full stream open and returns the handle the destination allocated. */
static uint32_t open_stream(struct node *a, struct node *b,
                            struct dmesh_peer_channel *ac)
{
    struct dmesh_peer_stream_open open = make_open(UID_A, UID_B);
    uint32_t before = a->fake.source_opens;
    assert(dmesh_peer_stream_request(&a->table, ac, &open) == DMESH_PEER_OK);
    pump(a, b, 8);
    assert(a->fake.source_opens == before + 1);
    assert(a->fake.last_source_status == 0);
    return a->fake.last_source_handle;
}

/* ---- tests -------------------------------------------------------------- */

/* Two nodes that hold each other's keys reach OPEN over a real mutual TLS
 * handshake, and each ends up with exactly one channel to the other. */
static void test_handshake(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    pump(&a, &b, 16);

    assert(ac->state == DMESH_PEER_OPEN);
    assert(ac->handshakes == 1);
    struct dmesh_peer_channel *bc = dmesh_peer_find(&b.table, NODE_A);
    assert(bc && bc->state == DMESH_PEER_OPEN);
    assert(live_channels(&a) == 1 && live_channels(&b) == 1);

    /* Both ends authenticated the same incarnation, which is what the
     * prologue carried across and what every handle will now be stamped with. */
    assert(bc->incarnation == ac->incarnation);
    assert(memcmp(ac->bound_key, b.pub, 32) == 0);
    assert(memcmp(bc->bound_key, a.pub, 32) == 0);

    struct peer_transport_stats stats;
    dmesh_peer_transport_stats(b.rt, &stats);
    assert(stats.accepted == 1 && stats.refused == 0 && stats.faults == 0);

    node_stop(&a);
    node_stop(&b);
}

/* A full extent crosses the wire byte for byte, is delivered, acknowledged,
 * and releases the custody the source pinned for it. */
static void test_stream_roundtrip(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    pump(&a, &b, 16);
    uint32_t handle = open_stream(&a, &b, ac);

    static uint8_t payload[EXTENT];
    for (uint32_t i = 0; i < EXTENT; i++)
        payload[i] = (uint8_t)(i * 7u + 3u);
    int cookie = 0;
    assert(dmesh_peer_stream_data_send(&a.table, ac, handle, 1, payload, EXTENT,
                                       DMESH_PEER_CUSTODY_L4, &cookie) ==
           DMESH_PEER_OK);
    assert(ac->inflight_bytes == EXTENT);
    pump(&a, &b, 16);

    assert(b.fake.delivered == 1);
    assert(b.fake.payload_len == EXTENT);
    assert(memcmp(b.fake.payload, payload, EXTENT) == 0);

    /* The acknowledgement came back and the source stopped holding the bytes. */
    assert(a.fake.releases == 1);
    assert(a.fake.released_bytes == EXTENT);
    assert(ac->inflight_bytes == 0);

    node_stop(&a);
    node_stop(&b);
}

/* FIN ends one stream; POD_GONE ends every stream a departed Pod sourced. */
static void test_fin_and_pod_gone(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    pump(&a, &b, 16);

    uint32_t handle = open_stream(&a, &b, ac);
    assert(dmesh_peer_stream_fin_send(&a.table, ac, handle) == DMESH_PEER_OK);
    pump(&a, &b, 8);
    assert(b.fake.destination_fins == 1);

    open_stream(&a, &b, ac);
    uint32_t poisons = b.fake.poisons;
    assert(dmesh_peer_pod_gone_send(&a.table, ac, UID_A) == DMESH_PEER_OK);
    pump(&a, &b, 8);
    assert(b.fake.poisons > poisons);

    node_stop(&a);
    node_stop(&b);
}

/* A connection that dies under a live stream ends the stream at both ends and
 * releases everything the source had pinned for a peer that is now gone. */
static void test_transport_loss(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    pump(&a, &b, 16);
    uint32_t handle = open_stream(&a, &b, ac);

    /* Bytes in flight that the peer will never acknowledge. */
    static uint8_t payload[4096];
    memset(payload, 0x5A, sizeof(payload));
    int cookie = 0;
    assert(dmesh_peer_stream_data_send(&a.table, ac, handle, 1, payload,
                                       sizeof(payload), DMESH_PEER_CUSTODY_L4,
                                       &cookie) == DMESH_PEER_OK);
    assert(ac->inflight_bytes == sizeof(payload));

    struct dmesh_peer_channel *bc = dmesh_peer_find(&b.table, NODE_A);
    assert(bc && bc->conn);
    b.table.transport->close(bc->conn);
    bc->conn = NULL;

    pump(&a, &b, 16);

    assert(ac->state == DMESH_PEER_CLOSED);
    assert(a.table.refused[DMESH_PEER_REFUSE_TRANSPORT] >= 1);
    assert(a.fake.resets >= 1);
    /* The handle lived in the destination's table, so that is where the stream
     * it carried ends. */
    assert(b.fake.poisons >= 1);
    /* Nothing stays pinned for a channel that no longer exists. */
    assert(a.fake.released_bytes == sizeof(payload));
    assert(ac->inflight_bytes == 0);
    assert(live_channels(&a) == 0);

    node_stop(&a);
    node_stop(&b);
}

/* The chain proves possession; the pin decides. A node presenting a valid
 * certificate for a name the generation binds to a different key is refused,
 * and refused by that reason rather than by a handshake failure. */
static void test_key_mismatch(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);
    a.fake.peer.key[0] ^= 0xFF;      /* the generation binds someone else */

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    pump(&a, &b, 16);

    assert(ac->state == DMESH_PEER_CLOSED);
    assert(a.table.refused[DMESH_PEER_REFUSE_NODE_KEY] >= 1);
    assert(live_channels(&a) == 0);

    node_stop(&a);
    node_stop(&b);
}

/* A node the held generation says nothing about has nowhere to land. */
static void test_node_unbound(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);
    b.fake.peer.bound = 0;           /* B has not adopted a generation naming A */

    open_channel(&a, &b);
    pump(&a, &b, 16);

    assert(b.table.refused[DMESH_PEER_REFUSE_NODE_UNBOUND] >= 1);
    assert(live_channels(&b) == 0);

    struct peer_transport_stats stats;
    dmesh_peer_transport_stats(b.rt, &stats);
    assert(stats.refused >= 1 && stats.accepted == 0);

    node_stop(&a);
    node_stop(&b);
}

/* Both nodes dial at the same instant. They converge on one connection rather
 * than replacing each other's forever, and they agree on which one. */
static void test_simultaneous_open(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    struct dmesh_peer_channel *bc = open_channel(&b, &a);
    pump(&a, &b, 24);

    assert(live_channels(&a) == 1);
    assert(live_channels(&b) == 1);
    /* The lexicographically smaller node keeps the connection it initiated. */
    assert(ac->state == DMESH_PEER_OPEN && ac->initiated_local == 1);
    assert(bc->state == DMESH_PEER_OPEN && bc->initiated_local == 0);
    assert(ac->incarnation == bc->incarnation);

    node_stop(&a);
    node_stop(&b);
}

/* The rule the whole send path is built around: once plaintext enters the TLS
 * session its ciphertext is committed, so a frame may not enter while the
 * previous frame's ciphertext is still waiting for the carrier. The channel
 * must be told "not now" instead, and nothing may be encrypted twice. */
static void test_send_backpressure(void)
{
    struct stub_ctx *sa = calloc(1, sizeof(*sa));
    struct stub_ctx *sb = calloc(1, sizeof(*sb));
    assert(sa && sb);
    sa->remote = sb;
    sb->remote = sa;

    struct node a, b;
    node_start(&a, NODE_A, 0xA1, &STUB_OPS, sa);
    node_start(&b, NODE_B, 0xB2, &STUB_OPS, sb);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    pump(&a, &b, 16);
    assert(ac->state == DMESH_PEER_OPEN);
    uint32_t handle = open_stream(&a, &b, ac);

    static uint8_t first[4096], second[4096];
    memset(first, 0x11, sizeof(first));
    memset(second, 0x22, sizeof(second));
    int cookie = 0;

    sa->block = 1;
    /* Accepted: the session encrypted it and the carrier is holding it. */
    assert(dmesh_peer_stream_data_send(&a.table, ac, handle, 1, first,
                                       sizeof(first), DMESH_PEER_CUSTODY_L4,
                                       &cookie) == DMESH_PEER_OK);
    /* Accepted too, but it never reached the session: the channel retained the
     * frame itself, which is the only place it can be retried from. */
    assert(dmesh_peer_stream_data_send(&a.table, ac, handle, 2, second,
                                       sizeof(second), DMESH_PEER_CUSTODY_L4,
                                       &cookie) == DMESH_PEER_OK);
    assert(ac->tx_len != 0);
    /* With one frame already retained there is nowhere to put a third. */
    assert(dmesh_peer_stream_data_send(&a.table, ac, handle, 3, second,
                                       sizeof(second), DMESH_PEER_CUSTODY_L4,
                                       &cookie) == DMESH_PEER_REFUSE_INFLIGHT);

    sa->block = 0;
    pump(&a, &b, 24);

    /* Both arrived, in order, once each. */
    assert(ac->tx_len == 0);
    assert(b.fake.delivered == 2);
    assert(b.fake.delivered_bytes == 2u * sizeof(first));
    assert(b.fake.seen_count == 2);
    assert(b.fake.seq_seen[0] == 1 && b.fake.first_seen[0] == 0x11);
    assert(b.fake.seq_seen[1] == 2 && b.fake.first_seen[1] == 0x22);
    assert(a.fake.releases == 2);

    node_stop(&a);
    node_stop(&b);
}

/* An inbound connection that opens and then says nothing must not hold a slot
 * forever, and neither must the channel waiting on one that never answers. */
static void test_handshake_deadline(void)
{
    struct node a, b;
    node_start_tcp(&a, NODE_A, 0xA1);
    node_start_tcp(&b, NODE_B, 0xB2);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    /* One tick opens the socket; B never runs, so nothing answers. */
    dmesh_peer_transport_progress(a.rt);
    assert(dmesh_peer_transport_pending(a.rt) == 1);

    a.fake.now += 6ull * 1000000000ull;
    dmesh_peer_transport_progress(a.rt);

    assert(ac->state == DMESH_PEER_CLOSED);
    assert(dmesh_peer_transport_pending(a.rt) == 0);
    struct peer_transport_stats stats;
    dmesh_peer_transport_stats(a.rt, &stats);
    assert(stats.faults >= 1 && stats.live == 0);

    node_stop(&a);
    node_stop(&b);
}

/* A carrier fault is terminal even before peer_key can answer. Treating the
 * same negative answer as merely "not ready" would leave the channel in
 * AUTHENTICATING forever and keep its worker awake forever. */
static void test_handshake_carrier_fault(void)
{
    struct stub_ctx *sa = calloc(1, sizeof(*sa));
    struct stub_ctx *sb = calloc(1, sizeof(*sb));
    assert(sa && sb);
    sa->remote = sb;
    sb->remote = sa;

    struct node a, b;
    node_start(&a, NODE_A, 0xA1, &STUB_OPS, sa);
    node_start(&b, NODE_B, 0xB2, &STUB_OPS, sb);
    node_bind(&a, &b);
    node_bind(&b, &a);

    struct dmesh_peer_channel *ac = open_channel(&a, &b);
    assert(sb->npending == 1);
    stub_close(sb->pending[0]);
    assert(dmesh_peer_transport_progress(a.rt) == 1);

    assert(ac->state == DMESH_PEER_CLOSED);
    assert(a.fake.resets == 1);
    assert(a.table.refused[DMESH_PEER_REFUSE_TRANSPORT] == 1);
    assert(dmesh_peer_transport_pending(a.rt) == 0);
    struct peer_transport_stats stats;
    dmesh_peer_transport_stats(a.rt, &stats);
    assert(stats.faults == 1 && stats.live == 0);

    node_stop(&a);
    node_stop(&b);
}

int main(void)
{
    test_handshake();
    test_stream_roundtrip();
    test_fin_and_pod_gone();
    test_transport_loss();
    test_key_mismatch();
    test_node_unbound();
    test_simultaneous_open();
    test_send_backpressure();
    test_handshake_deadline();
    test_handshake_carrier_fault();
    printf("peer_transport_test: PASS\n");
    return 0;
}
