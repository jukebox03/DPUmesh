/* Ordered forwarding. A connection's routing is resolved once from its service:
 * L4 passthrough, or the L7 layer behind linkerd/include/dmesh_l7.h, which is
 * handed staging extents and names its own backend. See design/L7.md. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dpu_proxy.h"
#include <pthread.h>
#include <dmesh_l7.h>

#include "object.h"
#include "workload_grant.h"
#include "dpu_worker.h"
#include "comch_server.h"
#include "dpa_common.h"
#include "buffer.h"
#include <dpumesh/dmesh_topology.h>

#include <doca_log.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <doca_ctx.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dma.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, for the L7 worker selection */
#include <time.h>
#include <unistd.h>

DOCA_LOG_REGISTER(DPU_PROXY);

/* ====== tunables ====== */

/* One REV_DONE entry (one delivered chunk) is capped at the slot size — the
 * host guards len <= slot_size and its RX credit is per-entry. */
#define PX_ENTRY_BYTES_MAX  DPUMESH_SLOT_SIZE

/* doca_dma task pool: SG batches + credit-refresh reads share it. */
#define PX_DMA_TASKS        256

/* Source pieces per SG task, clamped by the device capability at init. */
#define PX_SG_PIECES_MAX    64

/* Bound ACK delay while collapsing physically adjacent 8 KiB DPA completions
 * into one ARM window extent. */
#define PX_ARRIVAL_COALESCE_MAX (64u * 1024u)

/* Sequences one extent may cover. Its acknowledgement publishes a 16-bit count
 * whose zero value means one, so a run stops one short of that range. */
#define PX_ACK_RUN_MAX 0xFFFFu

#define PX_DST_DEFER        (-2)

/* Every ARM data worker owns an L7 layer, so no request needs a fixed owner. */
#define PX_L7_WORKER_ALL    (-1)

/* How far the L7 layer is involved in one connection, selected per service.
 * Recorded in conntrack when the upstream port is issued, so a reply inherits
 * its request's mode. */
enum px_l7_mode {
    PX_L7_NONE = 0,      /* fallback: the data plane alone */
    PX_L7_DECISION,      /* one query per connection; payload does not traverse */
    PX_L7_OPAQUE,        /* payload traverses the L7 layer */
    PX_L7_FULL,          /* the above, plus request-level routing */
};

/* Why a connection is being forwarded at L4 instead of through the L7 layer.
 * The layer names the cause with a DMESH_L7_DECLINE_* return; the rest are
 * conditions the data plane sees on its own. */
enum px_l7_fallback_reason {
    PX_L7_FB_ADAPTER_ERROR = 0,
    PX_L7_FB_NOT_ATTACHED,
    PX_L7_FB_UNSUPPORTED_MODE,
    PX_L7_FB_SESSION_LIMIT,
    PX_L7_FB_UNKNOWN_REPLY,
    PX_L7_FB_NO_VERDICT,
    PX_L7_FB_KINDS
};

static const char *const px_l7_fallback_name[PX_L7_FB_KINDS] = {
    "adapter-error", "worker-not-attached", "unsupported-mode",
    "single-session-limit", "unknown-reply", "no-verdict",
};

/* The decline codes are wire ABI with the layer: a value it returns is looked
 * up here, so an unknown one is counted rather than mistaken for a known one. */
static inline enum px_l7_fallback_reason px_l7_reason_of(int rc) {
    switch (rc) {
    case DMESH_L7_DECLINE_NOT_ATTACHED:  return PX_L7_FB_NOT_ATTACHED;
    case DMESH_L7_DECLINE_MODE:          return PX_L7_FB_UNSUPPORTED_MODE;
    case DMESH_L7_DECLINE_SESSION_LIMIT: return PX_L7_FB_SESSION_LIMIT;
    case DMESH_L7_DECLINE_UNKNOWN_REPLY: return PX_L7_FB_UNKNOWN_REPLY;
    default:                             return PX_L7_FB_ADAPTER_ERROR;
    }
}

/* Modes whose payload is handed to the L7 layer. */
static inline int px_l7_carries_bytes(uint8_t mode) {
    return mode == PX_L7_OPAQUE || mode == PX_L7_FULL;
}

/* Bytes handed to the L7 layer and not yet released. The sender's TX credit is
 * the real bound; this caps how far one connection runs ahead of a slow
 * consumer, and how much staging a single stream can hold hostage. */
#define PX_L7_CUSTODY_MAX       (256u * 1024u)
/* Hand-overs per parse pass, so a layer that takes one byte at a time cannot
 * monopolize the worker. What is left re-enters through the stall list. */
#define PX_L7_SEGMENTS_PER_PASS 16

/* Egress arena for bytes the L7 layer produced rather than forwarded. A chunk is
 * wider than one reverse entry; emission still cuts entries at
 * PX_ENTRY_BYTES_MAX, so credit accounting is unchanged. */
#define PX_ARENA_CHUNK      (2u * PX_ENTRY_BYTES_MAX)
#define PX_ARENA_CHUNKS     1024u
/* One send is one delivery: a payload longer than a chunk chains chunks rather
 * than being split across deliveries. */
#define PX_L7_SEND_MAX      (4u * PX_ARENA_CHUNK)
#define PX_L7_SEND_CHUNKS   (PX_L7_SEND_MAX / PX_ARENA_CHUNK)

/* Refresh the DMA-read host credit cache when headroom drops below this many
 * entries. */
#define PX_CREDIT_REFRESH_MARGIN 64
#define PX_CREDIT_REFRESH_RETRY_NS (100u * 1000u)

/* Pool sizes. Arrivals are bounded by total in-flight sender slots
 * (MAX_PODS x DPU_BUFFER_SIZE/slot); units/pieces match pass-through 1:1. */
#define PX_ARRIVAL_POOL  (MAX_PODS * (DPU_BUFFER_SIZE / DPUMESH_SLOT_SIZE))
#define PX_UNIT_POOL     PX_ARRIVAL_POOL
#define PX_PIECE_POOL    (2 * PX_ARRIVAL_POOL)
#define PX_BATCH_POOL    1024
#define PX_CONN_HASH     (1u << 15)   /* buckets (chained) */

/* ====== internal structs ====== */

/* One arrived forward completion = one staging extent, held for custody until
 * every byte is either egress-completed or dropped; then its (port,seq) is
 * acknowledged to the sender and the node freed. Outlives its conn (a piece
 * in flight keeps it via unfreed > 0). */
struct px_arrival {
    struct px_arrival *next;      /* window list / freelist */
    struct px_arrival *release_next;
    uint64_t stream_base;         /* stream offset of this extent's byte 0 */
    int32_t  pod_idx;             /* staging owner (objs->pods[] slot) */
    uint32_t staging_off;
    uint32_t len;
    /* Remaining bytes plus one reference while linked in the input window. */
    atomic_uint unfreed;          /* custody: (bytes not yet egressed/dropped) + in-window ref */
    uint32_t claimed_round;       /* scratch: seg-claimed bytes, one parse round (forward-thread-only) */
    int32_t  ack_pod;             /* TX_ACK target (original sender, untranslated) */
    /* The consecutive run this extent acknowledges: it only grows across a
     * sequence delta of one. */
    uint16_t ack_port, ack_first_seq, ack_seq;
};

/* One egress arena chunk: DPU-local bytes the SG engine can source. Interchangeable
 * across workers, like the other pool nodes, so it is freed wherever its unit
 * retires. */
struct px_chunk {
    struct px_chunk *next;
    uint32_t off;                 /* byte offset into dmesh_proxy.arena */
};

/* One contiguous SG source piece: either an extent of arrival staging (arr set,
 * custody attached) or an arena chunk the L7 layer wrote (chunk set). */
struct px_piece {
    struct px_piece   *next;
    struct px_arrival *arr;       /* custody countdown target; NULL for arena */
    struct px_chunk   *chunk;     /* arena source; NULL for arrival staging */
    int32_t  pod_idx;             /* staging owner; -1 for arena */
    uint32_t staging_off, len;    /* offset into the pod buffer, or into the arena */
};

/* One delivery to one receiving conn: one seg (or a FIN marker, total_len 0).
 * Self-contained (values, not conn pointers) so it survives conn teardown. */
struct px_unit {
    struct px_unit *next;
    int8_t   src_pod_id, src_service, dst_service;
    uint16_t src_port, dst_port, seq;
    /* The port the origin sent these bytes on, un-rewritten: src_port is the DPU's
     * upstream id on a request, which the client cannot be addressed by. A failed
     * unit reports EOF back through it (px_eof_to_origin) without the conn table.
     * 0 = nothing to notify, which is what a synthetic EOF carries. */
    uint16_t org_port;
    uint32_t total_len;           /* 0 == FIN / notify-only (still 1 RX credit) */
    uint32_t landing_pos;         /* absolute pos in the host RX buffer (at submit) */
    uint32_t emit_off;            /* REV_DONE emission cursor (resumable) */
    uint8_t  emit_fin_done;
    int8_t   dst_pod_idx;         /* receiver pod slot (REV_DONE target; set at ship) */
    uint8_t  err;                 /* batch errored; skip REV_DONE */
    struct px_piece *pieces, *pieces_tail;
    int npieces;
};

/* DMA completion dispatch tag. */
struct px_op {
    int kind;                     /* 0=data, 1=credit, 2=rev entries, 4=rev ctrl */
    struct px_batch *batch;       /* kind 0 */
    int pod_idx, region;          /* kind 1 */
    struct doca_buf *src_buf, *dst_buf;   /* kind 1 (kind 0 keeps them in the batch) */
};

#define PX_REV_STAGE_ENTRIES 64u
#define PX_REV_STAGE_STRIDE  4096u
#define PX_REV_ENTRIES_OFF(pi, r) \
    (((size_t)(pi) * MAX_EU_PER_POD + (size_t)(r)) * PX_REV_STAGE_STRIDE)
#define PX_REV_CTRL_OFF(pi, r) \
    (PX_REV_ENTRIES_OFF((pi), (r)) + \
     PX_REV_STAGE_ENTRIES * sizeof(struct dmesh_rev_ring_entry) + 64u)

enum px_rev_state {
    PX_REV_IDLE = 0,
    PX_REV_META_INFLIGHT,
    PX_REV_CTRL_PENDING,
    PX_REV_CTRL_INFLIGHT,
};

struct px_rev_pub {
    uint32_t count;
    uint32_t publish_count;
    int state;
    int ctrl_after_publish;
    uint64_t producer_tail;
    uint64_t publish_tail;
    uint64_t cached_head;
    uint64_t notified_epoch;
};

/* One submitted SG-DMA op: a FIFO prefix of a lane's units landing at one
 * contiguous dst range. Retired (entries emitted + custody released) strictly
 * in submission order per lane. */
enum px_batch_state {
    PX_BATCH_INFLIGHT = 0,
    PX_BATCH_DONE,
    PX_BATCH_RETRY_PENDING,
    PX_BATCH_ERROR,
};

#define PX_BATCH_RETRY_MAX 1u
#define PX_BATCH_RETRY_GRACE_NS (1000u * 1000u)

struct px_batch {
    struct px_batch *next;
    struct px_unit  *units;       /* FIFO */
    int      pod_idx, region;
    uint32_t pod_generation;      /* destination slot incarnation at submit */
    uint32_t entries;             /* RX credits consumed */
    uint32_t bytes;
    volatile int state;           /* enum px_batch_state */
    uint8_t retry_count;
    struct doca_buf *src_head, *dst_buf;
    struct px_op op;
};

#define PX_ACK_RELEASE_CAP 4096u
_Static_assert((PX_ACK_RELEASE_CAP & (PX_ACK_RELEASE_CAP - 1u)) == 0,
               "PX_ACK_RELEASE_CAP must be a power of two");
struct px_ack_release_slot {
    atomic_size_t sequence;
    struct px_arrival *arrival;
};

struct px_ack_release_queue {
    struct px_ack_release_slot slots[PX_ACK_RELEASE_CAP];
    _Alignas(64) atomic_size_t enqueue_pos;
    _Alignas(64) size_t dequeue_pos;
};

/* Per-(destination pod, landing stripe) egress lane. */
struct px_lane {
    /* One atomic LIFO per ARM data worker. The egress owner exchanges each list
     * and reverses it before appending, preserving FIFO order per worker/QP. */
    struct px_unit  *inq[MAX_ARM_WORKERS];
    struct px_unit  *qhead, *qtail;   /* queued units (not yet submitted) — worker-local */
    struct px_batch *fhead, *ftail;   /* in-flight/completed batches, FIFO — worker-local */
    uint32_t cursor;                  /* next landing byte offset within the region */
    uint64_t sent_entries;            /* credits consumed (cumulative) */
    uint64_t cached_freed;            /* host freed counter, DMA-read cache */
    uint64_t refresh_after_ns;
    int      refresh_inflight;
    int      warned_no_credit_addr;
    /* Pod generation associated with this lane's credit counters. */
    uint32_t pod_generation;
    struct px_rev_pub rev;
};

struct px_conn {
    dmesh_proxy_conn pub;
    struct px_conn *hnext;
    struct px_arrival *whead, *wtail; /* input window (unparsed tail kept) */
    uint64_t stream_end;              /* total bytes arrived */
    uint64_t parse_pos;               /* window cursor (consumed boundary) */
    int      fin_pending, dead;
    int      eof_pending;             /* poisoned, but its sender has not been told yet
                                       * (unit pool was dry) — px_drain_stalled retries */
    int      disconnect_pending;      /* peer pod vanished; delete after the deferred
                                       * EOF has been queued to the surviving sender */
    /* Backpressure park (px_stall): 1 = this conn has window bytes it could not ship
     * because a pool was momentarily empty. parse_pos did NOT advance, so the bytes are
     * still in the window; px_drain_stalled re-parses from exactly where it stopped. */
    int      stalled;
    struct px_conn *stall_next;       /* worker-local stall list (worker thread only) */
    int32_t  fin_ack_pod;
    uint16_t fin_ack_port, fin_ack_seq;
    uint16_t forward_seq;              /* latest DPA forward completion consumed */
    uint8_t  forward_seq_valid;
    uint16_t return_seq;              /* units serialized back to this downstream QP */
    int      dst_service_set;
    uint8_t  l7_mode;                 /* enum px_l7_mode, resolved once per conn */
    uint8_t  l7_open;                 /* the L7 layer holds this conn */
    uint8_t  l7_closed;               /* closed once; never reopened */
    /* Bytes handed to the L7 layer, ahead of parse_pos. The window keeps them
     * until dmesh_l7_release reports consumption, so custody outlives the parse
     * pass that handed them over. */
    uint64_t l7_handed;
    uint32_t l7_release_pending;      /* released but not yet applied (see px_l7_apply_release) */
    struct px_chunk *l7_tx_chunk;     /* egress memory lent out by dmesh_l7_tx_reserve */
    uint8_t  l7_resolved;             /* answered once by l7_resolve; owes a report */
    uint64_t l7_open_ns;              /* when that answer was given */
    uint64_t l7_shipped;              /* bytes forwarded, reported back as load */
    /* Connection-scoped backend stickiness: the backend this byte stream was
     * pinned to. An L4 stream carries no message boundaries, so it stays on one
     * backend for life; `decision` mode pins it from the L7 layer's answer.
     * Cluster-scoped, so a message to a different service re-picks. -1 =
     * unpinned. */
    int32_t  pinned_backend;
    int16_t  pinned_cluster;
};

/* A data worker owns its DOCA resources and regions where region % A == id.
 * There is exactly one engine per data worker (both counted by n_workers). */
struct px_engine {
    struct objects *objs;
    int      id;
    struct doca_dma           *dma;
    struct doca_ctx           *dma_ctx;
    struct doca_pe            *pe;
    struct doca_buf_inventory *inv;
    int      dma_tasks_inflight;
    /* Set when the doca_dma ctx faults: gates every submit on this engine and
     * tells px_engine_pump/px_drain to run px_engine_recover, which restarts the
     * ctx and clears it. Every path that can observe the fault latches it —
     * px_dma_err_cb, the SG-batch submit, and px_lane_refresh_credit. */
    int      dma_stalled;
    int      dma_fault_warned;
    uint64_t retry_after_ns;       /* earliest retry timestamp */
    uint32_t retry_batches;        /* retry-pending batches */
    struct px_batch *retry_probe;  /* active exclusive retry */
    struct px_batch *batch_mem, *batch_free;
    struct px_unit  *emit_head, *emit_tail;
    struct px_ack_release_queue ack_releases;
    struct px_arrival *ack_retry_head, *ack_retry_tail;
};

/* Per-ARM-worker routing state. */
struct px_worker_state {
    struct px_conn **buckets;      /* PX_CONN_HASH buckets, per-worker */
    struct dpu_conntrack *ct;
    struct objects *objs;          /* the L7 entry points reach the proxy through this */
    int id;
    struct px_conn *stall_head;    /* conns parked by px_stall; drained by px_drain_stalled */
    /* Set while px_parse_l7 walks the window. A release reported from inside
     * that walk is applied at its end instead of parking the conn, because
     * applying it there would unlink arrivals the walk still holds. */
    int in_l7_parse;
};

struct dmesh_proxy {
    uint8_t  svc_mode[POD_ID_SPACE];     /* service id → enum px_l7_mode */
    int      l7_attached;                /* some service selects a mode with an L7 layer */
    int      l7_worker;                  /* ARM worker owning the L7 layer's session state */
    int      l7_fail_closed;             /* a declined L7 connection is refused, not forwarded */
    uint32_t sg_pieces_max;

    /* Per-worker connection and routing tables. */
    struct px_worker_state workers[MAX_ARM_WORKERS];
    int n_workers;

    /* Fixed arrival, piece, and unit pools shared by forward and emit. */
    struct px_arrival *arr_mem,  *arr_free;
    struct px_piece   *piece_mem, *piece_free;
    struct px_unit    *unit_mem, *unit_free;
    /* Egress arena: one DPU-local mmap carved into fixed chunks, allocated only
     * when a service selects a mode whose payload the L7 layer rewrites. */
    struct doca_mmap  *arena_mmap;
    uint8_t           *arena;
    struct px_chunk   *chunk_mem, *chunk_free;
    pthread_mutex_t    pool_lock;
    struct px_lane lanes[MAX_PODS][MAX_EU_PER_POD];
    struct px_op   refresh_ops[MAX_PODS][MAX_EU_PER_POD];
    struct px_op   rev_ops[MAX_PODS][MAX_EU_PER_POD];

    /* ARM SG-DMA engines, one per data worker; ownership is region % n_workers. */
    struct px_engine engines[MAX_ARM_WORKERS];

    /* credit-read landing cells: one 64B cell per lane, DPU-local mmap */
    struct doca_mmap *scratch_mmap;
    uint8_t *scratch;
    struct doca_mmap *rev_scratch_mmap;
    uint8_t *rev_scratch;

    atomic_ullong stat_stall_unit;
    atomic_ullong stat_stall_piece;
    atomic_ullong stat_stall_uport;
    atomic_ullong stat_stall_arena;
    atomic_ullong stat_l7_fallback;      /* conns the L7 layer declined */
    /* The same total, split by the reason the layer gave. */
    atomic_ullong stat_l7_fallback_by[PX_L7_FB_KINDS];
    atomic_ullong stat_l7_over_release;  /* releases naming more than is outstanding */
    atomic_ullong stat_l7_stray_release; /* releases against a conn holding nothing */
    atomic_ullong l7_report_ns;          /* when the L7 counters were last reported */
    atomic_ullong l7_report_mark;        /* what they summed to then */
};

static inline uint64_t px_stat_inc(atomic_ullong *counter)
{
    return atomic_fetch_add_explicit(counter, 1, memory_order_relaxed) + 1;
}

/* Destination-lane engine owner. */
static inline int px_engine_id_for_lane(const struct dmesh_proxy *px,
                                        int pod_idx, int region) {
    (void)pod_idx;
    return region % px->n_workers;
}

/* Landing stripes of a pod. A data-ready pod carries L == A; the clamp covers a
 * slot whose geometry is zeroed because it is not ready. */
static inline int
px_landing_stripes(const struct pod_state *pod)
{
    int K = pod->k_rings > 0 ? pod->k_rings : 1;
    int L = pod->landing_stripes > 0 ? pod->landing_stripes : 1;
    return L <= K && K % L == 0 ? L : 1;
}

