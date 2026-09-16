#include "peer_pair_wire.h"
#include "peer_security_wire.h"
#include <stdlib.h>
#include <string.h>

#define OPEN_BODY (2u * DMESH_POD_UID_MAX + DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX + 10u)
#define ACK_ENTRY 20u
struct pending_open { uint32_t token; uint64_t generation; };
struct stream {
    uint64_t generation, rx_sequence;
    unsigned rx_fin, tx_fin;
};
struct peer_pair_wire {
    struct dmesh_peer_pair pair;
    uint64_t next_generation, remote_generation, received_generation;
    uint32_t received_token;
    struct pending_open pending[DMESH_PEER_STREAMS_MAX];
    struct stream streams[2u * DMESH_PEER_STREAMS_MAX];
};
static void put(uint8_t *p, uint64_t n, unsigned bytes)
{ for (unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(n >> (8u * (bytes - i - 1))); }
static uint64_t get(const uint8_t *p, unsigned bytes)
{ uint64_t n = 0; for (unsigned i = 0; i < bytes; i++) n = (n << 8) | p[i]; return n; }
static int text_valid(const char *p, size_t len)
{
    size_t n = strnlen(p, len);
    if (!n || n == len) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126) return 0;
    for (size_t i = n; i < len; i++) if (p[i]) return 0;
    return 1;
}
static struct stream *stream(struct peer_pair_wire *w, uint32_t handle)
{
    uint32_t index = handle & DMESH_PEER_HANDLE_INDEX_MASK;
    if (!index || index > DMESH_PEER_STREAMS_MAX ||
        (handle & ~(DMESH_PEER_HANDLE_OWNER_BIT | DMESH_PEER_HANDLE_INDEX_MASK))) return NULL;
    return &w->streams[index - 1 + ((handle & DMESH_PEER_HANDLE_OWNER_BIT) ? DMESH_PEER_STREAMS_MAX : 0)];
}
struct peer_pair_wire *peer_pair_wire_new(const struct dmesh_peer_pair *p)
{
    if (!p || !peer_sec_nonzero(p->association, sizeof(p->association)) ||
        !p->lane_id || !p->lane_generation) return NULL;
    struct peer_pair_wire *w = calloc(1, sizeof(*w));
    if (w) w->pair = *p;
    return w;
}
void peer_pair_wire_free(struct peer_pair_wire *w) { free(w); }

