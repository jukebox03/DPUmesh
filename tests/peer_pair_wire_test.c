#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doca/peer_pair_wire.h"
#include "doca/peer_security_wire.h"

static uint8_t host[DMESH_PEER_FRAME_MAX + 16], wire[DMESH_PEER_FRAME_MAX + 16];
static uint8_t payload[DMESH_PEER_FRAME_MAX], saved[DMESH_PEER_FRAME_MAX + 16];
static long encode(struct peer_pair_wire *w, uint8_t type, uint32_t handle,
                    const void *body, size_t length)
{
    long n = dmesh_peer_frame_build(host, sizeof(host), type, 1, handle, body, (uint32_t)length);
    assert(n > 0); return peer_pair_wire_encode(w, host, (size_t)n, wire, sizeof(wire));
}
static long decode(struct peer_pair_wire *w, size_t len, struct dmesh_peer_msg_header *h)
{ return peer_pair_wire_decode(w, wire, len, 99, h, payload, sizeof(payload)); }
static void handshake(struct peer_pair_wire *a, struct peer_pair_wire *b, uint32_t token, uint32_t handle)
{
    struct dmesh_peer_stream_open o = {.source_token = token, .dst_port = 8080, .src_generation = 7};
    strcpy(o.src_pod_uid, "pod-a"); strcpy(o.dst_pod_uid, "pod-b"); strcpy(o.src_service_key, "ns/service");
    long n = encode(a, DMESH_PEER_MSG_STREAM_OPEN, 0, &o, sizeof(o)); assert(n > 0);
    struct dmesh_peer_msg_header h;
    for (long i = 0; i < n; i++) assert(decode(b, (size_t)i, &h) == 0);
    assert(decode(b, (size_t)n, &h) == n && h.type == DMESH_PEER_MSG_STREAM_OPEN && h.incarnation == 99);
    assert(!memcmp(payload, &o, sizeof(o)));
    struct dmesh_peer_stream_open_ack ack = {.source_token = token, .handle = handle};
    n = encode(b, DMESH_PEER_MSG_STREAM_OPEN_ACK, 0, &ack, sizeof(ack)); assert(n == 72);
    assert(decode(a, (size_t)n, &h) == n && h.type == DMESH_PEER_MSG_STREAM_OPEN_ACK);
    assert(!memcmp(payload, &ack, sizeof(ack)));
}
static void test_roundtrip_and_generation(void)
{
    struct dmesh_peer_pair p = {.lane_id = 1, .lane_generation = 2}; p.association[0] = 3;
    struct peer_pair_wire *a = peer_pair_wire_new(&p), *b = peer_pair_wire_new(&p); assert(a && b);
    uint32_t handle = DMESH_PEER_HANDLE_OWNER_BIT | DMESH_PEER_STREAMS_MAX;
    handshake(a, b, 1, handle); /* handle 4096 is valid, as are 1..4095. */
    size_t body_len = sizeof(struct dmesh_peer_data_prefix) + DMESH_PEER_EXTENT_MAX;
    uint8_t *body = calloc(1, body_len); assert(body);
    for (size_t i = sizeof(struct dmesh_peer_data_prefix); i < body_len; i++) body[i] = (uint8_t)i;
    long n = encode(a, DMESH_PEER_MSG_DATA, handle, body, body_len);
    assert(n == PEER_SEC_DATA_HEADER_LEN + DMESH_PEER_EXTENT_MAX);
    struct dmesh_peer_msg_header h;
    memcpy(saved, wire, (size_t)n);
    assert(decode(b, (size_t)n, &h) == n && h.length == body_len && h.handle == handle);
    assert(!memcmp(payload, body, body_len));
    assert(decode(b, (size_t)n, &h) < 0); /* duplicate DATA */
    long data_n = n;
    struct dmesh_peer_ack_entry ack = {.handle = handle, .seq_count = 1};
    n = encode(b, DMESH_PEER_MSG_STREAM_ACK, 0, &ack, sizeof(ack)); assert(n == 84);
    uint8_t old_ack[84]; memcpy(old_ack, wire, 84);
    assert(decode(a, (size_t)n, &h) == n && !memcmp(payload, &ack, sizeof(ack)));
    n = encode(a, DMESH_PEER_MSG_STREAM_FIN, handle, NULL, 0); assert(n == 64);
    assert(decode(b, (size_t)n, &h) == n);
    assert(encode(a, DMESH_PEER_MSG_DATA, handle, body, body_len) < 0);
    n = encode(b, DMESH_PEER_MSG_STREAM_FIN, handle, NULL, 0); assert(n == 64);
    assert(decode(a, (size_t)n, &h) == n);
    handshake(a, b, 2, handle); /* same handle, new stream generation */
    memcpy(wire, saved, (size_t)data_n); assert(decode(b, (size_t)data_n, &h) < 0);
    memcpy(wire, old_ack, 84); assert(decode(a, 84, &h) < 0);
    n = encode(a, DMESH_PEER_MSG_DATA, handle, body, body_len); assert(n == data_n);
    wire[12] ^= 1; assert(decode(b, (size_t)n, &h) < 0); wire[12] ^= 1; /* other association */
    wire[43] ^= 1; assert(decode(b, (size_t)n, &h) < 0); wire[43] ^= 1; /* old lane generation */
    assert(decode(b, (size_t)n, &h) == n);
    /* ACK batches bind each member's generation, not merely the first one. */
    handshake(a, b, 3, DMESH_PEER_HANDLE_OWNER_BIT | 5);
    struct dmesh_peer_ack_entry entries[2] = {ack, {.handle = DMESH_PEER_HANDLE_OWNER_BIT | 5, .seq_count = 1}};
    n = encode(b, DMESH_PEER_MSG_STREAM_ACK, 0, entries, sizeof(entries)); assert(n == 104);
    wire[103] ^= 1; assert(decode(a, (size_t)n, &h) < 0); wire[103] ^= 1;
    assert(decode(a, (size_t)n, &h) == n && !memcmp(payload, entries, sizeof(entries)));
    free(body); peer_pair_wire_free(a); peer_pair_wire_free(b);
}
int main(void)
{
    test_roundtrip_and_generation();
    puts("peer_pair_wire_test: PASS (canonical v2, 64 KiB, stream reuse, stale DATA/ACK, batched ACK generations)");
    return 0;
}
