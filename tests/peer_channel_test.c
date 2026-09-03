/* The peer channel: what one DPU is allowed to believe from another.
 *
 * Every case here is a clause of the node-authentication gate (CONTROL.md
 * §2-0). A peer is authenticated, not trusted, so the tests are about
 * refusals: a node the generation does not
 * bind, a key that is not the bound one, a handle from a previous
 * incarnation, a destination bound exceeded, a stalled peer at a source, and
 * the two ways a stream's custody ends without delivery.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    uint32_t source_opens;
    uint32_t source_fins;
    uint32_t source_deliveries;
    uint32_t destination_fins;
    uint32_t last_source_token;
    uint32_t last_source_handle;
    int32_t  last_source_status;
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

static void fake_source_opened(void *ctx, struct dmesh_peer_channel *channel,
                               uint32_t source_token, uint32_t handle,
                               int32_t status)
{
    struct fake *f = ctx;
    (void)channel;
    f->source_opens++;
    f->last_source_token = source_token;
    f->last_source_handle = handle;
    f->last_source_status = status;
}

static int fake_source_fin(void *ctx, struct dmesh_peer_channel *channel,
                           uint32_t handle)
{
    struct fake *f = ctx;
    (void)channel;
    if (handle != f->last_source_handle)
        return 0;
    f->source_fins++;
    return 1;
}

static int fake_source_deliver(void *ctx, struct dmesh_peer_channel *channel,
                               uint32_t handle, const uint8_t *bytes,
                               uint32_t len, uint32_t seq)
{
    struct fake *f = ctx;
    (void)channel; (void)bytes; (void)len; (void)seq;
    if (handle != f->last_source_handle)
        return -1;
    f->source_deliveries++;
    return 0;
}

static int fake_destination_fin(void *ctx,
                                const struct dmesh_peer_handle *handle)
{
    struct fake *f = ctx;
    (void)handle;
    f->destination_fins++;
    return 0;
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
    .source_opened = fake_source_opened,
    .source_deliver = fake_source_deliver,
    .destination_fin = fake_destination_fin,
    .source_fin = fake_source_fin,
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
    uint8_t  received[1 << 16];
    size_t   received_len;
    size_t   received_pos;
    size_t   recv_limit;
    int      send_blocked;
    int      key_pending;
    const uint8_t *peer_key;
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
    if (w->send_blocked)
        return 0;
    if (w->sent_len + len > sizeof(w->sent))
        return -1;
    memcpy(w->sent + w->sent_len, buf, len);
    w->sent_len += len;
    return (long)len;
}

static long wire_recv(void *conn, void *buf, size_t len)
{
    struct wire *w = (struct wire *)((char *)conn - offsetof(struct wire, object));
    size_t left = w->received_len - w->received_pos;
    if (left == 0)
        return 0;
    size_t take = left < len ? left : len;
    if (w->recv_limit && take > w->recv_limit)
        take = w->recv_limit;
    memcpy(buf, w->received + w->received_pos, take);
    w->received_pos += take;
    return (long)take;
}

static int wire_peer_key(void *conn, uint8_t key[32])
{
    struct wire *w = (struct wire *)((char *)conn - offsetof(struct wire, object));
    if (w->key_pending)
        return -1;
    memcpy(key, w->peer_key ? w->peer_key : KEY_B, 32);
    return 0;
}

static void wire_close(void *conn)
{
    struct wire *w = (struct wire *)((char *)conn - offsetof(struct wire, object));
    w->closes++;
}

static const struct dmesh_peer_transport WIRE = {
    .connect = wire_connect,
    .peer_key = wire_peer_key,
    .send = wire_send,
    .recv = wire_recv,
    .close = wire_close,
};

static void wire_receive_frame(struct wire *wire, uint8_t type,
                               uint32_t incarnation, uint32_t handle,
                               const void *payload, uint32_t payload_len)
{
    long built = dmesh_peer_frame_build(
        wire->received + wire->received_len,
        sizeof(wire->received) - wire->received_len,
        type, incarnation, handle, payload, payload_len);
    assert(built > 0);
    wire->received_len += (size_t)built;
}

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
    snprintf(open.src_service_key, sizeof(open.src_service_key), "%s", "test/source");
    open.dst_port = 9091;
    open.source_token = 77;
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

    /* FIN is directional. The request FIN delivers EOF but retains the
     * reverse half; the handle leaves only after the reply FIN is sent. */
    assert(dmesh_peer_stream_fin(&table, channel, channel->incarnation, first) ==
           DMESH_PEER_OK);
    assert(channel->handle_count == 2 && fake.poisons == 0 &&
           fake.destination_fins == 1);
    assert(dmesh_peer_stream_fin(&table, channel, channel->incarnation, first) ==
           DMESH_PEER_REFUSE_STATE);
    assert(dmesh_peer_stream_fin_send(&table, channel, first) == DMESH_PEER_OK);
    assert(channel->handle_count == 1);

    /* Source Pod gone: within a node a sweep sees it, across nodes the source
     * has to say, and every handle whose source is that Pod is released. A
     * held asynchronous landing is reclaimed with it; its late completion is
     * ignored without underflowing the channel bound. */
    fake.deliver_holds = 1;
    assert(dmesh_peer_data(&table, channel, channel->incarnation, second, 0,
                           (const uint8_t *)"held", 4) == DMESH_PEER_OK);
    assert(channel->staging_bytes == 4);
    assert(dmesh_peer_pod_gone(&table, channel, channel->incarnation, UID_A) ==
           DMESH_PEER_OK);
    assert(channel->handle_count == 0 && channel->staging_bytes == 0 &&
           fake.poisons == 1);
    dmesh_peer_delivered(&table, channel, second, 0, 4);
    assert(channel->staging_bytes == 0);

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
    uint8_t frame[512];
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
    assert(dmesh_peer_stream_fin_send(&table, channel, handle) == DMESH_PEER_OK);
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

