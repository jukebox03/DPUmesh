/* The peer channel: what one DPU is allowed to believe from another.
 *
 * Every case here is a clause of S7's gate. A peer is authenticated, not
 * trusted, so the tests are about refusals: a node the generation does not
 * bind, a key that is not the bound one, a handle from a previous
 * incarnation, a destination bound exceeded, a stalled peer at a source, and
 * the two ways a stream's custody ends without delivery.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "doca/peer_channel.h"

#define NODE_A "rapids4"
#define NODE_B "rapids5"
#define NODE_C "rapids6"
#define UID_A "12345678-1234-1234-1234-123456789abc"
#define UID_B "abcdef01-2345-6789-abcd-ef0123456789"

static uint8_t KEY_B[32];
static uint8_t KEY_C[32];
static uint8_t KEY_OTHER[32];

/* ---- the node this DPU pretends to be ---------------------------------- */

struct fake {
    uint64_t now;
    int      bind_b;
    int      bind_c;
    const uint8_t *bind_b_key;           /* NULL = KEY_B */
    int      pod_a_on_b;
    int32_t  local_slot;
    uint32_t pod_generation;
    int      deliver_holds;
    uint32_t delivered;
    uint64_t released_bytes;
    uint32_t releases;
    uint32_t poisons;
    uint32_t events;
    char     last_event[64];
};

static int fake_node_binding(void *ctx, const char *node, const uint8_t **key,
                             uint32_t *ip_be, uint16_t *port)
{
    struct fake *f = ctx;
    if (strcmp(node, NODE_B) == 0 && f->bind_b) {
        *key = f->bind_b_key ? f->bind_b_key : KEY_B;
        *ip_be = 0xC0A80102u;
        *port = 4791;
        return 1;
    }
    if (strcmp(node, NODE_C) == 0 && f->bind_c) {
        *key = KEY_C;
        *ip_be = 0xC0A80103u;
        *port = 4791;
        return 1;
    }
    return 0;
}

static int fake_pod_on_node(void *ctx, const char *pod_uid, const char *node)
{
    struct fake *f = ctx;
    return f->pod_a_on_b && strcmp(pod_uid, UID_A) == 0 && strcmp(node, NODE_B) == 0;
}

static int32_t fake_local_pod(void *ctx, const char *pod_uid, uint32_t *generation)
{
    struct fake *f = ctx;
    if (strcmp(pod_uid, UID_B) != 0)
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
    (void)handle; (void)bytes; (void)len; (void)seq;
    if (f->deliver_holds)
        return 1;
    f->delivered++;
    return 0;
}

static void fake_release(void *ctx, uint8_t kind, void *cookie, uint32_t bytes)
{
    struct fake *f = ctx;
    (void)kind; (void)cookie;
    f->released_bytes += bytes;
    f->releases++;
}

static void fake_poison(void *ctx, const struct dmesh_peer_handle *handle, const char *why)
{
    struct fake *f = ctx;
    (void)handle; (void)why;
    f->poisons++;
}

static void fake_event(void *ctx, const char *reason)
{
    struct fake *f = ctx;
    f->events++;
    snprintf(f->last_event, sizeof(f->last_event), "%s", reason);
}

static uint64_t fake_now(void *ctx)
{
    struct fake *f = ctx;
    return f->now;
}

static const struct dmesh_peer_ops FAKE_OPS = {
    .node_binding = fake_node_binding,
    .pod_on_node = fake_pod_on_node,
    .local_pod = fake_local_pod,
    .upstream_for = fake_upstream_for,
    .pod_generation = fake_pod_generation,
    .deliver = fake_deliver,
    .release = fake_release,
    .poison = fake_poison,
    .event = fake_event,
    .now_ns = fake_now,
};

/* ---- a transport that only records ------------------------------------- */

struct wire {
    int      connects;
    uint8_t  prologue[DMESH_PEER_PROLOGUE_MAX];
    size_t   prologue_len;
    uint8_t  sent[1 << 16];
    size_t   sent_len;
    int      closes;
    int      object;
};

