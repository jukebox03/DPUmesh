#ifndef DMESH_PEER_CHANNEL_H
#define DMESH_PEER_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "comch_common.h"

/* The DPU-to-DPU channel: what one DPU is allowed to believe from another.
 *
 * A peer DPU is authenticated, not trusted. Everything on this wire is input —
 * stream opens, lengths, handles, and the rate of all of them — so every rule
 * here is a bound or a check, and every refusal is counted by reason.
 *
 * The transport is deliberately not here. The RDMA layer supplies ordered,
 * reliable delivery within a handle and a mutually authenticated key
 * agreement; this module supplies the one rule that stock authentication
 * cannot know — that the peer's static key must be the one the held topology
 * generation binds to the node name it claims — together with the handle
 * namespace, the custody that crosses the boundary, and the lifetime that ends
 * both. `struct dmesh_peer_transport` is the seam between them.
 */

/* Parameters. The two that interact are the idle eviction and the generation
 * interval: a channel evicted and reopened between two generations pays
 * authentication twice. */
#define DMESH_CHANNEL_IDLE_NS       (60ull * 1000000000ull)
#define DMESH_CHANNEL_MAX           256u
#define DMESH_PEER_STREAMS_MAX      4096u
#define DMESH_PEER_STAGING_MAX      (16u * 1024u * 1024u)
#define DMESH_PEER_OPEN_RATE        1000u
#define DMESH_PEER_TX_INFLIGHT_MAX  (16u * 1024u * 1024u)
#define DMESH_STREAM_ACK_BATCH      64u
/* One un-ACKed slot per extent a source may have in flight to one peer. The
 * byte bound is what actually stalls a peer; this is the slot bound that makes
 * the smallest extents finite too. */
#define DMESH_PEER_TX_SLOTS         8192u
#define DMESH_PEER_TX_NIL           0xFFFFFFFFu
/* Handles are allocated independently at both ends of a full-duplex channel.
 * The owner bit gives the two allocators disjoint wire namespaces without
 * halving either one's 4096-stream capacity. Which node owns the high half is
 * deterministic from the authenticated node names. */
#define DMESH_PEER_HANDLE_OWNER_BIT  0x80000000u
#define DMESH_PEER_HANDLE_INDEX_MASK 0x00000FFFu
/* The unit that crosses is a staging extent, and nothing is re-segmented at
 * the boundary, so the largest frame is the arrival coalescing bound. */
#define DMESH_PEER_EXTENT_MAX       (64u * 1024u)
#define DMESH_PEER_FRAME_MAX        (DMESH_PEER_EXTENT_MAX + 64u)
/* The prologue the stock handshake is bound to: what a completed handshake
 * therefore authenticates. */
#define DMESH_PEER_PROLOGUE_MAX     288u

/* ---- wire ------------------------------------------------------------- */

enum dmesh_peer_msg_type {
    DMESH_PEER_MSG_INVALID         = 0,
    DMESH_PEER_MSG_STREAM_OPEN     = 1,  /* source → destination */
    DMESH_PEER_MSG_STREAM_OPEN_ACK = 2,  /* destination → source */
    DMESH_PEER_MSG_STREAM_FIN      = 3,  /* either */
    DMESH_PEER_MSG_STREAM_ACK      = 4,  /* destination → source */
    DMESH_PEER_MSG_POD_GONE        = 5,  /* source → destination */
    DMESH_PEER_MSG_DATA            = 6,  /* a handle and bytes; nothing else */
};

#define DMESH_PEER_WIRE_VERSION 1u

/* Every frame carries the channel incarnation, and it is matched on every
 * path: an asynchronous completion outlives the state that named it, which is
 * what `dma_generation` already guards for a Pod slot. */
struct dmesh_peer_msg_header {
    uint8_t  type;
    uint8_t  version;
    uint16_t reserved;
    uint32_t length;        /* payload bytes following this header */
    uint32_t incarnation;
    uint32_t handle;        /* 0 on frames that name no stream */
};
_Static_assert(sizeof(struct dmesh_peer_msg_header) == 16,
               "dmesh_peer_msg_header wire ABI drift");