/* The worker that stages reverse entries for (pod, port). Every reverse
 * producer — TX_ACK, arrival-release handoff, REV_DONE — routes through it. */
static inline int
px_rev_owner(const struct dmesh_proxy *px, const struct pod_state *pod,
             uint16_t port)
{
    return (port % (uint16_t)px_landing_stripes(pod)) % px->n_workers;
}

/* One cell holds every credit shard of a landing stripe (at most K counters). */
#define PX_SCRATCH_CELL 64
#define PX_SCRATCH_OFF(pi, r) (((size_t)(pi) * MAX_EU_PER_POD + (size_t)(r)) * PX_SCRATCH_CELL)
_Static_assert(MAX_EU_PER_POD * sizeof(uint64_t) <= PX_SCRATCH_CELL,
               "credit shard cell must hold K counters");

/* Release a worker parked on its notification handles. */
static void px_engine_wake(struct px_engine *eng) {
    if (!eng->objs || eng->id < 0 ||
        eng->id >= eng->objs->n_data_workers)
        return;
    struct dpu_data_worker *worker_state = &eng->objs->data_workers[eng->id];
    dpu_wake_eventfd(&worker_state->parked, worker_state->wake_fd);
}

/* Routing state for the current ARM data worker. */
static __thread struct px_worker_state *px_cur_worker;

/* ====== pools ====== */

/* Per-thread magazines cache up to PX_MAG_CAP nodes per type on both the
 * allocation and the free side, so a worker whose allocs and frees interleave
 * touches pool_lock only to refill (a PX_MAG_N run) or to spill a full cache.
 * Nodes are interchangeable across threads. */
#define PX_MAG_N   64
#define PX_MAG_CAP (2 * PX_MAG_N)
static __thread struct px_arrival *tls_arr_mag;
static __thread int                tls_arr_mag_n;
static __thread struct px_piece   *tls_piece_mag;
static __thread int                tls_piece_mag_n;
static __thread struct px_unit    *tls_unit_mag;
static __thread int                tls_unit_mag_n;
static __thread struct px_chunk   *tls_chunk_mag;
static __thread int                tls_chunk_mag_n;

static void
px_ack_queue_init(struct px_ack_release_queue *q)
{
    atomic_init(&q->enqueue_pos, 0);
    q->dequeue_pos = 0;
    for (size_t i = 0; i < PX_ACK_RELEASE_CAP; i++)
        atomic_init(&q->slots[i].sequence, i);
}

static int
px_ack_queue_push(struct px_ack_release_queue *q, struct px_arrival *arrival)
{
    size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
    struct px_ack_release_slot *slot;
    for (;;) {
        slot = &q->slots[pos & (PX_ACK_RELEASE_CAP - 1u)];
        size_t sequence =
            atomic_load_explicit(&slot->sequence, memory_order_acquire);
        intptr_t delta = (intptr_t)sequence - (intptr_t)pos;
        if (delta == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->enqueue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (delta < 0) {
            return 0;
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos,
                                       memory_order_relaxed);
        }
    }
    slot->arrival = arrival;
    atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);
    return 1;
}

static struct px_arrival *
px_ack_queue_front(struct px_ack_release_queue *q)
{
    size_t pos = q->dequeue_pos;
    struct px_ack_release_slot *slot =
        &q->slots[pos & (PX_ACK_RELEASE_CAP - 1u)];
    return atomic_load_explicit(&slot->sequence, memory_order_acquire) ==
            pos + 1 ? slot->arrival : NULL;
}

static void
px_ack_queue_pop(struct px_ack_release_queue *q)
{
    size_t pos = q->dequeue_pos;
    struct px_ack_release_slot *slot =
        &q->slots[pos & (PX_ACK_RELEASE_CAP - 1u)];
    q->dequeue_pos = pos + 1;
    atomic_store_explicit(&slot->sequence,
                          pos + PX_ACK_RELEASE_CAP,
                          memory_order_release);
}

static int px_rev_append_ack(struct px_engine *eng, struct pod_state *pod,
                             uint16_t port, uint16_t seq, uint16_t count);

static int
px_emit_tx_ack(struct objects *objs, int32_t pod_id, uint16_t port, uint16_t seq)
{
    struct pod_state *pod = find_pod_by_id(objs, pod_id);
    if (!pod)
        return 1;
    if (!px_cur_worker)
        return 0;
    int owner = px_rev_owner(objs->proxy, pod, port);
    if (owner != px_cur_worker->id)
        return 0;
    return px_rev_append_ack(&objs->proxy->engines[owner], pod, port, seq, 1);
}

/* Take the shared free-list lock. The per-worker magazines are meant to keep
 * this off the hot path. */
static inline void px_pool_lock(struct dmesh_proxy *px) {
    if (pthread_mutex_trylock(&px->pool_lock) == 0)
        return;
    pthread_mutex_lock(&px->pool_lock);
}

/* Alloc: pop the local cache, else refill a PX_MAG_N run from the shared list
 * under one lock. Free: push the local cache; a full cache spills entirely to
 * the shared list under one lock. */