static int wire_connect(void *ctx, uint32_t ip_be, uint16_t port,
                        const uint8_t *prologue, size_t prologue_len, void **conn)
{
    struct wire *w = ctx;
    (void)ip_be; (void)port;
    w->connects++;
    w->prologue_len = prologue_len < sizeof(w->prologue) ? prologue_len : sizeof(w->prologue);
    memcpy(w->prologue, prologue, w->prologue_len);
    *conn = &w->object;
    return 0;
}

static long wire_send(void *conn, const void *buf, size_t len)
{
    struct wire *w = (struct wire *)((char *)conn - offsetof(struct wire, object));
    if (w->sent_len + len > sizeof(w->sent))
        return -1;
    memcpy(w->sent + w->sent_len, buf, len);
    w->sent_len += len;
    return (long)len;
}

static void wire_close(void *conn)
{
    struct wire *w = (struct wire *)((char *)conn - offsetof(struct wire, object));
    w->closes++;
}

static const struct dmesh_peer_transport WIRE = {
    .connect = wire_connect,
    .send = wire_send,
    .close = wire_close,
};

/* ------------------------------------------------------------------------ */

static struct dmesh_peer_channel *open_authenticated(struct dmesh_peer_table *table)
{
    enum dmesh_peer_refusal reason = DMESH_PEER_REFUSE_MAX;
    struct dmesh_peer_channel *channel = dmesh_peer_open(table, NODE_B, &reason);
    assert(channel && reason == DMESH_PEER_OK);
    assert(dmesh_peer_authenticated(table, channel, KEY_B) == DMESH_PEER_OK);
    assert(channel->state == DMESH_PEER_OPEN);
    return channel;
}

static struct dmesh_peer_stream_open make_open(void)
{
    struct dmesh_peer_stream_open open;
    memset(&open, 0, sizeof(open));
    snprintf(open.src_pod_uid, sizeof(open.src_pod_uid), "%s", UID_A);
    snprintf(open.dst_pod_uid, sizeof(open.dst_pod_uid), "%s", UID_B);
    open.dst_port = 9091;
    open.src_generation = 42;
    return open;
}

static void test_binding_and_key(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);

    /* A channel to a node the generation does not bind is refused: there is no
     * key to authenticate it against and no address worth believing. */
    enum dmesh_peer_refusal reason = DMESH_PEER_OK;
    assert(dmesh_peer_open(&table, "rapids9", &reason) == NULL);
    assert(reason == DMESH_PEER_REFUSE_NODE_UNBOUND);
    assert(strcmp(fake.last_event, "node-unbound") == 0);
    assert(wire.connects == 0);

    /* The incarnation is bound into the handshake, so a completed handshake
     * authenticates the incarnation its handles will carry. */
    struct dmesh_peer_channel *channel = dmesh_peer_open(&table, NODE_B, &reason);
    assert(channel && reason == DMESH_PEER_OK && wire.connects == 1);
    assert(memmem(wire.prologue, wire.prologue_len, NODE_A, strlen(NODE_A)));
    assert(memmem(wire.prologue, wire.prologue_len, NODE_B, strlen(NODE_B)));
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "\n%u\n", channel->incarnation);
    assert(memmem(wire.prologue, wire.prologue_len, stamp, strlen(stamp)));

    /* The one rule on top of the stock protocol: a key that differs from the
     * one the generation binds refuses the channel. */
    assert(dmesh_peer_authenticated(&table, channel, KEY_OTHER) == DMESH_PEER_REFUSE_NODE_KEY);
    assert(channel->state == DMESH_PEER_CLOSED);
    assert(table.refused[DMESH_PEER_REFUSE_NODE_KEY] == 1);

    dmesh_peer_table_fini(&table);
}

