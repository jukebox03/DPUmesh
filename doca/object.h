#ifndef OBJECT_H_
#define OBJECT_H_

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <doca_comch.h>

#include "comch_server.h"
#include "comch_common.h"
#include <dpumesh/dmesh_common.h>

struct dmesh_doca_dpa_thread;
struct dmesh_doca_dpa_comch;
struct doca_dpa;
struct dma_ring;
typedef uint64_t doca_dpa_dev_buf_arr_t;
/* doca_dpa_dev_mmap_t is 32-bit in the SDK (doca_dpa_dev_buf.h:35) — must
 * stay uint32_t to match the dma_desc.mmap field at offset 0 (followed by
 * the 64-bit addr at offset 4). */
typedef uint32_t doca_dpa_dev_mmap_t;

/* Deferred completion queue — DPU only.
 * Consumer callback enqueues; main loop drains.
 * Single-threaded (same DPU worker), so no lock needed.
 * Sized with headroom so in-flight recv tasks across all active EUs cannot
 * overflow it above BP_HIGH even at MAX_DPA_RINGS active EUs. */
#define DPU_COMP_QUEUE_SIZE 16384

/* One forward-DMA completion (CPU→DPU), handed from the DPA recv callback to the
 * main loop, which feeds it to the SG-DMA egress engine (dpu_proxy.c). */
typedef struct {
    int32_t  src_pod_id;
    int32_t  dst_pod_id;   /* DMESH_POD_BLANK -> resolve dst_service */
    int16_t  src_service;  /* caller service (opaque passthrough) */
    int16_t  dst_service;  /* callee service (routing input when dst_pod_id==BLANK) */
    uint16_t src_port;     /* sender port (opaque passthrough) */
    uint16_t dst_port;     /* dest port (opaque passthrough; PORT_BLANK -> accept queue on host) */
    uint16_t seq;          /* per-conn sequence (opaque passthrough) */
    uint32_t length;
    uint32_t buf_offset;   /* offset of the body in the pod's DPU staging buffer */
    uint32_t generation;   /* source staging incarnation; rejects queued stale work */
    int32_t  pod_idx;      /* index into pods[] (staging owner) */
} dpu_comp_entry_t;

typedef struct {
    dpu_comp_entry_t entries[DPU_COMP_QUEUE_SIZE];
    uint32_t head;  /* dequeue index */
    uint32_t tail;  /* enqueue index */
} dpu_comp_queue_t;

/* Force inline so these collapse into the caller. */
#define CQ_INLINE static inline __attribute__((always_inline))

/* Single-producer/single-consumer queue. The producer owns tail and publishes it
 * with release ordering; the consumer owns head and acquires tail. */
CQ_INLINE int comp_queue_full(const dpu_comp_queue_t *q) {
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    return ((tail + 1) % DPU_COMP_QUEUE_SIZE) == head;
}

CQ_INLINE int comp_queue_empty(const dpu_comp_queue_t *q) {
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    return head == tail;
}

CQ_INLINE int comp_queue_enqueue(dpu_comp_queue_t *q, const dpu_comp_entry_t *e) {
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);       /* producer owns */
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    if (((tail + 1) % DPU_COMP_QUEUE_SIZE) == head) return -1;         /* full */
    q->entries[tail] = *e;
    __atomic_store_n(&q->tail, (tail + 1) % DPU_COMP_QUEUE_SIZE, __ATOMIC_RELEASE);
    return 0;
}

CQ_INLINE dpu_comp_entry_t *comp_queue_peek(dpu_comp_queue_t *q) {
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);       /* consumer owns */
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return NULL;                                     /* empty */
    return &q->entries[head];
}

CQ_INLINE void comp_queue_dequeue(dpu_comp_queue_t *q) {
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);       /* consumer owns */
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return;                                         /* empty */
    __atomic_store_n(&q->head, (head + 1) % DPU_COMP_QUEUE_SIZE, __ATOMIC_RELEASE);
}

