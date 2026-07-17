#ifndef OBJECT_H_
#define OBJECT_H_

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
    int32_t  pod_idx;      /* index into pods[] (staging owner) */
} dpu_comp_entry_t;

typedef struct {
    dpu_comp_entry_t entries[DPU_COMP_QUEUE_SIZE];
    uint32_t head;  /* dequeue index */
    uint32_t tail;  /* enqueue index */
} dpu_comp_queue_t;

/* Force inline so these collapse into the caller. */
#define CQ_INLINE static inline __attribute__((always_inline))

/* comp_queue is a single-producer / single-consumer ring. Historically both ends
 * ran on the one DPU worker thread. With the ingest reaper (DPUMESH_INGEST_REAP=1)
 * the PRODUCER is the reaper thread (recv-cb enqueues) and the CONSUMER is the main
 * loop (process_completion_queue) — two threads. So head/tail are accessed with
 * acquire/release: the producer publishes the entry THEN the tail with RELEASE; the
 * consumer reads the tail with ACQUIRE then the entry (symmetric for head). This is
 * also correct — and essentially free (ARM ldar/stlr) — when both ends run on one
 * thread (reaper disabled). Producer owns `tail`, consumer owns `head`. */
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

/* ====== DOCA task pool capacity tracking (check-first model) ======
 * DOCA does not expose in-flight task counts, so we mirror them at
 * submit/completion boundaries. Submits are gated on our counter rather
 * than relying on DOCA_ERROR_AGAIN + retry loop (which can block PE threads).
 *
 * TASK_POOL_MARGIN: safety headroom below max to absorb counter races.
 * Increment-then-check means we may briefly exceed max by the number of
 * concurrent submitters; margin covers that.
 *
 * MAX_CONSUMER_RETRY: fallback stash size for the rare case a gated submit
 * still fails (e.g. transient state during ctx restart). Drained from the
 * main PE loop.
 */
#define TASK_POOL_MARGIN 8
#define MAX_CONSUMER_RETRY 256


/* Per-pod state (DPU only) */
struct pod_state {
    struct doca_comch_connection *connection;
    int32_t pod_id;
    int32_t service_id;     /* this pod's service id (an LB backend of that service; the live
                             * set is derived from pods[] by service_id); SVC_NONE if none */
    int registered;         /* 1 = DMESH_MSG_POD_REGISTER received */
    int dma_ready;          /* 1 = both mmaps arrived, DPA ring added */
    /* Bumped by setup_pod_dma per incarnation of this SLOT. A DMA error names its pod
     * by slot index, which is RECYCLED, and lands asynchronously — possibly after the
     * slot's next tenant registered. Stamped into the submitted op so px_dma_err_cb
     * can tell whose DMA failed and not kill a live pod. Also drives px_lane_rearm. */
    uint32_t dma_generation;