struct dmesh_peer_stream_open {
    char     src_pod_uid[DMESH_POD_UID_MAX];
    char     dst_pod_uid[DMESH_POD_UID_MAX];
    /* Cluster-qualified Service asserted by the source registration. The
     * destination verifies that the signed generation places src_pod_uid in
     * this Service before using its protection class. */
    char     src_service_key[DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX];
    uint16_t dst_port;
    uint16_t reserved;
    /* Source-local correlation token. The destination never interprets it;
     * STREAM_OPEN_ACK returns it so several opens may be in flight together. */
    uint32_t source_token;
    /* Not an identity input. It lets a destination notice it is behind and
     * adopt sooner, which shortens ordinary generation skew. */
    uint64_t src_generation;
};
_Static_assert(sizeof(struct dmesh_peer_stream_open) == 272,
               "dmesh_peer_stream_open wire ABI drift");

struct dmesh_peer_stream_open_ack {
    uint32_t source_token;
    uint32_t handle;
    int32_t  status;        /* 0, or enum dmesh_peer_refusal */
    uint32_t reserved;
};
_Static_assert(sizeof(struct dmesh_peer_stream_open_ack) == 16,
               "dmesh_peer_stream_open_ack wire ABI drift");

struct dmesh_peer_data_prefix {
    uint32_t seq;
    uint32_t reserved;
};
_Static_assert(sizeof(struct dmesh_peer_data_prefix) == 8,
               "dmesh_peer_data_prefix wire ABI drift");

/* One entry names a run of consecutive sequences rather than one, which is the
 * encoding `dmesh_tx_ack_entry` already uses on the reverse ring: one
 * acknowledgement per released extent rather than one per transport unit. */
struct dmesh_peer_ack_entry {
    uint32_t handle;
    uint32_t seq_first;
    uint32_t seq_count;
    uint32_t reserved;
};
_Static_assert(sizeof(struct dmesh_peer_ack_entry) == 16,
               "dmesh_peer_ack_entry wire ABI drift");

struct dmesh_peer_pod_gone {
    char pod_uid[DMESH_POD_UID_MAX];
};
_Static_assert(sizeof(struct dmesh_peer_pod_gone) == 64,
               "dmesh_peer_pod_gone wire ABI drift");

/* Every refusal is counted by reason; nothing is refused silently. */
enum dmesh_peer_refusal {
    DMESH_PEER_OK = 0,
    DMESH_PEER_REFUSE_INCARNATION,   /* a frame from a previous connection */
    DMESH_PEER_REFUSE_STATE,         /* the channel is not open */
    DMESH_PEER_REFUSE_MALFORMED,     /* framing, length or canonical form */
    DMESH_PEER_REFUSE_NOT_ON_PEER,   /* the generation does not place that Pod there */
    DMESH_PEER_REFUSE_NO_POD,        /* no live local registration for the destination */
    DMESH_PEER_REFUSE_STREAMS,       /* PEER_STREAMS_MAX */
    DMESH_PEER_REFUSE_STAGING,       /* PEER_STAGING_MAX */
    DMESH_PEER_REFUSE_RATE,          /* PEER_OPEN_RATE */
    DMESH_PEER_REFUSE_HANDLE,        /* unknown, stale or re-tenanted handle */
    DMESH_PEER_REFUSE_INFLIGHT,      /* PEER_TX_INFLIGHT_MAX at a source */
    DMESH_PEER_REFUSE_NODE_UNBOUND,  /* the generation binds no such node */
    DMESH_PEER_REFUSE_NODE_KEY,      /* the static key is not the bound one */
    DMESH_PEER_REFUSE_TRANSPORT,     /* the transport faulted */
    DMESH_PEER_REFUSE_MAX
};

const char *dmesh_peer_refusal_name(enum dmesh_peer_refusal reason);

/* ---- handles ---------------------------------------------------------- */

/* One per (peer, handle). Self-contained by value, as `px_unit` is, so it
 * survives the teardown of anything that named it. Two generations guard it:
 * the channel incarnation rejects a handle from a previous connection to the
 * same peer, and the pod generation rejects one whose destination slot has
 * been re-tenanted. */
struct dmesh_peer_handle {
    uint32_t incarnation;
    uint32_t wire_handle;
    int32_t  dst_pod_idx;
    uint32_t dst_pod_generation;
    uint16_t dst_port;
    uint16_t up_port;          /* the intra-node upstream this stream feeds */
    char     src_pod_uid[DMESH_POD_UID_MAX];
    uint8_t  in_use;
    uint8_t  reserved[3];
    uint32_t staging_bytes;    /* what this stream holds of the peer's bound */
    uint32_t rx_seq;            /* newest ordered DATA sequence */
    uint8_t  rx_seq_valid;
    uint8_t  rx_fin;             /* peer ended source -> destination */
    uint8_t  tx_fin;             /* local reply direction ended */
};