CQ_INLINE uint32_t comp_queue_usage(const dpu_comp_queue_t *q) {
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    if (tail >= head)
        return tail - head;
    return DPU_COMP_QUEUE_SIZE - head + tail;
}

/* Bounded MPSC completion queue. */
typedef struct {
    atomic_size_t sequence;
    dpu_comp_entry_t entry;
} dpu_mpsc_comp_slot_t;

typedef struct {
    dpu_mpsc_comp_slot_t slots[DPU_COMP_QUEUE_SIZE];
    _Alignas(64) atomic_size_t enqueue_pos;
    _Alignas(64) size_t dequeue_pos;       /* single-consumer owned */
} dpu_mpsc_comp_queue_t;

_Static_assert((DPU_COMP_QUEUE_SIZE & (DPU_COMP_QUEUE_SIZE - 1)) == 0,
               "DPU_COMP_QUEUE_SIZE must be a power of two");

CQ_INLINE void mpsc_comp_queue_init(dpu_mpsc_comp_queue_t *q) {
    atomic_init(&q->enqueue_pos, 0);
    q->dequeue_pos = 0;
    for (size_t i = 0; i < DPU_COMP_QUEUE_SIZE; i++)
        atomic_init(&q->slots[i].sequence, i);
}

CQ_INLINE int mpsc_comp_queue_enqueue(dpu_mpsc_comp_queue_t *q,
                                      const dpu_comp_entry_t *e) {
    size_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
    dpu_mpsc_comp_slot_t *slot;

    for (;;) {
        slot = &q->slots[pos & (DPU_COMP_QUEUE_SIZE - 1u)];
        size_t sequence =
            atomic_load_explicit(&slot->sequence, memory_order_acquire);
        intptr_t delta = (intptr_t)sequence - (intptr_t)pos;
        if (delta == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->enqueue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (delta < 0) {
            return -1;                    /* bounded queue full */
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos,
                                       memory_order_relaxed);
        }
    }

    slot->entry = *e;
    atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);
    return 0;
}

CQ_INLINE dpu_comp_entry_t *
mpsc_comp_queue_peek(dpu_mpsc_comp_queue_t *q) {
    size_t pos = q->dequeue_pos;
    dpu_mpsc_comp_slot_t *slot =
        &q->slots[pos & (DPU_COMP_QUEUE_SIZE - 1u)];
    size_t sequence =
        atomic_load_explicit(&slot->sequence, memory_order_acquire);
    return sequence == pos + 1 ? &slot->entry : NULL;
}

CQ_INLINE void mpsc_comp_queue_dequeue(dpu_mpsc_comp_queue_t *q) {
    size_t pos = q->dequeue_pos;
    dpu_mpsc_comp_slot_t *slot =
        &q->slots[pos & (DPU_COMP_QUEUE_SIZE - 1u)];
    size_t sequence =
        atomic_load_explicit(&slot->sequence, memory_order_acquire);
    if (sequence != pos + 1)
        return;
    q->dequeue_pos = pos + 1;
    atomic_store_explicit(&slot->sequence, pos + DPU_COMP_QUEUE_SIZE,
                          memory_order_release);
}

CQ_INLINE int mpsc_comp_queue_empty(dpu_mpsc_comp_queue_t *q) {
    return mpsc_comp_queue_peek(q) == NULL;
}

/* Backpressure threshold: defer recv task resubmission when queue exceeds
 * BP_HIGH to slow DPA inflow; resume resubmission below BP_LOW. Absolute values
 * so enlarging the queue does not move the trip points. */
#define COMP_QUEUE_BP_HIGH  3072
#define COMP_QUEUE_BP_LOW   2048

/* Max deferred recv tasks. When comp_queue ≥ BP_HIGH the DPA recv-cb stashes
 * completed recv tasks here for the main loop to resubmit once it drains below
 * BP_LOW. Sized to hold every in-flight recv task across all EUs. */