    /* EU-sharding: K forward descriptor rings spread across K EUs (K=k_rings).
     * The host exports K DMA_RING mmaps in order; ring_mmap_count counts arrivals
     * so the import handler fills ring_mmaps[0..K-1]. The host TX data buffer
     * (remote_mmap) and host RX buffer are shared/partitioned, not K-plural. */
    int k_rings;                                   /* = objs->k_rings (1 = legacy) */
    struct doca_mmap *ring_mmaps[MAX_EU_PER_POD];  /* Host-exported forward rings */
    /* Host VA of each forward ring's desc array. The RX credit counter lives at
     * ring_host_addrs[j] + DMA_RING_SIZE*sizeof(dma_desc) (the +1 slot); the
     * proxy engine (dpu_proxy.c) DMA-reads it for its egress admission — the
     * same counter the DPA reverse admission polls. */
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

/* Data-plane publication gate. `dma_ready` is set with RELEASE at the END of
 * setup_pod_dma (after dma_buffer and the forward rings are all written). When
 * setup writes run on thread B while the hot path reads run on thread A, the
 * `registered` gate is not sufficient for the data fields, which are written
 * later. ACQUIRE-loading dma_ready before dereferencing dma_buffer establishes
 * the missing happens-before. */
static inline int pod_data_ready(const struct pod_state *pod) {
    return __atomic_load_n(&pod->dma_ready, __ATOMIC_ACQUIRE);
}

/* ===================================================================
 * DPU connection tracking (model B: the DPU owns every connection)
 * ===================================================================
 * A client addresses a SERVICE (dst_pod=BLANK); the DPU picks a backend per
 * message and owns the "upstream" connection to it. Each upstream gets a
 * DPU-assigned id `up_port` from [DMESH_UPORT_BASE, 65535]; host client conns
 * use [1, DMESH_UPORT_BASE), so a host that is BOTH client and backend
 * (loopback) never collides in its own ports[] table.
 *
 * Toward the backend the DPU rewrites the tuple to src=(client_pod, up_port),
 * dst_port=up_port, so the backend sees the DPU id (not the real client) and
 * replies to it. The reply returns to the DPU (which forwards everything), maps
 * up_port -> (client_pod, client_port) and rewrites dst_port back to the client's
 * real port. The client's TX_ACK is likewise translated up_port -> client_port
 * so the client frees the right slot. (DMESH_UPORT_BASE is defined in the shared
 * dmesh_common.h so the host side sees the same split.) */

struct dpu_upstream {
    int      in_use;
    int32_t  client_pod;
    uint16_t client_port;   /* the downstream client's REAL port */
    int32_t  backend_pod;
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

/* Allocate a new up_port for (cp,cport,bpod) and index it. Returns 0 if the
 * upstream id space [BASE,65535) is exhausted.
 *
 * OWNER-STRIDED allocation (② share-nothing, diagram): only up_ports p with
 * (p-BASE)%stride == owner are handed out by this shard, so a backend reply on p
 * dispatches back (px_uport_owner) to the shard that owns the session — keeping
 * each shard's conntrack single-threaded. stride==1 (owner==0) = the full range,
 * i.e. the ①/single-shard behaviour (byte-identical). */
static inline uint16_t dpu_upstream_create(struct dpu_conntrack *ct, int32_t cp,
                                           uint16_t cport, int32_t bpod,
                                           uint16_t owner, uint16_t stride) {
    uint32_t span = 65536u - DMESH_UPORT_BASE;
    uint16_t uP = 0;
    if (stride < 1) stride = 1;
    for (uint32_t k = 0; k < span; k++) {
        uint32_t p = DMESH_UPORT_BASE + ((ct->next_uport - DMESH_UPORT_BASE + k) % span);
        if (stride > 1 && ((p - DMESH_UPORT_BASE) % stride) != owner) continue;
        if (!ct->upstream[p].in_use) { uP = (uint16_t)p; break; }
    }
    if (uP == 0) return 0;
    ct->next_uport = (uP + 1u >= 65536u) ? DMESH_UPORT_BASE : (uint32_t)(uP + 1u);
    ct->upstream[uP].in_use      = 1;
    ct->upstream[uP].client_pod  = cp;
    ct->upstream[uP].client_port = cport;
    ct->upstream[uP].backend_pod = bpod;
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

/* ====== Ingest-processor sharding (diagram ①②③) ======
 * The single ARM funnel is split so the CPU-heavy parse/route runs on M threads.
 * A single reaper still OWNS consumer_pe (one thread progresses one PE, and the
 * DPA WAKE keepalive stays with the consumer_pe owner — the WAKE-race fix). After
 * the reaper reaps DPA completions into comp_queue it DISPATCHES each one to a
 * shard's private queue (by conn hash for ①, by up_port-encoded OWNER for ②); the
 * M shard threads run px_ingest_forward (window/parse/route/lane-enqueue). The
 * reaper→shard SPSC queues ARE the "lock-free cross-shard ring" of the diagram.
 * All comch sends still happen on main (objs->pe is single-threaded): a shard
 * DEFERS its TX_ACKs to main via pending_txack and its REV_DONEs ride the egress
 * lanes' done-queue that main drains. Event-driven end to end — a shard SLEEPS on
 * its wake_fd and the reaper writes it only while the shard is parked (no per-msg
 * syscall under load); a missed edge is backstopped by the shard's 1 ms epoll
 * timeout. M<=1 keeps the proven single-reaper (reap+process) path unchanged. */
#define MAX_INGEST_SHARDS 8

struct dpu_ingest_shard {
    struct objects *objs;
    int id;
    /* Design A (M>=2): this shard OWNS consumer_pes[id] and SLEEPS on its DOCA
     * notification fd directly — no reaper, no per-message eventfd. It reaps its
     * own PE (recv-cb fills `queue`, same thread → SPSC), then processes. */
    struct doca_pe *pe;          /* = objs->consumer_pes[id]; reaped + WAKE'd by this shard only */
    dpu_comp_queue_t queue;      /* recv-cb (this shard) -> process (this shard): SPSC same-thread */
    /* Per-PE recv backpressure (the recv tasks of the channels bound to this PE). */
    struct doca_task *deferred_recv[MAX_DEFERRED_RECV];
    int num_deferred_recv;
    /* ② cross-shard ring: a reply lands on the BACKEND's EU-shard but the session
     * is owned by the CLIENT's EU-shard (up_port owner). The landing shard hands it
     * here to the owner (MPSC: many shards push, this shard drains). Unused in ①
     * (shared conntrack) and M==1. */
    dpu_comp_queue_t xshard;
    pthread_mutex_t  xshard_lock;
    int wake_fd;                 /* eventfd: a cross-shard producer wakes this shard when it parks */
    atomic_int parked;           /* 1 = shard is about to / is blocked in epoll_wait */
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

    /* DPA (shared device, N EU threads for multi-EU data plane).
     *
     * num_dpa_threads (= N, clamp [1, MAX_DPA_RINGS]) EU threads share ONE
     * doca_dpa device (`dpa`). Each EU k owns its own dpa_threads[k]
     * (doca_dpa_thread + arg) and its own 1c/1p comch channel dpa_comches[k].
     * A pod's K forward rings map to EUs (pod_id*K + j) % num_dpa_threads (ring j). The DPU side
     * stays single-threaded: all N recv-msgq consumers connect to the one
     * consumer_pe, so one pe_progress drains every channel into the single
     * comp_queue — no lock, tx_ring stays single-producer.
     *
     * Kept as pointer arrays (not inline) so host-side translation units that
     * include object.h never pull in doca_dpa.h. */
    struct doca_dpa *dpa;                                   /* shared DPA device */
    struct dmesh_doca_dpa_thread *dpa_threads[MAX_DPA_EU];
    struct dmesh_doca_dpa_comch  *dpa_comches[MAX_DPA_EU];
    int num_dpa_threads;                                    /* N (auto-detected unless DPUMESH_DPA_THREADS set) */
    int dpa_threads_auto;                                   /* 1 = N auto-detected from the device */
    int k_rings;                            /* K = rings per pod, spread across K EUs (1 = legacy) */
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
    int dpa_thread_running[MAX_DPA_RINGS];  /* per-EU: 1 = thread k started */
    int dpa_thread_running_any;             /* 1 = at least one EU started (keepalive guard) */

    /* comch data path related */
    struct local_mem_bufs *consumer_mem;
    struct doca_comch_consumer *consumer;
    struct doca_pe *consumer_pe;   /* == consumer_pes[0] (the data-path consumer + M==1 channels) */
    /* Design A ingest sharding (M>=2): one consumer PE per shard; DPA channel k is
     * bound to consumer_pes[k % M] (comch_msgq.c), and shard m OWNS consumer_pes[m]
     * (reaps it + sends the DPA WAKE to its channels). consumer_pes[0] is the same
     * object as consumer_pe (created by init_comch_datapath_consumer); [1..M-1] are
     * created in run_dpu_worker before the DPA msgq is built. */
    struct doca_pe *consumer_pes[MAX_INGEST_SHARDS];

	doca_error_t consumer_result;  /* Last result from a consumer callback (comch_consumer.c). */

    /* RX data hook (comch control path → dpumesh_ctx) */
    void (*rx_data_hook)(void *hook_ctx, const uint8_t *data, uint32_t len);
    void *rx_hook_ctx;

    /* Multi-pod table (DPU only).
     *
     * Concurrency model: lock-free with publication ordering on `registered`.
     *
     *   1. Slots are append-only: pods_add_connection writes into
     *      pods[num_pods] then increments num_pods. Slots are NEVER compacted
     *      or recycled, so &pods[i] is a stable pointer for the lifetime of
     *      the process.
     *   2. `registered` is the publication gate. Writers set every other
     *      field of pod_state FIRST, then publish via
     *      __atomic_store_n(&pods[i].registered, 1, __ATOMIC_RELEASE).
     *      Disconnect tears down in the opposite order: store registered=0
     *      with RELEASE first, then NULL-ify connection/mmap/etc.
     *   3. Readers (find_pod_by_id / find_pod_by_connection / hot path)
     *      observe via __atomic_load_n(&pods[i].registered, __ATOMIC_ACQUIRE).
     *      Seeing registered=1 guarantees visibility of the prior field
     *      writes. Seeing registered=0 is treated as "not found".
     *
     * Single writer (control PE callbacks dispatched on the one PE thread).
     */
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
     * backend from the set automatically (no blackhole). This cursor just rotates
     * the pick across that live set. Indexed by service_id [0,POD_ID_SPACE).
     * Single ARM control thread advances it → no lock. Init to 0. */
    uint32_t svc_rr[POD_ID_SPACE];

    /* DPU-owned connection tracking (model B). Heap-allocated (large) in
     * run_dpu_worker; single-threaded (control PE thread) so no lock. */
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

    /* ====== Ingest reaper thread (DPUMESH_INGEST_REAP=1) ======
     * Splits the DPA forward-completion REAP off the single main funnel. When
     * active, a dedicated thread owns consumer_pe: it runs doca_pe_progress
     * (recv-cb -> comp_queue), the recv backpressure release (deferred_recv
     * resubmit), and the consumer_retry drain. The main loop then only DRAINS
     * comp_queue (parse/route/egress/emit/sends) + ctrl pe. consumer_pe is thus
     * touched by exactly one thread (the reaper) — DOCA PEs are not thread-safe —
     * and comp_queue becomes a cross-thread SPSC ring (acquire/release above).
     * Default 0 = reap inline on the main loop (original single-thread path). */
    int reaper_active;
    volatile int reaper_stop;
    pthread_t reaper_thread;
    /* Ingest sharding (diagram ①): when the reaper also runs process (px_ingest_forward
     * = parse/route), main is left with only emit+ctrl+sends. A comch send MUST stay on
     * the main thread (objs->pe is single-threaded), so the ingest thread never sends —
     * it ENQUEUES its (rare, FIN/error/drop) TX_ACKs here and main drains + sends. The
     * frequent custody TX_ACKs already run on main (px_drain emit). Count-based, drained
     * every main iteration (bounded by messages between drains). */
    struct { int32_t pod_id; uint16_t port; uint16_t seq; } pending_txack[16384];
    int pending_txack_n;
    pthread_mutex_t pending_txack_lock;
    /* Event-driven hand-off reaper -> main (NO busy-spin): the reaper sleeps on
     * consumer_pe's epoll fd; after it reaps into comp_queue it wakes the main loop
     * via reaper_wake_fd — but ONLY when main is parked (main_parked=1), so there is
     * no write() per message under load. Main sleeps on {ctrl_pe fd, reaper_wake_fd}. */
    int reaper_wake_fd;          /* eventfd: reaper writes, main reads */
    atomic_int main_parked;      /* 1 = main is about to / is blocked in epoll_wait */

    /* ====== Ingest-processor sharding (DPUMESH_INGEST_SHARDS=M, diagram ①②③) ======
     * n_ingest_shards >= 2 activates the sharded pipeline (implies the reaper).
     * shard_shared_routing selects ① (shared conntrack + route tables under
     * routing_lock; per-shard conn buckets/pools stay lock-free) vs ② (everything
     * per-shard, share-nothing; up_port encodes the owner shard so a backend reply
     * dispatches back to the session's owner). ③ additionally shards the emit/send
     * path (see run_dpu_worker). Default M=1 (single reaper) — unchanged. */
    int n_ingest_shards;                       /* M (>=2 = sharded; <=1 = single reaper) */
    int shard_shared_routing;                  /* 1 = ① (shared+lock), 0 = ② (share-nothing) */
    int shard_host_emit;                       /* 1 = ③ (shard emits its own REV_DONE/TX_ACK) */
    struct dpu_ingest_shard ingest_shards[MAX_INGEST_SHARDS];
    pthread_mutex_t routing_lock;              /* ① : guards shared conntrack + route tables */

};

/* Ingest hand-off: the single ARM thread's recv-cb pushes each completion to the
 * comp_queue, drained by the main loop. Returns 0 on enqueue, -1 if full
 * (caller drops + logs). */
static inline int ingest_push(struct objects *objs, const dpu_comp_entry_t *e) {
    return comp_queue_enqueue(&objs->comp_queue, e);
}
static inline uint32_t ingest_usage(struct objects *objs) {
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

/* Progress both PEs: control-path PE + consumer PE.
 * consumer_pe must be progressed here to resubmit DPA recv tasks during
 * server_send_msg_to_conn retry loops. Without this, DPA exhausts consumer
 * credits and permanently stalls under load.
 * Safe because server_send_msg_to_conn is only called from:
 *   - process_completion_queue (main loop, not inside any callback)
 *   - server_message_recv_callback (pe callback, not consumer_pe callback)
 * So consumer_pe is never re-entered. */
static inline void progress_all_pes(struct objects *objs) {
    doca_pe_progress(objs->pe);
    if (objs->consumer_pe)
        doca_pe_progress(objs->consumer_pe);
}

void
cleanup_objects(struct objects *objs);

/* Prime task-pool counters (call once from control-path init). */
void
objects_init_task_pools(struct objects *objs);

/* Drain consumer_retry list: try gated-submit each stashed task.
 * Safe to call from any thread that progresses the consumer PE.
 * Returns number of tasks successfully submitted. */
int
objects_drain_consumer_retry(struct objects *objs);

#endif // OBJECT_H_