#define PX_POOL_FUNCS(type, alloc_fn, free_fn, shared)                        \
static struct type *alloc_fn(struct dmesh_proxy *px) {                        \
    struct type *n = tls_##shared##_mag;                                      \
    if (n) { tls_##shared##_mag = n->next; tls_##shared##_mag_n--; return n; }\
    px_pool_lock(px);                                                         \
    n = px->shared##_free;                                                    \
    int got = 0;                                                              \
    if (n) {                                                                  \
        struct type *t = n; got = 1;                                          \
        while (got < PX_MAG_N && t->next) { t = t->next; got++; }             \
        px->shared##_free = t->next; t->next = NULL;  /* detach [n..t] */     \
    }                                                                         \
    pthread_mutex_unlock(&px->pool_lock);                                     \
    if (!n)                                                                   \
        return NULL;                                                          \
    tls_##shared##_mag = n->next;                     /* rest → cache */      \
    tls_##shared##_mag_n = got - 1;                                           \
    return n;                                                                 \
}                                                                             \
static void free_fn(struct dmesh_proxy *px, struct type *n) {                 \
    if (tls_##shared##_mag_n >= PX_MAG_CAP) {                                 \
        struct type *h = tls_##shared##_mag, *t = h;                          \
        while (t->next) t = t->next;                                          \
        px_pool_lock(px);                                                     \
        t->next = px->shared##_free; px->shared##_free = h;                   \
        pthread_mutex_unlock(&px->pool_lock);                                 \
        tls_##shared##_mag = NULL; tls_##shared##_mag_n = 0;                  \
    }                                                                         \
    n->next = tls_##shared##_mag; tls_##shared##_mag = n;                     \
    tls_##shared##_mag_n++;                                                   \
}

PX_POOL_FUNCS(px_arrival, px_arrival_alloc,   px_arrival_free, arr)
PX_POOL_FUNCS(px_piece,   px_piece_alloc,     px_piece_free,   piece)
PX_POOL_FUNCS(px_unit,    px_unit_alloc_node, px_unit_free,    unit)
PX_POOL_FUNCS(px_chunk,   px_chunk_alloc,     px_chunk_free,   chunk)

/* Units start zeroed; the other node types are fully field-initialized. */
static struct px_unit *px_unit_alloc(struct dmesh_proxy *px) {
    struct px_unit *u = px_unit_alloc_node(px);
    if (u)
        memset(u, 0, sizeof(*u));
    return u;
}
/* Free the unit and its piece chain. Arena chunks return here — the single
 * place every unit passes through, whether it was delivered, dropped, or
 * abandoned before submit — so a chunk cannot be leaked on an error path. */
static void px_unit_free_node(struct dmesh_proxy *px, struct px_unit *u) {
    while (u->pieces) {
        struct px_piece *p = u->pieces;
        u->pieces = p->next;
        if (p->chunk)
            px_chunk_free(px, p->chunk);
        px_piece_free(px, p);
    }
    px_unit_free(px, u);
}
static struct px_batch *px_batch_alloc(struct px_engine *eng) {
    struct px_batch *b = eng->batch_free;
    if (b) { eng->batch_free = b->next; memset(b, 0, sizeof(*b)); }
    return b;
}
static void px_batch_free_node(struct px_engine *eng, struct px_batch *b) {
    b->next = eng->batch_free; eng->batch_free = b;
}

/* ====== custody ====== */

static int
px_queue_arrival_release(struct objects *objs, struct px_arrival *a)
{
    struct dmesh_proxy *px = objs->proxy;
    struct pod_state *src = find_pod_by_id(objs, a->ack_pod);
    if (!src || !px_cur_worker || px_cur_worker->id < 0 ||
        px_cur_worker->id >= px->n_workers)
        return 0;

    int owner = px_rev_owner(px, src, a->ack_port);
    if (owner < 0 || owner >= px->n_workers)
        return 0;
    a->release_next = NULL;
    if (px_ack_queue_push(&px->engines[owner].ack_releases, a))
        return 1;

    /* Detached arrivals remain covered by proxy_source_refs until ACK publication. */
    struct px_engine *current = &px->engines[px_cur_worker->id];
    if (current->ack_retry_tail)
        current->ack_retry_tail->release_next = a;
    else
        current->ack_retry_head = a;
    current->ack_retry_tail = a;
    return 1;
}

/* Return every source TX unit after its arrival leaves the egress path. */
static void px_arrival_release(struct objects *objs, struct px_arrival *a) {
    if (px_queue_arrival_release(objs, a))
        return;
    __atomic_fetch_sub(&objs->pods[a->pod_idx].proxy_source_refs, 1,
                       __ATOMIC_ACQ_REL);
    px_arrival_free(objs->proxy, a);
}

/* Subtract n from the arrival's cross-thread custody counter; release exactly once
 * when it reaches 0 (the decrementing thread that observes prev==n owns the release).
 * n = egressed/dropped bytes, or 1 to remove the window reference. */
static inline void px_custody_sub(struct objects *objs, struct px_arrival *a, uint32_t n) {
    if (n == 0)
        return;
    if (atomic_fetch_sub_explicit(&a->unfreed, n, memory_order_acq_rel) == n)
        px_arrival_release(objs, a);
}

/* Release one source piece's hold once the egress path is done reading it. An
 * arena piece holds no arrival; its chunk returns with the unit. */
static inline void px_piece_release(struct objects *objs, struct px_piece *p) {
    if (p->arr)
        px_custody_sub(objs, p->arr, p->len);
}

/* ====== conn table ====== */

static inline uint32_t px_conn_hash(int32_t pod, uint16_t port) {
    uint32_t k = ((uint32_t)(uint8_t)pod << 16) | port;
    return (k * 2654435761u) & (PX_CONN_HASH - 1u);
}

static struct px_conn *px_conn_find(struct dmesh_proxy *px, int32_t pod, uint16_t port) {
    (void)px;   /* conn table is per-worker (px_cur_worker) */
    struct px_conn *c = px_cur_worker->buckets[px_conn_hash(pod, port)];
    while (c && !(c->pub.src_pod == pod && c->pub.src_port == port))
        c = c->hnext;
    return c;
}

static struct px_conn *px_conn_get(struct dmesh_proxy *px, int32_t pod, uint16_t port,
                                   int is_reply, int create) {
    struct px_conn *c = px_conn_find(px, pod, port);
    if (c || !create)
        return c;
    c = (struct px_conn *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->pub.src_pod = pod;
    c->pub.src_port = port;
    c->pub.is_reply = is_reply;
    c->pub.peer_pod = DMESH_POD_BLANK;
    c->pub.dst_service = DMESH_SVC_NONE;
    c->pinned_backend = -1;                /* unpinned until the first LB pick */
    c->pinned_cluster = DMESH_SVC_NONE;
    uint32_t h = px_conn_hash(pod, port);
    c->hnext = px_cur_worker->buckets[h];
    px_cur_worker->buckets[h] = c;
    return c;
}

static uint16_t *px_delivery_seq_counter(struct dmesh_proxy *px,
                                         int32_t pod, uint16_t port) {
    if (port >= DMESH_UPORT_BASE) {
        struct dpu_upstream *up = &px_cur_worker->ct->upstream[port];
        if (!up->in_use || up->backend_pod != pod)
            return NULL;
        return &up->delivery_seq;
    }
    struct px_conn *c = px_conn_find(px, pod, port);
    return c ? &c->return_seq : NULL;
}

static void px_drop_window(struct objects *objs, struct px_conn *c, const char *why);
/* Defined below with the lanes; px_fin_to_sender (above them) queues the EOF unit. */
static void px_lane_enqueue(struct dmesh_proxy *px, int pod_idx, int region, struct px_unit *u);

static void px_l7_close(struct objects *objs, struct px_conn *c, int eof);
static uint64_t px_monotonic_ns(void);

/* Resolve the mode once for each request or upstream reply connection. A request
 * reads the service table; a reply cannot see a service id, so it inherits the
 * mode conntrack recorded when its upstream port was issued. */
static void px_resolve_route(struct objects *objs, struct px_conn *c,
                             int is_reply, int16_t svc) {
    struct dmesh_proxy *px = objs->proxy;
    c->l7_mode = PX_L7_NONE;
    if (!is_reply) {
        if (svc >= 0 && svc < POD_ID_SPACE)
            c->l7_mode = px->svc_mode[svc];
        return;
    }
    if (c->pub.src_port >= DMESH_UPORT_BASE) {
        struct dpu_upstream *u = &px_cur_worker->ct->upstream[c->pub.src_port];
        if (u->in_use)
            c->l7_mode = u->l7_mode;
    }
}

static void px_conn_del(struct objects *objs, struct px_conn *c) {
    px_l7_close(objs, c, 0);                   /* before the window goes: the L7
                                                * layer may still point into it */
    if (c->parse_pos < c->stream_end)
        px_drop_window(objs, c, "conn teardown");
    /* remaining window arrivals are fully parsed; any with pending egress
     * bytes are released by batch retirement (they are self-contained). */
    while (c->whead) {
        struct px_arrival *a = c->whead;
        c->whead = a->next;
        a->next = NULL;
        px_custody_sub(objs, a, 1);            /* remove window ref; release iff bytes done */
    }
    c->wtail = NULL;
    if (c->stalled) {                          /* unlink before the free — px_drain_stalled
                                                * would otherwise walk into freed memory */
        struct px_conn **sl = &px_cur_worker->stall_head;
        while (*sl && *sl != c)
            sl = &(*sl)->stall_next;
        if (*sl)
            *sl = c->stall_next;
        c->stalled = 0;
    }
    struct px_conn **link = &px_cur_worker->buckets[px_conn_hash(c->pub.src_pod, c->pub.src_port)];
    while (*link && *link != c)
        link = &(*link)->hnext;
    if (*link)
        *link = c->hnext;
    free(c);
}

static void px_conn_del_key(struct objects *objs, int32_t pod, uint16_t port) {
    struct px_conn *c = px_conn_find(objs->proxy, pod, port);
    if (c)
        px_conn_del(objs, c);
}

/* ====== window: view / advance ====== */

/* Bytes the parser may take in one contiguous piece at parse_pos: the remainder
 * of the staging run containing the cursor. Per-conn staging mirrors the host TX
 * byte-ring, so the run extends across arrivals whose staging bytes physically
 * abut in the same pod buffer and stops at the ring wrap. Zero when the window
 * holds nothing at the cursor. */
static uint32_t px_view(struct px_conn *c) {
    if (c->parse_pos >= c->stream_end)
        return 0;
    struct px_arrival *a = c->whead;
    while (a && a->stream_base + a->len <= c->parse_pos)
        a = a->next;
    if (!a)
        return 0;
    uint64_t run_end  = a->stream_base + a->len;
    uint32_t phys_end = a->staging_off + a->len;
    for (struct px_arrival *n = a->next;
         n && n->pod_idx == a->pod_idx && n->staging_off == phys_end;
         n = n->next) {
        run_end  += n->len;
        phys_end += n->len;
    }
    return (uint32_t)(run_end - c->parse_pos);
}

/* Advance the window cursor by `consumed`. Consumed bytes NOT claimed by any
 * seg this round are DROPS — released from custody immediately. Fully-parsed
 * extents leave the window (custody continues via their pieces if in flight). */
static void px_advance(struct objects *objs, struct px_conn *c, uint32_t consumed) {
    uint64_t from = c->parse_pos, to = from + consumed;
    struct px_arrival *a = c->whead;
    while (a && a->stream_base < to) {
        uint64_t abeg = a->stream_base, aend = abeg + a->len;
        if (aend > from) {
            uint64_t cbeg = from > abeg ? from : abeg;
            uint64_t cend = to < aend ? to : aend;
            uint32_t covered = (uint32_t)(cend - cbeg);
            uint32_t dropped = covered > a->claimed_round ? covered - a->claimed_round : 0;
            a->claimed_round = 0;
            px_custody_sub(objs, a, dropped);  /* still in window (+1 ref) → cannot release here */
        }
        a = a->next;
    }
    c->parse_pos = to;
    while (c->whead && c->whead->stream_base + c->whead->len <= c->parse_pos) {
        struct px_arrival *h = c->whead;
        c->whead = h->next;
        if (!c->whead)
            c->wtail = NULL;
        h->next = NULL;
        px_custody_sub(objs, h, 1);            /* remove window ref; release iff bytes done */
    }
    /* A drop can consume past what the L7 layer was handed. Keep the hand-over
     * cursor at or ahead of the window cursor so nothing is offered twice. */
    if (c->l7_handed < c->parse_pos)
        c->l7_handed = c->parse_pos;
}

/* Drop every unparsed byte of the window (no segs). Sender slots come back
 * via the drop accounting in px_advance. */
static void px_drop_window(struct objects *objs, struct px_conn *c, const char *why) {
    uint32_t remaining = (uint32_t)(c->stream_end - c->parse_pos);
    if (remaining) {
        DOCA_LOG_WARN("proxy: dropping %u buffered bytes of conn (%d:%u): %s",
                      remaining, c->pub.src_pod, c->pub.src_port, why);
        px_advance(objs, c, remaining);
    }
}

/* Queue one FIN/EOF unit (total_len 0) in (dst_pod, dst_port)'s delivery
 * sequence. Returns 1 when queued — or when there is no delivery sequence,
 * i.e. nobody left to tell — and 0 when the unit pool was dry (nothing
 * mutated; the caller decides between retry and loss). */
static int px_queue_eof_unit(struct objects *objs, struct pod_state *dst_pod,
                             int8_t src_pod_id, int8_t src_service,
                             int8_t dst_service, uint16_t src_port,
                             uint16_t dst_port)
{
    struct dmesh_proxy *px = objs->proxy;
    uint16_t *seq = px_delivery_seq_counter(px, dst_pod->pod_id, dst_port);
    if (!seq)
        return 1;
    struct px_unit *u = px_unit_alloc(px);
    if (!u)
        return 0;
    u->src_pod_id  = src_pod_id;
    u->src_service = src_service;
    u->dst_service = dst_service;
    u->src_port    = src_port;
    u->dst_port    = dst_port;             /* the demux key at the destination */
    u->seq         = ++*seq;
    u->total_len   = 0;                    /* 0-length == FIN == EOF */
    u->org_port    = 0;                    /* synthetic EOF: never notify about itself */
    u->dst_pod_idx = (int8_t)(dst_pod - objs->pods);
    int L = px_landing_stripes(dst_pod);
    px_lane_enqueue(px, (int)(dst_pod - objs->pods),
                    (int)(dst_port % (uint16_t)L), u);
    return 1;
}

/* Queue EOF in the sender's delivery sequence. */
static int px_fin_to_sender(struct objects *objs, struct px_conn *c) {
    struct pod_state *sp = find_pod_by_id(objs, c->pub.src_pod);
    if (!sp || !pod_data_ready(sp) || !sp->host_rx_addr)
        return 1;                          /* the sender is gone too — nobody to tell */
    if (px_queue_eof_unit(objs, sp, (int8_t)c->pub.src_pod,
                          (int8_t)c->pub.dst_service,   /* "from" the service it addressed */
                          (int8_t)sp->service_id,
                          c->pub.src_port, c->pub.src_port))
        return 1;
    uint64_t stalls = px_stat_inc(&objs->proxy->stat_stall_unit);
    if (((stalls - 1u) & 0xFFFFu) == 0)
        DOCA_LOG_WARN("proxy: unit pool dry — deferring EOF to %d:%u (total %llu)",
                      c->pub.src_pod, c->pub.src_port,
                      (unsigned long long)stalls);
    return 0;
}

static void px_stall(struct px_conn *c);

/* Terminate a connection whose other pod disappeared. The surviving sender is
 * owed an EOF; if the unit pool is temporarily empty, retain only the small
 * connection object on the worker-local stall list and finish there later.
 * Input windows are dropped now so a disconnected pod's staging can be
 * reclaimed independently of that retry. */
static void
px_conn_peer_disconnected(struct objects *objs, struct px_conn *c)
{
    if (c->disconnect_pending)
        return;
    px_l7_close(objs, c, 0);
    if (c->parse_pos < c->stream_end)
        px_drop_window(objs, c, "peer pod disconnected");
    c->dead = 1;
    c->disconnect_pending = 1;
    if (px_fin_to_sender(objs, c)) {
        px_conn_del(objs, c);
        return;
    }
    c->eof_pending = 1;
    px_stall(c);
}

/* Worker-owned pod teardown. No control thread walks these tables: each ARM
 * worker closes its own L7 sessions, drops its own windows, and removes every
 * conntrack edge that names the disappearing pod. This must run before that
 * worker publishes its egress-quiesced bit, otherwise an idle long-lived L7
 * session can keep a source staging reference after the control path starts
 * destroying imported mappings. */
static int
px_worker_quiesce_pod_connections(struct objects *objs, int32_t pod_id)
{
    struct px_worker_state *worker_state = px_cur_worker;
    struct dpu_conntrack *ct = worker_state->ct;
    int closed = 0;

    for (uint32_t p = DMESH_UPORT_BASE; p < 65536u; p++) {
        struct dpu_upstream *u = &ct->upstream[p];
        if (!u->in_use ||
            (u->client_pod != pod_id && u->backend_pod != pod_id))
            continue;

        int32_t client_pod = u->client_pod;
        uint16_t client_port = u->client_port;
        int32_t backend_pod = u->backend_pod;
        px_conn_del_key(objs, backend_pod, (uint16_t)p);
        dpu_upstream_free(ct, (uint16_t)p);
        closed++;

        if (client_pod != pod_id) {
            struct px_conn *client =
                px_conn_find(objs->proxy, client_pod, client_port);
            if (client && !client->disconnect_pending) {
                px_conn_peer_disconnected(objs, client);
                closed++;
            }
        }
    }

    for (uint32_t h = 0; h < PX_CONN_HASH; h++) {
        struct px_conn *c = worker_state->buckets[h];
        while (c) {
            struct px_conn *next = c->hnext;
            if (c->pub.src_pod == pod_id) {
                px_conn_del(objs, c);
                closed++;
            } else if (!c->disconnect_pending &&
                       (c->pub.peer_pod == pod_id ||
                        c->pinned_backend == pod_id)) {
                px_conn_peer_disconnected(objs, c);
                closed++;
            }
            c = next;
        }
    }

    return closed;
}

/* Kill a conn whose stream can no longer be delivered intact and notify its
 * sender. Idempotent. If the unit pool is dry, eof_pending latches and
 * px_drain_stalled retries the EOF. */
static void px_poison(struct objects *objs, struct px_conn *c, const char *why) {
    if (c->dead)
        return;
    DOCA_LOG_ERR("proxy: poisoning conn (%d:%u): %s", c->pub.src_pod, c->pub.src_port, why);
    px_l7_close(objs, c, 0);                   /* the window is about to go */
    px_drop_window(objs, c, why);
    c->dead = 1;
    if (!px_fin_to_sender(objs, c)) {
        c->eof_pending = 1;
        px_stall(c);
    }
}

/* ====== lanes / units ====== */

static inline uint32_t px_unit_entries(const struct px_unit *u) {
    return u->total_len ? (u->total_len + PX_ENTRY_BYTES_MAX - 1) / PX_ENTRY_BYTES_MAX : 1;
}

enum px_lane_wrap_action {
    PX_LANE_WRAP_NONE,
    PX_LANE_WRAP_WAIT,
    PX_LANE_WRAP_RESET,
};

static inline enum px_lane_wrap_action
px_lane_wrap_action(uint64_t cursor, uint32_t len,
                    uint64_t region_size, uint64_t inflight)
{
    if (cursor + len <= region_size)
        return PX_LANE_WRAP_NONE;
    return inflight == 0 ? PX_LANE_WRAP_RESET : PX_LANE_WRAP_WAIT;
}

static void px_lane_enqueue(struct dmesh_proxy *px, int pod_idx, int region, struct px_unit *u) {
    struct px_lane *ln = &px->lanes[pod_idx][region];
    u->next = NULL;
    int owner = px_engine_id_for_lane(px, pod_idx, region);
    /* Same-owner traffic uses the private FIFO; cross-owner traffic uses the
     * publication inbox. */
    if (px_cur_worker && px_cur_worker->id == owner) {
        if (ln->qtail)
            ln->qtail->next = u;
        else
            ln->qhead = u;
        ln->qtail = u;
        return;
    }
    int producer = px_cur_worker ? px_cur_worker->id : owner;
    struct px_unit *old = __atomic_load_n(&ln->inq[producer], __ATOMIC_RELAXED);
    do {
        u->next = old;
    } while (!__atomic_compare_exchange_n(&ln->inq[producer], &old, u, 0,
                                           __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (px->n_workers > 1)
        px_engine_wake(&px->engines[owner]);
}

static int px_lane_inbox_nonempty(struct dmesh_proxy *px, struct px_lane *ln) {
    int nprod = px->n_workers;
    for (int s = 0; s < nprod; s++)
        if (__atomic_load_n(&ln->inq[s], __ATOMIC_ACQUIRE) != NULL)
            return 1;
    return 0;
}

/* Exchange and reverse each producer's LIFO publication list, then append it to
 * the worker-local FIFO. Returns non-zero when at least one unit was transferred. */
static int px_lane_splice_inbox(struct dmesh_proxy *px, struct px_lane *ln) {
    int moved = 0;
    int nprod = px->n_workers;
    for (int s = 0; s < nprod; s++) {
        /* Read before claiming: an empty inbox must not steal the line from its
         * producer. A push racing the read is republished by the producer wake. */
        if (__atomic_load_n(&ln->inq[s], __ATOMIC_RELAXED) == NULL)
            continue;
        struct px_unit *stack = __atomic_exchange_n(&ln->inq[s], NULL, __ATOMIC_ACQ_REL);
        if (!stack)
            continue;
        struct px_unit *head = NULL, *tail = stack;
        while (stack) {
            struct px_unit *next = stack->next;
            stack->next = head;
            head = stack;
            stack = next;
        }
        if (ln->qtail) ln->qtail->next = head; else ln->qhead = head;
        ln->qtail = tail;
        moved = 1;
    }
    return moved;
}

/* Queue one FIN unit in the destination delivery sequence. */
static int px_queue_fin_unit(struct objects *objs, struct px_conn *c,
                             struct pod_state *dst_pod,
                             uint16_t out_src_port, uint16_t out_dst_port) {
    struct pod_state *sp = find_pod_by_id(objs, c->pub.src_pod);
    if (px_queue_eof_unit(objs, dst_pod, (int8_t)c->pub.src_pod,
                          sp ? (int8_t)sp->service_id : (int8_t)DMESH_SVC_NONE,
                          (int8_t)c->pub.dst_service,
                          out_src_port, out_dst_port))
        return 1;
    uint64_t stalls = px_stat_inc(&objs->proxy->stat_stall_unit);
    if (((stalls - 1u) & 0xFFFFu) == 0)
        DOCA_LOG_WARN("proxy: unit pool dry — deferring FIN to pod %d port %u "
                      "(total %llu)", dst_pod->pod_id, out_dst_port,
                      (unsigned long long)stalls);
    return 0;
}

/* Queue EOF to the origin of an undeliverable unit. The emit path has no conn
 * to park, so a dry pool here loses the EOF outright. */
static void px_eof_to_origin(struct objects *objs, const struct px_unit *fu) {
    struct pod_state *sp = find_pod_by_id(objs, fu->src_pod_id);
    if (!sp || !pod_data_ready(sp) || !sp->host_rx_addr)
        return;                            /* the origin is gone too — nobody to tell */
    if (!px_queue_eof_unit(objs, sp, fu->src_pod_id,
                           fu->dst_service, /* "from" the service it addressed */
                           (int8_t)sp->service_id,
                           fu->org_port, fu->org_port))
        DOCA_LOG_ERR("proxy: unit pool empty — EOF to %d:%u LOST (it will hang)",
                     (int)fu->src_pod_id, fu->org_port);
}

/* Resolve a byte stream to its pinned backend. A dead pin is terminal. */
static int32_t px_resolve_backend(struct objects *objs, struct px_conn *c,
                                  int16_t cluster) {
    if (c->pinned_backend >= 0 && c->pinned_cluster == cluster) {
        struct pod_state *tp = find_pod_by_id(objs, c->pinned_backend);
        return dmesh_l4_pinned_backend(c->pinned_backend,
                                       tp && pod_data_ready(tp));
    }
    int32_t b = dpu_route_l4(objs, cluster);
    if (b >= 0) {
        c->pinned_backend = b;
        c->pinned_cluster = cluster;
    }
    return b;
}

/* A unit with everything but its source bytes: the destination resolved, the
 * upstream port issued, the delivery-sequence counter located. Its own bytes
 * come from staging (px_build_range) or from the egress arena
 * (px_ship_arm_bytes), which is the only difference between the two. */
struct px_unit_slot {
    struct px_unit *u;
    uint16_t       *seq_counter;   /* bumped once the pieces are attached */
};

static int px_unit_prepare(struct objects *objs, struct px_conn *c,
                           uint32_t len, int32_t route_dst, int reverse,
                           struct px_unit_slot *out) {
    struct dmesh_proxy *px = objs->proxy;
    struct dpu_conntrack *ct = px_cur_worker->ct;   /* private or locked shared state */
    int32_t dst_pod;
    uint16_t out_src_port = 0, out_dst_port = 0;
    uint16_t *seq_counter = NULL;

    out->u = NULL;
    out->seq_counter = NULL;

    if (reverse) {
        /* Back to the connection's own sender: no conntrack entry, no upstream
         * port, and the sequence a reply serializes against. */
        dst_pod = c->pub.src_pod;
        out_dst_port = c->pub.src_port;
        out_src_port = c->pub.src_port;
        seq_counter = &c->return_seq;
    } else if (c->pub.is_reply) {
        /* dst comes from the conntrack table; the proxy only confirms. */
        dst_pod = c->pub.peer_pod;
        out_dst_port = c->pub.peer_port;
        out_src_port = c->pub.src_port;
        struct px_conn *seq_owner = px_conn_find(px, dst_pod, out_dst_port);
        if (!seq_owner)
            return -1;
        seq_counter = &seq_owner->return_seq;
        if (route_dst >= 0 && route_dst != dst_pod)
            DOCA_LOG_WARN("proxy: reply seg dst=%d overridden by conntrack (pod %d)",
                          route_dst, dst_pod);
    } else {
        dst_pod = route_dst;
        if (dst_pod == PX_DST_DEFER)
            /* No codec named a destination → byte stream → conn-pinned LB. */
            dst_pod = px_resolve_backend(objs, c, c->pub.dst_service);
        if (dst_pod < 0) {
            DOCA_LOG_ERR("proxy: unroutable seg (svc=%d) — %u bytes dropped",
                         c->pub.dst_service, len);
            return -1;                         /* no live backend → undeliverable */
        }
    }

    struct pod_state *tp = find_pod_by_id(objs, dst_pod);
    if (!tp || !pod_data_ready(tp) || !tp->host_rx_mmap || !tp->host_rx_addr) {
        /* Throttle destination-unavailable logs. */
        static atomic_ullong dst_notready_drops;
        uint64_t drops = px_stat_inc(&dst_notready_drops);
        if (((drops - 1u) & 0xFFFFu) == 0)
            DOCA_LOG_ERR("proxy: dst pod %d not ready — %u bytes dropped (total %llu)",
                         dst_pod, len, (unsigned long long)drops);
        return -1;
    }

    if (!reverse && !c->pub.is_reply) {
        uint16_t uP = dpu_upstream_find(ct, c->pub.src_pod, c->pub.src_port, dst_pod);
        int created = 0;
        if (uP == 0) {
            /* Encode this worker in the upstream port for reply dispatch. */
            uP = dpu_upstream_create(ct, c->pub.src_pod, c->pub.src_port, dst_pod,
                                     c->l7_mode,
                                     (uint16_t)px_cur_worker->id,
                                     (uint16_t)px->n_workers);
            created = (uP != 0);
        }
        /* Clear reply state for a newly allocated upstream port. */
        if (created)
            px_conn_del_key(objs, dst_pod, uP);
        if (uP == 0) {
            /* Every uP is in use. Transient — a client FIN frees one. */
            uint64_t stalls = px_stat_inc(&px->stat_stall_uport);
            if (((stalls - 1u) & 0xFFFFu) == 0)
                DOCA_LOG_WARN("proxy: upstream space full (%d:%u -> pod %d) — stalling "
                              "(total %llu); a FIN frees one",
                              c->pub.src_pod, c->pub.src_port, dst_pod,
                              (unsigned long long)stalls);
            return 0;                          /* EAGAIN: nothing mutated */
        }
        out_src_port = uP;
        out_dst_port = uP;
        seq_counter = &ct->upstream[uP].delivery_seq;
    }

    struct px_unit *u = px_unit_alloc(px);
    if (!u) {
        uint64_t stalls = px_stat_inc(&px->stat_stall_unit);
        if (((stalls - 1u) & 0xFFFFu) == 0)
            DOCA_LOG_WARN("proxy: unit pool dry — stalling %u bytes (total %llu). The pool "
                          "is sized 1:1 with arrivals; frame/L7 can spend several per arrival.",
                          len, (unsigned long long)stalls);
        return 0;                              /* EAGAIN: the egress will free one */
    }
    struct pod_state *sp = find_pod_by_id(objs, c->pub.src_pod);
    u->src_pod_id = (int8_t)c->pub.src_pod;
    u->src_service = sp ? (int8_t)sp->service_id : (int8_t)DMESH_SVC_NONE;
    u->dst_service = (int8_t)c->pub.dst_service;
    u->src_port = out_src_port;
    u->dst_port = out_dst_port;
    u->org_port = c->pub.src_port;         /* un-rewritten: who to EOF if this unit dies */
    u->total_len = len;
    u->dst_pod_idx = (int8_t)(tp - objs->pods);
    out->u = u;
    out->seq_counter = seq_counter;
    return 1;
}

/* Gather the front stream range into one egress unit without publishing it.
 * This separation lets the L7 parser collapse complete, already-arrived frames
 * before the egress worker can observe them; L4 publishes immediately below. */
static int px_build_range(struct objects *objs, struct px_conn *c,
                          uint32_t len, int32_t route_dst,
                          struct px_unit **out_unit) {
    struct dmesh_proxy *px = objs->proxy;
    uint64_t sbeg = c->parse_pos, send_ = sbeg + len;
    struct px_unit_slot slot;

    *out_unit = NULL;
    int prepared = px_unit_prepare(objs, c, len, route_dst, 0, &slot);
    if (prepared <= 0)
        return prepared;
    struct px_unit *u = slot.u;

    /* map the stream range onto staging extents (zero-copy SG sources) */
    struct px_arrival *a = c->whead;
    while (a && a->stream_base + a->len <= sbeg)
        a = a->next;
    uint64_t pos = sbeg;
    while (pos < send_ && a) {
        uint64_t aend = a->stream_base + a->len;
        uint64_t take_end = send_ < aend ? send_ : aend;
        struct px_piece *p = px_piece_alloc(px);
        if (!p) {
            uint64_t stalls = px_stat_inc(&px->stat_stall_piece);
            if (((stalls - 1u) & 0xFFFFu) == 0)
            DOCA_LOG_WARN("proxy: piece pool dry — stalling %u bytes (total %llu)",
                          len, (unsigned long long)stalls);
            px_unit_free_node(px, u);          /* frees its pieces too */
            return 0;                          /* EAGAIN: nothing claimed yet */
        }
        p->arr = a;
        p->chunk = NULL;
        p->pod_idx = a->pod_idx;
        p->staging_off = a->staging_off + (uint32_t)(pos - a->stream_base);
        p->len = (uint32_t)(take_end - pos);
        p->next = NULL;
        if (u->pieces_tail)
            u->pieces_tail->next = p;
        else
            u->pieces = p;
        u->pieces_tail = p;
        u->npieces++;
        pos = take_end;
        if (pos >= aend)
            a = a->next;
    }
    if (pos < send_) {
        DOCA_LOG_ERR("proxy: seg maps past the window (bug) — dropped");
        px_unit_free_node(px, u);
        return -1;
    }
    for (struct px_piece *p = u->pieces; p; p = p->next)
        p->arr->claimed_round += p->len;

    u->seq = ++*slot.seq_counter;
    *out_unit = u;
    return 1;
}

static void px_enqueue_unit(struct objects *objs, struct px_unit *u) {
    struct pod_state *tp = &objs->pods[(int)u->dst_pod_idx];
    int L = px_landing_stripes(tp);
    px_lane_enqueue(objs->proxy, (int)u->dst_pod_idx,
                    (int)(u->dst_port % (uint16_t)L), u);
}

static int px_ship_range(struct objects *objs, struct px_conn *c,
                         uint32_t len, int32_t route_dst) {
    struct px_unit *u = NULL;
    int r = px_build_range(objs, c, len, route_dst, &u);
    if (r > 0)
        px_enqueue_unit(objs, u);
    return r;
}

/* Attach one arena chunk to a unit as its next SG source piece. */
static int px_unit_attach_chunk(struct dmesh_proxy *px, struct px_unit *u,
                                struct px_chunk *ch, uint32_t len) {
    struct px_piece *p = px_piece_alloc(px);
    if (!p)
        return 0;
    p->next = NULL;
    p->arr = NULL;
    p->chunk = ch;
    p->pod_idx = -1;
    p->staging_off = ch->off;
    p->len = len;
    if (u->pieces_tail)
        u->pieces_tail->next = p;
    else
        u->pieces = p;
    u->pieces_tail = p;
    u->npieces++;
    return 1;
}

/* Publish bytes the L7 layer produced. They are not in any pod's staging, so the
 * unit sources arena chunks and carries no arrival custody. One call is one
 * delivery: a payload longer than a chunk chains several rather than splitting.
 * Returns the bytes published, 0 when a pool is momentarily empty, or -1 when
 * undeliverable. */
static int px_ship_arm_bytes(struct objects *objs, struct px_conn *c,
                             int32_t backend, int reverse,
                             const uint8_t *buf, uint32_t len) {
    struct dmesh_proxy *px = objs->proxy;
    struct px_chunk *chunks[PX_L7_SEND_CHUNKS];
    uint32_t want = (len + PX_ARENA_CHUNK - 1u) / PX_ARENA_CHUNK;
    uint32_t got = 0;

    while (got < want) {
        struct px_chunk *ch = px_chunk_alloc(px);
        if (!ch)
            break;
        chunks[got++] = ch;
    }
    if (got < want) {
        /* Publish whole chunks only, so what does go out keeps its own extent. */
        len = got * PX_ARENA_CHUNK;
        uint64_t stalls = px_stat_inc(&px->stat_stall_arena);
        if (((stalls - 1u) & 0xFFFFu) == 0)
            DOCA_LOG_WARN("proxy: egress arena dry — %u of %u chunks (total %llu)",
                          got, want, (unsigned long long)stalls);
        if (got == 0)
            return 0;
    }

    struct px_unit_slot slot;
    int32_t route_dst = c->pub.is_reply ? c->pub.peer_pod :
                        (backend >= 0 ? backend : PX_DST_DEFER);
    int prepared = px_unit_prepare(objs, c, len, route_dst, reverse, &slot);
    if (prepared <= 0) {
        for (uint32_t i = 0; i < got; i++)
            px_chunk_free(px, chunks[i]);
        return prepared;
    }
    uint32_t off = 0;
    for (uint32_t i = 0; i < got; i++) {
        uint32_t n = len - off < PX_ARENA_CHUNK ? len - off : PX_ARENA_CHUNK;
        memcpy(px->arena + chunks[i]->off, buf + off, n);
        if (!px_unit_attach_chunk(px, slot.u, chunks[i], n)) {
            uint64_t stalls = px_stat_inc(&px->stat_stall_piece);
            if (((stalls - 1u) & 0xFFFFu) == 0)
                DOCA_LOG_WARN("proxy: piece pool dry — stalling %u L7 bytes (total %llu)",
                              len, (unsigned long long)stalls);
            for (uint32_t j = i; j < got; j++)   /* the attached ones go with the unit */
                px_chunk_free(px, chunks[j]);
            px_unit_free_node(px, slot.u);
            return 0;
        }
        off += n;
    }
    slot.u->seq = ++*slot.seq_counter;
    px_enqueue_unit(objs, slot.u);
    return (int)len;
}

/* Park a connection whose pool allocation failed. Its parse position remains in
 * the window, and the forward loop retries it on the next drain pass. */
static void px_stall(struct px_conn *c) {
    if (c->stalled)
        return;
    c->stalled = 1;
    c->stall_next = px_cur_worker->stall_head;
    px_cur_worker->stall_head = c;
}

/* ====== parse loop ====== */

static int px_resolve_reply_peer(struct px_conn *c) {
    if (!c->pub.is_reply)
        return 1;
    int have = 0;
    int32_t cpod = 0;
    uint16_t cport = 0;
    if (c->pub.src_port >= DMESH_UPORT_BASE) {
        struct dpu_upstream *u = &px_cur_worker->ct->upstream[c->pub.src_port];
        if (u->in_use) {
            have = 1;
            cpod = u->client_pod;
            cport = u->client_port;
        }
    }
    if (have) {
        c->pub.peer_pod = cpod;
        c->pub.peer_port = cport;
    }
    return have;
}

static void px_parse_l7(struct objects *objs, struct px_conn *c);
static int  px_l7_decide(struct objects *objs, struct px_conn *c);

static void px_parse(struct objects *objs, struct px_conn *c) {
    if (!px_resolve_reply_peer(c)) {
        px_drop_window(objs, c, "stale upstream (client closed)");
        c->dead = 1;
        return;
    }
    switch (c->l7_mode) {
    case PX_L7_OPAQUE:
    case PX_L7_FULL:
        px_parse_l7(objs, c);
        return;
    case PX_L7_DECISION:
        /* Asked once, on the request side; the reply inherits the pin through
         * conntrack. The payload then takes the path below, untouched. */
        if (!c->pub.is_reply && !c->l7_resolved && !c->l7_closed &&
            !px_l7_decide(objs, c))
            return;
        break;
    default:
        break;              /* the data plane forwards these itself */
    }

    while (c->parse_pos < c->stream_end) {
        uint32_t avail = px_view(c);
        if (avail == 0)
            break;
        int32_t dst = c->pub.is_reply ? c->pub.peer_pod : PX_DST_DEFER;
        int r = px_ship_range(objs, c, avail, dst);
        if (r == 0) {
            px_stall(c);
            break;
        }
        if (r < 0) {
            px_poison(objs, c, "stream range cannot be delivered");
            return;
        }
        px_advance(objs, c, avail);
        if (c->l7_resolved)
            c->l7_shipped += avail;
    }
}

/* ====== L7 layer: hand-over, custody, egress ====== */

/* Opaque connection handle. Not a pointer: px_conn objects are recycled, and a
 * late call from the L7 layer must not land on a different connection. The key
 * is the same (pod, port) pair the connection table is indexed by. */
static inline uint64_t px_conn_handle(const struct px_conn *c) {
    return dmesh_l7_conn_handle(c->pub.src_pod, c->pub.src_port);
}

static struct px_conn *px_conn_by_handle(struct dmesh_proxy *px, uint64_t conn) {
    return px_conn_find(px, dmesh_l7_handle_pod(conn), dmesh_l7_handle_port(conn));
}

/* Requests for a service the L7 layer carries belong to the worker that owns
 * the layer: its session state is thread-local to one Tokio runtime, and any
 * other worker would decline the connection. When every worker owns a layer
 * there is no such constraint, so the request keeps the ordinary port policy
 * and the load spreads. Replies are never routed here — an upstream port is
 * allocated in its owner's residue class (see dpu_upstream_create), so the
 * return path already lands on the same worker either way. */
int px_l7_request_owner(struct objects *objs, int32_t dst_pod_id,
                        int16_t dst_service) {
    struct dmesh_proxy *px = objs->proxy;
    if (!px || !px->l7_attached || px->l7_worker == PX_L7_WORKER_ALL ||
        dst_pod_id != DMESH_POD_BLANK)
        return -1;
    if (dst_service < 0 || dst_service >= POD_ID_SPACE)
        return -1;
    if (!px_l7_carries_bytes(px->svc_mode[dst_service]))
        return -1;
    return px->l7_worker;
}

/* Count a connection the L7 layer is not carrying, by cause and in total. */
static uint64_t px_l7_fallback(struct dmesh_proxy *px, enum px_l7_fallback_reason why) {
    px_stat_inc(&px->stat_l7_fallback_by[why]);
    return px_stat_inc(&px->stat_l7_fallback);
}

static inline uint64_t px_stat_get(atomic_ullong *counter) {
    return atomic_load_explicit(counter, memory_order_relaxed);
}

/* What the L7 layer did not carry, by cause. The counters are the process's; the
 * worker id names who is reporting them. */
static void px_l7_log_fallbacks(struct dmesh_proxy *px, int worker_id) {
    char by[192];
    int n = 0;
    for (int i = 0; i < PX_L7_FB_KINDS && n >= 0 && (size_t)n < sizeof(by); i++) {
        int w = snprintf(by + n, sizeof(by) - (size_t)n, "%s%s=%llu", i ? " " : "",
                         px_l7_fallback_name[i],
                         (unsigned long long)px_stat_get(&px->stat_l7_fallback_by[i]));
        if (w < 0)
            break;
        n += w;
    }
    DOCA_LOG_WARN("proxy: worker %d L7 fallbacks total=%llu (%s) over_release=%llu "
                  "stray_release=%llu",
                  worker_id, (unsigned long long)px_stat_get(&px->stat_l7_fallback), by,
                  (unsigned long long)px_stat_get(&px->stat_l7_over_release),
                  (unsigned long long)px_stat_get(&px->stat_l7_stray_release));
}

/* Report the L7 audit and shared-pool lock counters when they move, at most every
 * 10 s. The linkerd backend owns the worker thread for the process's lifetime and
 * reaches no detach point, so the maintenance tick is where a deployed run reports
 * them. */
#define PX_L7_REPORT_NS (10ull * 1000000000ull)

void px_l7_stats_report(struct objects *objs, int worker_id)
{
    struct dmesh_proxy *px = objs ? objs->proxy : NULL;
    if (!px || !px->l7_attached)
        return;
    uint64_t now = px_monotonic_ns();
    uint64_t last = atomic_load_explicit(&px->l7_report_ns, memory_order_relaxed);
    if (last && now - last < PX_L7_REPORT_NS)
        return;
    uint64_t mark = px_stat_get(&px->stat_l7_fallback) +
                    px_stat_get(&px->stat_l7_over_release) +
                    px_stat_get(&px->stat_l7_stray_release);
    uint64_t seen = atomic_load_explicit(&px->l7_report_mark, memory_order_relaxed);
    atomic_store_explicit(&px->l7_report_ns, now, memory_order_relaxed);
    if (mark == seen)
        return;                                /* no new fault to audit */
    atomic_store_explicit(&px->l7_report_mark, mark, memory_order_relaxed);
    px_l7_log_fallbacks(px, worker_id);
}

static void px_l7_apply_release(struct objects *objs, struct px_conn *c);

/* Tell the L7 layer to drop every reference into this connection's staging, and
 * return the load a `decision` connection placed on the backend it named — its
 * bytes never traversed the layer, so nothing else would account for them. */
static void px_l7_close(struct objects *objs, struct px_conn *c, int eof) {
    if (c->l7_tx_chunk) {                      /* reservation the layer never committed */
        px_chunk_free(objs->proxy, c->l7_tx_chunk);
        c->l7_tx_chunk = NULL;
    }
    uint64_t handle = px_conn_handle(c);
    if (c->l7_resolved) {
        l7_report(px_cur_worker->id, handle, c->stream_end, c->l7_shipped,
                  px_monotonic_ns() - c->l7_open_ns, eof ? 0 : 1);
        c->l7_resolved = 0;
    }
    if (c->l7_open) {
        uint64_t outstanding = c->l7_handed - c->parse_pos;
        if (eof)
            l7_conn_eof(px_cur_worker->id, handle);
        l7_conn_close(px_cur_worker->id, handle);
        c->l7_open = 0;
        /* Closing is where the layer gives back what it still holds. Apply it
         * while the window is still standing: the release names arrivals the
         * caller is about to reclaim. What is still held after this never came
         * back. */
        if (outstanding) {
            px_l7_apply_release(objs, c);
            if (c->l7_handed > c->parse_pos)
                DOCA_LOG_DBG("proxy: L7 close left %llu of %llu bytes in custody on "
                             "conn (%d:%u)",
                             (unsigned long long)(c->l7_handed - c->parse_pos),
                             (unsigned long long)outstanding,
                             c->pub.src_pod, c->pub.src_port);
        }
    }
    c->l7_closed = 1;
}

/* The identity DPUmesh can state about a connection. */
static void px_l7_fill_flow(struct objects *objs, const struct px_conn *c,
                            struct dmesh_l7_flow *flow) {
    memset(flow, 0, sizeof(*flow));
    flow->src_pod     = c->pub.src_pod;
    flow->dst_service = c->pub.dst_service;
    flow->src_port    = c->pub.src_port;
    flow->dst_port    = c->pub.is_reply ? c->pub.peer_port : c->pub.src_port;
    flow->peer_pod    = c->pub.is_reply ? c->pub.peer_pod : c->pub.src_pod;
    flow->mode        = c->l7_mode;
    flow->is_reply    = (uint8_t)(c->pub.is_reply != 0);
    /* Workload bound to this Pod's registration, never read from payload. */
    struct pod_state *sp = find_pod_by_id(objs, c->pub.src_pod);
    if (sp)
        memcpy(flow->workload, sp->workload, sizeof(flow->workload));
    flow->workload[sizeof(flow->workload) - 1] = '\0';
}

_Static_assert(PX_L7_DECISION == DMESH_L7_MODE_DECISION &&
               PX_L7_OPAQUE   == DMESH_L7_MODE_OPAQUE &&
               PX_L7_FULL     == DMESH_L7_MODE_FULL,
               "mode values are shared with the L7 layer");

/* Present this connection to the L7 layer. Nothing has been handed over yet, so a
 * refusal leaves the data plane's own forwarding correct. */
static int px_l7_open_conn(struct objects *objs, struct px_conn *c) {
    struct dmesh_l7_flow flow;
    /* A drain stops admitting sessions without stopping the ones in flight, so
     * identity material can be replaced against a quiet proxy. The decline
     * follows the same policy as any other: refusal under fail-closed. */
    if (__atomic_load_n(&objs->admission_drain, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&objs->admission_drain_refusals, 1, __ATOMIC_RELAXED);
        l7_control_event("admission", "draining");
        return 0;
    }
    px_l7_fill_flow(objs, c, &flow);
    int rc = l7_conn_open(px_cur_worker->id, px_conn_handle(c), &flow);
    if (rc < 0) {
        enum px_l7_fallback_reason why = px_l7_reason_of(rc);
        uint64_t n = px_l7_fallback(objs->proxy, why);
        /* The caller decides what a decline means, so say what will happen
         * rather than what the relaxed configuration would do: the deployed
         * configuration is fail-closed and ends the stream here. */
        if (((n - 1u) & 0xFFFu) == 0)
            DOCA_LOG_WARN("proxy: L7 layer declined conn (%d:%u) svc %d reason=%s — %s "
                          "(total %llu)",
                          c->pub.src_pod, c->pub.src_port, c->pub.dst_service,
                          px_l7_fallback_name[why],
                          objs->proxy->l7_fail_closed ?
                              "refusing the connection" :
                              "forwarding at L4 without policy",
                          (unsigned long long)n);
        return 0;
    }
    c->l7_open = 1;
    return 1;
}

/* Apply consumption the L7 layer reported. Deferred out of the hand-over walk:
 * px_advance unlinks fully-consumed arrivals, which the walk is still holding. */
static void px_l7_apply_release(struct objects *objs, struct px_conn *c) {
    uint32_t n = c->l7_release_pending;
    if (n == 0)
        return;
    uint64_t outstanding = c->l7_handed - c->parse_pos;
    if ((uint64_t)n > outstanding) {
        /* The layer named more than it was ever handed. The surplus is clamped to
         * keep the window consistent, and counted. */
        uint64_t bad = px_stat_inc(&objs->proxy->stat_l7_over_release);
        DOCA_LOG_DBG("proxy: L7 over-release worker %d conn (%d:%u) asked %u of %llu "
                     "outstanding (total %llu)",
                     px_cur_worker->id, c->pub.src_pod, c->pub.src_port, n,
                     (unsigned long long)outstanding, (unsigned long long)bad);
        n = (uint32_t)outstanding;
    }
    c->l7_release_pending = 0;
    if (n)
        px_advance(objs, c, n);
}

/* The staging extent holding a stream offset. Unlike px_view this never uses the
 * seam: the L7 layer takes a segment list, so extents go over as they lie and
 * nothing is linearized. Returns the pod's staging base, with the offset in
 * *pos — the (base, pos, len) triple the contract passes. */
static const uint8_t *px_stage_view(struct objects *objs, struct px_conn *c,
                                    uint64_t from, uint32_t *pos, uint32_t *avail) {
    *pos = 0;
    *avail = 0;
    struct px_arrival *a = c->whead;
    while (a && a->stream_base + a->len <= from)
        a = a->next;
    if (!a)
        return NULL;
    /* Extend across arrivals whose staging physically abuts, as px_view does, so
     * one hand-over covers a run the host wrote contiguously. */
    uint64_t run_end = a->stream_base + a->len;
    uint32_t phys_end = a->staging_off + a->len;
    for (struct px_arrival *n = a->next;
         n && n->pod_idx == a->pod_idx && n->staging_off == phys_end;
         n = n->next) {
        run_end += n->len;
        phys_end += n->len;
    }
    *pos = a->staging_off + (uint32_t)(from - a->stream_base);
    *avail = (uint32_t)(run_end - from);
    return (const uint8_t *)objs->pods[a->pod_idx].dma_buffer;
}

/* `decision` mode: one question at connection establishment, and no payload.
 * The answer admits the connection and names the backend the stream is pinned
 * to, after which the bytes take the data plane's own path and the L7 layer
 * costs nothing per byte. Returns 0 when the connection was denied and
 * poisoned. */
static int px_l7_decide(struct objects *objs, struct px_conn *c) {
    struct dmesh_l7_flow flow;
    px_l7_fill_flow(objs, c, &flow);
    struct dmesh_l7_verdict verdict;
    verdict.allow = 1;
    verdict.backend_pod = -1;

    c->l7_resolved = 1;                        /* owes a report either way */
    c->l7_open_ns = px_monotonic_ns();
    if (l7_resolve(px_cur_worker->id, &flow, &verdict) < 0) {
        uint64_t n = px_l7_fallback(objs->proxy, PX_L7_FB_NO_VERDICT);
        /* No verdict is not permission. Under fail-closed a Service assigned to
         * the layer ends here for the same reason a declined open does: the
         * stream would otherwise carry without the policy its Service selected.
         * The admission drain is deliberately not consulted — a decision-mode
         * connection builds no session and holds no identity material, so it is
         * not what a drain waits for. */
        if (objs->proxy->l7_fail_closed) {
            l7_control_event("admission", "no-verdict");
            if (((n - 1u) & 0xFFFu) == 0)
                DOCA_LOG_WARN("proxy: L7 layer gave no verdict for conn (%d:%u) "
                              "svc %d — refusing the connection (total %llu)",
                              c->pub.src_pod, c->pub.src_port, c->pub.dst_service,
                              (unsigned long long)n);
            px_poison(objs, c, "l7 layer gave no verdict for a protected service");
            return 0;
        }
        if (((n - 1u) & 0xFFFu) == 0)
            DOCA_LOG_WARN("proxy: L7 layer gave no verdict for conn (%d:%u) — "
                          "forwarding without policy (total %llu)",
                          c->pub.src_pod, c->pub.src_port, (unsigned long long)n);
        return 1;                              /* fall open */
    }
    if (!verdict.allow) {
        px_poison(objs, c, "l7 policy denied the connection");
        return 0;
    }
    if (verdict.backend_pod >= 0) {
        c->pinned_backend = verdict.backend_pod;
        c->pinned_cluster = c->pub.dst_service;
    }
    return 1;
}

/* Hand arrival extents to the L7 layer. Nothing is consumed here: the bytes stay
 * in the window, and their custody is released only when the layer reports having
 * read them, so the layer may hold them across worker revolutions. */
static void px_parse_l7(struct objects *objs, struct px_conn *c) {
    px_l7_apply_release(objs, c);
    if (c->l7_closed)
        return;
    if (!c->l7_open && !px_l7_open_conn(objs, c)) {
        c->l7_mode = PX_L7_NONE;               /* nothing handed over yet */
        c->l7_closed = 1;
        if (objs->proxy->l7_fail_closed) {
            /* The service selected a policy layer that did not take the
             * connection. Forwarding it here would carry the stream without
             * that policy, so the stream ends instead. */
            px_poison(objs, c, "l7 layer declined a protected service");
            return;
        }
        px_parse(objs, c);
        return;
    }

    px_cur_worker->in_l7_parse = 1;
    int budget = PX_L7_SEGMENTS_PER_PASS;
    while (c->l7_handed < c->stream_end && budget > 0) {
        uint64_t outstanding = c->l7_handed - c->parse_pos;
        if (outstanding >= PX_L7_CUSTODY_MAX)
            break;                             /* the layer is behind; let it drain */
        uint32_t pos, avail;
        const uint8_t *base = px_stage_view(objs, c, c->l7_handed, &pos, &avail);
        if (!base || avail == 0)
            break;
        uint32_t room = (uint32_t)(PX_L7_CUSTODY_MAX - outstanding);
        if (avail > room)
            avail = room;
        int took = l7_conn_segment(px_cur_worker->id, px_conn_handle(c),
                                   base, pos, avail);
        /* The answer is bytes taken, in [0, avail]. Above it names bytes that
         * were never offered, and the hand-over would skip payload that has
         * arrived; below zero is terminal. Neither is recoverable here. */
        if (took < 0 || (uint32_t)took > avail) {
            px_cur_worker->in_l7_parse = 0;
            px_l7_apply_release(objs, c);
            px_poison(objs, c, took < 0 ? "l7 layer rejected a segment"
                                        : "l7 layer took more than was offered");
            return;
        }
        if (took == 0)
            break;                             /* egress full — retry next pass */
        c->l7_handed += (uint32_t)took;
        budget--;
    }
    px_cur_worker->in_l7_parse = 0;
    px_l7_apply_release(objs, c);
    if (c->l7_handed < c->stream_end)
        px_stall(c);
}

/* ---- entry points the L7 layer calls ---- */

/* Resolve the caller's handle to a live connection on this worker. */
static struct px_conn *px_l7_caller_conn(int worker_id, uint64_t conn,
                                         struct objects **out_objs) {
    struct px_worker_state *worker_state = px_cur_worker;
    if (!worker_state || worker_state->id != worker_id)
        return NULL;
    struct px_conn *c = px_conn_by_handle(worker_state->objs->proxy, conn);
    if (!c || c->dead)
        return NULL;
    *out_objs = worker_state->objs;
    return c;
}

/* Verify a feed document the adapter read, against the registration keyring.
 * Returns the length of the signed prefix, or -1 when the document is unsigned
 * or its signature does not verify. The adapter parses only that prefix. */
long dmesh_l7_verify_feed(const uint8_t *document, size_t length) {
    const char *key_dir = getenv("DPUMESH_REGISTRATION_KEY_DIR");
    size_t signed_length = 0;
    if (key_dir == NULL || *key_dir == '\0' || document == NULL)
        return -1;
    if (dmesh_feed_verify((const char *)document, length, key_dir,
                          &signed_length) != DMESH_FEED_OK)
        return -1;
    return (long)signed_length;
}

int dmesh_l7_backends(int worker_id, int32_t service, int32_t *out, int max) {
    struct px_worker_state *worker_state = px_cur_worker;
    if (!worker_state || worker_state->id != worker_id || !out || max <= 0 ||
        service < 0 || service >= POD_ID_SPACE)
        return 0;
    int32_t hosts[MAX_PODS];
    int n = collect_live_hosts(worker_state->objs, (int16_t)service, hosts);
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++)
        out[i] = hosts[i];
    return n;
}

int dmesh_l7_send(int worker_id, uint64_t conn, int32_t backend_pod,
                  const uint8_t *buf, size_t len) {
    struct objects *objs;
    struct px_conn *c = px_l7_caller_conn(worker_id, conn, &objs);
    if (!c || !buf || len == 0)
        return -1;
    uint32_t take = len > PX_L7_SEND_MAX ? PX_L7_SEND_MAX : (uint32_t)len;
    return px_ship_arm_bytes(objs, c, backend_pod,
                             backend_pod == DMESH_L7_ORIGIN, buf, take);
}

uint8_t *dmesh_l7_tx_reserve(int worker_id, uint64_t conn, uint32_t *cap) {
    struct objects *objs;
    struct px_conn *c = px_l7_caller_conn(worker_id, conn, &objs);
    if (!c || !cap)
        return NULL;
    *cap = 0;
    if (c->l7_tx_chunk)                        /* one reservation at a time */
        return NULL;
    struct dmesh_proxy *px = objs->proxy;
    struct px_chunk *ch = px_chunk_alloc(px);
    if (!ch) {
        px_stat_inc(&px->stat_stall_arena);
        return NULL;
    }
    c->l7_tx_chunk = ch;
    *cap = PX_ARENA_CHUNK;
    return px->arena + ch->off;                /* already DMA-able: no second copy */
}

int dmesh_l7_tx_commit(int worker_id, uint64_t conn, int32_t backend_pod,
                       uint32_t len) {
    struct objects *objs;
    struct px_conn *c = px_l7_caller_conn(worker_id, conn, &objs);
    if (!c)
        return -1;
    struct dmesh_proxy *px = objs->proxy;
    struct px_chunk *ch = c->l7_tx_chunk;
    if (!ch)
        return -1;
    c->l7_tx_chunk = NULL;
    if (len == 0 || len > PX_ARENA_CHUNK) {
        px_chunk_free(px, ch);
        return len == 0 ? 0 : -1;
    }
    struct px_unit_slot slot;
    int32_t route_dst = c->pub.is_reply ? c->pub.peer_pod :
                        (backend_pod >= 0 ? backend_pod : PX_DST_DEFER);
    int prepared = px_unit_prepare(objs, c, len, route_dst,
                                   backend_pod == DMESH_L7_ORIGIN, &slot);
    if (prepared <= 0) {
        px_chunk_free(px, ch);
        return prepared;
    }
    if (!px_unit_attach_chunk(px, slot.u, ch, len)) {
        px_stat_inc(&px->stat_stall_piece);
        px_unit_free_node(px, slot.u);
        px_chunk_free(px, ch);
        return 0;
    }
    slot.u->seq = ++*slot.seq_counter;
    px_enqueue_unit(objs, slot.u);
    return (int)len;
}

void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len) {
    (void)pos;                                 /* releases are reported in order */
    struct px_worker_state *worker_state = px_cur_worker;
    if (!worker_state || worker_state->id != worker_id || len == 0)
        return;
    struct dmesh_proxy *px = worker_state->objs->proxy;
    struct px_conn *c = px_conn_by_handle(px, conn);
    if (!c)
        return;
    /* Everything outstanding is already spoken for, so this release names an
     * extent that was returned once. Applying it would advance the window past
     * bytes the layer has not read, so it is counted instead. Inside a hand-over
     * walk the two are legitimately out of step, and px_l7_apply_release catches
     * an over-release there. */
    if (!worker_state->in_l7_parse &&
        c->l7_handed - c->parse_pos <= (uint64_t)c->l7_release_pending) {
        uint64_t n = px_stat_inc(&px->stat_l7_stray_release);
        DOCA_LOG_DBG("proxy: L7 released %u bytes on conn (%d:%u) holding none "
                     "(pending %u, total %llu)",
                     len, c->pub.src_pod, c->pub.src_port, c->l7_release_pending,
                     (unsigned long long)n);
        return;
    }
    c->l7_release_pending += len;
    if (!worker_state->in_l7_parse)
        px_stall(c);                           /* reported outside a parse: schedule one */
}

/* ---- worker lifecycle, called from dpu_worker.c ---- */

#ifndef DMESH_L7_RUNTIME_OWNER
int px_l7_attach_worker(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs->proxy;
    if (!px || !px->l7_attached || worker_id < 0 || worker_id >= px->n_workers)
        return 0;
    px_cur_worker = &px->workers[worker_id];
    return l7_worker_attach(worker_id) < 0 ? -1 : 0;
}

int px_l7_step_worker(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs->proxy;
    if (!px || !px->l7_attached || worker_id < 0 || worker_id >= px->n_workers)
        return 0;
    px_cur_worker = &px->workers[worker_id];
    return l7_worker_step(worker_id) ? 1 : 0;
}

void px_l7_detach_worker(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs->proxy;
    if (!px || !px->l7_attached || worker_id < 0 || worker_id >= px->n_workers)
        return;
    px_cur_worker = &px->workers[worker_id];
    l7_worker_detach(worker_id);
}
#endif

/* ====== FIN ====== */

/* Returns 1 after freeing the connection. On pool pressure, retains fin_pending
 * and parks the connection. Each upstream is freed after its FIN unit is queued. */
static int px_try_fin(struct objects *objs, struct px_conn *c) {
    struct dpu_conntrack *ct = px_cur_worker->ct;
    if (!c->fin_pending)
        return 0;
    /* A tail sitting in the L7 layer is not truncation — it is payload the layer
     * has not finished with. Give it the remaining passes before the window goes,
     * then close so it stops pointing into staging. */
    if (px_l7_carries_bytes(c->l7_mode) && !c->l7_closed && !c->dead) {
        if (c->parse_pos < c->stream_end) {
            px_parse(objs, c);
            if (!c->dead && c->parse_pos < c->stream_end) {
                px_stall(c);
                return 0;
            }
        }
        px_l7_close(objs, c, 1);
    }
    /* FIN = no more input: an unconsumed tail is a truncated unit — drop it
     * (the parser could never complete it). Idempotent across retries. */
    if (c->parse_pos < c->stream_end)
        px_drop_window(objs, c, "FIN with unconsumed tail");

    if (!c->pub.is_reply) {
        /* Fan out client FIN behind each upstream's queued data. */
        for (int i = 0; i < objs->num_pods; i++) {
            int32_t b = objs->pods[i].pod_id;
            uint16_t uP = dpu_upstream_find(ct, c->pub.src_pod, c->pub.src_port, b);
            if (uP == 0)
                continue;                      /* none, or an earlier pass already did it */
            struct pod_state *B = find_pod_by_id(objs, b);
            if (B && pod_data_ready(B) && B->host_rx_addr) {
                if (!px_queue_fin_unit(objs, c, B, uP, uP)) {
                    px_stall(c);               /* pool dry — uP still findable, so retry */
                    return 0;
                }
            }
            dpu_upstream_free(ct, uP);         /* only now: this one's FIN is on its lane */
            px_conn_del_key(objs, b, uP);
        }
    } else {
        int have = 0; int32_t cpod = 0; uint16_t cport = 0;
        if (c->pub.src_port >= DMESH_UPORT_BASE) {
            struct dpu_upstream *u = &ct->upstream[c->pub.src_port];
            if (u->in_use) { have = 1; cpod = u->client_pod; cport = u->client_port; }
        }
        if (have) {
            struct pod_state *cp = find_pod_by_id(objs, cpod);
            if (cp && pod_data_ready(cp) && cp->host_rx_addr) {
                if (!px_queue_fin_unit(objs, c, cp, c->pub.src_port, cport)) {
                    px_stall(c);
                    return 0;
                }
            }
            /* The client FIN fan-out frees the upstream. */
        }
    }
    if (!px_emit_tx_ack(objs, c->fin_ack_pod,
                        c->fin_ack_port, c->fin_ack_seq)) {
        px_stall(c);
        return 0;
    }
    px_conn_del(objs, c);
    return 1;
}

/* Resume connections on one worker's stall list. */
int px_drain_stalled(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs->proxy;
    px_cur_worker = &px->workers[worker_id];
    struct px_conn *c = px_cur_worker->stall_head;
    if (!c)
        return 0;
    px_cur_worker->stall_head = NULL;           /* pop all; px_parse re-parks what still stalls */
    int did = 0;
    while (c) {
        struct px_conn *next = c->stall_next;
        c->stalled = 0;
        c->stall_next = NULL;

        /* A poisoned conn owes its sender an EOF that the pool could not carry. Pay it
         * before anything else — until it lands, the sender is blocked forever. */
        if (c->eof_pending) {
            if (!px_fin_to_sender(objs, c)) { px_stall(c); c = next; continue; }
            c->eof_pending = 0;
            did = 1;
        }
        if (c->disconnect_pending) {
            px_conn_del(objs, c);
            did = 1;
            c = next;
            continue;
        }
        if (!c->dead) {
            uint64_t before = c->parse_pos;
            px_parse(objs, c);
            if (c->parse_pos != before)
                did = 1;
        }
        /* Resume a teardown the pool cut short (a poisoned conn still gets one — its
         * upstreams must go). Returns 1 having FREED c, so nothing may touch it after. */
        if (c->fin_pending && px_try_fin(objs, c))
            did = 1;
        c = next;
    }
    return did;
}

/* Collapse one physically adjacent DPA completion into an untouched tail window
 * extent, as a pure state transition over the contiguity and custody fields. */
static int px_arrival_try_extend(struct px_conn *c, const dpu_comp_entry_t *e,
                                 uint16_t seq_delta) {
    struct px_arrival *tail = c->wtail;
    if (seq_delta != 1 || !tail || tail->pod_idx != e->pod_idx ||
        tail->ack_pod != e->src_pod_id || tail->ack_port != e->src_port ||
        tail->staging_off + tail->len != e->buf_offset ||
        tail->len + e->length > PX_ARRIVAL_COALESCE_MAX ||
        (uint32_t)(uint16_t)(e->seq - tail->ack_first_seq) + 1u >=
            PX_ACK_RUN_MAX ||
        tail->claimed_round != 0 ||
        atomic_load_explicit(&tail->unfreed, memory_order_acquire) != tail->len + 1u)
        return 0;

    atomic_fetch_add_explicit(&tail->unfreed, e->length, memory_order_relaxed);
    tail->len += e->length;
    tail->ack_seq = e->seq;
    c->stream_end += e->length;
    return 1;
}

/* ====== forward (forward completion → per-conn input window) ====== */

int px_process_forward(struct objects *objs, int worker_id, void *ventry) {
    struct dmesh_proxy *px = objs->proxy;
    dpu_comp_entry_t *e = (dpu_comp_entry_t *)ventry;

    /* Bind the worker's connection and routing state. */
    px_cur_worker = &px->workers[worker_id];

    struct pod_state *fwd = (e->pod_idx >= 0 && e->pod_idx < objs->num_pods)
        ? &objs->pods[e->pod_idx] : NULL;
    if (!fwd || !pod_data_ready(fwd) || !fwd->dma_buffer ||
        fwd->pod_id != e->src_pod_id ||
        __atomic_load_n(&fwd->dma_generation, __ATOMIC_ACQUIRE) != e->generation) {
        /* The source was unpublished after the recv callback queued this item,
         * or the slot was already re-tenanted. Never ACK through find_pod_by_id:
         * that could return the new tenant and corrupt its sequence accounting. */
        return -1;
    }

    int is_reply = (e->dst_pod_id != DMESH_POD_BLANK);
    struct px_conn *c = px_conn_get(px, e->src_pod_id, e->src_port, is_reply, 1);
    if (!c)
        return 0;                              /* alloc pressure — retry */
    if (c->pub.is_reply != is_reply) {
        /* a port number cannot flip roles (client < UPORT_BASE <= uP) */
        DOCA_LOG_ERR("proxy forward: conn (%d:%u) role flip (reply=%d->%d)",
                     e->src_pod_id, e->src_port, c->pub.is_reply, is_reply);
        c->pub.is_reply = is_reply;
    }
    if (!c->dst_service_set) {
        c->pub.dst_service = e->dst_service;
        c->dst_service_set = 1;
        /* Resolve this connection's parse path once. */
        px_resolve_route(objs, c, is_reply, e->dst_service);
    }

    uint16_t seq_delta = c->forward_seq_valid ?
        (uint16_t)(e->seq - c->forward_seq) : 1u;
    if (c->forward_seq_valid && (seq_delta == 0 || seq_delta >= 0x8000u))
        return 1;                             /* the original owns custody and ACK */
    if (c->forward_seq_valid && seq_delta != 1)
        DOCA_LOG_WARN("proxy forward: sequence gap conn=(%d:%u) prev=%u next=%u",
                      e->src_pod_id, e->src_port, c->forward_seq, e->seq);

    if (e->length == 0) {                      /* FIN */
        c->forward_seq = e->seq;
        c->forward_seq_valid = 1;
        c->fin_pending = 1;
        c->fin_ack_pod = e->src_pod_id;
        c->fin_ack_port = e->src_port;
        c->fin_ack_seq = e->seq;
        px_try_fin(objs, c);                   /* window is drained per-arrival */
        return 1;
    }

    if (c->dead) {                             /* poisoned stream: drop + ack */
        if (!px_emit_tx_ack(objs, e->src_pod_id, e->src_port, e->seq))
            return 0;
        c->forward_seq = e->seq;
        c->forward_seq_valid = 1;
        return -1;
    }

    /* The DPA must copy in <=8 KiB operations, but the ARM window does not need one
     * object per operation. Extend an untouched tail arrival while sequence and DPU
     * offsets are contiguous. Custody retains the full [ack_first_seq, ack_seq]
     * range and releases every exact ACK together. Bound the run so one incomplete
     * frame cannot hold an arbitrarily large prefix's ACK. */
    if (px_arrival_try_extend(c, e, seq_delta)) {
        c->forward_seq = e->seq;
        c->forward_seq_valid = 1;
        px_parse(objs, c);
        if (c->fin_pending)
            px_try_fin(objs, c);
        return 1;
    }

    struct px_arrival *a = px_arrival_alloc(px);
    if (!a)
        return 0;                              /* pool full — retry (backpressure) */
    c->forward_seq = e->seq;
    c->forward_seq_valid = 1;
    a->stream_base = c->stream_end;
    a->pod_idx = e->pod_idx;
    a->staging_off = e->buf_offset;
    a->len = e->length;
    /* +1 window reference: removed when the arrival leaves the window (px_advance /
     * px_conn_del). Keeps custody > 0 while bytes are still in flight so egress
     * emit cannot release the arrival before it is unlinked. */
    atomic_store_explicit(&a->unfreed, e->length + 1u, memory_order_relaxed);
    a->claimed_round = 0;
    a->ack_pod = e->src_pod_id;
    a->ack_port = e->src_port;
    a->ack_first_seq = e->seq;
    a->ack_seq = e->seq;
    a->release_next = NULL;
    __atomic_fetch_add(&objs->pods[a->pod_idx].proxy_source_refs, 1,
                       __ATOMIC_ACQ_REL);
    a->next = NULL;
    if (c->wtail)
        c->wtail->next = a;
    else
        c->whead = a;
    c->wtail = a;
    c->stream_end += a->len;
    px_parse(objs, c);
    /* px_parse may have poisoned + FIN may already be latched */
    if (c->fin_pending)
        px_try_fin(objs, c);
    return 1;
}

/* ====== egress: SG-DMA submit / completion / retirement ====== */

static inline void
px_pod_inflight_add(struct px_engine *eng, struct pod_state *pod)
{
    __atomic_fetch_add(&pod->egress_inflight_worker[eng->id].v, 1,
                       __ATOMIC_ACQ_REL);
}

static inline void
px_pod_inflight_sub(struct px_engine *eng, struct pod_state *pod)
{
    __atomic_fetch_sub(&pod->egress_inflight_worker[eng->id].v, 1,
                       __ATOMIC_ACQ_REL);
}

static uint64_t
px_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static int
px_engine_retry_grace_active(struct px_engine *eng)
{
    if (eng->retry_after_ns == 0)
        return 0;
    if (px_monotonic_ns() < eng->retry_after_ns)
        return 1;
    eng->retry_after_ns = 0;
    return 0;
}

/* Test whether a batch still targets the published pod incarnation. */
static int
px_batch_destination_current(struct objects *objs, const struct px_batch *b)
{
    if (b->pod_idx < 0 || b->pod_idx >= MAX_PODS)
        return 0;
    const struct pod_state *pod = &objs->pods[b->pod_idx];
    return pod_data_ready(pod) &&
           pod->host_rx_mmap != NULL &&
           pod->host_rx_addr != NULL &&
           __atomic_load_n(&pod->dma_generation, __ATOMIC_ACQUIRE) ==
               b->pod_generation;
}

static void
px_batch_leave_retry(struct px_engine *eng, struct px_batch *b)
{
    if (b->retry_count == 0)
        return;
    if (eng->retry_probe == b)
        eng->retry_probe = NULL;
    if (eng->retry_batches > 0)
        eng->retry_batches--;
}

static void
px_batch_record_error(struct px_engine *eng, struct px_batch *b,
                      doca_error_t status)
{
    int was_retry = b->retry_count != 0;

    if (eng->retry_probe == b)
        eng->retry_probe = NULL;
    if (status == DOCA_ERROR_IO_FAILED &&
        px_batch_destination_current(eng->objs, b) &&
        b->retry_count < PX_BATCH_RETRY_MAX) {
        if (!was_retry)
            eng->retry_batches++;
        b->retry_count++;
        b->state = PX_BATCH_RETRY_PENDING;
        return;
    }
    if (was_retry)
        px_batch_leave_retry(eng, b);
    b->state = PX_BATCH_ERROR;
}

static void
px_rev_release_bufs(struct px_op *op)
{
    if (op->src_buf) {
        doca_buf_dec_refcount(op->src_buf, NULL);
        op->src_buf = NULL;
    }
    if (op->dst_buf) {
        doca_buf_dec_refcount(op->dst_buf, NULL);
        op->dst_buf = NULL;
    }
}

static void
px_rev_complete(struct px_engine *eng, struct px_op *op)
{
    struct objects *objs = eng->objs;
    struct dmesh_proxy *px = objs->proxy;
    struct pod_state *pod = &objs->pods[op->pod_idx];
    struct px_lane *ln = &px->lanes[op->pod_idx][op->region];
    struct px_rev_pub *pub = &ln->rev;

    px_rev_release_bufs(op);

    if (op->kind == 2) {
        struct dmesh_rev_ring_entry *stage =
            (struct dmesh_rev_ring_entry *)(px->rev_scratch +
                PX_REV_ENTRIES_OFF(op->pod_idx, op->region));
        pub->producer_tail = pub->publish_tail;
        if (pub->count > pub->publish_count)
            memmove(stage, stage + pub->publish_count,
                    (size_t)(pub->count - pub->publish_count) *
                        sizeof(*stage));
        pub->count -= pub->publish_count;
        pub->publish_count = 0;
        pub->ctrl_after_publish = 1;
        pub->state = PX_REV_CTRL_PENDING;
        /* This is the reclaim publication fence: the control thread may
         * destroy the remote mmap as soon as the count reaches zero. Publish
         * it only after every DOCA buffer referring to that mmap is returned. */
        px_pod_inflight_sub(eng, pod);
        return;
    }

    /* The landed control block is struct dmesh_rev_ring_ctrl, read positionally:
     * ctrl[0] is consumer_head and ctrl[1] is arm_epoch. */
    uint64_t *ctrl = (uint64_t *)(px->rev_scratch +
        PX_REV_CTRL_OFF(op->pod_idx, op->region));
    pub->cached_head = ctrl[0];
    uint64_t epoch = ctrl[1];
    if (pub->ctrl_after_publish && epoch != 0 &&
        epoch != pub->notified_epoch) {
        pub->notified_epoch = epoch;
        dpu_request_host_doorbell(objs, pod, epoch);
    }
    pub->ctrl_after_publish = 0;
    pub->state = PX_REV_IDLE;
    px_pod_inflight_sub(eng, pod);
}

static void px_dma_done_cb(struct doca_dma_task_memcpy *t, union doca_data tud,
                           union doca_data cud) {
    struct px_engine *eng = (struct px_engine *)cud.ptr;
    struct objects *objs = eng->objs;
    struct px_op *op = (struct px_op *)tud.ptr;
    doca_task_free(doca_dma_task_memcpy_as_task(t));
    eng->dma_tasks_inflight--;
    if (op->kind >= 2) {
        px_rev_complete(eng, op);
        return;
    }
    if (op->kind == 1) {                       /* credit refresh landed */
        struct pod_state *pod = &objs->pods[op->pod_idx];
        struct px_lane *ln = &objs->proxy->lanes[op->pod_idx][op->region];
        int K = pod->k_rings > 0 ? pod->k_rings : 1;
        int L = px_landing_stripes(pod);
        int nshards = K / L;
        volatile uint64_t *freed = (volatile uint64_t *)
            (objs->proxy->scratch +
             PX_SCRATCH_OFF(op->pod_idx, op->region));
        uint64_t total = 0;
        for (int i = 0; i < nshards; i++)
            total += freed[i];
        uint64_t previous = ln->cached_freed;
        if (total > ln->sent_entries)
            DOCA_LOG_ERR("proxy: reverse credit exceeds submissions "
                         "pod=%d region=%d freed=%llu sent=%llu",
                         pod->pod_id, op->region,
                         (unsigned long long)total,
                         (unsigned long long)ln->sent_entries);
        ln->cached_freed = total;
        ln->refresh_after_ns = total == previous
            ? px_monotonic_ns() + PX_CREDIT_REFRESH_RETRY_NS : 0;
        ln->refresh_inflight = 0;
        if (op->src_buf) { doca_buf_dec_refcount(op->src_buf, NULL); op->src_buf = NULL; }
        if (op->dst_buf) { doca_buf_dec_refcount(op->dst_buf, NULL); op->dst_buf = NULL; }
        px_pod_inflight_sub(eng, pod);
        return;
    }
    struct px_batch *b = op->batch;
    if (b->src_head) { doca_buf_dec_refcount(b->src_head, NULL); b->src_head = NULL; }
    if (b->dst_buf)  { doca_buf_dec_refcount(b->dst_buf, NULL);  b->dst_buf = NULL; }
    px_batch_leave_retry(eng, b);
    b->state = PX_BATCH_DONE;                  /* retired in px_drain (in order) */
    px_pod_inflight_sub(eng, &objs->pods[b->pod_idx]);
}

static void px_dma_err_cb(struct doca_dma_task_memcpy *t, union doca_data tud,
                          union doca_data cud) {
    struct px_engine *eng = (struct px_engine *)cud.ptr;
    struct objects *objs = eng->objs;
    struct px_op *op = (struct px_op *)tud.ptr;
    doca_error_t st = doca_task_get_status(doca_dma_task_memcpy_as_task(t));
    doca_task_free(doca_dma_task_memcpy_as_task(t));
    eng->dma_tasks_inflight--;

    /* IO failures stall the shared engine context. */
    if (st == DOCA_ERROR_IO_FAILED)
        eng->dma_stalled = 1;

    if (op->kind >= 2) {
        struct pod_state *pod = &objs->pods[op->pod_idx];
        struct px_rev_pub *pub =
            &objs->proxy->lanes[op->pod_idx][op->region].rev;
        px_rev_release_bufs(op);
        if (op->kind == 2)
            pub->state = PX_REV_IDLE;
        else {
            pub->state = PX_REV_IDLE;
            if (pub->ctrl_after_publish)
                dpu_request_host_doorbell(objs, pod,
                    pub->notified_epoch + 1u);
        }
        DOCA_LOG_ERR("proxy: reverse ring DMA kind=%d failed "
                     "(pod slot %d region %d): %s",
                     op->kind, op->pod_idx, op->region,
                     doca_error_get_descr(st));
        px_pod_inflight_sub(eng, pod);
        return;
    }

    if (op->kind == 1) {
        DOCA_LOG_ERR("proxy: credit refresh DMA failed: %s", doca_error_get_descr(st));
        struct px_lane *ln =
            &objs->proxy->lanes[op->pod_idx][op->region];
        ln->refresh_inflight = 0;
        ln->refresh_after_ns = px_monotonic_ns() +
            PX_CREDIT_REFRESH_RETRY_NS;
        if (op->src_buf) { doca_buf_dec_refcount(op->src_buf, NULL); op->src_buf = NULL; }
        if (op->dst_buf) { doca_buf_dec_refcount(op->dst_buf, NULL); op->dst_buf = NULL; }
        px_pod_inflight_sub(eng, &objs->pods[op->pod_idx]);
        return;
    }
    struct px_batch *b = op->batch;
    DOCA_LOG_ERR("proxy: SG-DMA batch failed (pod slot %d region %d, %u bytes): %s",
                 b->pod_idx, b->region, b->bytes, doca_error_get_descr(st));
    if (b->src_head) { doca_buf_dec_refcount(b->src_head, NULL); b->src_head = NULL; }
    if (b->dst_buf)  { doca_buf_dec_refcount(b->dst_buf, NULL);  b->dst_buf = NULL; }
    px_batch_record_error(eng, b, st);
    px_pod_inflight_sub(eng, &objs->pods[b->pod_idx]);
}

/* Submit a batch at its recorded landing position. */
static doca_error_t
px_batch_submit_dma(struct objects *objs, struct px_engine *eng,
                    struct pod_state *pod, struct px_batch *b)
{
    struct doca_buf *src_head = NULL;
    struct doca_buf *dst = NULL;
    doca_error_t ret = DOCA_SUCCESS;

    /* A batch reaches submit with units, bytes, and no attached buffers. */
    if (b->units == NULL || b->bytes == 0 ||
        b->src_head != NULL || b->dst_buf != NULL) {
        DOCA_LOG_ERR("proxy: refusing malformed SG-DMA batch (pod slot %d region %d, "
                     "%u bytes, units=%p src=%p dst=%p)",
                     b->pod_idx, b->region, b->bytes, (void *)b->units,
                     (void *)b->src_head, (void *)b->dst_buf);
        return DOCA_ERROR_INVALID_VALUE;
    }

    for (struct px_unit *u = b->units;
         u && ret == DOCA_SUCCESS; u = u->next) {
        for (struct px_piece *p = u->pieces; p; p = p->next) {
            /* Two source kinds: arrival staging (forwarded bytes) and the egress
             * arena (bytes the L7 layer produced). One SG list may hold both. */
            struct doca_mmap *src_mmap;
            void *addr;
            if (p->chunk) {
                src_mmap = objs->proxy->arena_mmap;
                addr = objs->proxy->arena + p->staging_off;
            } else {
                struct pod_state *src_pod = &objs->pods[p->pod_idx];
                src_mmap = src_pod->local_mmap;
                addr = (uint8_t *)src_pod->dma_buffer + p->staging_off;
            }
            struct doca_buf *src = NULL;
            ret = doca_buf_inventory_buf_get_by_addr(
                eng->inv, src_mmap, addr, p->len, &src);
            if (ret != DOCA_SUCCESS)
                break;
            ret = doca_buf_set_data(src, addr, p->len);
            if (ret != DOCA_SUCCESS) {
                doca_buf_dec_refcount(src, NULL);
                break;
            }
            if (!src_head) {
                src_head = src;
            } else {
                ret = doca_buf_chain_list(src_head, src);
                if (ret != DOCA_SUCCESS) {
                    doca_buf_dec_refcount(src, NULL);
                    break;
                }
            }
        }
    }
    if (ret == DOCA_SUCCESS) {
        uint64_t dst_off = b->units->landing_pos;
        ret = doca_buf_inventory_buf_get_by_addr(
            eng->inv, pod->host_rx_mmap,
            (uint8_t *)pod->host_rx_addr + dst_off, b->bytes, &dst);
    }

    struct doca_dma_task_memcpy *task = NULL;
    if (ret == DOCA_SUCCESS) {
        union doca_data user_data = { .ptr = &b->op };
        ret = doca_dma_task_memcpy_alloc_init(
            eng->dma, src_head, dst, user_data, &task);
        if (ret == DOCA_SUCCESS) {
            ret = doca_task_try_submit(
                doca_dma_task_memcpy_as_task(task));
            if (ret != DOCA_SUCCESS)
                doca_task_free(doca_dma_task_memcpy_as_task(task));
        }
    }
    if (ret != DOCA_SUCCESS) {
        if (src_head)
            doca_buf_dec_refcount(src_head, NULL);
        if (dst)
            doca_buf_dec_refcount(dst, NULL);
        return ret;
    }

    b->src_head = src_head;
    b->dst_buf = dst;
    b->state = PX_BATCH_INFLIGHT;
    eng->dma_tasks_inflight++;
    px_pod_inflight_add(eng, pod);
    return DOCA_SUCCESS;
}

/* Read all host credit shards for one landing stripe. */
static void px_lane_refresh_credit(struct objects *objs, struct px_engine *eng,
                                   int pod_idx, struct pod_state *pod, int region,
                                   struct px_lane *ln) {
    struct dmesh_proxy *px = objs->proxy;
    uint64_t now = px_monotonic_ns();
    if (ln->refresh_inflight || eng->dma_tasks_inflight >= PX_DMA_TASKS ||
        now < ln->refresh_after_ns)
        return;
    int K = pod->k_rings > 0 ? pod->k_rings : 1;
    int L = px_landing_stripes(pod);
    int nshards = K / L;
    uint8_t *cell = px->scratch + PX_SCRATCH_OFF(pod_idx, region);
    struct px_op *op = &px->refresh_ops[pod_idx][region];
    struct doca_buf *src_head = NULL, *dst = NULL;
    doca_error_t ret = DOCA_SUCCESS;

    for (int shard = region; shard < K; shard += L) {
        if (!pod->ring_mmaps[shard] || !pod->ring_host_addrs[shard]) {
            if (!ln->warned_no_credit_addr) {
                DOCA_LOG_ERR("proxy: pod %d stripe %d has no credit shard %d",
                             pod->pod_id, region, shard);
                ln->warned_no_credit_addr = 1;
            }
            goto fail;
        }
        uint8_t *host_credit =
            (uint8_t *)pod->ring_host_addrs[shard] +
            (size_t)DMA_RING_CREDIT_SLOT(DMA_RING_SIZE) *
                sizeof(struct dma_desc);
        struct doca_buf *src = NULL;
        ret = doca_buf_inventory_buf_get_by_addr(
            eng->inv, pod->ring_mmaps[shard], host_credit,
            sizeof(uint64_t), &src);
        if (ret != DOCA_SUCCESS)
            goto fail;
        ret = doca_buf_set_data(src, host_credit, sizeof(uint64_t));
        if (ret != DOCA_SUCCESS) {
            doca_buf_dec_refcount(src, NULL);
            goto fail;
        }
        if (!src_head) {
            src_head = src;
        } else {
            ret = doca_buf_chain_list(src_head, src);
            if (ret != DOCA_SUCCESS) {
                doca_buf_dec_refcount(src, NULL);
                goto fail;
            }
        }
    }
    ret = doca_buf_inventory_buf_get_by_addr(
        eng->inv, px->scratch_mmap, cell,
        (size_t)nshards * sizeof(uint64_t), &dst);
    if (ret != DOCA_SUCCESS)
        goto fail;

    op->kind = 1;
    op->pod_idx = pod_idx;
    op->region = region;
    op->src_buf = src_head;
    op->dst_buf = dst;
    union doca_data ud = { .ptr = op };
    struct doca_dma_task_memcpy *t = NULL;
    ret = doca_dma_task_memcpy_alloc_init(eng->dma, src_head, dst, ud, &t);
    if (ret != DOCA_SUCCESS) {
        if (ret == DOCA_ERROR_BAD_STATE)
            eng->dma_stalled = 1;
        goto fail;
    }
    ret = doca_task_try_submit(doca_dma_task_memcpy_as_task(t));
    if (ret != DOCA_SUCCESS) {
        doca_task_free(doca_dma_task_memcpy_as_task(t));
        if (ret == DOCA_ERROR_BAD_STATE)
            eng->dma_stalled = 1;
        goto fail;
    }
    ln->refresh_inflight = 1;
    eng->dma_tasks_inflight++;
    px_pod_inflight_add(eng, pod);
    return;
fail:
    if (src_head) doca_buf_dec_refcount(src_head, NULL);
    if (dst) doca_buf_dec_refcount(dst, NULL);
    op->src_buf = op->dst_buf = NULL;
    ln->refresh_after_ns = now + PX_CREDIT_REFRESH_RETRY_NS;
}

static int
px_rev_submit_copy(struct px_engine *eng, int pod_idx, int region, int kind,
                   struct doca_mmap *src_mmap, void *src_addr,
                   struct doca_mmap *dst_mmap, void *dst_addr, size_t len)
{
    if (eng->dma_stalled || eng->dma_tasks_inflight >= PX_DMA_TASKS)
        return 0;
    struct dmesh_proxy *px = eng->objs->proxy;
    struct px_op *op = &px->rev_ops[pod_idx][region];
    struct doca_buf *src = NULL, *dst = NULL;
    doca_error_t ret = doca_buf_inventory_buf_get_by_addr(
        eng->inv, src_mmap, src_addr, len, &src);
    if (ret != DOCA_SUCCESS)
        return 0;
    ret = doca_buf_set_data(src, src_addr, len);
    if (ret != DOCA_SUCCESS)
        goto fail;
    ret = doca_buf_inventory_buf_get_by_addr(
        eng->inv, dst_mmap, dst_addr, len, &dst);
    if (ret != DOCA_SUCCESS)
        goto fail;

    op->kind = kind;
    op->pod_idx = pod_idx;
    op->region = region;
    op->src_buf = src;
    op->dst_buf = dst;
    union doca_data ud = { .ptr = op };
    struct doca_dma_task_memcpy *task = NULL;
    ret = doca_dma_task_memcpy_alloc_init(eng->dma, src, dst, ud, &task);
    if (ret != DOCA_SUCCESS)
        goto fail;
    ret = doca_task_try_submit(doca_dma_task_memcpy_as_task(task));
    if (ret != DOCA_SUCCESS) {
        doca_task_free(doca_dma_task_memcpy_as_task(task));
        goto fail;
    }
    eng->dma_tasks_inflight++;
    px_pod_inflight_add(eng, &eng->objs->pods[pod_idx]);
    return 1;
fail:
    if (src) doca_buf_dec_refcount(src, NULL);
    if (dst) doca_buf_dec_refcount(dst, NULL);
    op->src_buf = op->dst_buf = NULL;
    if (ret == DOCA_ERROR_BAD_STATE)
        eng->dma_stalled = 1;
    return 0;
}

static int
px_rev_kick_lane(struct px_engine *eng, int pod_idx, int region)
{
    struct dmesh_proxy *px = eng->objs->proxy;
    struct pod_state *pod = &eng->objs->pods[pod_idx];
    struct px_rev_pub *pub = &px->lanes[pod_idx][region].rev;
    uint8_t *local_base = px->rev_scratch +
        PX_REV_ENTRIES_OFF(pod_idx, region);
    uint8_t *remote_base = (uint8_t *)pod->rev_ring_host_addrs[region];
    if (!pod->rev_ring_mmaps[region] || !remote_base)
        return 0;

    if (pub->state == PX_REV_IDLE) {
        if (pub->count == 0)
            return 0;
        if (pub->producer_tail + pub->count - pub->cached_head >
            DMA_REV_RING_SIZE) {
            pub->ctrl_after_publish = 0;
            pub->state = PX_REV_CTRL_PENDING;
        } else {
            uint32_t contiguous = DMA_REV_RING_SIZE -
                (uint32_t)(pub->producer_tail % DMA_REV_RING_SIZE);
            pub->publish_count = pub->count < contiguous ?
                pub->count : contiguous;
            struct dmesh_rev_ring_entry *stage =
                (struct dmesh_rev_ring_entry *)local_base;
            for (uint32_t i = 0; i < pub->publish_count; i++)
                stage[i].publish_seq = pub->producer_tail + i + 1u;
            size_t bytes = (size_t)pub->publish_count *
                sizeof(struct dmesh_rev_ring_entry);
            void *remote_entries = remote_base +
                (size_t)(pub->producer_tail % DMA_REV_RING_SIZE) *
                    sizeof(struct dmesh_rev_ring_entry);
            if (!px_rev_submit_copy(eng, pod_idx, region, 2,
                                    px->rev_scratch_mmap, local_base,
                                    pod->rev_ring_mmaps[region],
                                    remote_entries, bytes))
                return 0;
            pub->publish_tail = pub->producer_tail + pub->publish_count;
            pub->state = PX_REV_META_INFLIGHT;
            return 1;
        }
    }

    if (pub->state == PX_REV_CTRL_PENDING) {
        void *remote_ctrl = remote_base +
            (size_t)DMA_REV_RING_SIZE *
                sizeof(struct dmesh_rev_ring_entry) + 64u;
        void *local_ctrl = px->rev_scratch +
            PX_REV_CTRL_OFF(pod_idx, region);
        if (!px_rev_submit_copy(eng, pod_idx, region, 4,
                                pod->rev_ring_mmaps[region], remote_ctrl,
                                px->rev_scratch_mmap, local_ctrl, 64u))
            return 0;
        pub->state = PX_REV_CTRL_INFLIGHT;
        return 1;
    }
    return 0;
}

/* Reserve one zeroed entry in the (pod, dst_port) region's worker-owned
 * reverse stage. NULL when the region belongs to another worker or the stage
 * is full or mid-publication (backpressure — the caller retries later). */
static struct dmesh_rev_ring_entry *
px_rev_stage_append(struct px_engine *eng, struct pod_state *pod,
                    uint16_t dst_port)
{
    struct dmesh_proxy *px = eng->objs->proxy;
    int pod_idx = (int)(pod - eng->objs->pods);
    int region = dst_port % px_landing_stripes(pod);
    if (region % px->n_workers != eng->id)
        return NULL;
    struct px_rev_pub *pub = &px->lanes[pod_idx][region].rev;
    if (pub->state == PX_REV_META_INFLIGHT ||
        pub->count >= PX_REV_STAGE_ENTRIES)
        return NULL;
    struct dmesh_rev_ring_entry *stage =
        (struct dmesh_rev_ring_entry *)(px->rev_scratch +
            PX_REV_ENTRIES_OFF(pod_idx, region));
    struct dmesh_rev_ring_entry *dst = &stage[pub->count++];
    memset(dst, 0, sizeof(*dst));
    return dst;
}

/* Acknowledge the run [seq, seq + count) in one entry. */
static int
px_rev_append_ack(struct px_engine *eng, struct pod_state *pod,
                  uint16_t port, uint16_t seq, uint16_t count)
{
    struct dmesh_rev_ring_entry *dst = px_rev_stage_append(eng, pod, port);
    if (!dst)
        return 0;
    dst->kind = DMESH_REV_ENTRY_TX_ACK;
    dst->payload.ack.port = port;
    dst->payload.ack.seq = seq;
    dst->payload.ack.seq_count = count;
    return 1;
}

static int
px_ack_retry_handoffs(struct px_engine *eng)
{
    struct dmesh_proxy *px = eng->objs->proxy;
    int moved = 0;
    while (eng->ack_retry_head) {
        struct px_arrival *a = eng->ack_retry_head;
        /* A vanished sender has no staging owner; this engine's drain frees it. */
        struct pod_state *src = find_pod_by_id(eng->objs, a->ack_pod);
        int owner = src ? px_rev_owner(px, src, a->ack_port) : eng->id;
        if (owner < 0 || owner >= px->n_workers ||
            !px_ack_queue_push(&px->engines[owner].ack_releases, a))
            break;
        eng->ack_retry_head = a->release_next;
        if (!eng->ack_retry_head)
            eng->ack_retry_tail = NULL;
        a->release_next = NULL;
        moved++;
    }
    return moved;
}

static int
px_drain_ack_releases(struct px_engine *eng, uint32_t budget)
{
    struct objects *objs = eng->objs;
    int emitted = 0;
    while ((uint32_t)emitted < budget) {
        struct px_arrival *a = px_ack_queue_front(&eng->ack_releases);
        if (!a)
            break;
        struct pod_state *src = find_pod_by_id(objs, a->ack_pod);
        if (!src) {
            /* A disconnected sender has no mapping or TX state to reclaim. */
            __atomic_fetch_sub(&objs->pods[a->pod_idx].proxy_source_refs, 1,
                               __ATOMIC_ACQ_REL);
            px_arrival_free(objs->proxy, a);
            px_ack_queue_pop(&eng->ack_releases);
            emitted++;
            continue;
        }
        /* PX_ACK_RUN_MAX keeps the count inside the field that carries it. */
        uint16_t count = (uint16_t)(a->ack_seq - a->ack_first_seq + 1u);
        if (!px_rev_append_ack(eng, src, a->ack_port, a->ack_first_seq, count))
            break;
        emitted++;
        __atomic_fetch_sub(&objs->pods[a->pod_idx].proxy_source_refs, 1,
                           __ATOMIC_ACQ_REL);
        px_arrival_free(objs->proxy, a);
        px_ack_queue_pop(&eng->ack_releases);
    }
    return emitted;
}

/* Append one REV_DONE entry to the destination region's worker-owned stage. */
static int px_emit_rev_entry(struct px_engine *eng, struct pod_state *pod,
                             const struct dmesh_rev_done_entry *e) {
    struct dmesh_rev_ring_entry *dst =
        px_rev_stage_append(eng, pod, e->dst_port);
    if (!dst)
        return 0;
    dst->kind = DMESH_REV_ENTRY_DONE;
    dst->payload.done = *e;
    return 1;
}

static int px_lane_submit(struct objects *objs, struct px_engine *eng, int pod_idx,
                          struct pod_state *pod, int region, struct px_lane *ln);

/* Append retired units to this worker's emission queue. */
static void
px_emit_append(struct px_engine *eng, struct px_unit *head,
               struct px_unit *tail, int pod_idx, uint32_t count)
{
    __atomic_fetch_add(&eng->objs->pods[pod_idx].egress_pending_emit,
                       count, __ATOMIC_RELAXED);
    if (eng->emit_tail)
        eng->emit_tail->next = head;
    else
        eng->emit_head = head;
    eng->emit_tail = tail;
}

/* Retire completed lane batches into the worker emission queue. */
static int px_lane_retire(struct px_engine *eng, struct px_lane *ln) {
    if (!ln->fhead ||
        (ln->fhead->state != PX_BATCH_DONE &&
         ln->fhead->state != PX_BATCH_ERROR))
        return 0;

    struct px_unit *head = NULL, *tail = NULL;
    uint32_t count = 0;
    while (ln->fhead &&
           (ln->fhead->state == PX_BATCH_DONE ||
            ln->fhead->state == PX_BATCH_ERROR)) {
        struct px_batch *b = ln->fhead;
        int error = b->state == PX_BATCH_ERROR;
        if (error)
            ln->sent_entries -= b->entries;   /* host never freed these landings */
        for (struct px_unit *u = b->units; u; ) {
            struct px_unit *nx = u->next;
            u->err = (uint8_t)error;
            u->next = NULL;
            if (tail) tail->next = u; else head = u;
            tail = u;
            count++;
            u = nx;
        }
        b->units = NULL;
        ln->fhead = b->next;
        if (!ln->fhead) ln->ftail = NULL;
        px_batch_free_node(eng, b);
    }
    if (!head)
        return 0;
    px_emit_append(eng, head, tail, (int)head->dst_pod_idx, count);
    return 1;
}

/* Emit completed units and release custody. */
static int px_engine_emit(struct objects *objs, struct px_engine *eng) {
    struct dmesh_proxy *px = objs->proxy;
    int did = 0;
    int32_t eof_pod = -1; uint16_t eof_port = 0;   /* collapse a run of same-origin failures */
    while (eng->emit_head) {
        struct px_unit *u = eng->emit_head;
        struct pod_state *pod = &objs->pods[(int)u->dst_pod_idx];
        if (!pod_data_ready(pod))
            u->err = 1;                        /* destination disappeared after DMA */
        if (u->err && u->org_port &&
            !(u->src_pod_id == eof_pod && u->org_port == eof_port)) {
            px_eof_to_origin(objs, u);             /* undelivered != delivered */
            eof_pod = u->src_pod_id; eof_port = u->org_port;
        }
        if (!u->err) {
            struct dmesh_rev_done_entry e;
            memset(&e, 0, sizeof(e));
            e.src_pod_id = u->src_pod_id;
            e.src_service = u->src_service;
            e.dst_service = u->dst_service;
            e.src_port = u->src_port;
            e.dst_port = u->dst_port;
            e.seq = u->seq;
            if (u->total_len == 0) {           /* FIN / notify-only */
                if (!u->emit_fin_done) {
                    e.length = 0;
                    e.pos = u->landing_pos;
                    if (!px_emit_rev_entry(eng, pod, &e))
                        return did;             /* backpressure — resume later */
                    u->emit_fin_done = 1;
                    did = 1;
                }
            } else {
                while (u->emit_off < u->total_len) {
                    uint32_t elen = u->total_len - u->emit_off;
                    if (elen > PX_ENTRY_BYTES_MAX)
                        elen = PX_ENTRY_BYTES_MAX;
                    e.length = (uint16_t)elen;
                    e.pos = u->landing_pos + u->emit_off;
                    if (!px_emit_rev_entry(eng, pod, &e))
                        return did;             /* backpressure — resume later */
                    u->emit_off += elen;
                    did = 1;
                }
            }
        }
        /* custody: the SG op has read (or abandoned) these staging bytes */
        for (struct px_piece *p = u->pieces; p; p = p->next)
            px_piece_release(objs, p);         /* egressed bytes; release iff last */
        eng->emit_head = u->next;
        if (!eng->emit_head) eng->emit_tail = NULL;
        __atomic_fetch_sub(&pod->egress_pending_emit, 1, __ATOMIC_ACQ_REL);
        px_unit_free_node(px, u);
        did = 1;
    }
    return did;
}

/* Retire queued units for a disconnected destination. */
static int px_lane_drop_dead(struct px_engine *eng, struct px_lane *ln) {
    if (!ln->qhead)
        return 0;
    struct px_unit *head = ln->qhead, *tail = ln->qtail;
    uint32_t count = 0;
    ln->qhead = ln->qtail = NULL;
    for (struct px_unit *u = head; u; u = u->next) {
        u->err = 1;                            /* skip REV_DONE; custody still released */
        count++;
    }
    px_emit_append(eng, head, tail, (int)head->dst_pod_idx, count);
    return 1;
}

/* Terminalize retry-pending batches during pod cleanup. */
static int
px_lane_fail_pending_retries(struct px_engine *eng, struct px_lane *ln)
{
    int failed = 0;
    for (struct px_batch *b = ln->fhead; b; b = b->next) {
        if (b->state != PX_BATCH_RETRY_PENDING)
            continue;
        px_batch_leave_retry(eng, b);
        b->state = PX_BATCH_ERROR;
        failed = 1;
    }
    return failed;
}

/* Clear reverse publication state after its in-flight task exits. */
static int
px_rev_drop_dead(struct px_rev_pub *pub)
{
    if (pub->state == PX_REV_META_INFLIGHT ||
        pub->state == PX_REV_CTRL_INFLIGHT)
        return 0;
    int changed = pub->count != 0 ||
                  pub->publish_count != 0 ||
                  pub->state != PX_REV_IDLE ||
                  pub->producer_tail != 0 ||
                  pub->publish_tail != 0 ||
                  pub->cached_head != 0 ||
                  pub->notified_epoch != 0;
    if (changed)
        memset(pub, 0, sizeof(*pub));
    return changed;
}

static void
px_engine_latch_bad_state(struct px_engine *eng, doca_error_t status)
{
    if (!eng->dma_fault_warned) {
        DOCA_LOG_ERR("proxy: egress dma ctx faulted (engine %d): %s — "
                     "stopping submit (needs restart)", eng->id,
                     doca_error_get_descr(status));
        eng->dma_fault_warned = 1;
    }
    eng->dma_stalled = 1;
}

/* Retry the lane head with at most one retry DMA active per engine. */
static int
px_lane_retry_head(struct objects *objs, struct px_engine *eng,
                   struct pod_state *pod, struct px_lane *ln)
{
    struct px_batch *b = ln->fhead;
    if (!b || b->state != PX_BATCH_RETRY_PENDING ||
        eng->retry_probe != NULL || eng->dma_stalled)
        return 0;
    if (px_engine_retry_grace_active(eng))
        return 0;
    if (!px_batch_destination_current(objs, b)) {
        px_batch_leave_retry(eng, b);
        b->state = PX_BATCH_ERROR;
        return 1;
    }

    doca_error_t ret = px_batch_submit_dma(objs, eng, pod, b);
    if (ret == DOCA_SUCCESS) {
        eng->retry_probe = b;
        DOCA_LOG_WARN("proxy: retrying collateral SG-DMA batch "
                      "(pod slot %d region %d, %u bytes)",
                      b->pod_idx, b->region, b->bytes);
        return 1;
    }
    if (ret == DOCA_ERROR_BAD_STATE)
        px_engine_latch_bad_state(eng, ret);
    return 0;
}

/* Reset the per-incarnation state of one lane. Queues are NOT touched: they hold
 * units, whose lifetime is custody-managed (px_lane_drop_dead retires them when the
 * pod goes not-ready). Only the credit/landing accounting is incarnation-scoped. */
static void px_lane_rearm(struct px_lane *ln, uint32_t gen) {
    ln->cursor = 0;
    ln->sent_entries = 0;
    ln->cached_freed = 0;
    ln->refresh_after_ns = 0;
    ln->refresh_inflight = 0;
    ln->warned_no_credit_addr = 0;
    ln->pod_generation = gen;
}

/* Restart this engine's shared DMA context once it reaches IDLE. */
static int px_engine_recover(struct px_engine *eng) {
    enum doca_ctx_states st;

    if (doca_ctx_get_state(eng->dma_ctx, &st) != DOCA_SUCCESS)
        return 0;

    if (st == DOCA_CTX_STATE_RUNNING) {
        eng->retry_after_ns = px_monotonic_ns() + PX_BATCH_RETRY_GRACE_NS;
        eng->dma_stalled = 0;
        eng->dma_fault_warned = 0;
        return 1;
    }
    if (st == DOCA_CTX_STATE_IDLE) {
        if (doca_ctx_start(eng->dma_ctx) != DOCA_SUCCESS)
            return 0;
        eng->dma_tasks_inflight = 0;
        /* Clear refresh state only for lanes owned by this engine. */
        struct dmesh_proxy *px = eng->objs->proxy;
        for (int p = 0; p < MAX_PODS; p++) {
            for (int r = eng->id; r < MAX_EU_PER_POD; r += px->n_workers)
                px->lanes[p][r].refresh_inflight = 0;
            (void)__atomic_exchange_n(
                &eng->objs->pods[p].egress_inflight_worker[eng->id].v, 0,
                __ATOMIC_ACQ_REL);
        }
        eng->retry_after_ns = px_monotonic_ns() + PX_BATCH_RETRY_GRACE_NS;
        eng->dma_stalled = 0;
        eng->dma_fault_warned = 0;
        DOCA_LOG_WARN("proxy: egress DMA context restarted (engine %d)",
                      eng->id);
        return 1;
    }
    return doca_pe_progress(eng->pe) ? 1 : 0;
}

/* One pass over this engine's owned lanes (splice inbox→qhead, submit, retire).
 * An engine owns the destination regions where region % A == its worker id. */
static int px_engine_pump(struct objects *objs, struct px_engine *eng,
                          int *published_done) {
    struct dmesh_proxy *px = objs->proxy;
    int progressed = 0;
    int npods = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);

    if (eng->dma_stalled) {
        int recovering = px_engine_recover(eng);
        if (eng->dma_stalled)
            return recovering;            /* still down: submit nothing, but stay awake
                                           * so the caller keeps driving the recovery */
    }
    uint32_t own_bit = 1u << eng->id;
    for (int i = 0; i < npods; i++) {
        struct pod_state *pod = &objs->pods[i];
        int dead = !pod_data_ready(pod);
        /* A slot whose teardown is finished, and which no tenant has taken
         * since, holds nothing this engine can advance. pod_begin_cleanup and
         * setup_pod_dma both clear the quiesced mask, so a revived slot fails
         * this test and is walked again. */
        if (dead &&
            !__atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE) &&
            (__atomic_load_n(&pod->egress_quiesced_mask, __ATOMIC_ACQUIRE) &
             own_bit) &&
            __atomic_load_n(&pod->egress_inflight_worker[eng->id].v,
                            __ATOMIC_ACQUIRE) == 0)
            continue;
        int L = px_landing_stripes(pod);
        int quiet = dead;
        /* Teardown bookkeeping only. Sampled before lane draining: if the last
         * connection producer joins during this pass, wait for the next one so
         * every cross-worker inbox is spliced after that producer's release
         * publication. */
        uint32_t expected_workers = 0;
        uint32_t producers_at_start = 0;
        uint32_t quiesced_at_start = 0;
        if (dead) {
            expected_workers = px->n_workers >= 32
                ? UINT32_MAX : ((1u << px->n_workers) - 1u);
            producers_at_start = __atomic_load_n(
                &pod->proxy_producers_quiesced_mask, __ATOMIC_ACQUIRE);
            quiesced_at_start = __atomic_load_n(
                &pod->egress_quiesced_mask, __ATOMIC_ACQUIRE);
        }
        for (int r = eng->id; r < L; r += px->n_workers) {
            struct px_lane *ln = &px->lanes[i][r];
            progressed |= px_lane_splice_inbox(px, ln);
            if (ln->fhead) {
                int retired = px_lane_retire(eng, ln);
                progressed |= retired;
                *published_done |= retired;
            }
            if (dead) {
                /* pod disconnected (or not yet ready): never submit to it. Drain any
                 * leftover queued units so they don't leak custody or mis-deliver to a
                 * pod that later REUSES this slot. */
                if (px_lane_fail_pending_retries(eng, ln)) {
                    int retired = px_lane_retire(eng, ln);
                    progressed |= retired;
                    *published_done |= retired;
                }
                progressed |= px_rev_drop_dead(&ln->rev);
                int dropped = px_lane_drop_dead(eng, ln);
                progressed |= dropped;
                *published_done |= dropped;
                if (px_lane_inbox_nonempty(px, ln) ||
                    ln->qhead || ln->fhead || ln->refresh_inflight ||
                    ln->rev.count || ln->rev.state != PX_REV_IDLE)
                    quiet = 0;
                continue;
            }
            /* Reset lane accounting when the pod generation changes. */
            uint32_t gen = __atomic_load_n(&pod->dma_generation, __ATOMIC_ACQUIRE);
            if (ln->pod_generation != gen)
                px_lane_rearm(ln, gen);
            if (ln->fhead &&
                ln->fhead->state == PX_BATCH_RETRY_PENDING) {
                progressed |= px_lane_retry_head(objs, eng, pod, ln);
                if (ln->fhead &&
                    (ln->fhead->state == PX_BATCH_DONE ||
                     ln->fhead->state == PX_BATCH_ERROR)) {
                    int retired = px_lane_retire(eng, ln);
                    progressed |= retired;
                    *published_done |= retired;
                }
            }
            if (ln->qhead) progressed |= px_lane_submit(objs, eng, i, pod, r, ln);
            /* Publish this lane's staged reverse entries. Data retries are
             * exclusive with reverse publication. */
            if (!eng->retry_batches && !px_engine_retry_grace_active(eng))
                progressed |= px_rev_kick_lane(eng, i, r);
        }
        if (dead) {
            uint32_t bit = own_bit;
            if (__atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE)) {
                uint32_t producer_mask = __atomic_load_n(
                    &pod->proxy_producers_quiesced_mask, __ATOMIC_ACQUIRE);
                if ((producer_mask & bit) == 0) {
                    progressed |= px_worker_quiesce_pod_connections(objs,
                                                                     pod->pod_id);
                    __atomic_fetch_or(&pod->proxy_producers_quiesced_mask,
                                      bit, __ATOMIC_ACQ_REL);
                    dpu_wake_main(eng->objs);
                }
                if ((producers_at_start & expected_workers) != expected_workers)
                    quiet = 0;
            }
            if (__atomic_load_n(&pod->egress_inflight_worker[eng->id].v,
                                __ATOMIC_ACQUIRE) != 0)
                quiet = 0;
            /* Read first: an unchanged bit must not steal the line from the
             * peers that publish their own quiesce state in the same word. */
            uint32_t mask = __atomic_load_n(&pod->egress_quiesced_mask,
                                            __ATOMIC_ACQUIRE);
            if (quiet) {
                if ((mask & bit) == 0) {
                    uint32_t previous = __atomic_fetch_or(
                        &pod->egress_quiesced_mask, bit, __ATOMIC_ACQ_REL);
                    if ((previous & bit) == 0) {
                        __atomic_fetch_and(&pod->egress_reclaim_fenced_mask,
                                           ~bit, __ATOMIC_ACQ_REL);
                        dpu_wake_main(eng->objs);
                    }
                } else if ((quiesced_at_start & bit) != 0) {
                    /* px_worker_progress called doca_pe_progress before this
                     * pump. A bit already present at entry therefore fences
                     * completion-callback return and any SDK-deferred task
                     * buffer release from the preceding quiet pass. */
                    uint32_t previous = __atomic_fetch_or(
                        &pod->egress_reclaim_fenced_mask, bit,
                        __ATOMIC_ACQ_REL);
                    if ((previous & bit) == 0)
                        dpu_wake_main(eng->objs);
                }
            } else if (mask & bit) {
                __atomic_fetch_and(&pod->egress_quiesced_mask, ~bit,
                                   __ATOMIC_ACQ_REL);
                __atomic_fetch_and(&pod->egress_reclaim_fenced_mask, ~bit,
                                   __ATOMIC_ACQ_REL);
            }
        }
    }
    return progressed;
}