#define MAX_DEFERRED_RECV  8192

/* Max deferred TX_ACK sends. The DPU MUST eventually deliver every TX_ACK —
 * dropping one parks the host's pending entry until the reclaim timeout — so we
 * never drop on the AGAIN path; we defer and retry each main-loop iteration. */
#define MAX_DEFERRED_TX_ACK  16384

/* TX_ACK we couldn't send synchronously because the comch send pool was
 * full. Stored verbatim so the main loop can retry without recomputing. */
typedef struct {
    struct doca_comch_connection *conn;
    uint16_t  port;   /* source endpoint port of the acked leg (TX_ACK key with seq) */
    uint16_t  seq;
} deferred_tx_ack_t;

/* Mirrored DOCA task counts gate submissions. TASK_POOL_MARGIN covers concurrent
 * submitters, and MAX_CONSUMER_RETRY bounds tasks awaiting resubmission. */
#define TASK_POOL_MARGIN 8
#define MAX_CONSUMER_RETRY 256


/* Per-pod state (DPU only) */
struct pod_state {
    struct doca_comch_connection *connection;
    int32_t pod_id;
    int32_t service_id;     /* this pod's service id (an LB backend of that service; the live
                             * set is derived from pods[] by service_id); SVC_NONE if none */
    int registered;         /* 1 = DMESH_MSG_POD_REGISTER received */
    int dma_ready;          /* 1 = all mmaps + worker barrier + DPA ADD ACKs complete */
    int init_result;        /* enum dmesh_pod_init_result; terminal once non-PENDING */
    int init_result_sent;   /* result message was submitted to this connection */
    /* Bumped by setup_pod_dma per incarnation of this SLOT. A DMA error names its pod
     * by slot index, which is RECYCLED, and lands asynchronously — possibly after the
     * slot's next tenant registered. Stamped into the submitted op so px_dma_err_cb
     * can tell whose DMA failed and not kill a live pod. Also drives px_lane_rearm. */
    uint32_t dma_generation;
    /* DPA setup barrier. Each EU contributes one bit to add_ack_mask. */
    uint32_t dpa_add_expected_mask;
    uint32_t dpa_add_ack_mask;
    int dpa_add_ack_failed;
    int dpa_setup_complete;
    uint64_t dpa_add_last_send_ns;

    /* Asynchronous unregister/reclaim state. registered/dma_ready are cleared
     * immediately so routing stops, but the slot and every imported handle stay
     * owned until all target EUs return generation-matched DEL_ACK and the ARM
     * egress engine reports no DMA operation or queued lane for this incarnation.
     * Only then may the control thread destroy buf_arrs/imported mmaps and reply
     * POD_QUIESCED. */
    int cleanup_pending;
    int cleanup_reply_sent;
    uint32_t dpa_del_expected_mask;
    uint32_t dpa_del_ack_mask;
    uint64_t dpa_del_last_send_ns;
    int egress_quiesced;
    /* Pod teardown waits for every region%A owner bit. */
    uint32_t egress_quiesced_mask;
    uint32_t egress_inflight;
    uint32_t egress_inflight_worker[MAX_EU_PER_POD];
    /* Retired units awaiting main-thread emission and custody release. */
    uint32_t egress_pending_emit;
    /* Number of proxy arrivals whose bytes still reference this slot's reusable
     * DPU staging buffer (window ref or queued/in-flight egress piece). Slot
     * reuse is forbidden until it reaches zero. */
    uint32_t proxy_source_refs;

    /* K forward descriptor rings mapped to K DPA EUs. */
    int k_rings;                                   /* = objs->k_rings */
    struct doca_mmap *ring_mmaps[MAX_EU_PER_POD];  /* Host-exported forward rings */
    /* Host VA of each forward ring. The proxy DMA-reads its credit slot for
     * egress admission. */
    void *ring_host_addrs[MAX_EU_PER_POD];
    int ring_mmap_count;                           /* DMA_RING exports received */
    struct doca_mmap *remote_mmap;   /* Host TX buffer mmap (shared by all K rings) */
    void *remote_addr;
    size_t remote_buf_size;