/* One accepted DATA extent stays here until its local DMA landing completes
 * and its acknowledgement has been staged. This both validates asynchronous
 * completions and bounds the number of one-byte extents a hostile peer can
 * leave pending independently of the byte bound. */
struct dmesh_peer_rxslot {
    uint32_t next;
    uint32_t handle;
    uint32_t seq;
    uint32_t bytes;
    uint8_t  source_side;
    uint8_t  completed;
    uint8_t  in_use;
    uint8_t  reserved;
};

/* ---- what the source owes --------------------------------------------- */

enum dmesh_peer_custody_kind {
    DMESH_PEER_CUSTODY_L4 = 0,   /* a staging piece; retires on STREAM_ACK */
    DMESH_PEER_CUSTODY_L7 = 1,   /* an egress arena chunk; retires on STREAM_ACK */
};

/* One extent in flight to a peer, held until the destination says the bytes
 * landed in the destination Pod's mapping. */
struct dmesh_peer_txslot {
    uint32_t next;
    uint32_t seq;
    uint32_t bytes;
    uint32_t handle;
    void    *cookie;
    uint8_t  kind;
    uint8_t  in_use;
    uint8_t  reserved[6];
};

/* ---- the seams -------------------------------------------------------- */

/* What the transport must provide. Ordered, reliable delivery within one
 * handle; a fault that is visible; completion reported to the source; and a
 * mutually authenticated key agreement whose peer static key this module can
 * read back. Reordering across handles is permitted and wanted. */
struct dmesh_peer_transport {
    /* Begin a connection to `ip_be:port`, binding `prologue` into the stock
     * handshake. Returns 0 and sets *conn, or a negative errno. */
    int  (*connect)(void *ctx, uint32_t ip_be, uint16_t port,
                    const uint8_t *prologue, size_t prologue_len, void **conn);
    /* The peer's authenticated static public key once the handshake completed;
     * negative until then. */
    int  (*peer_key)(void *conn, uint8_t key[32]);
    long (*send)(void *conn, const void *buf, size_t len);   /* <0 fault, 0 would-block */
    long (*recv)(void *conn, void *buf, size_t len);         /* <0 fault, 0 empty */
    void (*close)(void *conn);
};

struct dmesh_peer_channel;

/* What this module asks of the node it runs on. Every one of these is a
 * lookup in state the DPU already holds; none of them consults the peer. */