static int
px_worker_has_pending(struct px_engine *eng)
{
    return eng->dma_stalled || eng->dma_tasks_inflight > 0 ||
           eng->retry_batches > 0 || eng->retry_probe != NULL;
}

static enum px_progress_state
px_worker_progress(struct px_engine *eng) {
    px_cur_worker = &eng->objs->proxy->workers[eng->id];
    int published_done = 0;
    int progressed = doca_pe_progress(eng->pe) ? 1 : 0;
    progressed |= px_engine_pump(eng->objs, eng, &published_done);
    progressed |= px_engine_emit(eng->objs, eng);
    progressed |= px_ack_retry_handoffs(eng);
    progressed |= px_drain_ack_releases(eng, 256);
    if (progressed)
        return PX_PROGRESS_PROGRESSED;
    return px_worker_has_pending(eng) ?
        PX_PROGRESS_PENDING : PX_PROGRESS_IDLE;
}

int px_worker_notification_fd(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs ? objs->proxy : NULL;
    doca_notification_handle_t handle = 0;
    if (!px || px->n_workers < 1 || worker_id < 0 || worker_id >= px->n_workers ||
        doca_pe_get_notification_handle(px->engines[worker_id].pe,
                                        &handle) != DOCA_SUCCESS)
        return -1;
    return (int)handle;
}