    /* Per-pod DPA buffer arrays (one over each forward ring) */
    struct doca_buf_arr *buf_arrs[MAX_EU_PER_POD];

    /* Per-pod DPU staging buffer: forward DMA lands CPU→DPU data here (per-conn
     * contiguous, mirroring the host TX byte-ring) and the SG-DMA egress engine
     * reads its segments out in place — no separate DPU-side copy. */
    struct doca_mmap *local_mmap;
    void *dma_buffer;

    /* === Reverse direction (DPU→host): SG-DMA egress engine (dpu_proxy.c) === */

    /* Host RX buffer mmap (exported from Host; the egress SG-DMA lands into it) */
    struct doca_mmap *host_rx_mmap;
    void *host_rx_addr;
    size_t host_rx_buf_size;

    /* Host's RX RQ depth (= num_slots), derived from host_rx_buf_size. Used by the
     * SG-DMA egress admission as the cap on in-flight reverse DMAs (dpu_proxy.c). */
    uint32_t rq_depth;

    /* Batched TX_ACK accumulator. Response-forward TX_ACKs destined to THIS pod
     * accumulate here; flushed as one dmesh_batch_tx_ack_msg when full or on the
     * idle (drain-empty, proc==0) flush. Single ARM thread owns this — no lock. */
    struct dmesh_tx_ack_entry txack_batch[BATCH_TXACK_MAX];
    int      txack_batch_n;

    /* Batched REV_DONE accumulator (mirror of txack_batch). Reverse-DMA
     * completions destined to THIS pod accumulate here; flushed as one
     * dmesh_batch_rev_done_msg when full or on the idle (drain-empty, proc==0)
     * flush. Single ARM thread owns this — no lock. */
    struct dmesh_rev_done_entry rev_done_batch[BATCH_REVDONE_MAX];
    int      rev_done_batch_n;
};

/* Acquire the DMA publication gate before reading the pod's data-plane fields. */
static inline int pod_data_ready(const struct pod_state *pod) {
    return __atomic_load_n(&pod->dma_ready, __ATOMIC_ACQUIRE);
}

/* The DPU assigns each upstream connection a port in
 * [DMESH_UPORT_BASE, 65535] and maps it to the client and backend tuple. Host
 * client ports stay below DMESH_UPORT_BASE. Replies and acknowledgements are
 * translated through this mapping. */

struct dpu_upstream {
    int      in_use;
    int32_t  client_pod;
    uint16_t client_port;   /* the downstream client's REAL port */
    int32_t  backend_pod;
    uint8_t  codec_id;
};

/* Reuse lookup: (client_pod, client_port, backend_pod) -> up_port, so a
 * downstream reuses one upstream to a backend instead of creating a new one
 * (and a fresh backend accept) per message. Open-addressed, linear probe. */
#define DPU_CONN_HT_SIZE 131072u   /* power of two, >> max concurrent upstreams */
struct dpu_conn_ht_entry {
    int      in_use;
    int32_t  client_pod;
    uint16_t client_port;
    int32_t  backend_pod;
    uint16_t up_port;
};

struct dpu_conntrack {
    struct dpu_upstream      upstream[65536];      /* by up_port (only [BASE,65535) live) */
    struct dpu_conn_ht_entry ht[DPU_CONN_HT_SIZE]; /* reuse lookup */
    uint32_t next_uport;                           /* round-robin cursor */
};

static inline uint32_t dpu_ct_hash(int32_t cp, uint16_t cport, int32_t bpod) {
    uint32_t h = (uint32_t)cp * 2654435761u;
    h ^= (uint32_t)cport * 40503u;
    h ^= (uint32_t)bpod * 2246822519u;
    return h & (DPU_CONN_HT_SIZE - 1u);
}