struct dmesh_peer_ops {
    /* The held generation's binding for a node: 1 when it binds that name,
     * with the key and transport address written out; 0 when it does not. */
    int  (*node_binding)(void *ctx, const char *node_name, const uint8_t **key,
                         uint32_t *ip_be, uint16_t *port);
    /* The generation's placement: 1 when it places `pod_uid` on `node_name`. */
    int  (*pod_on_node)(void *ctx, const char *pod_uid, const char *node_name);
    /* A live local registration for `pod_uid`: its slot and pod generation, or
     * -1 when nothing here serves it. */
    int32_t (*local_pod)(void *ctx, const char *pod_uid, uint32_t *pod_generation);
    /* The intra-node upstream a stream to (slot, port) feeds. */
    uint16_t (*upstream_for)(void *ctx, int32_t dst_pod_idx, uint16_t dst_port);
    /* Finish destination-side stream setup after the peer handle is known.
     * The proxy uses this to allocate the node-local upstream/return mapping.
     * Zero refuses the OPEN; absent falls back to `upstream_for`. */
    uint16_t (*destination_opened)(void *ctx,
                                   struct dmesh_peer_channel *channel,
                                   uint32_t handle,
                                   const struct dmesh_peer_stream_open *open,
                                   int32_t dst_pod_idx);
    /* The destination slot's current pod generation. A handle whose slot has
     * been re-tenanted names a Pod that no longer exists, and the arithmetic
     * that catches it is the pair `px_batch` already carries: stamped at open,
     * matched on every arrival. */
    uint32_t (*pod_generation)(void *ctx, int32_t dst_pod_idx);
    /* Land one arrived extent in the destination Pod's mapping. 0: landed —
     * the staging charge releases and the acknowledgement stages inline.
     * Positive: accepted and held — the charge stays until the node reports
     * the landing through dmesh_peer_delivered, which is what makes
     * PEER_STAGING_MAX a real bound under an asynchronous landing.
     * Negative: cannot be delivered — the stream is poisoned. */
    int  (*deliver)(void *ctx, const struct dmesh_peer_handle *handle,
                    const uint8_t *bytes, uint32_t len, uint32_t seq);
    /* The reverse direction of a locally originated stream. The handle is in
     * the remote allocator's namespace, so it is deliberately not looked up
     * in the destination handle table above. Return values match `deliver`. */
    int  (*source_deliver)(void *ctx, struct dmesh_peer_channel *channel,
                           uint32_t handle, const uint8_t *bytes,
                           uint32_t len, uint32_t seq);
    /* Deliver EOF after all earlier DATA for a destination-owned handle has
     * been queued. A positive result means the local proxy retained the EOF
     * for retry; zero means it was queued synchronously. */
    int  (*destination_fin)(void *ctx, const struct dmesh_peer_handle *handle);
    /* Release custody of one extent at the source, now that it landed. */
    void (*release)(void *ctx, uint8_t kind, void *cookie, uint32_t bytes);
    /* End the streams a handle carried, as `px_poison` does within a node. */
    void (*poison)(void *ctx, const struct dmesh_peer_handle *handle,
                   const char *why);
    /* Source-side completions. source_token is the token supplied in OPEN;
     * `source_fin` returns nonzero only when the handle names a live source
     * stream, so an unknown FIN is refused rather than trusted. */
    void (*source_opened)(void *ctx, struct dmesh_peer_channel *channel,
                          uint32_t source_token, uint32_t handle, int32_t status);
    int  (*source_fin)(void *ctx, struct dmesh_peer_channel *channel,
                       uint32_t handle);
    void (*source_reset)(void *ctx, struct dmesh_peer_channel *channel,
                         const char *why);
    /* Counted, surfaced as dmesh_control_events_total{kind="peer",reason=...}. */
    void (*event)(void *ctx, const char *reason);
    uint64_t (*now_ns)(void *ctx);
};

/* ---- channel ---------------------------------------------------------- */

/* Teardown is synchronous — the reset poisons every stream and releases every
 * pinned extent before it returns — so there is no draining state to dwell in:
 * idle eviction applies only to a channel with no streams and nothing in
 * flight, and every other teardown cause is immediate. */
enum dmesh_peer_state {
    DMESH_PEER_CLOSED = 0,
    DMESH_PEER_AUTHENTICATING,
    DMESH_PEER_OPEN,
};

struct dmesh_peer_channel {
    char     node_name[DMESH_K8S_NAME_MAX];
    uint8_t  bound_key[32];          /* what the generation binds to that name */
    uint32_t ip_be;
    uint16_t port;
    uint8_t  in_use;
    uint8_t  state;                  /* enum dmesh_peer_state */
    uint8_t  initiated_local;        /* deterministic simultaneous-open winner */
    /* Advanced on every entry to AUTHENTICATING, so a completion belonging to
     * the previous incarnation is refused rather than applied to this one. */
    uint32_t incarnation;
    uint64_t last_active_ns;
    void    *conn;                   /* the transport's own object */

    /* destination bounds — what this peer may consume here */
    struct dmesh_peer_handle *handles;
    uint32_t handle_count;
    uint64_t staging_bytes;
    uint64_t open_tokens_milli;      /* PEER_OPEN_RATE token bucket */
    uint64_t open_refill_ns;

    /* source bounds — what this peer may pin here while it is stalled */
    struct dmesh_peer_txslot *tx;
    uint32_t tx_free;
    uint64_t inflight_bytes;
    uint8_t  stalled;

    struct dmesh_peer_rxslot *rx;
    uint32_t rx_free;

    struct dmesh_peer_ack_entry ack_stage[DMESH_STREAM_ACK_BATCH];
    uint32_t ack_staged;

    /* Ordered transport framing. Allocated only for a live channel, avoiding
     * 16 MiB of inline buffers in the fixed 256-channel table. */
    uint8_t *rx_frame;
    uint8_t *tx_frame;
    uint32_t rx_len;
    uint32_t tx_len;