static void test_identity_check_and_bounds(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 0, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t handle = 0;

    /* The identity check is a lookup in the signed table: a source claiming a
     * Pod the generation does not place on its node is refused. */
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_REFUSE_NOT_ON_PEER);
    fake.pod_a_on_b = 1;

    /* A destination with no live registration for the named Pod is refused
     * too, and refused by reason rather than silently. */
    fake.local_slot = -1;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_REFUSE_NO_POD);
    fake.local_slot = 3;

    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_OK);
    assert(handle != 0 && channel->handle_count == 1);

    /* A frame from a previous incarnation is refused rather than applied to
     * the current one. */
    assert(dmesh_peer_data(&table, channel, channel->incarnation - 1, handle, 0,
                           (const uint8_t *)"x", 1) == DMESH_PEER_REFUSE_INCARNATION);
    /* So is a handle the peer invented. */
    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle + 1000, 0,
                           (const uint8_t *)"x", 1) == DMESH_PEER_REFUSE_HANDLE);
    /* And an extent larger than the unit that crosses. */
    static uint8_t big[DMESH_PEER_EXTENT_MAX + 1];
    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, 0, big,
                           sizeof(big)) == DMESH_PEER_REFUSE_MALFORMED);

    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, 0,
                           (const uint8_t *)"hello", 5) == DMESH_PEER_OK);
    assert(fake.delivered == 1);

    /* A destination slot re-tenanted since the open ends the stream: the bytes
     * would land in a Pod that never opened it. */
    fake.pod_generation = 8;
    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, 1,
                           (const uint8_t *)"hello", 5) == DMESH_PEER_REFUSE_HANDLE);
    assert(channel->handle_count == 0);

    dmesh_peer_table_fini(&table);
}

static void test_open_rate_and_stream_bound(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();

    /* A flood of opens from one peer costs a bounded number of lookups: the
     * token bucket refuses beyond PEER_OPEN_RATE per second and the peer
     * becomes slow rather than disconnected. */
    uint32_t admitted = 0, handle = 0;
    for (uint32_t i = 0; i < DMESH_PEER_OPEN_RATE + 64; i++)
        if (dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
            DMESH_PEER_OK)
            admitted++;
    assert(admitted == DMESH_PEER_OPEN_RATE);
    assert(channel->refused[DMESH_PEER_REFUSE_RATE] == 64);
    assert(channel->state == DMESH_PEER_OPEN);       /* not torn down */

    /* Time passes; the bucket refills and the honest streams get through. */
    fake.now += 2ull * 1000000000ull;
    while (channel->handle_count < DMESH_PEER_STREAMS_MAX) {
        fake.now += 1000000ull;                      /* one millisecond of tokens */
        (void)dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle);
    }
    assert(channel->handle_count == DMESH_PEER_STREAMS_MAX);
    fake.now += 2ull * 1000000000ull;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_REFUSE_STREAMS);

    dmesh_peer_table_fini(&table);
}

static void test_source_custody(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);

    /* An L4 sender's capacity returns only after the bytes reach the
     * destination Pod's mapping, which is what STREAM_ACK reports. */
    for (uint32_t seq = 0; seq < 4; seq++)
        assert(dmesh_peer_tx_charge(channel, 1, seq, 1024, DMESH_PEER_CUSTODY_L4,
                                    (void *)(uintptr_t)(seq + 1)) == DMESH_PEER_OK);
    assert(channel->inflight_bytes == 4096 && fake.releases == 0);

    struct dmesh_peer_ack_entry entry = { .handle = 1, .seq_first = 0, .seq_count = 2 };
    assert(dmesh_peer_tx_ack(&table, channel, channel->incarnation, &entry) == DMESH_PEER_OK);
    assert(fake.releases == 2 && channel->inflight_bytes == 2048);

    /* An acknowledgement from a previous connection names extents this one has
     * already released, so it is refused on this path too. */
    assert(dmesh_peer_tx_ack(&table, channel, channel->incarnation - 1, &entry) ==
           DMESH_PEER_REFUSE_INCARNATION);

    /* A stalled peer pins no more than its source bound, and the bound stalls
     * that peer's streams rather than the shared arena. */
    uint32_t charged = 2048;
    while (dmesh_peer_tx_charge(channel, 2, charged, DMESH_PEER_EXTENT_MAX,
                                DMESH_PEER_CUSTODY_L7, (void *)(uintptr_t)charged) ==
           DMESH_PEER_OK)
        charged++;
    assert(channel->stalled == 1);
    assert(channel->inflight_bytes <= DMESH_PEER_TX_INFLIGHT_MAX);

    /* Losing the channel releases every extent it pinned: nothing will ever
     * acknowledge bytes on a connection that no longer exists. */
    uint64_t held = channel->inflight_bytes;
    uint32_t before = fake.releases;
    dmesh_peer_reset(&table, channel, "transport fault");
    assert(channel->inflight_bytes == 0);
    assert(fake.releases > before);
    assert(fake.released_bytes >= held);
    assert(channel->state == DMESH_PEER_CLOSED);

    dmesh_peer_table_fini(&table);
}