/* Return an existing up_port for (cp,cport,bpod), or 0 if none. */
static inline uint16_t dpu_upstream_find(struct dpu_conntrack *ct, int32_t cp,
                                         uint16_t cport, int32_t bpod) {
    uint32_t mask = DPU_CONN_HT_SIZE - 1u, i = dpu_ct_hash(cp, cport, bpod);
    for (uint32_t n = 0; n < DPU_CONN_HT_SIZE; n++) {
        struct dpu_conn_ht_entry *e = &ct->ht[(i + n) & mask];
        if (!e->in_use) return 0;
        if (e->client_pod == cp && e->client_port == cport && e->backend_pod == bpod)
            return e->up_port;
    }
    return 0;
}

/* Allocate an upstream port that encodes the return-path worker. */
static inline uint16_t dpu_upstream_create(struct dpu_conntrack *ct, int32_t cp,
                                           uint16_t cport, int32_t bpod, uint8_t codec_id,
                                           uint16_t owner, uint16_t stride) {
    uint32_t span = 65536u - DMESH_UPORT_BASE;
    uint16_t uP = 0;
    if (stride < 1) stride = 1;
    for (uint32_t k = 0; k < span; k++) {
        uint32_t p = DMESH_UPORT_BASE + ((ct->next_uport - DMESH_UPORT_BASE + k) % span);
        /* Encode the return-path worker as p % A. */
        if (stride > 1 && (p % stride) != owner) continue;
        if (!ct->upstream[p].in_use) { uP = (uint16_t)p; break; }
    }
    if (uP == 0) return 0;
    ct->next_uport = (uP + 1u >= 65536u) ? DMESH_UPORT_BASE : (uint32_t)(uP + 1u);
    ct->upstream[uP].in_use      = 1;
    ct->upstream[uP].client_pod  = cp;
    ct->upstream[uP].client_port = cport;
    ct->upstream[uP].backend_pod = bpod;
    ct->upstream[uP].codec_id    = codec_id;
    uint32_t mask = DPU_CONN_HT_SIZE - 1u, i = dpu_ct_hash(cp, cport, bpod);
    for (uint32_t n = 0; n < DPU_CONN_HT_SIZE; n++) {
        struct dpu_conn_ht_entry *e = &ct->ht[(i + n) & mask];
        if (!e->in_use) {
            e->in_use = 1; e->client_pod = cp; e->client_port = cport;
            e->backend_pod = bpod; e->up_port = uP;
            break;
        }
    }
    return uP;
}

/* Free an upstream (on close/FIN): clear the slot + remove its reuse entry
 * (backward-shift so the linear-probe chain stays intact — no tombstones). */
static inline void dpu_upstream_free(struct dpu_conntrack *ct, uint16_t uP) {
    if (uP < DMESH_UPORT_BASE || !ct->upstream[uP].in_use) return;
    int32_t  cp    = ct->upstream[uP].client_pod;
    uint16_t cport = ct->upstream[uP].client_port;
    int32_t  bpod  = ct->upstream[uP].backend_pod;
    ct->upstream[uP].in_use = 0;

    uint32_t mask = DPU_CONN_HT_SIZE - 1u, i = dpu_ct_hash(cp, cport, bpod);
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < DPU_CONN_HT_SIZE; n++) {
        uint32_t p = (i + n) & mask;
        if (!ct->ht[p].in_use) break;
        if (ct->ht[p].client_pod == cp && ct->ht[p].client_port == cport &&
            ct->ht[p].backend_pod == bpod) { idx = p; break; }
    }
    if (idx == UINT32_MAX) return;
    uint32_t hole = idx, p = (idx + 1u) & mask;
    while (ct->ht[p].in_use) {
        uint32_t home = dpu_ct_hash(ct->ht[p].client_pod, ct->ht[p].client_port,
                                    ct->ht[p].backend_pod);
        if (((p - home) & mask) >= ((p - hole) & mask)) {
            ct->ht[hole] = ct->ht[p];
            hole = p;
        }
        p = (p + 1u) & mask;
    }
    ct->ht[hole].in_use = 0;
}

