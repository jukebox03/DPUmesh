/* Exercise the production channel's Pod identity and authorization boundary.
 * The adapter below reports deterministic hardware readiness; it is not a
 * hardware offload test and cannot be selected by dpumesh_dpu. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "doca/peer_channel.h"
#include "doca/peer_pair_wire.h"

struct conn {
    struct dmesh_peer_pair pair;
    int ready, closed;
    unsigned sends;
    int blocked;
    int paused;
    size_t sent_len, received_len;
    uint8_t sent[DMESH_PEER_FRAME_MAX + 16], received[DMESH_PEER_FRAME_MAX + 16];
};
struct fixture {
    struct dmesh_peer_table table;
    struct conn conns[8];
    unsigned connects, verdicts, delivered, released;
    uint64_t now;
    uint8_t key[32];
    struct dmesh_peer_registration registration;
    int local_live, placed, allow;
    uint32_t source_handle;
};
static int binding(void *ctx, const char *node, const uint8_t **key,
                   uint32_t *ip, uint16_t *port)
{
    struct fixture *f = ctx;
    if (strcmp(node, "node-b")) return 0;
    *key = f->key; *ip = 1; *port = 7000; return 1;
}
static int placement(void *ctx, const char *uid, const char *node)
{
    struct fixture *f = ctx;
    return f->placed && ((!strcmp(uid, "pod-a") && !strcmp(node, "node-a")) ||
        ((!strcmp(uid, "pod-b") || !strcmp(uid, "pod-c")) && !strcmp(node, "node-b")));
}
static int registration(void *ctx, const char *uid, struct dmesh_peer_registration *r)
{
    struct fixture *f = ctx;
    if (!f->local_live || strcmp(uid, "pod-a")) return 0;
    *r = f->registration; return 1;
}
static int32_t local(void *ctx, const char *uid, uint32_t *generation)
{
    struct fixture *f = ctx;
    if (!f->local_live || strcmp(uid, "pod-a")) return -1;
    *generation = 11; return 0;
}
static uint32_t generation(void *ctx, int32_t slot) { (void)ctx; (void)slot; return 11; }
static uint16_t authorize(void *ctx, struct dmesh_peer_channel *c, uint32_t handle,
                           const struct dmesh_peer_stream_open *open, int32_t slot)
{
    struct fixture *f = ctx; (void)c; (void)handle; (void)slot;
    f->verdicts++;
    return f->allow && open->dst_port == 8080 ? 40000 : 0;
}
static int deliver(void *ctx, const struct dmesh_peer_handle *h,
                    const uint8_t *bytes, uint32_t len, uint32_t seq)
{
    struct fixture *f = ctx; (void)h; (void)seq;
    assert(len == 4 && !memcmp(bytes, "data", 4)); f->delivered++; return 1;
}
static void release(void *ctx, uint8_t kind, void *cookie, uint32_t bytes)
{ struct fixture *f = ctx; (void)kind; (void)cookie; f->released += bytes; }
static uint64_t now(void *ctx) { return ((struct fixture *)ctx)->now; }
static void source_opened(void *ctx, struct dmesh_peer_channel *c,
                          uint32_t token, uint32_t handle, int32_t status)
{ (void)c; assert(token == 5 && !status); ((struct fixture *)ctx)->source_handle = handle; }
static int connect_pair(void *ctx, const char *node, const uint8_t key[32],
                         uint32_t ip, uint16_t port, uint32_t incarnation,
                         const struct dmesh_peer_pair *pair, const struct dmesh_peer_stream_open *intent, void **out)
{
    struct fixture *f = ctx; (void)node; (void)key; (void)ip; (void)port; (void)incarnation;
    assert(intent && intent->dst_port == 8080 && !strcmp(intent->src_service_key, "ns/service"));
    assert(f->connects < 8);
    struct conn *c = &f->conns[f->connects++]; c->pair = *pair;
    c->pair.remote = f->registration; c->pair.remote.generation = 22;
    c->pair.remote.nonce[31] = 99;
    c->pair.association[0] = (uint8_t)f->connects;
    c->pair.lane_id = c->pair.lane_generation = 1;
    *out = c; return 0;
}
static int ready(void *ctx, struct dmesh_peer_pair *pair)
{ struct conn *c = ctx; *pair = c->pair; return c->closed ? -1 : c->ready; }
static int admit(void *ctx) { return !((struct conn *)ctx)->paused; }
static int key(void *ctx, uint8_t out[32]) { (void)ctx; memset(out, 3, 32); return 0; }
static long send_frame(void *ctx, const void *buf, size_t len)
{
    struct conn *c = ctx; assert(c->ready == 1 && !c->closed);
    if (c->blocked) return 0;
    assert(len <= sizeof(c->sent)); memcpy(c->sent, buf, len); c->sent_len = len;
    c->sends++; return (long)len;
}
static long recv_frame(void *ctx, void *buf, size_t len)
{
    struct conn *c = ctx; size_t n = c->received_len < len ? c->received_len : len;
    memcpy(buf, c->received, n); c->received_len -= n;
    memmove(c->received, c->received + n, c->received_len); return (long)n;
}
static void close_conn(void *ctx) { ((struct conn *)ctx)->closed++; }
static const struct dmesh_peer_transport TRANSPORT = {
    .connect_pair = connect_pair, .pair_ready = ready, .pair_admit = admit, .peer_key = key,
    .send = send_frame, .recv = recv_frame, .close = close_conn,
};
static const struct dmesh_peer_ops OPS = {
    .node_binding = binding, .pod_on_node = placement, .local_registration = registration,
    .local_pod = local, .pod_generation = generation, .destination_opened = authorize,
    .deliver = deliver, .release = release, .now_ns = now,
    .source_opened = source_opened,
};
static struct fixture *init(void)
{
    struct fixture *f = calloc(1, sizeof(*f)); assert(f);
    f->now = 1; f->local_live = f->placed = f->allow = 1;
    memset(f->key, 3, 32);
    f->registration.daemon[0] = 1; f->registration.nonce[0] = 2;
    f->registration.generation = 7;
    dmesh_peer_table_init(&f->table, "node-a", f->key, &TRANSPORT, f, &OPS, f);
    return f;
}
static struct dmesh_peer_channel *open_pair(struct fixture *f, const char *remote)
{
    enum dmesh_peer_refusal r;
    struct dmesh_peer_stream_open intent = {.dst_port = 8080, .src_generation = f->registration.generation};
    strcpy(intent.src_pod_uid, "pod-a"); strcpy(intent.dst_pod_uid, remote);
    strcpy(intent.src_service_key, "ns/service");
    struct dmesh_peer_channel *c = dmesh_peer_open_stream(&f->table, "node-b", &intent, &r);
    assert(c && !r && c->pair_scoped);
    return c;
}
static void activate(struct fixture *f, struct dmesh_peer_channel *c)
{ ((struct conn *)c->conn)->ready = 1; assert(dmesh_peer_channel_progress(&f->table, c, 1) == 0); assert(c->state == DMESH_PEER_OPEN); }
static struct dmesh_peer_stream_open incoming(const char *source)
{
    struct dmesh_peer_stream_open o = {.dst_port = 8080, .source_token = 1, .src_generation = 22};
    strcpy(o.src_pod_uid, source); strcpy(o.dst_pod_uid, "pod-a"); strcpy(o.src_service_key, "ns/service");
    return o;
}
static void fini(struct fixture *f) { dmesh_peer_table_fini(&f->table); free(f); }

static void isolation_and_policy(void)
{
    struct fixture *f = init(); enum dmesh_peer_refusal r;
    assert(!dmesh_peer_open(&f->table, "node-b", &r) && r == DMESH_PEER_REFUSE_PAIR);
    struct dmesh_peer_stream_open foreign = {.dst_port = 8080, .src_generation = 7};
    strcpy(foreign.src_pod_uid, "foreign"); strcpy(foreign.dst_pod_uid, "pod-b");
    strcpy(foreign.src_service_key, "ns/service");
    assert(!dmesh_peer_open_stream(&f->table, "node-b", &foreign, &r));
    assert(!f->connects);
    struct dmesh_peer_channel *ab = open_pair(f, "pod-b"), *ac = open_pair(f, "pod-c");
    assert(ab != ac && ab->conn != ac->conn && f->connects == 2);
    assert(open_pair(f, "pod-b") == ab && f->connects == 2);
    assert(!dmesh_peer_find(&f->table, "node-b"));
    assert(dmesh_peer_channel_progress(&f->table, ab, 1) == 0);
    assert(ab->state == DMESH_PEER_AUTHENTICATING);
    activate(f, ab); activate(f, ac);
    uint32_t h; struct dmesh_peer_stream_open o = incoming("pod-c");
    assert(dmesh_peer_stream_open(&f->table, ab, ab->incarnation, &o, &h) == DMESH_PEER_REFUSE_PAIR);
    assert(!f->verdicts && !h);
    strcpy(o.src_pod_uid, "pod-b"); o.src_generation++;
    assert(dmesh_peer_stream_open(&f->table, ab, ab->incarnation, &o, &h) == DMESH_PEER_REFUSE_PAIR);
    o.src_generation--; o.dst_port++;
    assert(dmesh_peer_stream_open(&f->table, ab, ab->incarnation, &o, &h) == DMESH_PEER_REFUSE_NO_POD);
    assert(f->verdicts == 1 && !h); o.dst_port--;
    assert(dmesh_peer_stream_open(&f->table, ab, ab->incarnation, &o, &h) == DMESH_PEER_OK);
    assert(f->verdicts == 2 && h);
    assert(dmesh_peer_data(&f->table, ab, ab->incarnation, h, 1, (uint8_t *)"data", 4) == DMESH_PEER_OK);
    assert(f->delivered == 1 && ab->staging_bytes == 4 && !ab->ack_staged);
    /* RNIC delivery is not the host DMA/REV_DONE custody acknowledgement. */
    dmesh_peer_delivered(&f->table, ab, h, 1, 4);
    assert(!ab->staging_bytes && ab->ack_staged == 1);
    f->allow = 0;
    assert(dmesh_peer_stream_open(&f->table, ab, ab->incarnation, &o, &h) == DMESH_PEER_REFUSE_NO_POD);
    assert(f->verdicts == 3);
    fini(f);
}
static void registration_fences(void)
{
    struct fixture *f = init(); struct dmesh_peer_channel *ab = open_pair(f, "pod-b");
    activate(f, ab); struct conn *old = ab->conn;
    struct dmesh_peer_stream_open o = incoming("pod-b");
    strcpy(o.src_pod_uid, "pod-a"); strcpy(o.dst_pod_uid, "pod-b"); o.src_generation = 7;
    assert(!dmesh_peer_stream_request(&f->table, ab, &o));
    o.src_generation++;
    assert(dmesh_peer_stream_request(&f->table, ab, &o) == DMESH_PEER_REFUSE_PAIR);
    /* A change only in the last nonce byte is a distinct registration. */
    f->registration.nonce[31]++;
    assert(dmesh_peer_channel_progress(&f->table, ab, 1) < 0);
    assert(old->closed == 1 && ab->state == DMESH_PEER_CLOSED);
    ab = open_pair(f, "pod-b"); activate(f, ab);
    f->placed = 0; dmesh_peer_table_rebind(&f->table);
    assert(ab->state == DMESH_PEER_CLOSED);
    fini(f);
}
static void incoming_and_retirement(void)
{
    struct fixture *f = init(); struct dmesh_peer_channel *ab = open_pair(f, "pod-b");
    activate(f, ab); struct conn *existing = ab->conn;
    struct conn candidate = {.pair = existing->pair, .ready = 1};
    enum dmesh_peer_refusal r; uint8_t bad[32] = {1};
    assert(!dmesh_peer_accept_pair(&f->table, "node-b", 42, &candidate, bad, &r));
    assert(r == DMESH_PEER_REFUSE_NODE_KEY && candidate.closed == 1 && !existing->closed);
    candidate.closed = 0; candidate.pair.local.nonce[31]++;
    assert(!dmesh_peer_accept_pair(&f->table, "node-b", 42, &candidate, f->key, &r));
    assert(r == DMESH_PEER_REFUSE_REGISTRATION && !existing->closed);
    struct dmesh_peer_channel *ac = open_pair(f, "pod-c"); activate(f, ac);
    assert(dmesh_peer_pod_gone(&f->table, ab, ab->incarnation, "pod-c") == DMESH_PEER_REFUSE_PAIR);
    assert(!dmesh_peer_pod_gone(&f->table, ab, ab->incarnation, "pod-b"));
    assert(ab->state == DMESH_PEER_CLOSED && ac->state == DMESH_PEER_OPEN);
    ((struct conn *)ac->conn)->ready = -1;
    assert(dmesh_peer_channel_progress(&f->table, ac, 1) < 0 && ac->state == DMESH_PEER_CLOSED);
    fini(f);
}
static void channel_v2_roundtrip(void)
{
    struct fixture *f = init(); struct dmesh_peer_channel *c = open_pair(f, "pod-b"); activate(f, c);
    struct conn *transport = c->conn;
    struct peer_pair_wire *peer = peer_pair_wire_new(&c->pair); assert(peer);
    struct dmesh_peer_stream_open o = {.source_token = 5, .dst_port = 8080, .src_generation = 7};
    strcpy(o.src_pod_uid, "pod-a"); strcpy(o.dst_pod_uid, "pod-b"); strcpy(o.src_service_key, "ns/service");
    transport->blocked = 1;
    assert(!dmesh_peer_stream_request(&f->table, c, &o) && c->tx_len > 64 && !transport->sends);
    assert(!memcmp(c->tx_frame, "DMSD", 4));
    transport->blocked = 0;
    assert(dmesh_peer_channel_progress(&f->table, c, 2) == 0 && !c->tx_len && transport->sends == 1);
    uint8_t host_frame[512], body[512]; struct dmesh_peer_msg_header h;
    assert(peer_pair_wire_decode(peer, transport->sent, transport->sent_len, 1, &h, body, sizeof(body)) > 0);
    assert(h.type == DMESH_PEER_MSG_STREAM_OPEN && !memcmp(body, &o, sizeof(o)));
    uint32_t handle = DMESH_PEER_HANDLE_OWNER_BIT | 4096;
    struct dmesh_peer_stream_open_ack ack = {.source_token = 5, .handle = handle};
    long n = dmesh_peer_frame_build(host_frame, sizeof(host_frame), DMESH_PEER_MSG_STREAM_OPEN_ACK, 1, 0, &ack, sizeof(ack));
    n = peer_pair_wire_encode(peer, host_frame, (size_t)n, transport->received, sizeof(transport->received)); assert(n > 0);
    transport->received_len = (size_t)n;
    assert(dmesh_peer_channel_progress(&f->table, c, 2) == 1 && f->source_handle == handle);
    assert(!dmesh_peer_stream_data_send(&f->table, c, handle, 1, (uint8_t *)"data", 4, 0, f));
    assert(c->inflight_bytes == 4 && !f->released);
    assert(peer_pair_wire_decode(peer, transport->sent, transport->sent_len, 1, &h, body, sizeof(body)) > 0);
    assert(h.type == DMESH_PEER_MSG_DATA && !memcmp(body + sizeof(struct dmesh_peer_data_prefix), "data", 4));
    transport->paused = 1;
    assert(dmesh_peer_stream_data_send(&f->table, c, handle, 2, (uint8_t *)"data", 4, 0, f) == DMESH_PEER_REFUSE_INFLIGHT);
    assert(c->inflight_bytes == 4 && !c->tx_len);
    assert(dmesh_peer_stream_request(&f->table, c, &o) == DMESH_PEER_REFUSE_INFLIGHT);
    struct dmesh_peer_ack_entry done = {.handle = handle, .seq_first = 1, .seq_count = 1};
    n = dmesh_peer_frame_build(host_frame, sizeof(host_frame), DMESH_PEER_MSG_STREAM_ACK, 1, 0, &done, sizeof(done));
    n = peer_pair_wire_encode(peer, host_frame, (size_t)n, transport->received, sizeof(transport->received)); assert(n > 0);
    transport->received_len = (size_t)n;
    assert(dmesh_peer_channel_progress(&f->table, c, 2) == 1);
    assert(!c->inflight_bytes && f->released == 4);
    assert(c->state == DMESH_PEER_OPEN); /* drain ACK was allowed while new DATA paused */
    peer_pair_wire_free(peer); fini(f);
}
static void from_peer(struct conn *transport, struct peer_pair_wire *peer,
                       uint8_t type, uint32_t handle, const void *body, size_t length)
{
    uint8_t frame[512];
    long n = dmesh_peer_frame_build(frame, sizeof(frame), type, 1, handle, body, (uint32_t)length);
    assert(n > 0 && !transport->received_len);
    n = peer_pair_wire_encode(peer, frame, (size_t)n, transport->received, sizeof(transport->received));
    assert(n > 0); transport->received_len = (size_t)n;
}
static struct dmesh_peer_msg_header to_peer(struct conn *transport, struct peer_pair_wire *peer, void *body)
{
    struct dmesh_peer_msg_header h;
    assert(peer_pair_wire_decode(peer, transport->sent, transport->sent_len, 1, &h, body, 512) > 0);
    return h;
}
static void handle_waits_for_custody(void)
{
    struct fixture *f = init(); struct dmesh_peer_channel *c = open_pair(f, "pod-b"); activate(f, c);
    struct conn *transport = c->conn;
    struct peer_pair_wire *peer = peer_pair_wire_new(&c->pair); assert(peer);
    struct dmesh_peer_stream_open o = incoming("pod-b");
    from_peer(transport, peer, DMESH_PEER_MSG_STREAM_OPEN, 0, &o, sizeof(o));
    assert(dmesh_peer_channel_progress(&f->table, c, 1) == 1);
    uint8_t body[512]; struct dmesh_peer_msg_header h = to_peer(transport, peer, body);
    assert(h.type == DMESH_PEER_MSG_STREAM_OPEN_ACK);
    struct dmesh_peer_stream_open_ack ack; memcpy(&ack, body, sizeof(ack));
    assert(!ack.status && ack.handle == 1);
    struct { struct dmesh_peer_data_prefix p; char bytes[4]; } data = {.bytes = {'d','a','t','a'}};
    from_peer(transport, peer, DMESH_PEER_MSG_DATA, 1, &data, sizeof(data));
    assert(dmesh_peer_channel_progress(&f->table, c, 1) == 1 && c->staging_bytes == 4);
    assert(!dmesh_peer_stream_data_send(&f->table, c, 1, 0, (uint8_t *)"data", 4, 0, f));
    h = to_peer(transport, peer, body); assert(h.type == DMESH_PEER_MSG_DATA);
    from_peer(transport, peer, DMESH_PEER_MSG_STREAM_FIN, 1, NULL, 0);
    assert(dmesh_peer_channel_progress(&f->table, c, 1) == 1);
    assert(!dmesh_peer_stream_fin_send(&f->table, c, 1));
    h = to_peer(transport, peer, body); assert(h.type == DMESH_PEER_MSG_STREAM_FIN);
    struct dmesh_peer_handle *held = &c->handles[0];
    assert(held->in_use && held->rx_fin && held->tx_fin && held->rx_ack_pending == 1);
    dmesh_peer_delivered(&f->table, c, 1, 0, 4);
    assert(!held->staging_bytes && held->in_use && held->tx_inflight_bytes == 4 && c->ack_staged == 1);
    assert(dmesh_peer_channel_progress(&f->table, c, 1) == 0);
    h = to_peer(transport, peer, body); assert(h.type == DMESH_PEER_MSG_STREAM_ACK);
    assert(held->in_use && !held->rx_ack_pending && held->tx_inflight_bytes == 4 && !f->released);
    struct dmesh_peer_ack_entry done = {.handle = 1, .seq_count = 1};
    from_peer(transport, peer, DMESH_PEER_MSG_STREAM_ACK, 0, &done, sizeof(done));
    assert(dmesh_peer_channel_progress(&f->table, c, 1) == 1);
    assert(!held->in_use && !c->handle_count && !c->inflight_bytes && f->released == 4);
    peer_pair_wire_free(peer); fini(f);
}