static void test_release_paths(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t first = 0, second = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &first) ==
           DMESH_PEER_OK);
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &second) ==
           DMESH_PEER_OK);
    assert(channel->handle_count == 2);

    /* Normal close: the destination releases the handle and nothing is
     * poisoned. */
    assert(dmesh_peer_stream_fin(&table, channel, channel->incarnation, first) ==
           DMESH_PEER_OK);
    assert(channel->handle_count == 1 && fake.poisons == 0);
    assert(dmesh_peer_stream_fin(&table, channel, channel->incarnation, first) ==
           DMESH_PEER_REFUSE_HANDLE);

    /* Source Pod gone: within a node a sweep sees it, across nodes the source
     * has to say, and every handle whose source is that Pod is released. */
    assert(dmesh_peer_pod_gone(&table, channel, channel->incarnation, UID_A) ==
           DMESH_PEER_OK);
    assert(channel->handle_count == 0 && fake.poisons == 1);

    /* A reconnection invalidates every handle from the previous incarnation. */
    uint32_t handle = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_OK);
    uint32_t previous = channel->incarnation;
    dmesh_peer_reset(&table, channel, "reconnecting");
    struct dmesh_peer_channel *again = dmesh_peer_open(&table, NODE_B, NULL);
    assert(again == channel && channel->incarnation != previous);
    assert(dmesh_peer_authenticated(&table, channel, KEY_B) == DMESH_PEER_OK);
    assert(dmesh_peer_data(&table, channel, previous, handle, 0,
                           (const uint8_t *)"x", 1) == DMESH_PEER_REFUSE_INCARNATION);
    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, 0,
                           (const uint8_t *)"x", 1) == DMESH_PEER_REFUSE_HANDLE);

    dmesh_peer_table_fini(&table);
}

static void test_acknowledgements_batch(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t handle = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_OK);

    /* Consecutive sequences on one handle collapse into one run, which is the
     * encoding the reverse ring already uses. */
    for (uint32_t seq = 0; seq < 8; seq++)
        assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, seq,
                               (const uint8_t *)"abcd", 4) == DMESH_PEER_OK);
    assert(channel->ack_staged == 1);
    assert(channel->ack_stage[0].seq_first == 0 && channel->ack_stage[0].seq_count == 8);

    assert(dmesh_peer_ack_flush(&table, channel) == 0);
    struct dmesh_peer_msg_header header;
    const uint8_t *payload = NULL;
    long consumed = dmesh_peer_frame_parse(wire.sent, wire.sent_len, &header, &payload);
    assert(consumed == (long)(sizeof(header) + sizeof(struct dmesh_peer_ack_entry)));
    assert(header.type == DMESH_PEER_MSG_STREAM_ACK);
    assert(header.incarnation == channel->incarnation);
    struct dmesh_peer_ack_entry entry;
    memcpy(&entry, payload, sizeof(entry));
    assert(entry.handle == handle && entry.seq_first == 0 && entry.seq_count == 8);

    dmesh_peer_table_fini(&table);
}