/* ARM workers own completion progress, connection state, and SG-DMA. */
#define MAX_ARM_WORKERS 8
#define DPU_PENDING_TXACK_SIZE 16384

_Static_assert((DPU_PENDING_TXACK_SIZE & (DPU_PENDING_TXACK_SIZE - 1)) == 0,
               "DPU_PENDING_TXACK_SIZE must be a power of two");

struct dpu_pending_txack_entry {
    int32_t pod_id;
    uint16_t port;
    uint16_t seq;
};

struct dpu_data_worker {
    struct objects *objs;
    int id;
    struct doca_pe *pe;          /* consumer_pes[id] */
    dpu_comp_queue_t queue;      /* callback-to-worker completion queue */
    /* Receive tasks deferred by completion-queue backpressure. */
    struct doca_task *deferred_recv[MAX_DEFERRED_RECV];
    int num_deferred_recv;
    /* Safety-net MPSC inbox for a completion received by the wrong owner. */
    dpu_mpsc_comp_queue_t cross_worker;
    /* Worker-produced, main-consumed SPSC TX-ACK ring. */
    struct dpu_pending_txack_entry pending_txack[DPU_PENDING_TXACK_SIZE];
    _Alignas(64) atomic_uint pending_txack_head; /* main-owned */
    _Alignas(64) atomic_uint pending_txack_tail; /* worker-owned */
    atomic_ullong stat_local_completions;
    atomic_ullong stat_cross_worker_out;
    atomic_ullong stat_cross_worker_in;
    int wake_fd;                 /* cross-worker eventfd */
    atomic_int parked;           /* worker is entering or blocked in epoll_wait */
    atomic_int init_state;       /* 0=pending, 1=epoll ready, -1=thread init failed */
    pthread_t thread;
    volatile int stop;
    int running;                 /* 1 = thread started (teardown guard) */
};

struct objects {
    struct doca_dev *dev;
    struct doca_dev_rep *rep_dev;
    struct doca_pe *pe;
    union {
        struct doca_comch_server *cc_server;
        struct doca_comch_client *cc_client;
    };
    struct doca_comch_connection *connection;  /* primary (first) connection */

    /* Host-only fields (used by dmesh_core.c client side) */
    struct doca_mmap *local_mmap;
    void *dma_buffer;
    /* Set by the client recv callback when a DMESH_MSG_POD_ASSIGNED arrives at
     * init; the register wait loop polls it. -1 = not yet assigned. Single init
     * thread drives doca_pe_progress, so the callback runs synchronously. */
    int32_t assigned_pod_id;
    /* POD_ASSIGNED is only phase 1. Phase 2 finishes when the DPU reports
     * READY after importing all mmaps and installing the DPA rings. */
    int32_t pod_init_result;
    /* Set by the client recv callback after the DPU has removed all DPA/ARM
     * references and destroyed its imported mmap views. Host teardown waits on
     * this before destroying the exported mmaps. */
    int32_t pod_quiesced;