    uint64_t refused[DMESH_PEER_REFUSE_MAX];
    uint64_t poisoned;
    uint64_t opened;
    uint64_t handshakes;
};

struct dmesh_peer_table {
    struct dmesh_peer_channel channels[DMESH_CHANNEL_MAX];
    char     node_name[DMESH_K8S_NAME_MAX];   /* this DPU's own node */
    uint8_t  static_public_key[32];
    const struct dmesh_peer_transport *transport;
    void    *transport_ctx;
    const struct dmesh_peer_ops *ops;
    void    *ops_ctx;
    uint64_t evictions;
    uint64_t refused[DMESH_PEER_REFUSE_MAX];
};

/* The node credential: one static keypair per DPU, generated at first boot
 * into a 0400 file that never leaves it. Loads it, or generates it when the
 * file is absent. Returns 0 and fills the 32-byte public half. */
int dmesh_peer_node_key_load(const char *path, uint8_t public_key[32],
                             char *error, size_t error_len);
/* Write the public half where the node agent reports it from. */
int dmesh_peer_node_key_publish(const char *path, const uint8_t public_key[32]);

void dmesh_peer_table_init(struct dmesh_peer_table *table, const char *node_name,
                           const uint8_t static_public_key[32],
                           const struct dmesh_peer_transport *transport,
                           void *transport_ctx,
                           const struct dmesh_peer_ops *ops, void *ops_ctx);
void dmesh_peer_table_fini(struct dmesh_peer_table *table);

/* Bind the incarnation into the stock handshake, so a completed handshake
 * authenticates the incarnation its handles will carry. Returns the length
 * written, or -1 when the names do not fit. */
int dmesh_peer_prologue(const char *local_node, const char *peer_node,
                        uint32_t incarnation, uint8_t *out, size_t out_len);

/* A channel opens on the first stream that needs the node at its far end. */
struct dmesh_peer_channel *dmesh_peer_open(struct dmesh_peer_table *table,
                                           const char *node_name,
                                           enum dmesh_peer_refusal *reason);
/* Adopt the connection accepted by the lower RDMA transport. The handshake
 * supplies the peer node, incarnation, and authenticated static key. */
struct dmesh_peer_channel *
dmesh_peer_accept(struct dmesh_peer_table *table, const char *node_name,
                  uint32_t incarnation, void *conn, const uint8_t peer_key[32],
                  enum dmesh_peer_refusal *reason);
/* The one rule added on top of the stock protocol: the peer's static key must
 * equal the one the held generation binds to the node name it claims. */
enum dmesh_peer_refusal dmesh_peer_authenticated(struct dmesh_peer_table *table,
                                                 struct dmesh_peer_channel *channel,
                                                 const uint8_t peer_key[32]);
/* Every handle from the previous incarnation is invalidated, every stream it
 * carried ends, and every extent it pinned at this source is released. */
void dmesh_peer_reset(struct dmesh_peer_table *table,
                      struct dmesh_peer_channel *channel, const char *why);
void dmesh_peer_evict_idle(struct dmesh_peer_table *table);
/* The held generation changed: a channel to a node it no longer binds, or
 * binds to a different static key, is reset — its streams end as a channel
 * loss. Called on every adoption; an address change needs nothing here, the
 * binding is re-read when a channel opens. */
void dmesh_peer_table_rebind(struct dmesh_peer_table *table);

/* ---- destination ------------------------------------------------------ */

/* Admit one STREAM_OPEN. On DMESH_PEER_OK, *handle names the allocated entry. */
enum dmesh_peer_refusal
dmesh_peer_stream_open(struct dmesh_peer_table *table,
                       struct dmesh_peer_channel *channel,
                       uint32_t incarnation,
                       const struct dmesh_peer_stream_open *open,
                       uint32_t *handle);
/* Land one DATA frame. Staging is charged on arrival and released on delivery. */
enum dmesh_peer_refusal
dmesh_peer_data(struct dmesh_peer_table *table, struct dmesh_peer_channel *channel,
                uint32_t incarnation, uint32_t handle, uint32_t seq,
                const uint8_t *bytes, uint32_t len);
/* The asynchronous half of a held delivery: the node reports that the extent
 * landed in the destination Pod's mapping, releasing the staging charge and
 * staging the acknowledgement. */
void dmesh_peer_delivered(struct dmesh_peer_table *table,
                          struct dmesh_peer_channel *channel,
                          uint32_t handle, uint32_t seq, uint32_t len);