static void test_framing(void)
{
    uint8_t frame[256];
    struct dmesh_peer_stream_open open = make_open();
    long built = dmesh_peer_frame_build(frame, sizeof(frame), DMESH_PEER_MSG_STREAM_OPEN,
                                        9, 0, &open, sizeof(open));
    assert(built == (long)(sizeof(struct dmesh_peer_msg_header) + sizeof(open)));

    struct dmesh_peer_msg_header header;
    const uint8_t *payload = NULL;
    /* A partial frame is not an error: the transport delivers bytes, and a
     * frame that has not arrived yet is simply not one. */
    assert(dmesh_peer_frame_parse(frame, 4, &header, &payload) == 0);
    assert(dmesh_peer_frame_parse(frame, (size_t)built - 1, &header, &payload) == 0);
    assert(dmesh_peer_frame_parse(frame, (size_t)built, &header, &payload) == built);
    assert(header.type == DMESH_PEER_MSG_STREAM_OPEN && header.incarnation == 9);

    /* What never will be a frame is refused rather than waited on. */
    uint8_t bad[sizeof(struct dmesh_peer_msg_header)];
    memcpy(bad, frame, sizeof(bad));
    bad[1] = DMESH_PEER_WIRE_VERSION + 1;
    assert(dmesh_peer_frame_parse(bad, sizeof(bad), &header, &payload) == -1);
    memcpy(bad, frame, sizeof(bad));
    bad[0] = DMESH_PEER_MSG_DATA + 1;
    assert(dmesh_peer_frame_parse(bad, sizeof(bad), &header, &payload) == -1);
    memcpy(bad, frame, sizeof(bad));
    struct dmesh_peer_msg_header oversized;
    memcpy(&oversized, bad, sizeof(oversized));
    oversized.length = DMESH_PEER_FRAME_MAX + 1;
    memcpy(bad, &oversized, sizeof(oversized));
    assert(dmesh_peer_frame_parse(bad, sizeof(bad), &header, &payload) == -1);
}

static void test_idle_eviction(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t handle = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_OK);

    /* A channel carrying streams is not idle, however long it has been quiet. */
    fake.now += 10ull * DMESH_CHANNEL_IDLE_NS;
    dmesh_peer_evict_idle(&table);
    assert(table.evictions == 0 && channel->state == DMESH_PEER_OPEN);

    assert(dmesh_peer_stream_fin(&table, channel, channel->incarnation, handle) ==
           DMESH_PEER_OK);
    fake.now += 10ull * DMESH_CHANNEL_IDLE_NS;
    dmesh_peer_evict_idle(&table);
    assert(table.evictions == 1);
    assert(wire.closes == 1);

    dmesh_peer_table_fini(&table);
}

static void test_staging_bound_under_held_delivery(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7, .deliver_holds = 1 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t handle = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_OK);

    /* A held delivery keeps the staging charge and defers the acknowledgement,
     * so a peer whose extents are not landing fills PEER_STAGING_MAX and is
     * refused beyond it rather than admitted without bound. */
    static uint8_t extent[DMESH_PEER_EXTENT_MAX];
    uint32_t fits = DMESH_PEER_STAGING_MAX / DMESH_PEER_EXTENT_MAX;
    for (uint32_t seq = 0; seq < fits; seq++)
        assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, seq,
                               extent, sizeof(extent)) == DMESH_PEER_OK);
    assert(channel->staging_bytes == DMESH_PEER_STAGING_MAX);
    assert(channel->ack_staged == 0);            /* nothing landed, nothing ACKed */
    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, fits,
                           extent, sizeof(extent)) == DMESH_PEER_REFUSE_STAGING);
    assert(channel->refused[DMESH_PEER_REFUSE_STAGING] == 1);

    /* A landing releases exactly its extent and is what stages the ACK. */
    dmesh_peer_delivered(&table, channel, handle, 0, DMESH_PEER_EXTENT_MAX);
    assert(channel->staging_bytes == DMESH_PEER_STAGING_MAX - DMESH_PEER_EXTENT_MAX);
    assert(channel->ack_staged == 1 && channel->ack_stage[0].seq_first == 0 &&
           channel->ack_stage[0].seq_count == 1);
    assert(dmesh_peer_data(&table, channel, channel->incarnation, handle, fits,
                           extent, sizeof(extent)) == DMESH_PEER_OK);

    dmesh_peer_table_fini(&table);
}