    /* Shared DPA device with N EU threads. The common topology maps each ring
     * to an EU whose EU%A equals ring%A. */
    struct doca_dpa *dpa;                                   /* shared DPA device */
    struct dmesh_doca_dpa_thread *dpa_threads[MAX_DPA_EU];
    struct dmesh_doca_dpa_comch  *dpa_comches[MAX_DPA_EU];
    int num_dpa_threads;                                    /* N (auto-detected unless DPUMESH_DPA_THREADS set) */
    int dpa_threads_auto;                                   /* 1 = N auto-detected from the device */
    int k_rings;                            /* K rings per pod across K EUs */
    int dpu_ready;   /* 0 until DPA + msgq init done. Gates setup_pod_dma so a fast
                      * host whose mmaps arrive DURING init (before the DPA msgq is
                      * up) doesn't setup too early; those pods run in a deferred
                      * pass in run_dpu_worker once this is published. */
    /* SG-DMA egress engine (dpu_proxy.c) — the unified, always-on DPU→host reverse
     * path (px_init aborts the worker on failure, so this is never NULL at run
     * time). Every forward completion (request AND reply) runs the per-conn input
     * window → proxy_route (mock) → per-dst SG-DMA egress machinery. DPUMESH_PROXY
     * only selects the request parser (passthru default / frame / L7). */
    struct dmesh_proxy *proxy;
    int dpa_thread_running[MAX_DPA_EU];     /* per-EU: 1 = thread k started */
    int dpa_thread_running_any;             /* 1 = at least one EU started (keepalive guard) */

    /* comch data path related */
    struct local_mem_bufs *consumer_mem;
    struct doca_comch_consumer *consumer;
    struct doca_pe *consumer_pe;   /* consumer_pes[0] */
    /* DPA channel k binds to consumer_pes[k % A]. */
    struct doca_pe *consumer_pes[MAX_ARM_WORKERS];

	doca_error_t consumer_result;  /* Last result from a consumer callback (comch_consumer.c). */

    /* RX data hook (comch control path → dpumesh_ctx) */
    void (*rx_data_hook)(void *hook_ctx, const uint8_t *data, uint32_t len);
    void *rx_hook_ctx;

    /* Append-only DPU pod table. `registered` is the release/acquire publication
     * gate; slots remain stable for the process lifetime. */
    struct pod_state pods[MAX_PODS];
    int num_pods;

    /* O(1) pod_id -> slot-index accelerator for find_pod_by_id. Indexed by
     * pod_id (valid range [0, POD_ID_SPACE)); entry = index into pods[], or -1
     * if no live pod holds that id. Published with RELEASE in pods_register,
     * cleared in pods_remove_connection; read with ACQUIRE in find_pod_by_id
     * which still re-validates pods[idx].registered + pod_id, so the map is only
     * an accelerator (the registered gate remains the authority).
     * Must be initialized to all -1 before the worker starts. */
    int pod_id_to_slot[POD_ID_SPACE];

    /* Per-service round-robin cursor for the L4 load balancer (lb_pick). The
     * healthy backend SET of a service is DERIVED on demand from pods[] (single
     * source of truth: registered + service_id + dma_ready), so there is no
     * separate service->backend table to keep in sync — a disconnect removes a
     * backend from the set automatically. Existing pins terminate; new
     * connections rotate across the live set. Indexed by service_id
     * [0,POD_ID_SPACE).
     * Data workers advance it with a relaxed atomic. Init to 0. */
    uint32_t svc_rr[POD_ID_SPACE];

    /* Connection tracking for A=1 and shared-routing mode. */
    struct dpu_conntrack *conntrack;

    /* Deferred completion queue (DPU only) */
    dpu_comp_queue_t comp_queue;

    /* Backpressure: deferred consumer recv tasks (DPU only).
     * When comp_queue is nearly full, consumer callbacks defer recv task
     * resubmission here. DPA sees consumer_empty and pauses. Main loop
     * resubmits when queue drains below BP_LOW. */
    struct doca_task *deferred_recv[MAX_DEFERRED_RECV];
    int num_deferred_recv;

    /* Deferred TX_ACK sends (DPU only). When a batched TX_ACK send returns
     * AGAIN (comch send pool full), we stash the ACK here and the main
     * loop retries each iteration after pe_progress drains completions.
     * The DPU is the only authority that can free a host's TX slot, so we
     * never drop a TX_ACK — deferring keeps the contract intact. */
    deferred_tx_ack_t deferred_tx_acks[MAX_DEFERRED_TX_ACK];
    int num_deferred_tx_acks;