int px_worker_arm_notification(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs ? objs->proxy : NULL;
    if (!px || px->n_workers < 1 || worker_id < 0 || worker_id >= px->n_workers)
        return 0;
    return doca_pe_request_notification(px->engines[worker_id].pe) ==
           DOCA_SUCCESS;
}

void px_worker_clear_notification(struct objects *objs, int worker_id, int fd) {
    struct dmesh_proxy *px = objs ? objs->proxy : NULL;
    if (!px || px->n_workers < 1 || worker_id < 0 ||
        worker_id >= px->n_workers || fd < 0)
        return;
    (void)doca_pe_clear_notification(px->engines[worker_id].pe,
                                     (doca_notification_handle_t)fd);
}

enum px_progress_state
px_worker_drain(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs ? objs->proxy : NULL;
    if (!px || px->n_workers < 1 || worker_id < 0 || worker_id >= px->n_workers)
        return PX_PROGRESS_IDLE;
    return px_worker_progress(&px->engines[worker_id]);
}

void px_bind_worker(struct objects *objs, int worker_id) {
    struct dmesh_proxy *px = objs ? objs->proxy : NULL;
    if (!px || worker_id < 0 || worker_id >= px->n_workers)
        return;
    px_cur_worker = &px->workers[worker_id];
}