static void delayed_custody_blocks_reuse(void)
{
    struct fixture *f = init();
    struct dmesh_peer_channel *c = open_pair(f, "pod-b"); activate(f, c);
    uint32_t incarnation = c->incarnation;
    assert(!dmesh_peer_channel_hold(c, incarnation));
    assert(!dmesh_peer_channel_hold(c, incarnation));
    assert(!dmesh_peer_channel_dma_fenced(NULL, c, incarnation));
    f->registration.nonce[31]++;
    struct dmesh_peer_stream_open in = {.dst_port = 8080, .src_generation = f->registration.generation};
    strcpy(in.src_pod_uid, "pod-a"); strcpy(in.dst_pod_uid, "pod-b"); strcpy(in.src_service_key, "ns/service");
    enum dmesh_peer_refusal why;
    assert(!dmesh_peer_open_stream(&f->table, "node-b", &in, &why));
    assert(c->state == DMESH_PEER_CLOSED && c->incarnation == incarnation && c->local_refs == 2);
    assert(dmesh_peer_table_fini(&f->table) == -1 && c->in_use);
    assert(dmesh_peer_channel_hold(c, incarnation) == -1);
    assert(dmesh_peer_channel_release(c, incarnation + 1) == -1 && c->local_refs == 2);
    assert(!dmesh_peer_channel_release(c, incarnation));
    assert(!dmesh_peer_open_stream(&f->table, "node-b", &in, &why));
    assert(!dmesh_peer_channel_release(c, incarnation));
    assert(dmesh_peer_channel_release(c, incarnation) == -1); /* no underflow */
    assert(dmesh_peer_channel_dma_fenced(NULL, c, incarnation));
    assert(open_pair(f, "pod-b") == c && c->incarnation == incarnation + 1);
    activate(f, c);
    assert(dmesh_peer_channel_release(c, incarnation) == -1);
    fini(f);
}

int main(void)
{
    isolation_and_policy(); registration_fences(); incoming_and_retirement(); channel_v2_roundtrip(); handle_waits_for_custody(); delayed_custody_blocks_reuse();
    puts("peer_pair_channel_test: PASS (pair isolation, registration, policy, readiness, custody)");
    return 0;
}