static void test_async_ack_queue_is_bounded(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7, .deliver_holds = 1 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t handles[2] = { 0, 0 };
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation,
                                  &open, &handles[0]) == DMESH_PEER_OK);
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation,
                                  &open, &handles[1]) == DMESH_PEER_OK);

    for (uint32_t i = 0; i < DMESH_STREAM_ACK_BATCH + 2u; i++) {
        uint32_t h = handles[i & 1u];
        uint32_t seq = i / 2u;
        assert(dmesh_peer_data(&table, channel, channel->incarnation, h, seq,
                               (const uint8_t *)"x", 1) == DMESH_PEER_OK);
    }

    /* Keep the ordered writer occupied while completions arrive. Alternating
     * handles prevents run compression and fills all 64 staged entries; the
     * extra two remain in RX slots instead of indexing past the array. */
    wire.send_blocked = 1;
    open.source_token++;
    assert(dmesh_peer_stream_request(&table, channel, &open) == DMESH_PEER_OK);
    for (uint32_t i = 0; i < DMESH_STREAM_ACK_BATCH + 2u; i++)
        dmesh_peer_delivered(&table, channel, handles[i & 1u], i / 2u, 1);
    assert(channel->ack_staged == DMESH_STREAM_ACK_BATCH);
    assert(channel->staging_bytes == 0);

    /* A duplicate completion is ignored exactly; it cannot underflow the
     * staging charge or manufacture a second ACK. */
    dmesh_peer_delivered(&table, channel, handles[0], 0, 1);
    assert(channel->staging_bytes == 0 &&
           channel->ack_staged == DMESH_STREAM_ACK_BATCH);

    wire.send_blocked = 0;
    assert(dmesh_peer_channel_progress(&table, channel, 1) == 0);
    assert(channel->ack_staged == 0);
    assert(channel->rx_free != DMESH_PEER_TX_NIL);
    dmesh_peer_table_fini(&table);
}