/* Asynchronous landing completion for DATA in the reverse direction of a
 * source-originated stream. */
void dmesh_peer_source_delivered(struct dmesh_peer_table *table,
                                 struct dmesh_peer_channel *channel,
                                 uint32_t handle, uint32_t seq, uint32_t len);
enum dmesh_peer_refusal
dmesh_peer_stream_fin(struct dmesh_peer_table *table,
                      struct dmesh_peer_channel *channel,
                      uint32_t incarnation, uint32_t handle);
/* The source says a Pod is gone: release every handle whose source is it. */
enum dmesh_peer_refusal
dmesh_peer_pod_gone(struct dmesh_peer_table *table,
                    struct dmesh_peer_channel *channel,
                    uint32_t incarnation, const char *pod_uid);
/* Flush the staged acknowledgements. Completed async landings that did not fit
 * the current wire batch remain in the bounded RX slot pool and are drained on
 * later progress passes. */
int dmesh_peer_ack_flush(struct dmesh_peer_table *table,
                         struct dmesh_peer_channel *channel);

/* ---- source ----------------------------------------------------------- */

/* Charge one extent to this peer and remember what releases it. Returns
 * DMESH_PEER_REFUSE_INFLIGHT when the peer is at its source bound, which is
 * what stalls the streams to it and nothing else. */
enum dmesh_peer_refusal
dmesh_peer_tx_charge(struct dmesh_peer_channel *channel, uint32_t handle,
                     uint32_t seq, uint32_t bytes, uint8_t kind, void *cookie);
/* Retire the run [seq_first, seq_first + seq_count) on one handle. */
enum dmesh_peer_refusal
dmesh_peer_tx_ack(struct dmesh_peer_table *table, struct dmesh_peer_channel *channel,
                  uint32_t incarnation, const struct dmesh_peer_ack_entry *entry);
/* Release everything this source holds for one handle, without delivery. */
void dmesh_peer_tx_drop(struct dmesh_peer_table *table,
                        struct dmesh_peer_channel *channel, uint32_t handle);

/* Source-side wire operations. DATA takes custody only after a complete frame
 * is accepted by the ordered transport; a failed/would-block send leaves the
 * caller's cookie untouched so it can retry. */
enum dmesh_peer_refusal
dmesh_peer_stream_request(struct dmesh_peer_table *table,
                          struct dmesh_peer_channel *channel,
                          const struct dmesh_peer_stream_open *open);
enum dmesh_peer_refusal
dmesh_peer_stream_data_send(struct dmesh_peer_table *table,
                            struct dmesh_peer_channel *channel,
                            uint32_t handle, uint32_t seq,
                            const uint8_t *bytes, uint32_t len,
                            uint8_t custody_kind, void *cookie);
enum dmesh_peer_refusal
dmesh_peer_stream_fin_send(struct dmesh_peer_table *table,
                           struct dmesh_peer_channel *channel,
                           uint32_t handle);
enum dmesh_peer_refusal
dmesh_peer_pod_gone_send(struct dmesh_peer_table *table,
                         struct dmesh_peer_channel *channel,
                         const char *pod_uid);

/* Authenticate a newly connected channel and drain/dispatch a bounded number
 * of ordered frames. Returns frames progressed, 0 when idle/handshaking, -1
 * after a transport or malformed-frame reset. */
int dmesh_peer_channel_progress(struct dmesh_peer_table *table,
                                struct dmesh_peer_channel *channel,
                                int frame_budget);

/* ---- frames ----------------------------------------------------------- */

/* Parse one frame out of `buf`. Returns the bytes consumed, 0 when the buffer
 * does not yet hold a whole frame, or -1 when it never will. */
long dmesh_peer_frame_parse(const uint8_t *buf, size_t len,
                            struct dmesh_peer_msg_header *header,
                            const uint8_t **payload);
/* Build one frame into `out`. Returns the bytes written or -1. */
long dmesh_peer_frame_build(uint8_t *out, size_t out_len, uint8_t type,
                            uint32_t incarnation, uint32_t handle,
                            const void *payload, uint32_t payload_len);

struct dmesh_peer_channel *dmesh_peer_find(struct dmesh_peer_table *table,
                                           const char *node_name);

#endif /* DMESH_PEER_CHANNEL_H */