long peer_pair_wire_encode(struct peer_pair_wire *w, const void *input, size_t len,
                           void *output, size_t capacity)
{
    if (!w || !input || !output || len < sizeof(struct dmesh_peer_msg_header) || capacity < 64) return -1;
    struct dmesh_peer_msg_header in;
    memcpy(&in, input, sizeof(in));
    if (in.version != 1 || in.reserved || len != sizeof(in) + in.length) return -1;
    const uint8_t *body = (const uint8_t *)input + sizeof(in);
    uint8_t *out = (uint8_t *)output + 64;
    struct peer_sec_data_header h = {.lane_id = w->pair.lane_id,
        .lane_generation = w->pair.lane_generation, .handle = in.handle};
    memcpy(h.association, w->pair.association, 16);
    struct stream *s = stream(w, in.handle);
    if (s) h.stream_generation = s->generation;
    switch (in.type) {
    case DMESH_PEER_MSG_STREAM_OPEN: {
        if (in.handle || in.length != sizeof(struct dmesh_peer_stream_open) || capacity < 64 + OPEN_BODY) return -1;
        struct dmesh_peer_stream_open o; memcpy(&o, body, sizeof(o));
        if (!text_valid(o.src_pod_uid, sizeof(o.src_pod_uid)) ||
            !text_valid(o.dst_pod_uid, sizeof(o.dst_pod_uid)) ||
            !text_valid(o.src_service_key, sizeof(o.src_service_key)) || !o.source_token ||
            !o.dst_port || o.reserved || !o.src_generation || w->next_generation == UINT64_MAX) return -1;
        struct pending_open *pending = NULL;
        for (unsigned i = 0; i < DMESH_PEER_STREAMS_MAX; i++) {
            if (w->pending[i].token == o.source_token) return -1;
            if (!w->pending[i].token && !pending) pending = &w->pending[i];
        }
        if (!pending) return -1;
        pending->token = o.source_token; pending->generation = ++w->next_generation;
        h.type = PEER_SEC_STREAM_OPEN; h.handle = o.source_token; h.stream_generation = pending->generation;
        memcpy(out, o.src_pod_uid, sizeof(o.src_pod_uid)); out += sizeof(o.src_pod_uid);
        memcpy(out, o.dst_pod_uid, sizeof(o.dst_pod_uid)); out += sizeof(o.dst_pod_uid);
        memcpy(out, o.src_service_key, sizeof(o.src_service_key)); out += sizeof(o.src_service_key);
        put(out, o.dst_port, 2); put(out + 2, o.src_generation, 8); h.body_len = OPEN_BODY;
        break;
    }
    case DMESH_PEER_MSG_STREAM_OPEN_ACK: {
        if (in.handle || in.length != sizeof(struct dmesh_peer_stream_open_ack) || capacity < 72) return -1;
        struct dmesh_peer_stream_open_ack a; memcpy(&a, body, sizeof(a));
        if (!w->received_token || a.source_token != w->received_token || a.reserved ||
            a.status < 0 || a.status >= DMESH_PEER_REFUSE_MAX ||
            (a.status ? a.handle != 0 : !stream(w, a.handle))) return -1;
        h.type = PEER_SEC_STREAM_OPEN_ACK; h.handle = a.source_token;
        h.stream_generation = w->received_generation; h.body_len = 8;
        put(out, a.handle, 4); put(out + 4, (uint32_t)a.status, 4);
        if (!a.status) *stream(w, a.handle) = (struct stream){.generation = h.stream_generation};
        w->received_token = 0; w->received_generation = 0;
        break;
    }
    case DMESH_PEER_MSG_DATA: {
        if (!s || !s->generation || s->tx_fin || in.length <= sizeof(struct dmesh_peer_data_prefix)) return -1;
        struct dmesh_peer_data_prefix p; memcpy(&p, body, sizeof(p));
        h.type = PEER_SEC_DATA; h.sequence = (uint64_t)p.seq + 1;
        h.body_len = in.length - sizeof(p);
        if (p.reserved || h.body_len > PEER_SEC_DATA_MAX || capacity < 64u + h.body_len) return -1;
        memcpy(out, body + sizeof(p), h.body_len); break;
    }
    case DMESH_PEER_MSG_STREAM_FIN:
        if (!s || !s->generation || s->tx_fin || in.length) return -1;
        h.type = PEER_SEC_STREAM_FIN; s->tx_fin = 1; break;
    case DMESH_PEER_MSG_STREAM_ACK: {
        if (in.handle || !in.length || in.length % sizeof(struct dmesh_peer_ack_entry)) return -1;
        unsigned count = in.length / sizeof(struct dmesh_peer_ack_entry);
        if (count > DMESH_STREAM_ACK_BATCH || capacity < 64 + count * ACK_ENTRY) return -1;
        h.type = PEER_SEC_STREAM_ACK; h.body_len = count * ACK_ENTRY;
        for (unsigned i = 0; i < count; i++) {
            struct dmesh_peer_ack_entry a; memcpy(&a, body + i * sizeof(a), sizeof(a));
            s = stream(w, a.handle);
            if (!s || !s->generation || a.reserved || !a.seq_count || a.seq_count > DMESH_PEER_TX_SLOTS) return -1;
            if (!i) { h.handle = a.handle; h.stream_generation = s->generation; }
            put(out, a.handle, 4); put(out + 4, a.seq_first, 4); put(out + 8, a.seq_count, 4);
            put(out + 12, s->generation, 8); out += ACK_ENTRY;
        }
        break;
    }
    default: return -1;
    }
    if (peer_sec_data_encode(&h, output)) return -1;
    return 64 + h.body_len;
}