/* Submit queued units of one lane as SG batches while credits, region space,
 * pieces and task slots allow. */
static int px_lane_submit(struct objects *objs, struct px_engine *eng, int pod_idx,
                          struct pod_state *pod, int region, struct px_lane *ln) {
    struct dmesh_proxy *px = objs->proxy;
    int did = 0;

    if (!ln->qhead)
        return 0;
    if (eng->dma_stalled)                      /* ctx faulted — don't spin/flood (below) */
        return 0;
    if (px_engine_retry_grace_active(eng))
        return 0;
    if (eng->retry_batches)                    /* serialize retry probes */
        return 0;
    if (!pod_data_ready(pod) || !pod->host_rx_addr || !pod->host_rx_mmap)
        return 0;

    int L = px_landing_stripes(pod);
    uint64_t region_size = pod->host_rx_buf_size / (uint64_t)L;
    uint32_t rq = pod->rq_depth / (uint32_t)L;
    if (region_size == 0 || rq == 0)
        return 0;

    while (ln->qhead && eng->dma_tasks_inflight < PX_DMA_TASKS) {
        /* admission: count credits, refreshed lazily via a DMA read */
        uint64_t inflight = ln->sent_entries - ln->cached_freed;
        uint32_t avail_entries = inflight < rq ? (uint32_t)(rq - inflight) : 0;
        uint32_t first_needed = px_unit_entries(ln->qhead);
        if (avail_entries < first_needed + PX_CREDIT_REFRESH_MARGIN)
            px_lane_refresh_credit(objs, eng, pod_idx, pod, region, ln);
        if (avail_entries < first_needed)
            break;                             /* wait for the refresh */

        if (ln->qhead->total_len > region_size) {
            DOCA_LOG_ERR("proxy: unit of %u bytes exceeds region (%llu) — dropped",
                         ln->qhead->total_len, (unsigned long long)region_size);
            struct px_unit *u = ln->qhead;
            ln->qhead = u->next;
            if (!ln->qhead) ln->qtail = NULL;
            for (struct px_piece *p = u->pieces; p; p = p->next)
                px_piece_release(objs, p);     /* over-region drop: release iff last */
            px_unit_free_node(px, u);
            did = 1;
            continue;
        }
        enum px_lane_wrap_action wrap =
            px_lane_wrap_action(ln->cursor, ln->qhead->total_len,
                                region_size, inflight);
        if (wrap == PX_LANE_WRAP_WAIT) {
            px_lane_refresh_credit(objs, eng, pod_idx, pod, region, ln);
            break;
        }
        if (wrap == PX_LANE_WRAP_RESET) {
            ln->cursor = 0;
        }

        struct px_batch *b = px_batch_alloc(eng);
        if (!b)
            break;

        /* take a FIFO prefix that fits pieces/credits/region-tail */
        struct px_unit *take_head = NULL, *take_tail = NULL;
        uint32_t pieces = 0, bytes = 0, entries = 0;
        int nunits = 0;
        while (ln->qhead) {
            struct px_unit *u = ln->qhead;
            uint32_t ue = px_unit_entries(u);
            if (nunits > 0 &&
                (pieces + (uint32_t)u->npieces > px->sg_pieces_max ||
                 entries + ue > avail_entries ||
                 ln->cursor + bytes + u->total_len > region_size))
                break;
            if (nunits == 0 && (uint32_t)u->npieces > px->sg_pieces_max) {
                /* single over-wide unit: cannot be one SG op — should not
                 * happen for a bounded L7 frame or one contiguous passthrough run,
                 * but drop rather than wedge the lane. */
                DOCA_LOG_ERR("proxy: unit with %d pieces exceeds SG cap %u — dropped",
                             u->npieces, px->sg_pieces_max);
                ln->qhead = u->next;
                if (!ln->qhead) ln->qtail = NULL;
                for (struct px_piece *p = u->pieces; p; p = p->next)
                    px_piece_release(objs, p); /* drop path: release iff last */
                px_unit_free_node(px, u);
                did = 1;
                continue;
            }
            u->landing_pos = (uint32_t)((uint64_t)region * region_size + ln->cursor + bytes);
            ln->qhead = u->next;
            if (!ln->qhead)
                ln->qtail = NULL;
            u->next = NULL;
            if (take_tail)
                take_tail->next = u;
            else
                take_head = u;
            take_tail = u;
            nunits++;
            pieces += (uint32_t)u->npieces;
            bytes += u->total_len;
            entries += ue;
        }
        if (!take_head) {
            px_batch_free_node(eng, b);
            break;
        }

        b->units = take_head;
        b->pod_idx = pod_idx;
        b->region = region;
        b->pod_generation = __atomic_load_n(&pod->dma_generation,
                                             __ATOMIC_ACQUIRE);
        b->entries = entries;
        b->bytes = bytes;
        b->op.kind = 0;
        b->op.batch = b;

        if (bytes == 0) {                      /* notify-only batch (FINs) */
            b->state = PX_BATCH_DONE;
        } else {
            doca_error_t ret = px_batch_submit_dma(objs, eng, pod, b);
            if (ret != DOCA_SUCCESS) {
                /* Put the units back at the lane HEAD (order preserved). */
                take_tail->next = ln->qhead;
                ln->qhead = take_head;
                if (!ln->qtail)
                    ln->qtail = take_tail;
                px_batch_free_node(eng, b);
                /* BAD_STATE = the doca_dma ctx stopped (fault): retrying can
                 * never succeed, so stall this engine's submits. Any other error
                 * (NO_MEMORY / inventory) is a transient shortage → retry. */
                if (ret == DOCA_ERROR_BAD_STATE)
                    px_engine_latch_bad_state(eng, ret);
                break;
            }
        }

        ln->cursor += bytes;
        ln->sent_entries += entries;
        b->next = NULL;
        if (ln->ftail)
            ln->ftail->next = b;
        else
            ln->fhead = b;
        ln->ftail = b;
        did = 1;
    }
    return did;
}