static void test_full_duplex_handle_namespaces_do_not_collide(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *channel = open_authenticated(&table);
    struct dmesh_peer_stream_open open = make_open();
    uint32_t local = 0;
    assert(dmesh_peer_stream_open(&table, channel, channel->incarnation,
                                  &open, &local) == DMESH_PEER_OK);
    assert(local == 1u);

    fake.last_source_handle = DMESH_PEER_HANDLE_OWNER_BIT | 1u;
    assert(dmesh_peer_data(&table, channel, channel->incarnation, local, 0,
                           (const uint8_t *)"a", 1) == DMESH_PEER_OK);
    assert(dmesh_peer_data(&table, channel, channel->incarnation,
                           fake.last_source_handle, 0,
                           (const uint8_t *)"b", 1) == DMESH_PEER_OK);
    assert(fake.delivered == 1 && fake.source_deliveries == 1);

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

static void test_source_wire_runtime(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7 };
    struct wire wire = { .recv_limit = 3 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);

    enum dmesh_peer_refusal reason = DMESH_PEER_REFUSE_MAX;
    struct dmesh_peer_channel *channel = dmesh_peer_open(&table, NODE_B, &reason);
    assert(channel && reason == DMESH_PEER_OK);
    assert(channel->state == DMESH_PEER_AUTHENTICATING);
    assert(dmesh_peer_channel_progress(&table, channel, 8) == 0);
    assert(channel->state == DMESH_PEER_OPEN);

    struct dmesh_peer_stream_open open = make_open();
    assert(dmesh_peer_stream_request(&table, channel, &open) == DMESH_PEER_OK);
    struct dmesh_peer_msg_header header;
    const uint8_t *payload = NULL;
    long consumed = dmesh_peer_frame_parse(wire.sent, wire.sent_len, &header, &payload);
    assert(consumed == (long)wire.sent_len);
    assert(header.type == DMESH_PEER_MSG_STREAM_OPEN && header.handle == 0);
    struct dmesh_peer_stream_open decoded;
    memcpy(&decoded, payload, sizeof(decoded));
    assert(decoded.source_token == open.source_token);

    const uint32_t remote_handle = DMESH_PEER_HANDLE_OWNER_BIT | 19u;
    struct dmesh_peer_stream_open_ack open_ack = {
        .source_token = open.source_token,
        .handle = remote_handle,
        .status = DMESH_PEER_OK,
    };
    wire_receive_frame(&wire, DMESH_PEER_MSG_STREAM_OPEN_ACK,
                       channel->incarnation, 0, &open_ack, sizeof(open_ack));
    assert(dmesh_peer_channel_progress(&table, channel, 8) == 1);
    assert(fake.source_opens == 1 && fake.last_source_token == open.source_token);
    assert(fake.last_source_handle == remote_handle &&
           fake.last_source_status == DMESH_PEER_OK);

    struct dmesh_peer_data_prefix reverse_prefix = { .seq = 1 };
    uint8_t reverse_payload[sizeof(reverse_prefix) + 5];
    memcpy(reverse_payload, &reverse_prefix, sizeof(reverse_prefix));
    memcpy(reverse_payload + sizeof(reverse_prefix), "reply", 5);
    wire_receive_frame(&wire, DMESH_PEER_MSG_DATA, channel->incarnation,
                       remote_handle,
                       reverse_payload, sizeof(reverse_payload));
    assert(dmesh_peer_channel_progress(&table, channel, 8) == 1);
    assert(fake.source_deliveries == 1);

    size_t data_offset = wire.sent_len;
    assert(dmesh_peer_stream_data_send(&table, channel, remote_handle, 9,
                                       (const uint8_t *)"payload", 7,
                                       DMESH_PEER_CUSTODY_L4, (void *)1) ==
           DMESH_PEER_OK);
    assert(channel->inflight_bytes == 7);
    consumed = dmesh_peer_frame_parse(wire.sent + data_offset,
                                      wire.sent_len - data_offset,
                                      &header, &payload);
    assert(consumed == (long)(wire.sent_len - data_offset));
    assert(header.type == DMESH_PEER_MSG_DATA &&
           header.handle == remote_handle);
    struct dmesh_peer_data_prefix prefix;
    memcpy(&prefix, payload, sizeof(prefix));
    assert(prefix.seq == 9 && prefix.reserved == 0);
    assert(memcmp(payload + sizeof(prefix), "payload", 7) == 0);

    struct dmesh_peer_ack_entry ack = {
        .handle = remote_handle,
        .seq_first = 9,
        .seq_count = 1,
    };
    wire_receive_frame(&wire, DMESH_PEER_MSG_STREAM_ACK,
                       channel->incarnation, 0, &ack, sizeof(ack));
    assert(dmesh_peer_channel_progress(&table, channel, 8) == 1);
    assert(channel->inflight_bytes == 0 && fake.releases == 1 &&
           fake.released_bytes == 7);

    wire_receive_frame(&wire, DMESH_PEER_MSG_STREAM_FIN,
                       channel->incarnation, remote_handle, NULL, 0);
    assert(dmesh_peer_channel_progress(&table, channel, 8) == 1);
    assert(fake.source_fins == 1);

    dmesh_peer_table_fini(&table);
}

static void test_pending_writer_and_accepted_channel(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7 };
    struct wire wire = { .send_blocked = 1 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);

    enum dmesh_peer_refusal reason = DMESH_PEER_REFUSE_MAX;
    struct dmesh_peer_channel *channel = dmesh_peer_accept(
        &table, NODE_B, 33, &wire.object, KEY_B, &reason);
    assert(channel && reason == DMESH_PEER_OK && channel->state == DMESH_PEER_OPEN);
    assert(channel->incarnation == 33 && wire.connects == 0);

    struct dmesh_peer_stream_open open = make_open();
    assert(dmesh_peer_stream_request(&table, channel, &open) == DMESH_PEER_OK);
    assert(channel->tx_len != 0 && wire.sent_len == 0);
    /* A second writer cannot overwrite the complete frame retained for retry. */
    assert(dmesh_peer_stream_fin_send(
               &table, channel, DMESH_PEER_HANDLE_OWNER_BIT | 1u) ==
           DMESH_PEER_REFUSE_INFLIGHT);
    wire.send_blocked = 0;
    assert(dmesh_peer_channel_progress(&table, channel, 1) == 0);
    assert(channel->tx_len == 0 && wire.sent_len != 0);

    /* Reserved bits are input, not padding: a malformed peer frame tears down
     * the authenticated channel and cannot be interpreted as a later ABI. */
    struct dmesh_peer_stream_open_ack bad = {
        .source_token = open.source_token,
        .handle = 1,
        .status = DMESH_PEER_OK,
        .reserved = 1,
    };
    wire_receive_frame(&wire, DMESH_PEER_MSG_STREAM_OPEN_ACK,
                       channel->incarnation, 0, &bad, sizeof(bad));
    assert(dmesh_peer_channel_progress(&table, channel, 1) == -1);
    assert(channel->state == DMESH_PEER_CLOSED && wire.closes == 1);

    dmesh_peer_table_fini(&table);
}