long peer_pair_wire_decode(struct peer_pair_wire *w, const void *input, size_t len,
                           uint32_t incarnation, struct dmesh_peer_msg_header *out,
                           void *payload, size_t capacity)
{
    if (!w || !input || !out || !payload || !incarnation) return -1;
    if (len < 64) return 0;
    const uint8_t *bytes = input, *body = bytes + 64;
    uint64_t length = get(bytes + 8, 4);
    if (memcmp(bytes, "DMSD", 4) || length > PEER_SEC_DATA_MAX) return -1;
    if (len < 64 + length) return 0;
    struct peer_sec_data_header h;
    if (peer_sec_data_decode(input, 64 + length, w->pair.association,
        w->pair.lane_id, w->pair.lane_generation, &h)) return -1;
    *out = (struct dmesh_peer_msg_header){.version = 1, .incarnation = incarnation, .handle = h.handle};
    struct stream *s = stream(w, h.handle);
    if (h.type != PEER_SEC_STREAM_OPEN && h.type != PEER_SEC_STREAM_OPEN_ACK &&
        (!s || !s->generation || s->generation != h.stream_generation)) return -1;
    if (h.type != PEER_SEC_DATA && h.sequence) return -1;
    switch (h.type) {
    case PEER_SEC_STREAM_OPEN: {
        if (length != OPEN_BODY || capacity < sizeof(struct dmesh_peer_stream_open) ||
            w->received_token || h.stream_generation <= w->remote_generation) return -1;
        struct dmesh_peer_stream_open o = {.source_token = h.handle};
        memcpy(o.src_pod_uid, body, sizeof(o.src_pod_uid)); body += sizeof(o.src_pod_uid);
        memcpy(o.dst_pod_uid, body, sizeof(o.dst_pod_uid)); body += sizeof(o.dst_pod_uid);
        memcpy(o.src_service_key, body, sizeof(o.src_service_key)); body += sizeof(o.src_service_key);
        o.dst_port = (uint16_t)get(body, 2); o.src_generation = get(body + 2, 8);
        if (!text_valid(o.src_pod_uid, sizeof(o.src_pod_uid)) ||
            !text_valid(o.dst_pod_uid, sizeof(o.dst_pod_uid)) ||
            !text_valid(o.src_service_key, sizeof(o.src_service_key)) || !o.dst_port || !o.src_generation) return -1;
        w->received_token = h.handle;
        w->received_generation = w->remote_generation = h.stream_generation;
        memcpy(payload, &o, sizeof(o)); out->type = DMESH_PEER_MSG_STREAM_OPEN;
        out->handle = 0; out->length = sizeof(o); break;
    }
    case PEER_SEC_STREAM_OPEN_ACK: {
        if (length != 8 || capacity < sizeof(struct dmesh_peer_stream_open_ack)) return -1;
        struct pending_open *pending = NULL;
        for (unsigned i = 0; i < DMESH_PEER_STREAMS_MAX; i++)
            if (w->pending[i].token == h.handle && w->pending[i].generation == h.stream_generation) {
                pending = &w->pending[i]; break;
            }
        if (!pending) return -1;
        struct dmesh_peer_stream_open_ack a = {.source_token = h.handle, .handle = (uint32_t)get(body, 4)};
        uint64_t status = get(body + 4, 4);
        if (status >= DMESH_PEER_REFUSE_MAX || (status ? a.handle != 0 : !stream(w, a.handle))) return -1;
        a.status = (int32_t)status;
        if (!status) *stream(w, a.handle) = (struct stream){.generation = h.stream_generation};
        memset(pending, 0, sizeof(*pending));
        memcpy(payload, &a, sizeof(a)); out->type = DMESH_PEER_MSG_STREAM_OPEN_ACK;
        out->handle = 0; out->length = sizeof(a); break;
    }
    case PEER_SEC_DATA: {
        if (s->rx_fin || h.sequence <= s->rx_sequence || h.sequence > (uint64_t)UINT32_MAX + 1 ||
            capacity < sizeof(struct dmesh_peer_data_prefix) + length) return -1;
        struct dmesh_peer_data_prefix p = {.seq = (uint32_t)(h.sequence - 1)};
        memcpy(payload, &p, sizeof(p)); memcpy((uint8_t *)payload + sizeof(p), body, length);
        s->rx_sequence = h.sequence; out->type = DMESH_PEER_MSG_DATA; out->length = sizeof(p) + length; break;
    }
    case PEER_SEC_STREAM_FIN:
        if (length || s->rx_fin) return -1;
        s->rx_fin = 1; out->type = DMESH_PEER_MSG_STREAM_FIN; break;
    case PEER_SEC_STREAM_ACK: {
        if (!length || length % ACK_ENTRY) return -1;
        unsigned count = length / ACK_ENTRY;
        if (count > DMESH_STREAM_ACK_BATCH || capacity < count * sizeof(struct dmesh_peer_ack_entry)) return -1;
        out->type = DMESH_PEER_MSG_STREAM_ACK; out->handle = 0;
        out->length = count * sizeof(struct dmesh_peer_ack_entry);
        for (unsigned i = 0; i < count; i++) {
            struct dmesh_peer_ack_entry a = {.handle = (uint32_t)get(body, 4),
                .seq_first = (uint32_t)get(body + 4, 4), .seq_count = (uint32_t)get(body + 8, 4)};
            s = stream(w, a.handle);
            if (!s || !s->generation || s->generation != get(body + 12, 8) || !a.seq_count ||
                a.seq_count > DMESH_PEER_TX_SLOTS || (!i && a.handle != h.handle)) return -1;
            memcpy((uint8_t *)payload + i * sizeof(a), &a, sizeof(a)); body += ACK_ENTRY;
        }
        break;
    }
    default: return -1;
    }
    return 64 + length;
}