int px_pod_reclaim_ready(struct objects *objs, int pod_idx) {
    struct dmesh_proxy *px = objs->proxy;
    if (!px || pod_idx < 0 || pod_idx >= MAX_PODS)
        return 0;
    struct pod_state *pod = &objs->pods[pod_idx];
    uint32_t expected_workers = px->n_workers >= 32
        ? UINT32_MAX : ((1u << px->n_workers) - 1u);
    uint32_t quiet_workers = __atomic_load_n(&pod->egress_quiesced_mask,
                                              __ATOMIC_ACQUIRE);
    uint32_t quiet_producers = __atomic_load_n(
        &pod->proxy_producers_quiesced_mask, __ATOMIC_ACQUIRE);
    uint32_t fenced_workers = __atomic_load_n(
        &pod->egress_reclaim_fenced_mask, __ATOMIC_ACQUIRE);
    uint32_t inflight = 0;
    for (int w = 0; w < px->n_workers; w++)
        inflight += __atomic_load_n(&pod->egress_inflight_worker[w].v,
                                    __ATOMIC_ACQUIRE);
    if ((quiet_producers & expected_workers) != expected_workers ||
        (quiet_workers & expected_workers) != expected_workers ||
        (fenced_workers & expected_workers) != expected_workers ||
        inflight != 0 ||
        __atomic_load_n(&pod->egress_pending_emit, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&pod->proxy_source_refs, __ATOMIC_ACQUIRE) != 0)
        return 0;
    return 1;
}