    /* ====== In-flight counters for DOCA task pools ======
     * Mirror DOCA's internal task pool usage so submits can be gated
     * BEFORE calling doca_task_submit, avoiding DOCA_ERROR_AGAIN entirely.
     * Counters are atomic because completions fire in PE threads while
     * submits may come from other threads (e.g. host worker threads). */
    atomic_int send_tasks_in_flight;   /* comch send task pool (server or client) */
    int        send_tasks_max;          /* CC_SEND_TASK_NUM */
    atomic_int recv_tasks_in_flight;   /* comch consumer post_recv task pool */
    int        recv_tasks_max;          /* CC_DATA_PATH_TASK_NUM */

    /* Rare-case retry list: consumer recv tasks whose gated submit still
     * failed (e.g. transient ctx state). Drained from main PE loop. */
    struct doca_task *consumer_retry[MAX_CONSUMER_RETRY];
    int num_consumer_retry;
    pthread_mutex_t consumer_retry_lock;
    int consumer_retry_lock_initialized;

    int dedicated_workers;       /* A>=2 uses dedicated ARM worker threads */
    int main_wake_fd;
    atomic_int main_parked;

    int n_data_workers;                         /* A */
    int worker_shared_routing;                  /* shared state with lock */
    struct dpu_data_worker data_workers[MAX_ARM_WORKERS];
    pthread_mutex_t routing_lock;

};

/* A=1 completion queue used by the main thread. */
static inline int completion_push(struct objects *objs, const dpu_comp_entry_t *e) {
    return comp_queue_enqueue(&objs->comp_queue, e);
}
static inline uint32_t completion_usage(struct objects *objs) {
    return comp_queue_usage(&objs->comp_queue);
}

/* ====== Task-pool helpers ======
 * Acquire/release a slot in an atomic in-flight counter. Acquire may fail
 * (return 0) if the pool is full; caller must treat that like DOCA_ERROR_AGAIN
 * and defer. These never sleep, never block, never call DOCA. */
static inline int doca_pool_try_acquire(atomic_int *cnt, int max) {
    int new_count = atomic_fetch_add(cnt, 1) + 1;
    if (new_count > max - TASK_POOL_MARGIN) {
        atomic_fetch_sub(cnt, 1);
        return 0;
    }
    return 1;
}
/* Race-free variant for callers known to be single-threaded (e.g. recv
 * completion → resubmit on the PE thread). Checks against the true pool
 * max without the concurrent-submitter margin, so a bootstrap that filled
 * the pool to `max` can round-trip every released slot back in. */
static inline int doca_pool_try_acquire_exact(atomic_int *cnt, int max) {
    int new_count = atomic_fetch_add(cnt, 1) + 1;
    if (new_count > max) {
        atomic_fetch_sub(cnt, 1);
        return 0;
    }
    return 1;
}
static inline void doca_pool_release(atomic_int *cnt) {
    atomic_fetch_sub(cnt, 1);
}

/* Progress the control and consumer PEs during send retry loops. */
static inline void progress_all_pes(struct objects *objs) {
    doca_pe_progress(objs->pe);
    if (objs->consumer_pe)
        doca_pe_progress(objs->consumer_pe);
}

void
cleanup_objects(struct objects *objs);

/* Stop and destroy only the Comch context. Host teardown calls this before
 * releasing memory exported to the DPU, then cleanup_objects closes PE/device. */
void
cleanup_comch_object(struct objects *objs);

/* Prime task-pool counters (call once from control-path init). */
void
objects_init_task_pools(struct objects *objs);

/* Drain consumer_retry list: try gated-submit each stashed task.
 * Safe to call from any thread that progresses the consumer PE.
 * Returns number of tasks successfully submitted. */
int
objects_drain_consumer_retry(struct objects *objs);

#endif // OBJECT_H_