static void test_stalled_peer_does_not_stall_others(void)
{
    struct fake fake = { .bind_b = 1, .bind_c = 1, .pod_a_on_b = 1,
                         .local_slot = 3, .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *stalled = open_authenticated(&table);
    enum dmesh_peer_refusal reason = DMESH_PEER_REFUSE_MAX;
    struct dmesh_peer_channel *other = dmesh_peer_open(&table, NODE_C, &reason);
    assert(other && reason == DMESH_PEER_OK);
    assert(dmesh_peer_authenticated(&table, other, KEY_C) == DMESH_PEER_OK);

    /* The bound is per peer: a dead peer's un-ACKed bytes cap what it pins,
     * and a healthy peer keeps charging as if nothing happened. */
    uint32_t seq = 0;
    while (dmesh_peer_tx_charge(stalled, 1, seq, DMESH_PEER_EXTENT_MAX,
                                DMESH_PEER_CUSTODY_L7, (void *)(uintptr_t)(seq + 1)) ==
           DMESH_PEER_OK)
        seq++;
    assert(stalled->stalled == 1);
    assert(dmesh_peer_tx_charge(other, 1, 0, DMESH_PEER_EXTENT_MAX,
                                DMESH_PEER_CUSTODY_L4, (void *)1) == DMESH_PEER_OK);
    assert(other->stalled == 0 && other->inflight_bytes == DMESH_PEER_EXTENT_MAX);

    dmesh_peer_table_fini(&table);
}

static void test_generation_rebind(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t handle = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation, &open, &handle) ==
           DMESH_PEER_OK);

    /* An adoption that drops the peer ends its channel: the streams it carried
     * are poisoned, counted, and nothing keeps speaking to an unbound node. */
    fake.bind_b = 0;
    dmesh_peer_table_rebind(&table);
    assert(channel->state == DMESH_PEER_CLOSED && channel->handle_count == 0);
    assert(fake.poisons == 1);
    assert(table.refused[DMESH_PEER_REFUSE_NODE_UNBOUND] == 1);

    /* An adoption that re-keys the peer ends the channel the old key
     * authenticated; the next open authenticates against the new binding. */
    fake.bind_b = 1;
    struct dmesh_peer_channel *again = dmesh_peer_open(&table, NODE_B, NULL);
    assert(again == channel);
    assert(dmesh_peer_authenticated(&table, channel, KEY_B) == DMESH_PEER_OK);
    fake.bind_b_key = KEY_OTHER;
    dmesh_peer_table_rebind(&table);
    assert(channel->state == DMESH_PEER_CLOSED);
    assert(table.refused[DMESH_PEER_REFUSE_NODE_KEY] == 1);

    dmesh_peer_table_fini(&table);
}

int main(void)
{
    for (int i = 0; i < 32; i++) {
        KEY_B[i] = (uint8_t)(0x10 + i);
        KEY_C[i] = (uint8_t)(0x50 + i);
        KEY_OTHER[i] = (uint8_t)(0x90 + i);
    }
    test_binding_and_key();
    test_identity_check_and_bounds();
    test_open_rate_and_stream_bound();
    test_source_custody();
    test_release_paths();
    test_acknowledgements_batch();
    test_framing();
    test_idle_eviction();
    test_staging_bound_under_held_delivery();
    test_stalled_peer_does_not_stall_others();
    test_generation_rebind();
    printf("peer_channel_test: PASS\n");
    return 0;
}
