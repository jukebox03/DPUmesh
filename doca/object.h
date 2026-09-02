#ifndef OBJECT_H_
#define OBJECT_H_

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <doca_comch.h>

#include "comch_server.h"
#include "pod_membership.h"
#include "topology.h"
#include "comch_common.h"
#include <dpumesh/dmesh_common.h>

struct dmesh_doca_dpa_thread;
struct dmesh_doca_dpa_comch;
struct doca_dpa;

/* Deferred completion queue — DPU only.
 * Consumer callback enqueues; the owning data worker drains.
 * Each queue has one producer PE and one worker consumer.
 * Sized with headroom so in-flight recv tasks across all active EUs cannot
 * overflow it above BP_HIGH even at MAX_DPA_RINGS active EUs. */
#define DPU_COMP_QUEUE_SIZE 16384

/* One forward-DMA completion (CPU→DPU), handed from the DPA callback to the
 * connection-owning SG-DMA worker. */
typedef struct {
    int32_t  src_pod_id;
    int32_t  dst_pod_id;   /* DMESH_POD_BLANK -> resolve dst_service */
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
/* Real exhaustion. Above this every receive is withheld, including the floor
 * that otherwise keeps ring ACKs sendable under soft backpressure. */
#define COMP_QUEUE_BP_HARD  (DPU_COMP_QUEUE_SIZE - 1024)

/* Max deferred recv tasks. When comp_queue ≥ BP_HIGH the DPA recv-cb stashes
 * completed recv tasks here for the PE owner to resubmit once it drains below
 * BP_LOW. Sized to hold every in-flight recv task across all EUs. */
#define MAX_DEFERRED_RECV  8192

/* Mirrored DOCA task counts gate submissions. */
#define TASK_POOL_MARGIN 8

/* ARM workers own completion progress, connection state, and SG-DMA. */
#define MAX_ARM_WORKERS 16

/* Cache-line-isolated per-worker counter: each worker mutates only its own
 * element on the DMA hot path, so elements must not share a line. */
struct dpu_worker_counter {
    _Alignas(64) uint32_t v;
};

#define DMESH_REGISTRATION_MAX_KEYS 4
#define DMESH_REGISTRATION_REPLAY_SLOTS 4096

struct dmesh_registration_key {
    uint8_t bytes[32];
    char key_id[DMESH_GRANT_KEY_ID_MAX];
};

/* Per-pod state (DPU only) */
struct pod_state {
    struct doca_comch_connection *connection;
    /* Admission deadline for a raw Comch peer. A connected peer that has not
     * completed trusted registration is disconnected after the fixed bound;
     * pending prevents duplicate submissions while its callback is pending. */
    uint64_t connected_ns;
    int registration_disconnect_pending;
    int32_t pod_id;
    int32_t service_id;     /* this pod's service id (an LB backend of that service; the live
                             * set is derived from pods[] by service_id); SVC_NONE if none */
    /* Linkerd workload bound to this connection by its verified assertion. */
    char workload[DMESH_WORKLOAD_MAX];
    /* Signed Kubernetes Pod UID of the asserted registration. It names the
     * exact live registration a membership withdrawal has to close. */
    char pod_uid[DMESH_POD_UID_MAX];
    /* Signed claims retained from the assertion: the inbound policy verdict
     * consumes the identity and source address, and the Service pair is what
     * a registration request is compared against. */
    char namespace_name[DMESH_K8S_NAMESPACE_MAX];
    char service_account[DMESH_K8S_NAME_MAX];
    char pod_ip[DMESH_POD_IP_MAX];
    char granted_service[DMESH_SVC_NAME_MAX];
    uint8_t registration_nonce[DMESH_REG_NONCE_SIZE];
    uint8_t registration_grant_id[DMESH_GRANT_ID_SIZE];
    /* Newest membership generation this registration has been judged against,
     * and how many consecutive generations have omitted it. A registration
     * accepted between a generation's snapshot and its publication is absent
     * from that one generation without having lost membership. */
    uint64_t membership_generation;
    uint32_t membership_absences;
    /* Whether the controller says this node may act for this Pod on the
     * control plane. Written by the control thread, read by a data worker
     * before it asks for an inbound verdict. */
    int8_t scope_state;
    int revoked;
    int registration_challenge_issued;
    int registration_challenge_sent;
    int registration_grant_verified;
    int registration_grant_consumed;
    int registered;         /* 1 = DMESH_MSG_POD_REGISTER received */
    int dma_ready;          /* 1 = all mmaps + worker barrier + DPA ADD ACKs complete */
    enum dmesh_pod_init_result init_result;   /* terminal once non-PENDING */
    int init_result_sent;   /* result message was submitted to this connection */
    /* Bumped by setup_pod_dma per incarnation of this slot. A DMA error names its
     * pod by recycled slot index and lands asynchronously, so the generation is
     * stamped into the submitted op: px_dma_err_cb matches it before killing a
     * pod. Also drives px_lane_rearm. */
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
    uint64_t cleanup_started_ns;
    uint64_t cleanup_stall_report_ns;
    uint32_t dpa_del_expected_mask;
    uint32_t dpa_del_ack_mask;
    uint64_t dpa_del_last_send_ns;
    /* Each worker closes its own connections and Linkerd sessions before the
     * egress owners may declare their destination lanes empty. This producer
     * barrier prevents a late worker close from publishing into a lane after
     * its owner has already reported quiescence. */
    uint32_t proxy_producers_quiesced_mask;
    /* Pod teardown waits for every region%A owner bit. */
    uint32_t egress_quiesced_mask;
    /* A second worker pass after the quiesce barrier. DOCA may release a task's
     * internal buffer reference only after its completion callback returns;
     * the first quiet bit can be published from the pump immediately following
     * that callback. The control thread destroys imported mmaps only after this
     * post-callback PE-progress fence is also complete on every worker. */
    uint32_t egress_reclaim_fenced_mask;
    /* In-flight DMA tasks naming this pod, sharded by owning worker. The total
     * exists only on the reclaim path, as the sum over workers. */
    struct dpu_worker_counter egress_inflight_worker[MAX_ARM_WORKERS];
    /* Retired units awaiting completion emission and custody release. */
    uint32_t egress_pending_emit;
    /* Number of proxy arrivals whose bytes still reference this slot's reusable
     * DPU staging buffer (window ref or queued/in-flight egress piece). Slot
     * reuse is forbidden until it reaches zero. */
    uint32_t proxy_source_refs;

    /* K forward descriptor rings mapped to K DPA EUs. */
    int k_rings;                                   /* = objs->k_rings */
    int landing_stripes;                           /* L = ARM data workers */
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
    /* Imported mmaps are not fast-path thread-safe. Worker 0 uses the canonical
     * handle above; every other egress worker owns an independent handle over
     * the same exported range. */
    struct doca_mmap *host_rx_worker_mmaps[MAX_ARM_WORKERS];
    void *host_rx_addr;
    size_t host_rx_buf_size;
    struct doca_mmap *rev_ring_mmaps[MAX_EU_PER_POD];
    void *rev_ring_host_addrs[MAX_EU_PER_POD];
    int rev_ring_mmap_count;
    uint64_t rev_doorbell_pending_epoch;
    uint64_t rev_doorbell_sent_epoch;

    /* Host's RX RQ depth (= num_slots), derived from host_rx_buf_size. Used by the
     * SG-DMA egress admission as the cap on in-flight reverse DMAs (dpu_proxy.c). */
    uint32_t rq_depth;

};

static inline struct doca_mmap *
pod_host_rx_mmap_for_worker(const struct pod_state *pod, int worker_id)
{
    if (worker_id <= 0)
        return pod->host_rx_mmap;
    if (worker_id >= MAX_ARM_WORKERS)
        return NULL;
    return pod->host_rx_worker_mmaps[worker_id];
}

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
    uint8_t  l7_mode;      /* enum px_l7_mode, inherited by the reply */
    /* TCP FIN is directional. Keep the return mapping until both input halves
     * ended; otherwise shutdown(SHUT_WR) makes a later response unroutable. */
    uint8_t  client_fin_sent;
    uint8_t  backend_fin_seen;
    uint16_t delivery_seq;
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

/* Allocate an upstream port that encodes the return-path worker as p % A. The
 * search walks that residue class from the round-robin cursor. */
static inline uint16_t dpu_upstream_create(struct dpu_conntrack *ct, int32_t cp,
                                           uint16_t cport, int32_t bpod, uint8_t l7_mode,
                                           uint16_t owner, uint16_t stride) {
    uint16_t uP = 0;
    if (stride < 1) stride = 1;
    /* Lowest upstream port in this worker's residue class. */
    uint32_t first = DMESH_UPORT_BASE +
        ((owner + stride - (DMESH_UPORT_BASE % stride)) % stride);
    if (first >= 65536u) return 0;
    uint32_t candidates = (65536u - first + stride - 1u) / stride;
    uint32_t cursor = ct->next_uport;
    uint32_t start = (cursor > first && cursor < 65536u)
        ? (((cursor - first + stride - 1u) / stride) % candidates) : 0;
    for (uint32_t k = 0; k < candidates; k++) {
        uint32_t p = first + ((start + k) % candidates) * stride;
        if (!ct->upstream[p].in_use) { uP = (uint16_t)p; break; }
    }
    if (uP == 0) return 0;
    ct->next_uport = (uP + 1u >= 65536u) ? DMESH_UPORT_BASE : (uint32_t)(uP + 1u);
    ct->upstream[uP].in_use      = 1;
    ct->upstream[uP].client_pod  = cp;
    ct->upstream[uP].client_port = cport;
    ct->upstream[uP].backend_pod = bpod;
    ct->upstream[uP].l7_mode     = l7_mode;
    ct->upstream[uP].client_fin_sent = 0;
    ct->upstream[uP].backend_fin_seen = 0;
    ct->upstream[uP].delivery_seq = 0;
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

/* The inter-node carrier's runtime, created at bring-up and owned by the
 * worker it serves. Opaque here: only dpu_worker.c drives it. */
struct peer_transport_rt;

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
    atomic_ullong stat_local_completions;
    atomic_ullong stat_cross_worker_out;
    atomic_ullong stat_cross_worker_in;
    int wake_fd;                 /* cross-worker eventfd */
    /* Every source that may wake this worker, in one descriptor: the eventfd
     * above and, once a peer carrier is configured, the descriptor that
     * carrier's connections are waited on. -1 leaves wake_fd as the only one. */
    int wake_epfd;
    struct peer_transport_rt *peer_rt;  /* NULL when no peer carrier is configured */
    uint64_t peer_evict_deadline;       /* next idle-channel sweep */
    uint64_t dpa_nudge_deadline; /* next optional DPA nudge; 0 while disabled */
    atomic_int parked;           /* worker is entering or blocked in epoll_wait */
    atomic_int wake_posted;      /* a wake tick is in the eventfd and must be read */
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
    int32_t landing_stripes;
    /* Set by the client recv callback after the DPU has removed all DPA/ARM
     * references and destroyed its imported mmap views. Host teardown waits on
     * this before destroying the exported mmaps. */
    int32_t pod_quiesced;
    /* Trusted registration challenge received from the DPU. The callback
     * publishes `registration_challenge_ready` last. */
    uint8_t registration_challenge[DMESH_REG_NONCE_SIZE];
    int32_t registration_trusted_required;
    int32_t registration_challenge_ready;
    /* DPU-interned id of this registration's Service, from POD_ASSIGNED. */
    int32_t assigned_service_id;
    /* One resolution answer at a time; requests are serialized host-side. The
     * callback publishes `resolve_ack_ready` last. */
    struct dmesh_resolve_ack_msg resolve_ack;
    int32_t resolve_ack_ready;
    /* Host client callback increments this for each pod-global REV_DOORBELL.
     * The broker uses it only to avoid rescanning reverse rings on unrelated
     * PE progress; it carries no EQ information to or from the DPU. */
    uint64_t rev_doorbell_count;

    /* DPU-only verifier configuration, installed by
     * dmesh_registration_configure() before the Comch server starts. */
    struct dmesh_registration_key registration_keys[DMESH_REGISTRATION_MAX_KEYS];
    size_t registration_key_count;
    char registration_key_dir[4096];
    /* Feed-signing keyring (DPUMESH_FEED_KEY_DIR). Disjoint from the
     * registration keyring, so a feed publisher holds no key that can mint
     * identity. Empty when unconfigured: every feed is then refused. */
    char feed_key_dir[4096];
    /* This DPU's own Kubernetes node name (DPUMESH_NODE_NAME). A grant's signed
     * node_name must equal it, so an assertion minted for another node's Pods
     * is refused here. */
    char node_name[DMESH_K8S_NAME_MAX];
    uint8_t consumed_grant_ids[DMESH_REGISTRATION_REPLAY_SLOTS][DMESH_GRANT_ID_SIZE];
    size_t consumed_grant_count;
    size_t consumed_grant_cursor;
    uint64_t registration_grants_accepted;
    uint64_t registration_grants_rejected;
    uint64_t registration_grants_replayed;

    /* Authoritative node membership. The controller publishes the (Pod UID,
     * Service) pairs this node may hold; a registration that leaves the set is
     * closed on the Comch control thread, which is the single owner of all of
     * this state. An unset DPUMESH_MEMBERSHIP_FILE leaves revocation off. */
    char membership_path[4096];
    int membership_enabled;
    struct dmesh_membership_entry membership[DMESH_MEMBERSHIP_MAX_ENTRIES];
    size_t membership_count;
    uint64_t membership_generation;
    uint64_t membership_stamp_ino;
    int64_t membership_stamp_sec;
    int64_t membership_stamp_nsec;
    uint64_t membership_stamp_size;
    uint64_t membership_rejected;
    uint64_t membership_revocations;
    uint64_t membership_next_check_ns;

    /* The node credential's public half. The private half is generated at
     * first boot into a 0400 file and never leaves the DPU; this is what the
     * node agent reports and the generation publishes, and what a peer checks
     * a handshake against. Zero until DPUMESH_NODE_KEY_FILE is configured. */
    uint8_t node_public_key[32];
    int node_key_ready;

    /* Where the controller answers the mediated control-plane lookup, reached
     * through this node's agent. Empty leaves mediation disabled. */
    char scope_host[256];
    uint16_t scope_port;

    /* Cluster topology generation (DPUMESH_TOPOLOGY_FILE), signed by the
     * controller and verified with public keys only. The Comch control thread
     * owns it, like membership. */
    struct dmesh_topology topology;

    /* Protected admission. A drain refuses new L7 sessions while established
     * ones continue, so identity material can be replaced against a quiet
     * proxy. The control thread polls the file; workers read the flag. */
    char admission_path[4096];
    int admission_enabled;
    int32_t admission_drain;
    uint64_t admission_drain_refusals;
    uint64_t admission_next_check_ns;

    /* Shared DPA device with N EU threads. The common topology maps each ring
     * to an EU whose EU%A equals ring%A. */
    struct doca_dpa *dpa;                                   /* shared DPA device */
    struct dmesh_doca_dpa_thread *dpa_threads[MAX_DPA_EU];
    struct dmesh_doca_dpa_comch  *dpa_comches[MAX_DPA_EU];
    int num_dpa_threads;                                    /* N (auto-detected unless DPUMESH_DPA_THREADS set) */
    int dpa_threads_auto;                                   /* 1 = N auto-detected from the device */
    int k_rings;                            /* K rings per pod across K EUs */
    int dpu_ready;
    /* Worker-owned routing, SG-DMA, and reverse-ring publication. */
    struct dmesh_proxy *proxy;
    /* Per-EU: 1 = thread k started. A started EU polls its own rings and hands
     * its EU to a same-affinity DPA helper for the watchdog; a ringless EU
     * parks until a control message wakes it. Neither needs an ARM tick. */
    int dpa_thread_running[MAX_DPA_EU];

    struct doca_pe *consumer_pe;   /* consumer_pes[0] */
    /* DPA channel k binds to consumer_pes[k % A]. */
    struct doca_pe *consumer_pes[MAX_ARM_WORKERS];

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

    /* Per-service round-robin cursor for the L4 load balancer (lb_pick). A
     * service's healthy backend set is derived on demand from pods[] —
     * registered + service_id + dma_ready — so a disconnect removes a backend
     * from it. Existing pins terminate; new connections rotate across the live
     * set. Indexed by service_id [0,POD_ID_SPACE), advanced by data workers with
     * a relaxed atomic, initialized to 0. */
    uint32_t svc_rr[POD_ID_SPACE];

    /* ====== In-flight counters for DOCA task pools ======
     * Mirror DOCA's task pool usage so a submit is gated before
     * doca_task_submit is called. The counters are atomic: completions fire in
     * PE threads while submits may come from other threads. */
    atomic_int send_tasks_in_flight;   /* comch send task pool (server or client) */
    int        send_tasks_max;          /* CC_SEND_TASK_NUM */

    int main_wake_fd;
    atomic_int main_parked;

    int n_data_workers;                         /* A */
    struct dpu_data_worker data_workers[MAX_ARM_WORKERS];
};

/* ====== Task-pool helpers ======
 * Acquire or release a slot in an atomic in-flight counter. Acquire returns 0
 * when the pool is full, which the caller treats like DOCA_ERROR_AGAIN. These
 * never sleep, block, or call DOCA. */
static inline int doca_pool_try_acquire(atomic_int *cnt, int max) {
    int new_count = atomic_fetch_add(cnt, 1) + 1;
    if (new_count > max - TASK_POOL_MARGIN) {
        atomic_fetch_sub(cnt, 1);
        return 0;
    }
    return 1;
}
static inline void doca_pool_release(atomic_int *cnt) {
    atomic_fetch_sub(cnt, 1);
}

/* Release a thread parked on its notification handles: claim its parked flag
 * and post one eventfd tick. Safe from any thread; no-op when not parked. */
static inline void dpu_wake_eventfd(atomic_int *parked, int wake_fd) {
    if (wake_fd < 0)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(parked, &expected, 0,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire))
        return;
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    (void)n;
}

/* Wake a data worker and record that its eventfd now holds a tick. The flag
 * is set after the write lands, so a clear that sees it clear may skip the
 * read; the tick it might miss is read by the pass that the readable eventfd
 * triggers next. */
static inline void dpu_wake_data_worker(atomic_int *parked, atomic_int *posted,
                                        int wake_fd) {
    if (wake_fd < 0)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(parked, &expected, 0,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire))
        return;
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    (void)n;
    atomic_store_explicit(posted, 1, memory_order_release);
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

#endif // OBJECT_H_