static void test_simultaneous_open_converges(void)
{
    struct fake fake = { .bind_b = 1, .pod_a_on_b = 1, .local_slot = 3,
                         .pod_generation = 7 };
    struct wire wire = { 0 };
    struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODE_A, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    struct dmesh_peer_channel *outbound = dmesh_peer_open(&table, NODE_B, NULL);
    assert(outbound && outbound->initiated_local);
    enum dmesh_peer_refusal reason = DMESH_PEER_OK;
    assert(dmesh_peer_accept(&table, NODE_B, 91, &wire.object, KEY_B,
                             &reason) == NULL);
    assert(reason == DMESH_PEER_REFUSE_STATE);
    assert(outbound->state == DMESH_PEER_AUTHENTICATING && wire.closes == 1);
    dmesh_peer_table_fini(&table);

    memset(&wire, 0, sizeof(wire));
    dmesh_peer_table_init(&table, NODE_C, NULL, &WIRE, &wire, &FAKE_OPS, &fake);
    outbound = dmesh_peer_open(&table, NODE_B, NULL);
    assert(outbound && outbound->initiated_local);
    struct dmesh_peer_channel *accepted = dmesh_peer_accept(
        &table, NODE_B, 92, &wire.object, KEY_B, &reason);
    assert(accepted == outbound && accepted->state == DMESH_PEER_OPEN);
    assert(!accepted->initiated_local && accepted->incarnation == 92);
    assert(wire.closes == 1);                  /* superseded local transport */
    dmesh_peer_table_fini(&table);
}

/* The node credential is generated once and read on every later boot, and the
 * public half travels to the controller. A key whose derivation is not stable
 * across those two paths would publish one key and present another. */
static void test_node_credential(void)
{
    char path[] = "/tmp/dmesh-node-key-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(unlink(path) == 0);            /* the load generates it */

    uint8_t born[32], reloaded[32], zero[32] = {0};
    char err[160] = {0};
    assert(dmesh_peer_node_key_load(path, born, NULL, err, sizeof(err)) == 0);
    assert(memcmp(born, zero, sizeof(zero)) != 0);

    struct stat st;
    assert(stat(path, &st) == 0);
    assert(st.st_size == 32);
    assert((st.st_mode & 077) == 0);      /* the private half stays private */

    assert(dmesh_peer_node_key_load(path, reloaded, NULL, err, sizeof(err)) == 0);
    assert(memcmp(born, reloaded, sizeof(born)) == 0);

    /* A credential the node did not write is refused rather than adopted. */
    assert(chmod(path, 0644) == 0);
    assert(dmesh_peer_node_key_load(path, reloaded, NULL, err, sizeof(err)) < 0);
    assert(err[0] != '\0');
    assert(unlink(path) == 0);
}

static void test_max_node_names_fit_prologue(void)
{
    char local[DMESH_K8S_NAME_MAX];
    char peer[DMESH_K8S_NAME_MAX];
    memset(local, 'a', sizeof(local) - 1);
    memset(peer, 'b', sizeof(peer) - 1);
    local[sizeof(local) - 1] = '\0';
    peer[sizeof(peer) - 1] = '\0';

    uint8_t prologue[DMESH_PEER_PROLOGUE_MAX];
    int len = dmesh_peer_prologue(local, peer, UINT32_MAX,
                                  prologue, sizeof(prologue));
    assert(len == (int)sizeof(prologue) - 1);
    assert(prologue[len] == '\0');
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
    test_async_ack_queue_is_bounded();
    test_full_duplex_handle_namespaces_do_not_collide();
    test_stalled_peer_does_not_stall_others();
    test_generation_rebind();
    test_source_wire_runtime();
    test_pending_writer_and_accepted_channel();
    test_simultaneous_open_converges();
    test_node_credential();
    test_max_node_names_fit_prologue();
    printf("peer_channel_test: PASS\n");
    return 0;
}