/* ====== init ====== */

/* Parse a csv of service ids from `env` into a POD_ID_SPACE flag table.
 * Returns the number of distinct in-range ids set. */
static int px_parse_svc_csv(const char *env, uint8_t *table) {
    int count = 0;
    if (!env || !*env)
        return 0;
    const char *p = env;
    while (*p) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;                       /* not a number → stop */
        if (v >= 0 && v < POD_ID_SPACE && !table[v]) {
            table[(int)v] = 1;
            count++;
        }
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return count;
}

int px_init(struct objects *objs) {
    objs->proxy = NULL;
    doca_error_t ret = DOCA_SUCCESS;

    /* The SG-DMA egress engine is the DPU-to-host reverse path. */
    struct dmesh_proxy *px = (struct dmesh_proxy *)calloc(1, sizeof(*px));
    if (!px)
        return DOCA_ERROR_NO_MEMORY;
    /* Each list names the services that select one mode. A service named twice
     * is a configuration error rather than a silent precedence rule. */
    static const struct { const char *env; uint8_t mode; } l7_gates[] = {
        { "DPUMESH_L7_DECISION_SVC", PX_L7_DECISION },
        { "DPUMESH_L7_OPAQUE_SVC",   PX_L7_OPAQUE },
        { "DPUMESH_L7_SVC",          PX_L7_FULL },
    };
    int n_l7_svc = 0;
    for (size_t g = 0; g < sizeof(l7_gates) / sizeof(l7_gates[0]); g++) {
        uint8_t named[POD_ID_SPACE];
        memset(named, 0, sizeof(named));
        int n = px_parse_svc_csv(getenv(l7_gates[g].env), named);
        for (int s = 0; s < POD_ID_SPACE; s++) {
            if (!named[s])
                continue;
            if (px->svc_mode[s] != PX_L7_NONE) {
                DOCA_LOG_ERR("proxy: service %d is named by two L7 mode lists "
                             "(second is %s)", s, l7_gates[g].env);
                ret = DOCA_ERROR_INVALID_VALUE;
                goto fail;
            }
            px->svc_mode[s] = l7_gates[g].mode;
        }
        n_l7_svc += n;
        if (n)
            px->l7_attached = 1;
    }

    /* Which worker owns the L7 layer's session state; the adapter reads the same
     * variable to decide which workers build a proxy. `all` gives every worker
     * one, and PX_L7_WORKER_ALL then leaves request routing to the port policy. */
    px->l7_worker = 0;
    { const char *w = getenv("DPUMESH_L7_LINKERD_WORKER");
      if (w && *w) {
          if (strcasecmp(w, "all") == 0) {
              px->l7_worker = PX_L7_WORKER_ALL;
          } else {
              char *end;
              long v = strtol(w, &end, 10);
              if (end == w || *end != '\0' || v < 0 ||
                  v >= (objs->n_data_workers >= 1 ? objs->n_data_workers : 1)) {
                  DOCA_LOG_ERR("proxy: DPUMESH_L7_LINKERD_WORKER=%s is not one of the "
                               "%d ARM data workers, nor \"all\"", w, objs->n_data_workers);
                  ret = DOCA_ERROR_INVALID_VALUE;
                  goto fail;
              }
              px->l7_worker = (int)v;
          }
      } }
    { const char *fc = getenv("DPUMESH_L7_FAIL_CLOSED");
      px->l7_fail_closed = (fc && *fc && *fc != '0'); }

    /* Each ARM data worker owns its connection and routing tables. */
    px->n_workers = objs->n_data_workers >= 1 ? objs->n_data_workers : 1;
    if (px->n_workers > MAX_ARM_WORKERS) px->n_workers = MAX_ARM_WORKERS;
    for (int s = 0; s < px->n_workers; s++) {
        struct px_worker_state *worker_state = &px->workers[s];
        worker_state->id = s;
        worker_state->objs = objs;
        worker_state->buckets = (struct px_conn **)calloc(PX_CONN_HASH, sizeof(*worker_state->buckets));
        if (!worker_state->buckets)
            goto oom;
        worker_state->ct = (struct dpu_conntrack *)calloc(1, sizeof(struct dpu_conntrack));
        if (!worker_state->ct)
            goto oom;
        worker_state->ct->next_uport = DMESH_UPORT_BASE;
    }

    px->arr_mem = (struct px_arrival *)calloc(PX_ARRIVAL_POOL, sizeof(*px->arr_mem));
    px->piece_mem = (struct px_piece *)calloc(PX_PIECE_POOL, sizeof(*px->piece_mem));
    px->unit_mem = (struct px_unit *)calloc(PX_UNIT_POOL, sizeof(*px->unit_mem));
    if (!px->arr_mem || !px->piece_mem || !px->unit_mem)
        goto oom;
    /* Free-list helpers require the pool lock. */
    pthread_mutex_init(&px->pool_lock, NULL);
    atomic_init(&px->stat_stall_unit, 0);
    atomic_init(&px->stat_stall_piece, 0);
    atomic_init(&px->stat_stall_uport, 0);
    atomic_init(&px->stat_stall_arena, 0);
    atomic_init(&px->stat_l7_fallback, 0);
    for (int i = 0; i < PX_L7_FB_KINDS; i++)
        atomic_init(&px->stat_l7_fallback_by[i], 0);
    atomic_init(&px->stat_l7_over_release, 0);
    atomic_init(&px->stat_l7_stray_release, 0);
    atomic_init(&px->l7_report_ns, 0);
    atomic_init(&px->l7_report_mark, 0);
    /* Splice the shared free lists directly: workers are not running yet, and
     * the init thread's per-thread caches must stay empty. */
    for (int i = PX_ARRIVAL_POOL - 1; i >= 0; i--) { px->arr_mem[i].next   = px->arr_free;   px->arr_free   = &px->arr_mem[i]; }
    for (int i = PX_PIECE_POOL - 1; i >= 0; i--)   { px->piece_mem[i].next = px->piece_free; px->piece_free = &px->piece_mem[i]; }
    for (int i = PX_UNIT_POOL - 1; i >= 0; i--)    { px->unit_mem[i].next  = px->unit_free;  px->unit_free  = &px->unit_mem[i]; }

    /* Clamp the SG piece cap to the device capability. */
    px->sg_pieces_max = PX_SG_PIECES_MAX;
    {
        uint32_t cap = 0;
        if (doca_dma_cap_task_memcpy_get_max_buf_list_len(doca_dev_as_devinfo(objs->dev),
                                                          &cap) == DOCA_SUCCESS &&
            cap > 0 && cap < px->sg_pieces_max)
            px->sg_pieces_max = cap;
    }
    int landing_stripes =
        objs->n_data_workers > 0 ? objs->n_data_workers : 1;
    int credit_shards =
        (objs->k_rings > 0 ? objs->k_rings : 1) / landing_stripes;
    if (credit_shards > (int)px->sg_pieces_max) {
        ret = DOCA_ERROR_INVALID_VALUE;
        goto fail;
    }

    /* One DMA engine and PE per data worker (n_workers is already clamped
     * to [1, MAX_ARM_WORKERS] above). */
    for (int e = 0; e < px->n_workers; e++) {
        struct px_engine *eng = &px->engines[e];
        eng->objs = objs;
        eng->id = e;
        px_ack_queue_init(&eng->ack_releases);
        eng->batch_mem = (struct px_batch *)calloc(PX_BATCH_POOL, sizeof(*eng->batch_mem));
        if (!eng->batch_mem) { ret = DOCA_ERROR_NO_MEMORY; goto fail; }
        for (int i = PX_BATCH_POOL - 1; i >= 0; i--) px_batch_free_node(eng, &eng->batch_mem[i]);

        ret = doca_dma_create(objs->dev, &eng->dma);
        if (ret != DOCA_SUCCESS) goto fail;
        eng->dma_ctx = doca_dma_as_ctx(eng->dma);
        ret = doca_dma_task_memcpy_set_conf(eng->dma, px_dma_done_cb, px_dma_err_cb, PX_DMA_TASKS);
        if (ret != DOCA_SUCCESS) goto fail;
        ret = doca_pe_create(&eng->pe);
        if (ret != DOCA_SUCCESS) goto fail;
        ret = doca_pe_set_event_mode(eng->pe,
                                     DOCA_PE_EVENT_MODE_PROGRESS_ALL);
        if (ret != DOCA_SUCCESS) goto fail;
        ret = doca_pe_connect_ctx(eng->pe, eng->dma_ctx);
        if (ret != DOCA_SUCCESS) goto fail;
        { union doca_data ud = { .ptr = eng };
          ret = doca_ctx_set_user_data(eng->dma_ctx, ud);
          if (ret != DOCA_SUCCESS) goto fail; }
        ret = doca_ctx_start(eng->dma_ctx);
        if (ret != DOCA_SUCCESS) goto fail;
        ret = doca_buf_inventory_create((size_t)PX_DMA_TASKS *
                                        (px->sg_pieces_max + 1u) + 128,
                                        &eng->inv);
        if (ret != DOCA_SUCCESS) goto fail;
        ret = doca_buf_inventory_start(eng->inv);
        if (ret != DOCA_SUCCESS) goto fail;
    }

    /* credit-read landing cells (shared mmap; each cell touched by one engine) */
    ret = alloc_buffer_and_set_mmap(&px->scratch_mmap, objs->dev,
                                    (void **)&px->scratch,
                                    (size_t)MAX_PODS * MAX_EU_PER_POD * PX_SCRATCH_CELL,
                                    DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if (ret != DOCA_SUCCESS) goto fail;
    ret = alloc_buffer_and_set_mmap(
        &px->rev_scratch_mmap, objs->dev, (void **)&px->rev_scratch,
        (size_t)MAX_PODS * MAX_EU_PER_POD * PX_REV_STAGE_STRIDE,
        DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if (ret != DOCA_SUCCESS) goto fail;

    /* Egress arena, only for modes that rewrite payload on the ARM. */
    if (px->l7_attached) {
        ret = alloc_buffer_and_set_mmap(
            &px->arena_mmap, objs->dev, (void **)&px->arena,
            (size_t)PX_ARENA_CHUNKS * PX_ARENA_CHUNK,
            DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
        if (ret != DOCA_SUCCESS) goto fail;
        px->chunk_mem = (struct px_chunk *)calloc(PX_ARENA_CHUNKS,
                                                  sizeof(*px->chunk_mem));
        if (!px->chunk_mem)
            goto oom;
        for (int i = (int)PX_ARENA_CHUNKS - 1; i >= 0; i--) {
            px->chunk_mem[i].off = (uint32_t)i * PX_ARENA_CHUNK;
            px->chunk_mem[i].next = px->chunk_free;
            px->chunk_free = &px->chunk_mem[i];
        }
    }

    objs->proxy = px;

    char l7_worker_name[16];
    if (px->l7_worker == PX_L7_WORKER_ALL)
        snprintf(l7_worker_name, sizeof(l7_worker_name), "all");
    else
        snprintf(l7_worker_name, sizeof(l7_worker_name), "%d", px->l7_worker);
    DOCA_LOG_WARN("DPU PROXY MODE ON (run-to-completion SG-DMA, N/K/A=%d/%d/%d; "
                  "l7-services=%d, l7-layer=%s, l7-worker=%s, l7-policy=%s, "
                  "lb=round-robin; passthru=conn-pinned, sg_pieces=%u)",
                  objs->num_dpa_threads, objs->k_rings, objs->n_data_workers,
                  n_l7_svc, px->l7_attached ? "attached" : "off", l7_worker_name,
                  px->l7_fail_closed ? "fail-closed" : "fallback-to-l4",
                  px->sg_pieces_max);
    return DOCA_SUCCESS;

oom:
    ret = DOCA_ERROR_NO_MEMORY;
fail:
    DOCA_LOG_ERR("proxy init failed: %s", doca_error_get_descr(ret));
    objs->proxy = NULL;
    for (int s = 0; s < MAX_ARM_WORKERS; s++) {
        free(px->workers[s].buckets);
        free(px->workers[s].ct);
    }
    free(px->arr_mem); free(px->piece_mem);
    free(px->unit_mem); free(px->chunk_mem);
    for (int e = 0; e < MAX_ARM_WORKERS; e++) {
        free(px->engines[e].batch_mem);
    }
    free(px);
    return ret;
}
