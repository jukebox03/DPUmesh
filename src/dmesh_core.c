/* Host DOCA transport engine for control, DMA data, connections, and buffers. */

#include "dmesh_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>      /* tx_reserve / lifecycle report EAGAIN|EINVAL|EBADMSG */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>        /* close() for the host epoll RX path */
#include <sys/epoll.h>
#include <sys/eventfd.h>   /* per-EQ readiness eventfd for native-epoll integration */

#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_buf_array.h>
#include <doca_dpa.h>

#include "doca/common.h"
#include "doca/object.h"
#include "doca/config.h"
#include "doca/buffer.h"
#include "doca/ring.h"
#include "doca/comch_client.h"
#include "doca/comch_common.h"
#include "doca/comch_msgq.h"
#include "doca/dpa_common.h"
#include "dmesh_attest.h"
#include <dpumesh/dmesh_topology.h>

DOCA_LOG_REGISTER(DPUMESH_DOCA);

static const char *doca_err_str(doca_error_t rc) {
    return doca_error_get_descr(rc);
}

static void cleanup_ctx(struct dpumesh_ctx *ctx);
static int drain_rev_rings(struct dpumesh_ctx *ctx, uint32_t budget);
static int dmesh_drain_tx_locked(dmesh_qp_t *c, int flush_partial);
static int dmesh_drain_tx_upto_locked(dmesh_qp_t *c, int flush_partial,
                                      uint64_t commit_limit);
static int tx_timer_start(struct dpumesh_ctx *ctx);
static void tx_timer_stop(struct dpumesh_ctx *ctx);

/* ====================================================================
 * dpumesh_ctx — internal state
 * ==================================================================== */

/* Accept queue between the PE thread (producer — a NEW conn's first message
 * lands here) and dmesh_accept (consumer). */
#define RX_QUEUE_SIZE 65536

/* Per-connection send-unit FIFOs are sized from the configured byte window.
 * Sequence arithmetic is 16-bit, so keep the live window strictly below half
 * the sequence space. */
#define TX_SU_DEPTH_MIN 64u
#define TX_SU_DEPTH_MAX 32768u

/* Per-connection TX block-chain defaults over the shared TX mapping. */
#define TX_BLOCK_SIZE          (512 * 1024)
#define TX_BLOCKS_PER_CONN     8
#define TX_RECYCLED_CUSHION    1
/* A busy stream retains only its newest partial unit until this deadline. The
 * timer sleeps until the earliest deadline in the context, clamped to the wait
 * range below. */
#define TX_TAIL_DELAY_NS        500000ull
#define TX_TIMER_TICK_NS       1000000ull
#define TX_TIMER_MIN_WAIT_NS     50000ull
/* Close publishes FIN only after every submitted unit has left DPU proxy
 * custody, bounded by this deadline. */
#define TX_CLOSE_DRAIN_DEADLINE_NS 5000000000ull
#define TX_CLOSE_DRAIN_MIN_WAIT_NS       1000L
#define TX_CLOSE_DRAIN_MAX_WAIT_NS      50000L

enum dmesh_tx_wait_state {
    DMESH_TX_WAIT_IDLE = 0,
    DMESH_TX_WAIT_ARMED,
    DMESH_TX_WAIT_READY,
};

enum dmesh_tx_wait_reason {
    DMESH_TX_WAIT_NONE = 0,
    DMESH_TX_WAIT_QP_RECLAIM,
    DMESH_TX_WAIT_SHARED_POOL,
};

/* Full-duplex connections are indexed by local port; port zero denotes an accept.
 * Inbound descriptor queues are allocated per live connection and sized from the
 * DPU reverse-credit budget. Bodies remain in the shared RX mapping. */
#define RX_INBOX_MIN_CAPACITY 256u

/* Starting width of the client-port window (dpumesh_alloc_port). It sets the
 * floor on port-number reuse distance and doubles on demand. */
#define DMESH_PORT_SPAN_MIN   256u
/* Fields are grouped by mutating thread and separated by cache-line pads:
 * owner-local read-mostly setup first, then a producer (PE) write line, then the
 * shared arm flag on its own line, then a consumer (owner) write line. */
struct dmesh_port_slot {
    uint8_t          role;            /* FREE / CLIENT / SERVER */
    int16_t          peer_pod;        /* established peer pod, DMESH_POD_BLANK = not yet learned */
    uint16_t         peer_port;       /* established peer port, 0 = not yet learned */
    void            *user;            /* app's conn handle (returned by dmesh_next_ready);
                                       * published before role */
    struct dmesh_eq *eq;              /* owning EQ: the one ready list this conn's edges are
                                       * pushed to and the one fd they wake. Published with
                                       * `user`, before role; cleared at free_port. */
    /* Inbound SPSC ring: PE thread = sole producer (in_tail), the conn's owning
     * app thread = sole consumer (in_head). Lock-free. inbox==NULL until alloc. */
    sw_descriptor_t *inbox;           /* malloc'd ring[inbox_ring]; NULL until alloc */
    uint32_t         inbox_ring;      /* this inbox's depth (power of two = ctx->inbox_ring),
                                       * stamped at malloc so inbox_push/pop stay self-contained */
    /* Per-connection TX byte-ring over shared-pool blocks:
     *   tx_w  alloc/write head — where the next message body is written / alloc'd (owner)
     *   tx_c  commit           — bytes finalized as whole messages, ready to ship (owner)
     *   tx_s  send             — bytes a descriptor was posted for (owner, at flush)
     *   tx_f  free             — bytes ACKed by the DPU, reclaimable (PE thread, atomic)
     * Invariant: tx_f <= tx_s <= tx_c <= tx_w. Messages remain within one block.
     * The owner manages live blocks; the PE advances atomic tx_f on ACK. */
    uint64_t         tx_w;                  /* owner logical write cursor */
    _Atomic uint64_t tx_s;                  /* owner/worker publication cursor */
    _Atomic uint64_t tx_c;                  /* commit publication for tail worker */
    uint32_t         resv_len;              /* live reserve length (owner); 0 = none */
    uint64_t         resv_moff;             /* exact TX-mmap offset returned to the caller */
    uint64_t         tail_blk;              /* oldest live logical block index (owner) */
    uint64_t         head_blk_next;         /* next logical block index needing a physical block
                                             * (blocks [tail_blk, head_blk_next) are backed) */
    int32_t          pblk[TX_BLOCKS_PER_CONN];    /* logical block slot -> physical block; -1 = none */
    int32_t          recyc[TX_BLOCKS_PER_CONN];   /* drained blocks held for reuse (owner) */
    _Atomic uint32_t blk_used[TX_BLOCKS_PER_CONN];/* committed content end within each block */
    int              nblk_owned;            /* live and recycled physical blocks */
    int              nrec;                  /* recyc depth (owner) */
    uint16_t        *su_seq;                /* [ctx->su_depth] shipped seq (lazy malloc) */
    uint64_t        *su_end;                /* [ctx->su_depth] shipped end cursor */
    uint8_t         *su_done;               /* [ctx->su_depth] exact ACK reorder marks */

    /* Deadline of a retained partial unit, stamped once per retention. Zero
     * while nothing is retained. Written under tx_gate; the timer reads it. */
    atomic_uint_fast64_t tx_deadline_ns;
    /* Transmit gate. A public TX call holds it from reserve through post; the
     * EQ's deadline pass takes it only when uncontended and skips otherwise. */
    atomic_int       tx_gate;
    /* Sticky errno for a failed tail publication, mirrored by the EQ's tx_error
     * bit. Zero while healthy. */
    atomic_int       tx_error;

    /* One-shot TX writable notification. The owner records the EAGAIN snapshot, then
     * release-publishes ARMED. Reclaim producers acquire that state before reading the
     * snapshot and change it to READY exactly once. */
    char _cl_tx_wait[64];
    atomic_uint_fast32_t tx_wait_state;
    atomic_uint_fast32_t tx_wait_reason;
    atomic_uint_fast64_t tx_wait_tail_blk;    /* oldest block at the failed reserve */
    atomic_uint_fast64_t tx_wait_tx_w;        /* full-drain target at the failed reserve */
    atomic_uint_fast64_t tx_wait_pool_epoch;  /* shared-pool generation at pool-empty EAGAIN */

    /* ---- PRODUCER (PE thread) cache line: fields the PE mutates every message ---- */
    char _cl_prod[64];
    atomic_uint_fast32_t in_tail;           /* inbound SPSC producer (PE) */
    atomic_uint_fast64_t tx_f;              /* PE-thread logical cursor (ACK reclaim) */
    atomic_uint_fast16_t su_tail;           /* send-unit FIFO tail (PE writes/release, owner reads) */
    uint16_t         rx_seq;                 /* current reverse-delivery unit */
    uint32_t         rx_next_pos;            /* next fragment position within that unit */
    uint8_t          rx_seq_valid;

    /* ---- shared ARM flag on its own line (both threads write it) ---- */
    /* Ready-list ownership flag. The PE arms after a push; the consumer disarms
     * after draining and rechecks with paired sequentially consistent fences. */
    char _cl_arm[64];
    atomic_uint_fast32_t on_ready;    /* PE arms; consumer (conn_recv) disarms */

    /* ---- CONSUMER (owner app thread) cache line: fields the owner mutates ---- */
    char _cl_cons[64];
    atomic_uint_fast32_t in_head;     /* inbound SPSC consumer (app) */
    atomic_uint_fast16_t su_head;     /* send-unit FIFO head (owner writes/release, PE reads) */
    char _cl_end[64];                 /* isolate this slot's consumer line from the next slot */
};
/* Ready-list SPSC ops (monotonic counters; PE producer, EQ thread consumer). Each
 * list carries conn PORTS; dmesh_next_ready maps each to its slot->user. The
 * on_ready flag admits a conn at most once between drains. */
static inline void ready_push(struct dmesh_eq *eq, uint16_t port);
static inline int  ready_pop(struct dmesh_eq *eq, uint16_t *port);
/* Inbound SPSC ring ops (monotonic counters; count = tail-head). */
static inline int inbox_push(struct dmesh_port_slot *psl, const sw_descriptor_t *d) {
    uint_fast32_t t = atomic_load_explicit(&psl->in_tail, memory_order_relaxed);
    uint_fast32_t h = atomic_load_explicit(&psl->in_head, memory_order_acquire);
    if (t - h >= psl->inbox_ring) return 0;                  /* full */
    psl->inbox[t & (psl->inbox_ring - 1)] = *d;
    atomic_store_explicit(&psl->in_tail, t + 1, memory_order_release);
    return (t == h) ? 2 : 1;   /* 2 = empty→non-empty transition (edge-trigger the fd) */
}
static inline int inbox_pop(struct dmesh_port_slot *psl, sw_descriptor_t *out) {
    uint_fast32_t h = atomic_load_explicit(&psl->in_head, memory_order_relaxed);
    uint_fast32_t t = atomic_load_explicit(&psl->in_tail, memory_order_acquire);
    if (h == t) return 0;                                    /* empty */
    *out = psl->inbox[h & (psl->inbox_ring - 1)];
    atomic_store_explicit(&psl->in_head, h + 1, memory_order_release);
    return 1;
}
/* One cell of the lock-free bounded SPMC RX ring. `seq` carries the turn-stamp:
 * the producer may write cell i only when seq==enq_pos; a consumer may read it
 * only when seq==deq_pos+1. */
struct rxq_cell {
    sw_descriptor_t desc;
    atomic_uint_fast32_t seq;
};

struct dpumesh_ctx {
    char worker_id[128];
    int  pod_id;             /* this node's address; -1 until the DPU assigns it at register */
    int  num_slots;
    int  slot_size;
    int  k_rings;              /* K = forward rings per pod; 1 selects one ring */
    int  landing_stripes;      /* L = host RX buffer partitions */
    int  rx_credit_shards;     /* K/L = credit counters per landing stripe */
    int  inbox_ring;           /* per-conn inbound descriptor ring depth (pow2), sized to the
                                * DPU per-region reverse-credit budget so the inbox-full drop
                                * (rx_deliver_desc) is unreachable in steady use. */
    /* DOCA objects */
    struct objects doca_objs;
    void *dma_buffer;          /* Host TX buffer (PCI mmap, CPU→DPU source) */
    /* K forward descriptor rings selected by source port. Posting uses an MPSC
     * ticket and per-descriptor generation sequence. */
    struct dma_ring *dma_rings[MAX_EU_PER_POD];
    struct rev_ring *rev_rings[MAX_EU_PER_POD];
    uint64_t rev_arm_epoch;
    /* Host RX landing stripe size. */
    size_t rx_region_size;
    doca_dpa_dev_mmap_t dpa_mmap_handle;  /* DPA handle for local mmap (used in TX descriptors) */

    /* Host RX buffer (PCI mmap, DPU→CPU destination) */
    void *rx_dma_buffer;
    struct doca_mmap *rx_dma_mmap;
    size_t rx_dma_buf_size;

    /* Persistent buffer for the initial registration message. */
    struct dmesh_register_msg reg_msg;

    /* Per-connection TX block chains draw from a shared Treiber free list. Live
     * chains and send-unit FIFOs are owner-thread-local. */
    int   block_size;          /* bytes per block (= max contiguous message = alloc unit) */
    int   n_blocks;            /* number of blocks = num_slots*slot_size / block_size */
    int   blocks_per_conn;     /* per-connection in-flight block limit */
    uint32_t su_depth;         /* power-of-two send-unit FIFO depth */
    int   recycle_reserve;     /* recycled blocks retained per connection */
    atomic_uint_fast64_t block_free;   /* Treiber head: (tag<<32) | head_index */
    uint32_t *block_next;      /* [n_blocks]: free-list links */
    pthread_mutex_t block_lock;    /* close-path block return (exactly-once handoff, cold) */
    int block_lock_initialized;
    /* QPs below their own limit but blocked on the process-wide pool. A returned
     * physical block claims one bit, so one capacity unit wakes at most one waiter.
     * The bitmap is channel-wide and indexed by local port. */
    atomic_uint_fast64_t pool_epoch;
    atomic_uint_fast64_t pool_waiters[DMESH_TX_READY_WORDS];
    atomic_uint_fast32_t pool_waiter_count;
    atomic_uint_fast32_t pool_wait_cursor;
    /* Block-pool event counters, reported by dmesh_get_tx_stats. */
    atomic_ullong st_pool_grabs;    /* shared-pool CAS pops (conn grow / first block) */
    atomic_ullong st_pool_returns;  /* shared-pool CAS pushes (shrink / close drain) */
    atomic_ullong st_recycle_hits;  /* grow served from the conn's recyc[] (no pool op) */
    atomic_ullong st_grow_waits;    /* backoff sleeps in reserve (window full / pool empty) */
    /* The two admission failures reserve can hit, split so a stall names its own
     * cause: the QP's own block window, or the channel-wide block pool. */
    atomic_ullong st_wait_window;
    atomic_ullong st_wait_pool;
    atomic_ullong st_block_pads;    /* message didn't fit the block tail → pad + next block */
    /* RX drop counters. */
    atomic_ullong st_rx_inbox_drops;   /* established/pending conn inbox full → message dropped */
    atomic_ullong st_rx_accept_drops;  /* accept queue full → NEW conn dropped */
    atomic_ullong st_rx_credit_drops;  /* landing offset outside the RX mapping */

    /* One channel-level timer writes the readiness fd of each EQ holding a
     * retained tail. It touches no port slot and publishes nothing. It parks
     * while tx_armed_total is zero. */
    pthread_t tx_timer_tid;
    pthread_mutex_t tx_timer_lock;
    pthread_cond_t tx_timer_cv;
    int tx_timer_lock_initialized;
    int tx_timer_cv_initialized;
    int tx_timer_running;
    int tx_timer_thread_started;
    atomic_uint_fast32_t tx_armed_total;

    /* RX descriptor queue — lock-free bounded SPMC ring (1 producer = PE
     * thread, N consumers = server workers). dpumesh_dequeue spin-polls it with
     * adaptive backoff; epoll_wait() on the readiness eventfd is the idle-sleep
     * path. */
    struct rxq_cell *rx_ring;          /* RX_QUEUE_SIZE cells (power of two) */
    /* rx_enq (producer-private) and rx_deq (CAS'd by every consumer) sit on
     * separate cache lines. */
    char _rx_pad0[64];
    atomic_uint_fast32_t rx_enq;       /* producer position (PE only) */
    char _rx_pad1[64];
    atomic_uint_fast32_t rx_deq;       /* consumer position (workers CAS) */
    char _rx_pad2[64];

    /* PE progress thread */
    pthread_t pe_tid;
    volatile int pe_running;

    /* EQ registry. An ESTABLISHED conn's delivery wakes only its own EQ (psl->eq),
     * which is what lets N threads receive in parallel. The registry serves the ONE
     * delivery that has no conn yet: a NEW conn goes on the shared accept queue, so
     * every EQ is notified and whichever one accepts it owns it. The lock also
     * excludes dmesh_destroy_eq's unregister against the PE. */
    struct dmesh_eq *eqs[DMESH_MAX_EQ];
    int              n_eqs;            /* high-water mark of eqs[]; slots may be NULL */
    pthread_mutex_t  eq_lock;
    int              eq_lock_initialized;

    /* Endpoint port table + allocator (oriented-tuple demux). */
    struct dmesh_port_slot *ports;     /* [DMESH_PORT_SPACE] */
    pthread_mutex_t port_lock;
    int port_lock_initialized;
    uint32_t next_port;                /* bump cursor, wraps within [1, port_span) */
    uint32_t port_span;                /* live client-port window: the cursor's wrap point,
                                        * and so the cap on how many per-port inboxes this
                                        * process ever allocates. Doubles (to
                                        * DMESH_UPORT_BASE) only when the window is full. */
    int32_t service_id;                /* this node's service id (SVC_NONE if client-only) */
};

/* ====================================================================
 * Retained-tail scheduling
 *
 * A QP's transmit state has one mutator: the thread that owns the QP. A post
 * leaving a fillable partial unit arms a bit on that QP's EQ and stamps a
 * deadline. The owner publishes the tail from its next transmit call or from
 * dmesh_poll_eq. The timer wakes an EQ that still holds armed bits.
 * ==================================================================== */

static void tx_error_publish(struct dmesh_port_slot *psl,
                             uint16_t port, int error_number);
static inline void eq_notify(struct dmesh_eq *eq);

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static inline void eq_tx_armed_set(struct dmesh_eq *eq, uint16_t port,
                                   uint64_t deadline)
{
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    uint64_t seen = atomic_load_explicit(&eq->tx_earliest_ns,
                                         memory_order_relaxed);
    if (seen == 0 || deadline < seen)
        atomic_store_explicit(&eq->tx_earliest_ns, deadline,
                              memory_order_relaxed);
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_or_explicit(&eq->tx_armed[word], mask,
                                                 memory_order_release);
    if ((old & mask) != 0)
        return;
    atomic_fetch_add_explicit(&eq->tx_armed_count, 1, memory_order_release);
    uint_fast32_t before = atomic_fetch_add_explicit(&ctx->tx_armed_total, 1,
                                                     memory_order_release);
    /* Only a zero-to-one transition of the channel total wakes the timer. */
    if (before == 0 && ctx->tx_timer_lock_initialized) {
        pthread_mutex_lock(&ctx->tx_timer_lock);
        if (ctx->tx_timer_cv_initialized)
            pthread_cond_signal(&ctx->tx_timer_cv);
        pthread_mutex_unlock(&ctx->tx_timer_lock);
    }
}

static inline void eq_tx_armed_clear(struct dmesh_eq *eq, uint16_t port)
{
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_and_explicit(&eq->tx_armed[word], ~mask,
                                                  memory_order_acq_rel);
    if (old & mask) {
        if (atomic_fetch_sub_explicit(&eq->tx_armed_count, 1,
                                      memory_order_relaxed) == 1)
            atomic_store_explicit(&eq->tx_earliest_ns, 0,
                                  memory_order_relaxed);
        atomic_fetch_sub_explicit(&eq->ch->ctx->tx_armed_total, 1,
                                  memory_order_relaxed);
    }
}

/* The transmit gate serializes a QP's public TX calls against the deadline pass
 * that dmesh_poll_eq runs on the EQ thread. */
static inline int tx_gate_try(struct dmesh_port_slot *psl)
{
    int expected = 0;
    return atomic_compare_exchange_strong_explicit(&psl->tx_gate, &expected, 1,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

/* True while a transmit call is open on this QP: dmesh_alloc holds the gate and
 * left a reservation that dmesh_post_send or a close consumes. */
static inline int tx_call_open(const struct dmesh_port_slot *psl)
{
    return psl->resv_len != 0;
}

/* The holder is one bounded drain: spin briefly, then yield. */
#if defined(__x86_64__) || defined(__i386__)
#  define TX_GATE_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
#  define TX_GATE_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#  define TX_GATE_RELAX() ((void)0)
#endif
#define TX_GATE_SPINS 256u

static inline void tx_gate_acquire(struct dmesh_port_slot *psl)
{
    for (unsigned spins = 0; !tx_gate_try(psl); spins++) {
        if (spins < TX_GATE_SPINS) TX_GATE_RELAX();
        else sched_yield();
    }
}

static inline void tx_gate_release(struct dmesh_port_slot *psl)
{
    atomic_store_explicit(&psl->tx_gate, 0, memory_order_release);
}

/* Retain this QP's tail until its deadline. A repeat arm leaves the existing
 * stamp in place. Held under tx_gate. */
static void tx_arm_tail(struct dmesh_port_slot *psl, uint16_t port)
{
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    if (!eq)
        return;
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    if (atomic_load_explicit(&eq->tx_armed[word], memory_order_acquire) & mask)
        return;                                   /* already retained */
    uint64_t deadline = monotonic_ns() + TX_TAIL_DELAY_NS;
    atomic_store_explicit(&psl->tx_deadline_ns, deadline, memory_order_relaxed);
    eq_tx_armed_set(eq, port, deadline);
}

/* Release the retention: the bit and its stamp clear together, so a nonzero
 * deadline always means retained. */
static void tx_disarm_tail(struct dmesh_port_slot *psl, uint16_t port)
{
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    if (eq)
        eq_tx_armed_clear(eq, port);
    atomic_store_explicit(&psl->tx_deadline_ns, 0, memory_order_relaxed);
}

/* Pop one armed port whose deadline has expired and whose transmit gate is
 * free, clearing its bit and returning with that gate held for the caller. A
 * port still coalescing, and one whose owner is inside a transmit call, keeps
 * its retention for a later pass. */
static int eq_tx_armed_pop_due(struct dmesh_eq *eq, uint64_t now, uint16_t *port)
{
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    /* The armed count bounds the scan. */
    uint_fast32_t remaining =
        atomic_load_explicit(&eq->tx_armed_count, memory_order_acquire);
    if (remaining == 0)
        return 0;
    uint32_t start = eq->tx_armed_cursor;
    for (uint32_t n = 0; n < DMESH_TX_READY_WORDS && remaining; n++) {
        uint32_t word = (start + n) & (DMESH_TX_READY_WORDS - 1);
        uint_fast64_t bits = atomic_load_explicit(&eq->tx_armed[word],
                                                  memory_order_acquire);
        while (bits && remaining) {
            unsigned bit = (unsigned)__builtin_ctzll((unsigned long long)bits);
            bits &= bits - 1;
            remaining--;
            uint16_t candidate = (uint16_t)(word * 64u + bit);
            struct dmesh_port_slot *psl = &ctx->ports[candidate];
            if (atomic_load_explicit(&psl->tx_deadline_ns,
                                     memory_order_relaxed) > now)
                continue;                          /* still coalescing */
            if (!tx_gate_try(psl))
                continue;                          /* owner is transmitting */
            uint_fast64_t mask = (uint_fast64_t)1u << bit;
            uint_fast64_t old = atomic_fetch_and_explicit(&eq->tx_armed[word],
                                                          ~mask,
                                                          memory_order_acq_rel);
            if ((old & mask) == 0) {
                tx_gate_release(psl);
                continue;
            }
            atomic_fetch_sub_explicit(&eq->tx_armed_count, 1,
                                      memory_order_relaxed);
            atomic_fetch_sub_explicit(&ctx->tx_armed_total, 1,
                                      memory_order_relaxed);
            atomic_store_explicit(&ctx->ports[candidate].tx_deadline_ns, 0,
                                  memory_order_relaxed);
            eq->tx_armed_cursor = word;
            *port = candidate;
            return 1;
        }
    }
    return 0;
}

/* Recompute the cached earliest deadline from the armed bits. Bounded by the
 * armed count and run only after a publication pass. */
static void eq_tx_armed_refresh(struct dmesh_eq *eq)
{
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    uint_fast32_t remaining =
        atomic_load_explicit(&eq->tx_armed_count, memory_order_acquire);
    if (remaining == 0) {
        atomic_store_explicit(&eq->tx_earliest_ns, 0, memory_order_relaxed);
        return;
    }
    uint64_t earliest = UINT64_MAX;
    for (uint32_t word = 0; word < DMESH_TX_READY_WORDS && remaining; word++) {
        uint_fast64_t bits = atomic_load_explicit(&eq->tx_armed[word],
                                                  memory_order_acquire);
        while (bits && remaining) {
            unsigned bit = (unsigned)__builtin_ctzll((unsigned long long)bits);
            bits &= bits - 1;
            remaining--;
            uint64_t deadline = atomic_load_explicit(
                &ctx->ports[word * 64u + bit].tx_deadline_ns,
                memory_order_relaxed);
            if (deadline < earliest) earliest = deadline;
        }
    }
    atomic_store_explicit(&eq->tx_earliest_ns,
                          earliest == UINT64_MAX ? 0 : earliest,
                          memory_order_relaxed);
}

/* Nanoseconds until this EQ's earliest retained tail must be published, or -1
 * when none is retained. */
static int64_t eq_tx_armed_wait_ns(struct dmesh_eq *eq, uint64_t now)
{
    uint64_t earliest = atomic_load_explicit(&eq->tx_earliest_ns,
                                             memory_order_relaxed);
    if (earliest == 0)
        return -1;
    return earliest <= now ? 0 : (int64_t)(earliest - now);
}

/* Nanoseconds until the earliest retained tail in the context comes due,
 * clamped to [TX_TIMER_MIN_WAIT_NS, TX_TIMER_TICK_NS]. */
static uint64_t tx_timer_wait_ns(dpumesh_ctx_t *ctx, uint64_t now)
{
    uint64_t wait = TX_TIMER_TICK_NS;
    pthread_mutex_lock(&ctx->eq_lock);
    for (int i = 0; i < ctx->n_eqs; i++) {
        struct dmesh_eq *eq = ctx->eqs[i];
        if (!eq) continue;
        int64_t due = eq_tx_armed_wait_ns(eq, now);
        if (due >= 0 && (uint64_t)due < wait) wait = (uint64_t)due;
    }
    pthread_mutex_unlock(&ctx->eq_lock);
    return wait < TX_TIMER_MIN_WAIT_NS ? TX_TIMER_MIN_WAIT_NS : wait;
}

/* Wake owners blocked in poll while holding a retained tail. Publishes nothing
 * and reads no port slot. */
static void *tx_timer_fn(void *arg)
{
    dpumesh_ctx_t *ctx = arg;
    for (;;) {
        pthread_mutex_lock(&ctx->tx_timer_lock);
        while (ctx->tx_timer_running &&
               atomic_load_explicit(&ctx->tx_armed_total,
                                    memory_order_acquire) == 0)
            pthread_cond_wait(&ctx->tx_timer_cv, &ctx->tx_timer_lock);
        int running = ctx->tx_timer_running;
        pthread_mutex_unlock(&ctx->tx_timer_lock);
        if (!running)
            break;

        uint64_t now = monotonic_ns();
        uint64_t wake_ns = now + tx_timer_wait_ns(ctx, now);
        struct timespec wake = {
            .tv_sec = (time_t)(wake_ns / 1000000000ull),
            .tv_nsec = (long)(wake_ns % 1000000000ull),
        };
        pthread_mutex_lock(&ctx->tx_timer_lock);
        if (ctx->tx_timer_running)
            pthread_cond_timedwait(&ctx->tx_timer_cv, &ctx->tx_timer_lock,
                                   &wake);
        running = ctx->tx_timer_running;
        pthread_mutex_unlock(&ctx->tx_timer_lock);
        if (!running)
            break;

        /* Wake only an EQ whose earliest tail has come due, once per EQ: its
         * owner drains every expired tail in one pass. */
        now = monotonic_ns();
        pthread_mutex_lock(&ctx->eq_lock);
        for (int i = 0; i < ctx->n_eqs; i++) {
            struct dmesh_eq *eq = ctx->eqs[i];
            if (eq && eq_tx_armed_wait_ns(eq, now) == 0) {
                atomic_store_explicit(&eq->tx_due_hint, 1,
                                      memory_order_release);
                eq_notify(eq);
            }
        }
        pthread_mutex_unlock(&ctx->eq_lock);
    }
    return NULL;
}

static int tx_timer_start(dpumesh_ctx_t *ctx)
{
    int rc = pthread_mutex_init(&ctx->tx_timer_lock, NULL);
    if (rc != 0) { errno = rc; return -1; }
    ctx->tx_timer_lock_initialized = 1;

    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc != 0) { errno = rc; return -1; }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(&ctx->tx_timer_cv, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) { errno = rc; return -1; }
    ctx->tx_timer_cv_initialized = 1;

    ctx->tx_timer_running = 1;
    rc = pthread_create(&ctx->tx_timer_tid, NULL, tx_timer_fn, ctx);
    if (rc != 0) {
        ctx->tx_timer_running = 0;
        errno = rc;
        return -1;
    }
    ctx->tx_timer_thread_started = 1;
    return 0;
}

static void tx_timer_stop(dpumesh_ctx_t *ctx)
{
    if (!ctx || !ctx->tx_timer_lock_initialized) return;
    pthread_mutex_lock(&ctx->tx_timer_lock);
    ctx->tx_timer_running = 0;
    if (ctx->tx_timer_cv_initialized) pthread_cond_broadcast(&ctx->tx_timer_cv);
    pthread_mutex_unlock(&ctx->tx_timer_lock);
    if (ctx->tx_timer_thread_started) {
        pthread_join(ctx->tx_timer_tid, NULL);
        ctx->tx_timer_thread_started = 0;
    }
}

/* ====================================================================
 * PE progress thread — drives DOCA progress engine
 * ==================================================================== */

/* Adaptive-poll tuning: spin this many empty iterations before the first
 * back-off, then sleep a short, capped interval per empty iteration. */
#define PE_IDLE_SPIN     2048
#define PE_BACKOFF_NS    20000   /* 20 us */
#define PE_BLOCK_MS      200     /* backstop so a missed wake cannot strand a drain */

static void *pe_progress_fn(void *arg) {
    dpumesh_ctx_t *ctx = (dpumesh_ctx_t *)arg;
    struct doca_pe *pe = ctx->doca_objs.pe;

    /* Use PE notifications when available; otherwise use adaptive polling. */
    int want_epoll = 1;

    doca_notification_handle_t pfd = 0;
    int ep = -1;
    if (want_epoll && pe &&
        doca_pe_get_notification_handle(pe, &pfd) == DOCA_SUCCESS) {
        ep = epoll_create1(0);
        if (ep >= 0) {
            struct epoll_event ev = { .events = EPOLLIN, .data = { .u32 = 0 } };
            if (epoll_ctl(ep, EPOLL_CTL_ADD, (int)pfd, &ev) != 0) { close(ep); ep = -1; }
        }
    }

    if (ep >= 0) {
        /* ===== Event-driven: drain → arm → re-check → block ===== */
        while (ctx->pe_running) {
            int did;
            do {
                did = 0;
                while (doca_pe_progress(pe))
                    did = 1;
                if (drain_rev_rings(ctx, 256) > 0)
                    did = 1;
            } while (did);

            /* Arm, then re-check once: a completion landing between the last
             * drain and the arm must not be stranded. */
            (void)doca_pe_request_notification(pe);
            /* Publishing a new epoch is what makes the DPU send REV_DOORBELL
             * for anything it lands while this thread is blocked. */
            uint64_t epoch = ++ctx->rev_arm_epoch;
            for (int r = 0; r < ctx->landing_stripes; r++)
                __atomic_store_n(&ctx->rev_rings[r]->ctrl->arm_epoch,
                                 epoch, __ATOMIC_RELEASE);
            if (doca_pe_progress(pe) || drain_rev_rings(ctx, 256) > 0) {
                (void)doca_pe_clear_notification(pe, pfd);
                continue;
            }
            /* Block until a completion raises the fd. The bound caps how long a
             * missed wake can hold an already-published reverse entry, and bounds
             * how late a pe_running=0 shutdown is observed. */
            struct epoll_event evs[1];
            (void)epoll_wait(ep, evs, 1, PE_BLOCK_MS);
            (void)doca_pe_clear_notification(pe, pfd);
        }
        close(ep);
        return NULL;
    }

    /* Adaptive polling fallback. */
    uint32_t idle = 0;
    while (ctx->pe_running) {
        uint8_t did = 0;
        if (pe)
            did |= doca_pe_progress(pe);
        did |= drain_rev_rings(ctx, 256) > 0;
        if (did) {
            idle = 0;                 /* work seen — keep spinning tight */
        } else if (++idle >= PE_IDLE_SPIN) {
            struct timespec t = {0, PE_BACKOFF_NS};
            nanosleep(&t, NULL);
        }
    }
    return NULL;
}

/* ====================================================================
 * RX data hook — called from PE progress thread via comch callback
 * ==================================================================== */

/* Map a landing byte offset to the forward ring holding its admission credit:
 * L stripes of rx_region_size, each aggregating K/L credit shards. Returns -1
 * for an offset outside the RX mapping. */
static inline int
rx_credit_shard_index(const dpumesh_ctx_t *ctx, int pos)
{
    if (pos < 0 || (size_t)pos >= ctx->rx_dma_buf_size)
        return -1;
    size_t absolute = (size_t)pos;
    int stripe = (int)(absolute / ctx->rx_region_size);
    size_t stripe_pos = absolute - (size_t)stripe * ctx->rx_region_size;
    int shard = (int)((stripe_pos / DPUMESH_SLOT_SIZE) %
                      (size_t)ctx->rx_credit_shards);
    return stripe + shard * ctx->landing_stripes;
}

/* Return one unit of reverse-DMA admission credit. */
static inline void rx_credit_return(dpumesh_ctx_t *ctx, int pos)
{
    int idx = rx_credit_shard_index(ctx, pos);
    if (idx < 0) {
        if (atomic_fetch_add_explicit(&ctx->st_rx_credit_drops, 1,
                                      memory_order_relaxed) == 0)
            DOCA_LOG_ERR("RX credit: landing offset %d outside the %zu-byte RX "
                         "mapping; credit dropped", pos, ctx->rx_dma_buf_size);
        return;
    }
    struct dma_ring *r = ctx->dma_rings[idx];
    if (r && r->descs) {
        volatile uint64_t *credit =
            (volatile uint64_t *)(r->descs +
                                  DMA_RING_CREDIT_SLOT(r->size));
        __sync_add_and_fetch(credit, 1);
    }
}

/* Lock-free SPMC dequeue. Multiple worker consumers race via CAS on
 * rx_deq; the single PE producer owns rx_enq. Returns 1 and fills *out on
 * success, 0 if the ring is empty. Never blocks. */
static inline int rxq_try_pop(dpumesh_ctx_t *ctx, sw_descriptor_t *out)
{
    for (;;) {
        uint_fast32_t pos = atomic_load_explicit(&ctx->rx_deq, memory_order_relaxed);
        struct rxq_cell *c = &ctx->rx_ring[pos & (RX_QUEUE_SIZE - 1)];
        uint_fast32_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        int_fast32_t diff = (int_fast32_t)(seq - (pos + 1));
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &ctx->rx_deq, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                *out = c->desc;
                /* Release the cell for reuse one full lap ahead. */
                atomic_store_explicit(&c->seq, pos + RX_QUEUE_SIZE,
                                      memory_order_release);
                return 1;
            }
            /* CAS lost to another consumer — retry. */
        } else if (diff < 0) {
            return 0;  /* empty */
        }
        /* diff > 0: producer mid-write of the cell we'd claim — retry. */
    }
}

/* Wake the EQ eventfd consumer when notifications are enabled. Poll-only EQs use
 * the ready list without eventfd writes. */
static inline void eq_notify(struct dmesh_eq *eq)
{
    if (eq->notify_efd >= 0 &&
        atomic_load_explicit(&eq->wants_notify, memory_order_seq_cst) &&
        atomic_load_explicit(&eq->suppress_notify, memory_order_seq_cst) == 0) {
        uint64_t one = 1;
        ssize_t w = write(eq->notify_efd, &one, sizeof(one));
        (void)w;
    }
}

static int eq_has_pending(const struct dmesh_eq *eq)
{
    const dpumesh_ctx_t *ctx = eq->ch->ctx;
    if (eq->drain_cur)
        return 1;
    if (atomic_load_explicit(&eq->ready_head, memory_order_acquire) !=
        atomic_load_explicit(&eq->ready_tail, memory_order_acquire))
        return 1;
    if (atomic_load_explicit(&eq->tx_ready_count, memory_order_acquire) != 0)
        return 1;
    if (atomic_load_explicit(&eq->tx_error_count, memory_order_acquire) != 0)
        return 1;
    return atomic_load_explicit(&ctx->rx_enq, memory_order_acquire) !=
           atomic_load_explicit(&ctx->rx_deq, memory_order_acquire);
}

void dmesh_eq_suppress_notify(dmesh_eq_t *eq, int delta)
{
    if (!eq || (delta != 1 && delta != -1))
        return;
    if (delta > 0) {
        atomic_fetch_add_explicit(&eq->suppress_notify, 1, memory_order_seq_cst);
        return;
    }
    int old = atomic_fetch_sub_explicit(&eq->suppress_notify, 1,
                                        memory_order_seq_cst);
    if (old <= 0) {
        atomic_fetch_add_explicit(&eq->suppress_notify, 1, memory_order_seq_cst);
        return;
    }
    if (old == 1 && eq_has_pending(eq))
        eq_notify(eq);
}

/* Wake EVERY EQ. Used only for the shared accept queue, whose conns have no EQ
 * yet, so any consumer may claim them. The lock also keeps a concurrent
 * dmesh_destroy_eq from freeing an EQ under this walk. */
static void notify_all_eqs(dpumesh_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->eq_lock);
    for (int i = 0; i < ctx->n_eqs; i++)
        if (ctx->eqs[i]) eq_notify(ctx->eqs[i]);
    pthread_mutex_unlock(&ctx->eq_lock);
}

/* Ready-list SPSC: PE pushes a ready conn's port; that conn's EQ thread pops it via
 * dmesh_next_ready. Monotonic counters (count = tail-head); ring index masks the
 * power-of-two DMESH_PORT_SPACE. */
static inline void ready_push(struct dmesh_eq *eq, uint16_t port) {
    uint_fast32_t t = atomic_load_explicit(&eq->ready_tail, memory_order_relaxed);
    uint_fast32_t h = atomic_load_explicit(&eq->ready_head, memory_order_acquire);
    if (t - h >= DMESH_PORT_SPACE) return;   /* provably never full; guard anyway */
    eq->ready_ring[t & (DMESH_PORT_SPACE - 1)] = port;
    atomic_store_explicit(&eq->ready_tail, t + 1, memory_order_release);
}
static inline int ready_pop(struct dmesh_eq *eq, uint16_t *port) {
    uint_fast32_t h = atomic_load_explicit(&eq->ready_head, memory_order_relaxed);
    uint_fast32_t t = atomic_load_explicit(&eq->ready_tail, memory_order_acquire);
    if (h == t) return 0;                                    /* empty */
    *port = eq->ready_ring[h & (DMESH_PORT_SPACE - 1)];
    atomic_store_explicit(&eq->ready_head, h + 1, memory_order_release);
    return 1;
}

/* TX-ready is a one-bit, one-shot event per QP with two producers — the PE ACK
 * path and any owner returning a shared block — so publication and cancellation
 * use an atomic bitmap. The count is only an empty fast path; the bit is
 * authoritative. */
static inline void eq_tx_ready_set(struct dmesh_eq *eq, uint16_t port) {
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_or_explicit(&eq->tx_ready[word], mask,
                                                  memory_order_release);
    if ((old & mask) == 0) {
        atomic_fetch_add_explicit(&eq->tx_ready_count, 1, memory_order_release);
        eq_notify(eq);
    }
}

static inline void eq_tx_ready_clear(struct dmesh_eq *eq, uint16_t port) {
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_and_explicit(&eq->tx_ready[word], ~mask,
                                                   memory_order_acq_rel);
    if (old & mask)
        atomic_fetch_sub_explicit(&eq->tx_ready_count, 1, memory_order_relaxed);
}

static int eq_tx_ready_pop(struct dmesh_eq *eq, uint16_t *port) {
    if (atomic_load_explicit(&eq->tx_ready_count, memory_order_acquire) == 0)
        return 0;
    uint32_t start = eq->tx_ready_cursor;
    for (uint32_t n = 0; n < DMESH_TX_READY_WORDS; n++) {
        uint32_t word = (start + n) & (DMESH_TX_READY_WORDS - 1);
        uint_fast64_t bits = atomic_load_explicit(&eq->tx_ready[word], memory_order_acquire);
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll((unsigned long long)bits);
            uint_fast64_t mask = (uint_fast64_t)1u << bit;
            uint_fast64_t old = atomic_fetch_and_explicit(&eq->tx_ready[word], ~mask,
                                                           memory_order_acq_rel);
            if (old & mask) {
                atomic_fetch_sub_explicit(&eq->tx_ready_count, 1, memory_order_relaxed);
                eq->tx_ready_cursor = (word + 1) & (DMESH_TX_READY_WORDS - 1);
                *port = (uint16_t)(word * 64u + bit);
                return 1;
            }
            bits = old & ~mask;
        }
    }
    return 0;
}

static inline void eq_tx_error_set(struct dmesh_eq *eq, uint16_t port) {
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_or_explicit(&eq->tx_error[word], mask,
                                                 memory_order_release);
    if ((old & mask) == 0) {
        atomic_fetch_add_explicit(&eq->tx_error_count, 1, memory_order_release);
        eq_notify(eq);
    }
}

static inline void eq_tx_error_clear(struct dmesh_eq *eq, uint16_t port) {
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_and_explicit(&eq->tx_error[word], ~mask,
                                                  memory_order_acq_rel);
    if (old & mask)
        atomic_fetch_sub_explicit(&eq->tx_error_count, 1, memory_order_relaxed);
}

static int eq_tx_error_pop(struct dmesh_eq *eq, uint16_t *port) {
    if (atomic_load_explicit(&eq->tx_error_count, memory_order_acquire) == 0)
        return 0;
    uint32_t start = eq->tx_error_cursor;
    for (uint32_t n = 0; n < DMESH_TX_READY_WORDS; n++) {
        uint32_t word = (start + n) & (DMESH_TX_READY_WORDS - 1);
        uint_fast64_t bits = atomic_load_explicit(&eq->tx_error[word],
                                                  memory_order_acquire);
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll((unsigned long long)bits);
            uint_fast64_t mask = (uint_fast64_t)1u << bit;
            uint_fast64_t old = atomic_fetch_and_explicit(&eq->tx_error[word],
                                                           ~mask,
                                                           memory_order_acq_rel);
            if (old & mask) {
                atomic_fetch_sub_explicit(&eq->tx_error_count, 1,
                                          memory_order_relaxed);
                eq->tx_error_cursor = (word + 1) & (DMESH_TX_READY_WORDS - 1);
                *port = (uint16_t)(word * 64u + bit);
                return 1;
            }
            bits = old & ~mask;
        }
    }
    return 0;
}

/* The event is one-shot; tx_error itself remains sticky. Publication may come
 * from the caller or the claimed deadline worker. */
static void tx_error_publish(struct dmesh_port_slot *psl,
                             uint16_t port, int error_number) {
    int expected = 0;
    int published = error_number ? error_number : EBADMSG;
    if (!atomic_compare_exchange_strong_explicit(&psl->tx_error, &expected,
                                                  published,
                                                  memory_order_release,
                                                  memory_order_relaxed))
        return;
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
    if (eq && (role == DMESH_ROLE_CLIENT || role == DMESH_ROLE_SERVER))
        eq_tx_error_set(eq, port);
}

static inline void pool_waiter_set(dpumesh_ctx_t *ctx, uint16_t port) {
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_or_explicit(&ctx->pool_waiters[word], mask,
                                                  memory_order_release);
    if ((old & mask) == 0)
        atomic_fetch_add_explicit(&ctx->pool_waiter_count, 1, memory_order_release);
}

static inline void pool_waiter_clear(dpumesh_ctx_t *ctx, uint16_t port) {
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    uint_fast64_t old = atomic_fetch_and_explicit(&ctx->pool_waiters[word], ~mask,
                                                   memory_order_acq_rel);
    if (old & mask)
        atomic_fetch_sub_explicit(&ctx->pool_waiter_count, 1, memory_order_relaxed);
}

static int pool_waiter_claim(dpumesh_ctx_t *ctx, uint16_t *port) {
    uint32_t start = atomic_fetch_add_explicit(&ctx->pool_wait_cursor, 1,
                                                memory_order_relaxed) &
                     (DMESH_TX_READY_WORDS - 1);
    for (uint32_t n = 0; n < DMESH_TX_READY_WORDS; n++) {
        uint32_t word = (start + n) & (DMESH_TX_READY_WORDS - 1);
        uint_fast64_t bits = atomic_load_explicit(&ctx->pool_waiters[word],
                                                   memory_order_acquire);
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll((unsigned long long)bits);
            uint_fast64_t mask = (uint_fast64_t)1u << bit;
            uint_fast64_t old = atomic_fetch_and_explicit(&ctx->pool_waiters[word], ~mask,
                                                           memory_order_acq_rel);
            if (old & mask) {
                atomic_fetch_sub_explicit(&ctx->pool_waiter_count, 1,
                                          memory_order_relaxed);
                *port = (uint16_t)(word * 64u + bit);
                return 1;
            }
            bits = old & ~mask;
        }
    }
    return 0;
}

/* Change an armed QP into a queued event exactly once. Cancellation may race
 * after the CAS (a polling caller can succeed before consuming the event), so
 * publication rechecks READY and removes the bit if the owner already cancelled it. */
static int tx_wait_make_ready(dpumesh_ctx_t *ctx, uint16_t port) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint_fast32_t expected = DMESH_TX_WAIT_ARMED;
    if (!atomic_compare_exchange_strong_explicit(&psl->tx_wait_state, &expected,
                                                  DMESH_TX_WAIT_READY,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
        return 0;

    pool_waiter_clear(ctx, port);   /* no-op for a per-QP reclaim waiter */
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
    if (!eq || (role != DMESH_ROLE_CLIENT && role != DMESH_ROLE_SERVER)) {
        atomic_store_explicit(&psl->tx_wait_state, DMESH_TX_WAIT_IDLE,
                              memory_order_release);
        return 0;
    }

    eq_tx_ready_set(eq, port);
    if (atomic_load_explicit(&psl->tx_wait_state, memory_order_acquire) !=
            DMESH_TX_WAIT_READY ||
        __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE) != eq)
        eq_tx_ready_clear(eq, port);
    return 1;
}

static void tx_wait_cancel(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl,
                           uint16_t port) {
    /* Only this QP's owner arms, so IDLE means there is nothing to clear. */
    if (atomic_load_explicit(&psl->tx_wait_state, memory_order_acquire) ==
        (uint_fast32_t)DMESH_TX_WAIT_IDLE)
        return;
    atomic_exchange_explicit(&psl->tx_wait_state, DMESH_TX_WAIT_IDLE,
                             memory_order_acq_rel);
    pool_waiter_clear(ctx, port);
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    if (eq) eq_tx_ready_clear(eq, port);
}

static int tx_wait_qp_retryable(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl) {
    uint64_t f = atomic_load_explicit(&psl->tx_f, memory_order_acquire);
    uint64_t wait_tail = atomic_load_explicit(&psl->tx_wait_tail_blk,
                                               memory_order_relaxed);
    if (f / (uint64_t)ctx->block_size > wait_tail)
        return 1;
    uint64_t wait_w = atomic_load_explicit(&psl->tx_wait_tx_w,
                                            memory_order_relaxed);
    uint16_t head = atomic_load_explicit(&psl->su_head, memory_order_acquire);
    uint16_t tail = atomic_load_explicit(&psl->su_tail, memory_order_acquire);
    return f == wait_w && head == tail;
}

/* Publish the failed-reserve snapshot, then recheck the relevant capacity source.
 * That final check closes the EAGAIN->ARM race: an ACK/block return concurrent with
 * arming either observes ARMED or changes the snapshot generation we inspect here. */
static void tx_wait_arm(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl,
                        uint16_t port, enum dmesh_tx_wait_reason reason) {
    tx_wait_cancel(ctx, psl, port);
    atomic_store_explicit(&psl->tx_wait_tail_blk, psl->tail_blk,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->tx_wait_tx_w, psl->tx_w, memory_order_relaxed);
    uint64_t epoch = atomic_load_explicit(&ctx->pool_epoch, memory_order_acquire);
    atomic_store_explicit(&psl->tx_wait_pool_epoch, epoch, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_wait_reason, (uint_fast32_t)reason,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->tx_wait_state, DMESH_TX_WAIT_ARMED,
                          memory_order_release);

    if (reason == DMESH_TX_WAIT_SHARED_POOL) {
        pool_waiter_set(ctx, port);
        uint64_t now = atomic_load_explicit(&ctx->pool_epoch, memory_order_acquire);
        uint64_t free_head = atomic_load_explicit(&ctx->block_free,
                                                  memory_order_acquire) & 0xFFFFFFFFu;
        if (now != epoch || free_head < (uint64_t)ctx->n_blocks)
            (void)tx_wait_make_ready(ctx, port);
    } else if (tx_wait_qp_retryable(ctx, psl)) {
        (void)tx_wait_make_ready(ctx, port);
    }
}

/* Per-conn TX region lifecycle + FIFO reclaim (defined with the TX functions below).
 * port_reset_tx is used by the PE (SERVER_PENDING) above its definition. */
static void port_reset_tx(struct dmesh_port_slot *psl);
static inline void tx_reclaim_ack(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq);

/* Arm a connection on its EQ ready list after inbox publication. The fence pairs
 * with the receive-side fence to preserve an empty-to-ready transition. */
static inline void arm_ready_after_push(struct dmesh_port_slot *psl, uint16_t dport) {
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) == DMESH_ROLE_SERVER_PENDING) return;
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    if (!eq) return;
    atomic_thread_fence(memory_order_seq_cst);
    /* Read first: an already-armed conn skips the store. */
    if (atomic_load_explicit(&psl->on_ready, memory_order_acquire) != 0)
        return;
    if (atomic_exchange_explicit(&psl->on_ready, 1u, memory_order_acq_rel) == 0) {
        ready_push(eq, dport);
        eq_notify(eq);
    }
}

/* Reverse notifications are ordered per destination QP. One delivery unit may span
 * several adjacent landing fragments with the same sequence. */
static inline int rx_seq_accept(struct dmesh_port_slot *psl,
                                const sw_descriptor_t *desc) {
    if (!psl->rx_seq_valid) {
        psl->rx_seq = desc->seq;
        psl->rx_next_pos = (uint32_t)desc->body_buf_slot + desc->body_len;
        psl->rx_seq_valid = 1;
        return 1;
    }
    uint16_t delta = (uint16_t)(desc->seq - psl->rx_seq);
    if (delta == 0) {
        if (desc->body_len == 0)
            return 0;
        if ((uint32_t)desc->body_buf_slot != psl->rx_next_pos)
            return 0;
        psl->rx_next_pos += desc->body_len;
        return 1;
    }
    if (delta >= 0x8000u)
        return 0;
    psl->rx_seq = desc->seq;
    psl->rx_next_pos = (uint32_t)desc->body_buf_slot + desc->body_len;
    return 1;
}

/* Demultiplex an inbound descriptor by local destination port. Live connections
 * receive it, free upstream ports enter the accept queue, and stale low ports
 * return the landing credit. Bodies remain in the shared RX mapping. */
static void rx_deliver_desc(dpumesh_ctx_t *ctx, const sw_descriptor_t *desc, int slot)
{
    uint16_t dport = desc->dst_port;
    struct dmesh_port_slot *psl = &ctx->ports[dport];
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);

    /* (1) A LIVE or PENDING conn takes it into its inbound ring. A SERVER_PENDING
     * conn — created by the PE at message-1 delivery and not yet accepted —
     * coalesces its pipelined messages there too. The ready list carries
     * ACCEPTED conns only; a pending conn is drained by dmesh_accept. */
    if (role != DMESH_ROLE_FREE) {
        if (!rx_seq_accept(psl, desc)) {
            rx_credit_return(ctx, slot);
            return;
        }
        int r = inbox_push(psl, desc);
        if (r == 0) {
            /* inbox full (app draining too slowly) → drop + reclaim the landing. */
            atomic_fetch_add_explicit(&ctx->st_rx_inbox_drops, 1, memory_order_relaxed);
            DOCA_LOG_ERR("RX deliver: conn %u inbox full, dropping seq=%u", dport, desc->seq);
            rx_credit_return(ctx, slot);
        } else {
            /* Arm the owning EQ's ready list. arm_ready_after_push re-reads the
             * role, so a slot promoted concurrently is armed and a still-pending
             * one is skipped. Single-producer (PE thread). */
            arm_ready_after_push(psl, dport);
        }
        return;
    }

    /* (2) FREE + dst_port is a DPU-assigned upstream id → a NEW server conn. Create
     * a PENDING port slot immediately so further messages for this uP coalesce into its
     * inbox), then push message 1 to the accept queue so dmesh_accept returns it
     * first. dst_port==BLANK never reaches a host (the DPU always resolves). */
    if (dport >= DMESH_UPORT_BASE) {
        pthread_mutex_lock(&ctx->port_lock);
        if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_FREE) {
            /* raced live between the initial load and the lock → coalesce to inbox */
            pthread_mutex_unlock(&ctx->port_lock);
            if (!rx_seq_accept(psl, desc)) {
                rx_credit_return(ctx, slot);
                return;
            }
            int r = inbox_push(psl, desc);
            if (r == 0) { atomic_fetch_add_explicit(&ctx->st_rx_inbox_drops, 1, memory_order_relaxed);
                          rx_credit_return(ctx, slot); }
            else arm_ready_after_push(psl, dport);
            return;
        }
        if (psl->nblk_owned > 0) {
            /* FREE but the prior conn's TX blocks are still draining: no conn
             * can be created on this slot yet, so drop and let the client
             * retry. */
            pthread_mutex_unlock(&ctx->port_lock);
            rx_credit_return(ctx, slot);
            return;
        }
        if (!psl->inbox) {
            psl->inbox = (sw_descriptor_t *)malloc((size_t)ctx->inbox_ring * sizeof(sw_descriptor_t));
            if (!psl->inbox) { pthread_mutex_unlock(&ctx->port_lock); rx_credit_return(ctx, slot); return; }
            psl->inbox_ring = (uint32_t)ctx->inbox_ring;
        } else {
            /* Return a prior owner's straggler deliveries (close/deliver race)
             * before the head/tail reset discards them — mirrors alloc_port. */
            sw_descriptor_t sd;
            while (inbox_pop(psl, &sd)) rx_credit_return(ctx, sd.body_buf_slot);
        }
        atomic_store_explicit(&psl->in_head, 0, memory_order_relaxed);
        atomic_store_explicit(&psl->in_tail, 0, memory_order_relaxed);
        psl->peer_pod  = desc->src_pod;
        psl->peer_port = desc->src_port;
        psl->rx_seq = desc->seq;
        psl->rx_next_pos = (uint32_t)desc->body_buf_slot + desc->body_len;
        psl->rx_seq_valid = 1;
        psl->user      = NULL;
        psl->eq        = NULL;   /* no owner until an EQ accepts it (dmesh_accept binds) */
        port_reset_tx(psl); /* fresh TX block-chain cursors */
        __atomic_store_n(&psl->role, DMESH_ROLE_SERVER_PENDING, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&ctx->port_lock);

        uint_fast32_t pos = atomic_load_explicit(&ctx->rx_enq, memory_order_relaxed);
        struct rxq_cell *c = &ctx->rx_ring[pos & (RX_QUEUE_SIZE - 1)];
        uint_fast32_t cseq = atomic_load_explicit(&c->seq, memory_order_acquire);
        if ((int_fast32_t)(cseq - pos) != 0) {
            __atomic_store_n(&psl->role, DMESH_ROLE_FREE, __ATOMIC_RELEASE);   /* roll back (no block borrowed) */
            atomic_fetch_add_explicit(&ctx->st_rx_accept_drops, 1, memory_order_relaxed);
            DOCA_LOG_ERR("RX deliver: accept queue full, dropping new conn uP=%u", dport);
            rx_credit_return(ctx, slot);
            return;
        }
        c->desc = *desc;
        atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
        atomic_store_explicit(&ctx->rx_enq, pos + 1, memory_order_relaxed);
        notify_all_eqs(ctx);       /* no owner yet → every EQ may accept it */
        return;
    }

    /* (3) FREE + a low (client) port → stale (conn closed) → reclaim the landing. */
    rx_credit_return(ctx, slot);
}

/* Deliver one completed reverse-DMA entry. */
static int process_rx_dma_entry(dpumesh_ctx_t *ctx, const struct dmesh_rev_done_entry *e) {
    uint32_t pos = e->pos, dma_len = e->length;
    if (!ctx->rx_dma_buffer || (size_t)pos + dma_len > ctx->rx_dma_buf_size) {
        DOCA_LOG_ERR("process_rx_dma_entry: bounds fail pos=%u len=%u buf=%zu",
                     pos, dma_len, ctx->rx_dma_buf_size);
        return -1;
    }
    /* Body must fit one slot; the DPA caps each reverse DMA at DPA_DMA_COPY_MAX
     * (= default slot_size), but guard so a non-default slot_size can't overrun. */
    if (dma_len > (uint32_t)ctx->slot_size) {
        DOCA_LOG_ERR("process_rx_dma_entry: len=%u exceeds slot_size=%d (seq=%u)",
                     dma_len, ctx->slot_size, e->seq);
        return -1;
    }

    /* Zero-copy: deliver the landing byte-offset `pos`; the consumer reads
     * rx_dma_buffer[pos] directly and returns the credit at rx_free. */
    int slot = (int)pos;

    sw_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.body_buf_slot = slot;
    desc.body_len      = dma_len;
    /* oriented tuple from the DPU completion (peer = src; dst is me) */
    desc.src_pod       = e->src_pod_id;
    desc.src_service   = e->src_service;
    desc.dst_service   = e->dst_service;
    desc.src_port      = e->src_port;
    desc.dst_port      = e->dst_port;
    desc.seq           = e->seq;
    desc.dst_pod       = (int16_t)ctx->pod_id;   /* delivered here → I am the dst pod */
    desc.valid         = 1;

    rx_deliver_desc(ctx, &desc, slot);
    return 0;
}

static int drain_rev_rings(dpumesh_ctx_t *ctx, uint32_t budget)
{
    if (budget == 0)
        return 0;

    uint32_t drained = 0;
    for (int r = 0; r < ctx->landing_stripes && drained < budget; r++) {
        struct rev_ring *ring = ctx->rev_rings[r];
        if (!ring)
            continue;
        uint64_t head = ring->head;
        while (drained < budget) {
            const struct dmesh_rev_ring_entry *entry =
                &ring->entries[head % ring->size];
            uint64_t expected = head + 1u;
            if (__atomic_load_n(&entry->publish_seq,
                                __ATOMIC_ACQUIRE) != expected)
                break;
            if (entry->kind == DMESH_REV_ENTRY_DONE) {
                if (process_rx_dma_entry(ctx, &entry->payload.done) != 0)
                    DOCA_LOG_WARN("Reverse ring %d rejected REV_DONE at ticket=%llu",
                                  r, (unsigned long long)head);
            } else if (entry->kind == DMESH_REV_ENTRY_TX_ACK) {
                tx_reclaim_ack(ctx, entry->payload.ack.port,
                               entry->payload.ack.seq);
            } else {
                DOCA_LOG_ERR("Reverse ring %d invalid kind=%u at ticket=%llu",
                             r, entry->kind, (unsigned long long)head);
            }
            head++;
            drained++;
        }
        if (head != ring->head) {
            ring->head = head;
            __atomic_store_n(&ring->ctrl->consumer_head, head,
                             __ATOMIC_RELEASE);
        }
    }
    return (int)drained;
}

static void init_config(dpumesh_ctx_t *ctx, const dpumesh_config_t *config, int service_id) {
    /* Programmatic values override the fixed slot-count and slot-size defaults. */
    ctx->num_slots = (config && config->num_slots > 0) ? config->num_slots
                                                       : DPUMESH_NUM_SLOTS_DEFAULT;
    ctx->slot_size = (config && config->slot_size > 0) ? config->slot_size
                                                       : DPUMESH_SLOT_SIZE_DEFAULT;

    /* K forward rings per pod. The host and DPU use the same value. */
    ctx->k_rings = DPUMESH_RINGS_PER_POD_DEFAULT;
    { const char *ke = getenv("DPUMESH_RINGS_PER_POD");
      if (ke && *ke) { int v = atoi(ke);
                       if (v >= 1 && v <= MAX_EU_PER_POD) ctx->k_rings = v; } }

    /* Fixed-size TX extents are allocated from the shared host TX buffer. */
    int bsz = TX_BLOCK_SIZE;
    if (bsz < ctx->slot_size) bsz = ctx->slot_size;
    size_t txbytes = (size_t)ctx->num_slots * (size_t)ctx->slot_size;
    if ((size_t)bsz > txbytes) bsz = (int)txbytes;
    ctx->block_size = bsz;
    ctx->n_blocks   = (int)(txbytes / (size_t)bsz);
    if (ctx->n_blocks < 1) ctx->n_blocks = 1;

    int mb = TX_BLOCKS_PER_CONN;
    if (mb > ctx->n_blocks) mb = ctx->n_blocks;
    ctx->blocks_per_conn = mb;

    /* A QP can fill its complete byte window with <=slot_size descriptors. Size the
     * reclaim FIFO to the next power of two so publication has no second admission
     * point. Keep it below half the 16-bit sequence space for unambiguous ACK order. */
    uint64_t need_su = ((uint64_t)ctx->block_size * (uint64_t)ctx->blocks_per_conn +
                        (uint64_t)ctx->slot_size - 1) / (uint64_t)ctx->slot_size;
    uint32_t sd = TX_SU_DEPTH_MIN;
    while ((uint64_t)sd < need_su && sd < TX_SU_DEPTH_MAX)
        sd <<= 1;
    if ((uint64_t)sd < need_su)
        sd = TX_SU_DEPTH_MAX;
    ctx->su_depth = sd;

    int hh = TX_RECYCLED_CUSHION;
    if (hh > mb) hh = mb;
    ctx->recycle_reserve = hh;

    /* The registry resolves $DPUMESH_SERVICE to the advertised service id.
     * SVC_NONE is client-only. The DPU assigns pod_id during registration. */
    ctx->service_id = service_id;

    ctx->pod_id = -1;   /* unassigned until the DPU replies */
    snprintf(ctx->worker_id, sizeof(ctx->worker_id), "svc%d", ctx->service_id);
}

static doca_error_t init_doca_device(dpumesh_ctx_t *ctx) {
    const char *pci_addr = getenv("DPUMESH_PCI_ADDR");
    if (!pci_addr) pci_addr = "94:00.0";

    doca_log_backend_create_standard();
    fprintf(stderr, "[dpumesh] Opening DOCA device at %s...\n", pci_addr);
    return open_doca_device_with_pci(pci_addr, NULL, &ctx->doca_objs.dev);
}

#define DPUMESH_POD_INIT_TIMEOUT_MS 30000
#define DPUMESH_CONTROL_RETRY_MS 100
#define DPUMESH_REG_CHALLENGE_TIMEOUT_MS 5000

static int init_elapsed_ms(const struct timespec *start, const struct timespec *now) {
    time_t sec = now->tv_sec - start->tv_sec;
    long nsec = now->tv_nsec - start->tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (int)(sec * 1000 + nsec / 1000000L);
}

static doca_error_t require_running_control_path(dpumesh_ctx_t *ctx) {
    if (ctx->doca_objs.cc_client == NULL)
        return DOCA_ERROR_NOT_CONNECTED;
    enum doca_ctx_states state;
    doca_error_t result = doca_ctx_get_state(
        doca_comch_client_as_ctx(ctx->doca_objs.cc_client), &state);
    if (result != DOCA_SUCCESS)
        return result;
    return state == DOCA_CTX_STATE_RUNNING ? DOCA_SUCCESS
                                           : DOCA_ERROR_CONNECTION_ABORTED;
}

static doca_error_t
configure_landing_geometry(dpumesh_ctx_t *ctx, int landing_stripes)
{
    int K = ctx->k_rings > 0 ? ctx->k_rings : 1;
    if (landing_stripes < 1 || landing_stripes > K ||
        K % landing_stripes != 0 ||
        ctx->rx_dma_buf_size < (size_t)landing_stripes * DPUMESH_SLOT_SIZE ||
        ctx->rx_dma_buf_size %
            ((size_t)landing_stripes * DPUMESH_SLOT_SIZE) != 0)
        return DOCA_ERROR_INVALID_VALUE;

    ctx->landing_stripes = landing_stripes;
    ctx->rx_credit_shards = K / landing_stripes;
    ctx->rx_region_size =
        ctx->rx_dma_buf_size / (size_t)landing_stripes;

    uint64_t rq_depth = ctx->rx_dma_buf_size / DPUMESH_SLOT_SIZE;
    uint64_t want = rq_depth / (uint32_t)landing_stripes;
    if (want > 65536u)
        want = 65536u;
    uint32_t ring = RX_INBOX_MIN_CAPACITY;
    while ((uint64_t)ring < want)
        ring <<= 1;
    ctx->inbox_ring = (int)ring;
    return DOCA_SUCCESS;
}

static doca_error_t wait_for_pod_init_result(dpumesh_ctx_t *ctx) {
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000 };
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct timespec last_register = start;

    for (;;) {
        int32_t init_result = __atomic_load_n(&ctx->doca_objs.pod_init_result,
                                              __ATOMIC_ACQUIRE);
        if (init_result == DMESH_POD_INIT_READY) {
            int32_t landing_stripes =
                __atomic_load_n(&ctx->doca_objs.landing_stripes,
                                __ATOMIC_RELAXED);
            doca_error_t geometry =
                configure_landing_geometry(ctx, landing_stripes);
            if (geometry != DOCA_SUCCESS) {
                DOCA_LOG_ERR("DPU returned invalid landing geometry: K=%d L=%d bytes=%zu",
                             ctx->k_rings, landing_stripes,
                             ctx->rx_dma_buf_size);
                return geometry;
            }
            return DOCA_SUCCESS;
        }
        if (init_result > DMESH_POD_INIT_READY) {
            DOCA_LOG_ERR("DPU rejected pod initialization: pod_id=%d result=%d",
                         ctx->pod_id, init_result);
            return DOCA_ERROR_INITIALIZATION;
        }

        if (ctx->doca_objs.pe != NULL)
            (void)doca_pe_progress(ctx->doca_objs.pe);
        doca_error_t control_result = require_running_control_path(ctx);
        if (control_result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Control path stopped while awaiting pod readiness: %s",
                         doca_error_get_name(control_result));
            return control_result;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (init_elapsed_ms(&last_register, &now) >= DPUMESH_CONTROL_RETRY_MS) {
            doca_error_t retry = client_send_msg(&ctx->doca_objs,
                                                  (const char *)&ctx->reg_msg,
                                                  sizeof(ctx->reg_msg));
            last_register = now;
            if (retry != DOCA_SUCCESS && retry != DOCA_ERROR_AGAIN) {
                DOCA_LOG_ERR("REGISTER retry failed while awaiting pod readiness: %s",
                             doca_error_get_name(retry));
                return retry;
            }
        }
        if (init_elapsed_ms(&start, &now) >= DPUMESH_POD_INIT_TIMEOUT_MS) {
            DOCA_LOG_ERR("Timed out after %d ms waiting for DPU pod readiness "
                         "(pod_id=%d, rings=%d)",
                         DPUMESH_POD_INIT_TIMEOUT_MS, ctx->pod_id, ctx->k_rings);
            return DOCA_ERROR_TIME_OUT;
        }
        nanosleep(&pause, NULL);
    }
}

static doca_error_t init_control_path(dpumesh_ctx_t *ctx) {
    doca_error_t result;

    /* Initialize challenge publication before connecting: the DPU may submit
     * REG_CHALLENGE as soon as the Comch connection callback runs. */
    memset(ctx->doca_objs.registration_challenge, 0,
           sizeof(ctx->doca_objs.registration_challenge));
    __atomic_store_n(&ctx->doca_objs.registration_trusted_required, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->doca_objs.registration_challenge_ready, 0,
                     __ATOMIC_RELEASE);

    fprintf(stderr, "[dpumesh] Connecting comch client...\n");
    result = init_comch_ctrl_path_client("DPUMesh", &ctx->doca_objs);
    if (result != DOCA_SUCCESS) return result;

    /* Register with pod_id = -1: the DPU allocates our pod_id and replies with
     * DMESH_MSG_POD_ASSIGNED. Block until it arrives, progressing the PE by hand
     * (the PE thread isn't started until the end of dpumesh_init). */
    __atomic_store_n(&ctx->doca_objs.assigned_pod_id, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->doca_objs.pod_init_result, DMESH_POD_INIT_PENDING,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->doca_objs.landing_stripes, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->doca_objs.pod_quiesced, 0, __ATOMIC_RELEASE);
    const char *attest_socket = getenv("DPUMESH_ATTEST_SOCKET");
    if (attest_socket == NULL || *attest_socket == '\0')
        attest_socket = DMESH_DEFAULT_ATTEST_SOCKET;

    /* The process relays the connection-bound nonce; only the node agent reads
     * SO_PEERCRED and holds the signing key. */
    struct timespec challenge_start, challenge_now;
    struct timespec challenge_pause = { .tv_sec = 0, .tv_nsec = 10000 };
    clock_gettime(CLOCK_MONOTONIC, &challenge_start);
    while (!__atomic_load_n(&ctx->doca_objs.registration_challenge_ready,
                            __ATOMIC_ACQUIRE)) {
        if (ctx->doca_objs.pe)
            (void)doca_pe_progress(ctx->doca_objs.pe);
        clock_gettime(CLOCK_MONOTONIC, &challenge_now);
        if (init_elapsed_ms(&challenge_start, &challenge_now) >=
            DPUMESH_REG_CHALLENGE_TIMEOUT_MS) {
            DOCA_LOG_ERR("Timed out awaiting trusted-registration challenge");
            return DOCA_ERROR_TIME_OUT;
        }
        nanosleep(&challenge_pause, NULL);
    }
    if (!__atomic_load_n(&ctx->doca_objs.registration_trusted_required,
                         __ATOMIC_RELAXED)) {
        DOCA_LOG_ERR("DPU challenge did not require trusted registration");
        return DOCA_ERROR_BAD_STATE;
    }

    struct dmesh_workload_grant_msg grant;
    char attest_error[256] = {0};
    if (dmesh_attest_request_grant(
            attest_socket, ctx->service_id,
            ctx->doca_objs.registration_challenge, &grant,
            attest_error, sizeof(attest_error)) != 0) {
        DOCA_LOG_ERR("Trusted node agent rejected registration: %s",
                     attest_error);
        return DOCA_ERROR_INITIALIZATION;
    }
    result = client_send_msg(&ctx->doca_objs, (const char *)&grant,
                             sizeof(grant));
    memset(&grant, 0, sizeof(grant));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("WORKLOAD_GRANT send failed: %s",
                     doca_error_get_name(result));
        return result;
    }

    ctx->reg_msg.type = DMESH_MSG_POD_REGISTER;
    ctx->reg_msg.pod_id = -1;                    /* DPU assigns this node's address */
    ctx->reg_msg.service_id = ctx->service_id;   /* DPU: pods[our slot].service_id = this (the LB set is derived from it) */
    result = client_send_msg(&ctx->doca_objs, (const char *)&ctx->reg_msg, sizeof(ctx->reg_msg));
    if (result != DOCA_SUCCESS) return result;

    /* Wait for phase 1 (address assignment). A registration failure may arrive
     * as POD_INIT_RESULT before POD_ASSIGNED, so check both atomics. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000 };   /* 10 us */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct timespec last_register = start;
    int32_t assigned = -1;
    for (;;) {
        assigned = __atomic_load_n(&ctx->doca_objs.assigned_pod_id, __ATOMIC_ACQUIRE);
        if (assigned >= 0) break;
        int32_t init_result = __atomic_load_n(&ctx->doca_objs.pod_init_result,
                                              __ATOMIC_ACQUIRE);
        if (init_result == DMESH_POD_INIT_REGISTER_FAILED) {
            DOCA_LOG_ERR("DPU rejected pod registration (service_id=%d)", ctx->service_id);
            return DOCA_ERROR_INITIALIZATION;
        }
        if (ctx->doca_objs.pe) (void)doca_pe_progress(ctx->doca_objs.pe);
        doca_error_t control_result = require_running_control_path(ctx);
        if (control_result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Control path stopped while awaiting pod assignment: %s",
                         doca_error_get_name(control_result));
            return control_result;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (init_elapsed_ms(&last_register, &now) >= DPUMESH_CONTROL_RETRY_MS) {
            doca_error_t retry = client_send_msg(&ctx->doca_objs,
                                                  (const char *)&ctx->reg_msg,
                                                  sizeof(ctx->reg_msg));
            last_register = now;
            if (retry != DOCA_SUCCESS && retry != DOCA_ERROR_AGAIN) {
                DOCA_LOG_ERR("REGISTER retry failed while awaiting pod assignment: %s",
                             doca_error_get_name(retry));
                return retry;
            }
        }
        if (init_elapsed_ms(&start, &now) >= DPUMESH_POD_INIT_TIMEOUT_MS)
            break;
        nanosleep(&ts, NULL);
    }
    if (assigned < 0) {
        DOCA_LOG_ERR("Timed out after %d ms waiting for DPU pod_id assignment "
                     "(service_id=%d)", DPUMESH_POD_INIT_TIMEOUT_MS, ctx->service_id);
        return DOCA_ERROR_TIME_OUT;
    }
    ctx->pod_id = assigned;
    int L = __atomic_load_n(&ctx->doca_objs.landing_stripes, __ATOMIC_RELAXED);
    int K = ctx->k_rings > 0 ? ctx->k_rings : 1;
    if (L < 1 || L > K || K % L != 0) {
        DOCA_LOG_ERR("DPU returned invalid landing stripes: K=%d L=%d", K, L);
        return DOCA_ERROR_INVALID_VALUE;
    }
    ctx->landing_stripes = L;
    ctx->rx_credit_shards = K / L;
    return DOCA_SUCCESS;
}

static doca_error_t init_datapath(dpumesh_ctx_t *ctx) {
    doca_error_t result;

    /* Comch carries control messages. DMA rings carry data and completions. */

    /* K forward descriptor rings, exported in order as DMA_RING before
     * DMA_BUFFER, so the DPU's setup trigger sees all K before pairing. */
    for (int j = 0; j < ctx->k_rings; j++) {
        result = setup_dma_ring(&ctx->doca_objs, DMA_RING_SIZE, &ctx->dma_rings[j]);
        if (result != DOCA_SUCCESS) return result;
    }

    size_t buf_size = (size_t)ctx->num_slots * ctx->slot_size;
    result = alloc_buffer_and_set_mmap(&ctx->doca_objs.local_mmap,
                                       ctx->doca_objs.dev,
                                       &ctx->doca_objs.dma_buffer,
                                       buf_size,
                                       DOCA_ACCESS_FLAG_PCI_READ_WRITE);
    if (result != DOCA_SUCCESS) return result;
    ctx->dma_buffer = ctx->doca_objs.dma_buffer;

    result = export_mmap_to_remote(&ctx->doca_objs, ctx->doca_objs.local_mmap,
                                   ctx->doca_objs.dma_buffer, buf_size,
                                   DMA_BUFFER);
    if (result != DOCA_SUCCESS) return result;

    result = doca_mmap_dev_get_dpa_handle(ctx->doca_objs.local_mmap, ctx->doca_objs.dev, &ctx->dpa_mmap_handle);
    if (result != DOCA_SUCCESS) return result;

    /* Allocate the Host RX DMA buffer. */
    ctx->rx_dma_buf_size = buf_size;
    ctx->rx_region_size = 0;
    result = alloc_buffer_and_set_mmap(&ctx->rx_dma_mmap,
                                       ctx->doca_objs.dev,
                                       &ctx->rx_dma_buffer,
                                       ctx->rx_dma_buf_size,
                                       DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to allocate Host RX DMA buffer: %s", doca_err_str(result));
        return result;
    }

    /* Export Host RX buffer to DPU so DPA can get mmap handle for reverse DMA */
    result = export_mmap_to_remote(&ctx->doca_objs, ctx->rx_dma_mmap,
                                   ctx->rx_dma_buffer, ctx->rx_dma_buf_size,
                                   DMA_HOST_RX_BUFFER);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to export Host RX buffer to DPU: %s", doca_err_str(result));
        return result;
    }

    for (int j = 0; j < ctx->landing_stripes; j++) {
        result = setup_rev_ring(&ctx->doca_objs, &ctx->rev_rings[j]);
        if (result != DOCA_SUCCESS)
            return result;
    }

    /* A successful send submission is not remote acceptance. The DPU returns
     * READY only after it has imported all three mapping classes and installed
     * every DPA ring (or a terminal failure explaining that this channel cannot
     * be used). */
    return wait_for_pod_init_result(ctx);
}

int dpumesh_init(dpumesh_ctx_t **out, int service_id,
                 const dpumesh_config_t *config) {
    if (out == NULL) { errno = EINVAL; return -1; }
    *out = NULL;
    dpumesh_ctx_t *ctx = (dpumesh_ctx_t *)calloc(1, sizeof(dpumesh_ctx_t));
    if (!ctx) { errno = ENOMEM; return -1; }
    int prc = pthread_mutex_init(&ctx->eq_lock, NULL);
    if (prc != 0) { errno = prc; goto fail; }
    ctx->eq_lock_initialized = 1;

    init_config(ctx, config, service_id);

    /* These limits are data-plane ABI, not tuning preferences. The DPA copies at
     * most 8 KiB per descriptor, TX byte offsets mirror into a fixed-size DPU
     * staging buffer, and reverse credits partition the RX buffer evenly over K. */
    size_t configured_bytes = (size_t)ctx->num_slots * (size_t)ctx->slot_size;
    size_t rx_quantum = (size_t)ctx->k_rings * DPUMESH_SLOT_SIZE;
    if (ctx->slot_size <= 0 || ctx->slot_size > DPUMESH_SLOT_SIZE ||
        configured_bytes == 0 || configured_bytes > DPU_BUFFER_SIZE ||
        rx_quantum == 0 || configured_bytes < rx_quantum ||
        configured_bytes % rx_quantum != 0) {
        DOCA_LOG_ERR("Invalid DPUmesh config: slots=%d slot_size=%d K=%d bytes=%zu "
                     "(slot<=%d bytes<=%d and bytes%%(K*%d)==0 required)",
                     ctx->num_slots, ctx->slot_size, ctx->k_rings, configured_bytes,
                     DPUMESH_SLOT_SIZE, DPU_BUFFER_SIZE, DPUMESH_SLOT_SIZE);
        errno = EINVAL;
        goto fail;
    }

    doca_error_t init_result = init_doca_device(ctx);
    if (init_result != DOCA_SUCCESS) { errno = EIO; goto fail; }
    init_result = init_control_path(ctx);
    if (init_result != DOCA_SUCCESS) {
        errno = (init_result == DOCA_ERROR_TIME_OUT) ? ETIMEDOUT : EIO;
        goto fail;
    }
    init_result = init_datapath(ctx);
    if (init_result != DOCA_SUCCESS) {
        errno = (init_result == DOCA_ERROR_TIME_OUT) ? ETIMEDOUT : EIO;
        goto fail;
    }

    /* Shared lock-free Treiber block pool. block_next[i] = i+1 threads the free-list;
     * block_free head starts at index 0 (all n_blocks free, tag 0); the last block links
     * to n_blocks (the empty sentinel). Per-conn send-unit FIFOs (su_seq/su_end/su_done) are
     * lazily malloc'd per port slot (kept for the slot's life). */
    ctx->block_next = (uint32_t *)malloc((size_t)ctx->n_blocks * sizeof(uint32_t));
    if (!ctx->block_next) { errno = ENOMEM; goto fail; }
    for (int i = 0; i < ctx->n_blocks; i++)
        ctx->block_next[i] = (uint32_t)(i + 1);          /* last -> n_blocks (empty sentinel) */
    atomic_init(&ctx->block_free, (uint_fast64_t)0);     /* tag 0, head index 0 */
    atomic_init(&ctx->pool_epoch, (uint_fast64_t)0);
    atomic_init(&ctx->pool_waiter_count, (uint_fast32_t)0);
    atomic_init(&ctx->pool_wait_cursor, (uint_fast32_t)0);
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&ctx->pool_waiters[i], (uint_fast64_t)0);
    prc = pthread_mutex_init(&ctx->block_lock, NULL);
    if (prc != 0) { errno = prc; goto fail; }
    ctx->block_lock_initialized = 1;

    /* Lock-free SPMC RX ring: seq[i] = i (cell i first writable at enq
     * position i), enq = deq = 0. */
    ctx->rx_ring = (struct rxq_cell *)malloc((size_t)RX_QUEUE_SIZE * sizeof(struct rxq_cell));
    if (!ctx->rx_ring) { errno = ENOMEM; goto fail; }
    for (uint32_t i = 0; i < RX_QUEUE_SIZE; i++)
        atomic_init(&ctx->rx_ring[i].seq, (uint_fast32_t)i);
    atomic_init(&ctx->rx_enq, (uint_fast32_t)0);
    atomic_init(&ctx->rx_deq, (uint_fast32_t)0);

    /* Endpoint port table + allocator (oriented-tuple demux). calloc → every slot
     * role=FREE, nblk_owned=0 (holds no TX blocks), su NULL, cursors 0. pblk[]
     * must start -1 (0 is a valid block id); a conn takes its first block on its
     * first write. */
    ctx->ports = (struct dmesh_port_slot *)calloc(DMESH_PORT_SPACE, sizeof(struct dmesh_port_slot));
    if (!ctx->ports) { errno = ENOMEM; goto fail; }
    for (uint32_t p = 0; p < DMESH_PORT_SPACE; p++) {
        atomic_init(&ctx->ports[p].tx_c, 0);
        atomic_init(&ctx->ports[p].tx_s, 0);
        atomic_init(&ctx->ports[p].tx_error, 0);
        atomic_init(&ctx->ports[p].tx_wait_state,
                    (uint_fast32_t)DMESH_TX_WAIT_IDLE);
        atomic_init(&ctx->ports[p].tx_wait_reason,
                    (uint_fast32_t)DMESH_TX_WAIT_NONE);
        atomic_init(&ctx->ports[p].tx_wait_tail_blk, (uint_fast64_t)0);
        atomic_init(&ctx->ports[p].tx_wait_tx_w, (uint_fast64_t)0);
        atomic_init(&ctx->ports[p].tx_wait_pool_epoch, (uint_fast64_t)0);
        for (int b = 0; b < TX_BLOCKS_PER_CONN; b++) {
            atomic_init(&ctx->ports[p].blk_used[b], 0);
            ctx->ports[p].pblk[b] = -1;
        }
    }
    prc = pthread_mutex_init(&ctx->port_lock, NULL);
    if (prc != 0) { errno = prc; goto fail; }
    ctx->port_lock_initialized = 1;
    ctx->next_port = 1;
    ctx->port_span = DMESH_PORT_SPAN_MIN;

    if (tx_timer_start(ctx) != 0)
        goto fail;

    ctx->pe_running = 1;
    prc = pthread_create(&ctx->pe_tid, NULL, pe_progress_fn, ctx);
    if (prc != 0) {
        ctx->pe_running = 0;   /* cleanup must not join a never-created thread */
        errno = prc;
        goto fail;
    }

    DOCA_LOG_INFO("DPUmesh DOCA initialized: worker=%s pod_id=%d "
                  "inbox_ring=%d K=%d L=%d",
                  ctx->worker_id, ctx->pod_id, ctx->inbox_ring,
                  ctx->k_rings, ctx->landing_stripes);

    *out = ctx;
    return 0;

fail:
    {
        int saved_errno = errno != 0 ? errno : EIO;
        cleanup_ctx(ctx);
        errno = saved_errno;
    }
    return -1;
}

#define DPUMESH_POD_CLEANUP_TIMEOUT_MS 5000

/* Graceful remote-resource barrier. The normal PE progress thread stays alive
 * while waiting: it owns Comch notification delivery for the lifetime of a
 * running channel. If initialization failed before that thread was started, this
 * function progresses the PE synchronously instead. Every exported mmap stays
 * alive until the DPU confirms that DPA rings, DPA producer DMAs, ARM SG-DMAs
 * and imported handles are gone. */
static int request_remote_pod_quiesce(dpumesh_ctx_t *ctx) {
    struct objects *objs = &ctx->doca_objs;
    int32_t pod_id = __atomic_load_n(&objs->assigned_pod_id, __ATOMIC_ACQUIRE);
    if (objs->cc_client == NULL || objs->connection == NULL || pod_id < 0)
        return 0;

    __atomic_store_n(&objs->pod_quiesced, 0, __ATOMIC_RELEASE);
    struct dmesh_pod_unregister_msg msg = {
        .type = DMESH_MSG_POD_UNREGISTER,
        .pod_id = pod_id,
    };
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000 };
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    struct timespec last_send = {0};
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (last_send.tv_sec == 0 ||
            init_elapsed_ms(&last_send, &now) >= DPUMESH_CONTROL_RETRY_MS) {
            doca_error_t send_result = client_send_msg(
                objs, (const char *)&msg, sizeof(msg));
            last_send = now;
            if (send_result != DOCA_SUCCESS && send_result != DOCA_ERROR_AGAIN)
                DOCA_LOG_WARN("POD_UNREGISTER retry failed for pod_id=%d: %s",
                              pod_id, doca_error_get_name(send_result));
        }
        if (!ctx->pe_running && objs->pe != NULL)
            (void)doca_pe_progress(objs->pe);
        if (__atomic_load_n(&objs->pod_quiesced, __ATOMIC_ACQUIRE)) {
            return 0;
        }
        doca_error_t result = require_running_control_path(ctx);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_WARN("Control path stopped while awaiting POD_QUIESCED: %s",
                          doca_error_get_name(result));
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (init_elapsed_ms(&start, &now) >= DPUMESH_POD_CLEANUP_TIMEOUT_MS) {
            DOCA_LOG_WARN("Timed out after %d ms waiting for POD_QUIESCED "
                          "(pod_id=%d); falling back to disconnect cleanup",
                          DPUMESH_POD_CLEANUP_TIMEOUT_MS, pod_id);
            return -1;
        }
        nanosleep(&pause, NULL);
    }
}

static void cleanup_ctx(dpumesh_ctx_t *ctx) {
    if (!ctx) return;

    /* No publisher may race remote quiesce or destruction of per-port state. */
    tx_timer_stop(ctx);

    /* QPs/channels are already gone, so no new application traffic can appear.
     * Receive the remote teardown ACK on the same notification/progress thread
     * that has owned the Comch PE throughout the channel lifetime. */
    (void)request_remote_pod_quiesce(ctx);

    if (ctx->pe_running) {
        ctx->pe_running = 0;
        pthread_join(ctx->pe_tid, NULL);
    }

    /* PE thread joined → no more eq_notify() writers. dmesh_destroy_channel
     * refuses to reach here with a live EQ, so this loop normally reaps
     * nothing. */
    for (int i = 0; i < ctx->n_eqs; i++) {
        struct dmesh_eq *eq = ctx->eqs[i];
        if (!eq) continue;
        if (eq->notify_efd >= 0) close(eq->notify_efd);
        ctx->eqs[i] = NULL;
        free(eq->accept_spare);
        free(eq);
    }
    if (ctx->eq_lock_initialized) {
        pthread_mutex_destroy(&ctx->eq_lock);
        ctx->eq_lock_initialized = 0;
    }

    /* Disconnect first. Reaching Comch IDLE causes the DPU to unpublish this pod
     * before any host-exported address is released. cleanup_objects is delayed
     * until after mmap teardown so PE/device remain valid for doca_mmap_destroy. */
    cleanup_comch_object(&ctx->doca_objs);

    /* Per-conn TX block chains need no drain at teardown — in-flight bytes die with
     * the ctx. The PE thread is joined, so no reserve/reclaim can race this. */
    if (ctx->block_next) { free(ctx->block_next); ctx->block_next = NULL; }
    if (ctx->block_lock_initialized) {
        pthread_mutex_destroy(&ctx->block_lock);
        ctx->block_lock_initialized = 0;
    }
    if (ctx->ports) {
        for (uint32_t p = 0; p < DMESH_PORT_SPACE; p++) {
            if (ctx->ports[p].inbox) {
                free(ctx->ports[p].inbox);
                ctx->ports[p].inbox = NULL;
            }
            if (ctx->ports[p].su_seq) { free(ctx->ports[p].su_seq); ctx->ports[p].su_seq = NULL; }
            if (ctx->ports[p].su_end) { free(ctx->ports[p].su_end); ctx->ports[p].su_end = NULL; }
            if (ctx->ports[p].su_done) { free(ctx->ports[p].su_done); ctx->ports[p].su_done = NULL; }
        }
    }

    if (ctx->tx_timer_cv_initialized) {
        pthread_cond_destroy(&ctx->tx_timer_cv);
        ctx->tx_timer_cv_initialized = 0;
    }
    if (ctx->tx_timer_lock_initialized) {
        pthread_mutex_destroy(&ctx->tx_timer_lock);
        ctx->tx_timer_lock_initialized = 0;
    }

    /* Destroy host mmaps while the DOCA device remains open. Failed mmap
     * destruction retains the backing memory. */
    if (ctx->rx_dma_mmap && ctx->rx_dma_buffer) {
        if (destroy_mmap_and_free_buffer(ctx->rx_dma_mmap,
                                         ctx->rx_dma_buffer) == DOCA_SUCCESS) {
            ctx->rx_dma_mmap = NULL;
            ctx->rx_dma_buffer = NULL;
        }
    }

    for (int j = 0; j < MAX_EU_PER_POD; j++) {
        struct rev_ring *rev = ctx->rev_rings[j];
        if (rev) {
            if (rev->mmap && rev->entries &&
                destroy_mmap_and_free_buffer(rev->mmap,
                                             rev->entries) == DOCA_SUCCESS) {
                rev->mmap = NULL;
                rev->entries = NULL;
            }
            free(rev);
            ctx->rev_rings[j] = NULL;
        }
        struct dma_ring *ring = ctx->dma_rings[j];
        if (!ring) continue;
        if (ring->mmap && ring->descs &&
            destroy_mmap_and_free_buffer(ring->mmap, ring->descs) == DOCA_SUCCESS) {
            ring->mmap = NULL;
            ring->descs = NULL;
        }
        free(ring);
        ctx->dma_rings[j] = NULL;
    }

    if (ctx->doca_objs.local_mmap && ctx->doca_objs.dma_buffer &&
        destroy_mmap_and_free_buffer(ctx->doca_objs.local_mmap,
                                     ctx->doca_objs.dma_buffer) == DOCA_SUCCESS) {
        ctx->doca_objs.local_mmap = NULL;
        ctx->doca_objs.dma_buffer = NULL;
        ctx->dma_buffer = NULL;
    }

    cleanup_objects(&ctx->doca_objs);

    if (ctx->rx_ring) { free(ctx->rx_ring); ctx->rx_ring = NULL; }
    if (ctx->ports) { free(ctx->ports); ctx->ports = NULL; }
    if (ctx->port_lock_initialized) {
        pthread_mutex_destroy(&ctx->port_lock);
        ctx->port_lock_initialized = 0;
    }

    free(ctx);
}

void dpumesh_destroy(dpumesh_ctx_t *ctx) {
    if (!ctx) return;
    {   /* RX-drop summary (should be all zero — inbox sized to the reverse-credit budget) */
        unsigned long long idr = atomic_load_explicit(&ctx->st_rx_inbox_drops, memory_order_relaxed);
        unsigned long long adr = atomic_load_explicit(&ctx->st_rx_accept_drops, memory_order_relaxed);
        unsigned long long cdr = atomic_load_explicit(&ctx->st_rx_credit_drops, memory_order_relaxed);
        if (idr || adr || cdr)
            DOCA_LOG_WARN("RX drops at teardown: inbox_full=%llu accept_full=%llu bad_offset=%llu (MESSAGES LOST)",
                          idr, adr, cdr);
    }
    cleanup_ctx(ctx);
}

/* ====================================================================
 * TX functions
 * ==================================================================== */

/* ---- Shared lock-free Treiber block pool (grab on grow / return on shrink+close) ---- */
/* Pop a free block id (ABA-safe: block_free = tag<<32 | head; head==n_blocks = empty).
 * -1 if the pool is empty (caller backs off + retries). MPMC (conn owners + the PE). */
static int32_t block_pool_grab(dpumesh_ctx_t *ctx) {
    uint_fast64_t old = atomic_load_explicit(&ctx->block_free, memory_order_acquire);
    for (;;) {
        uint32_t head = (uint32_t)(old & 0xFFFFFFFFu);
        if (head >= (uint32_t)ctx->n_blocks) return -1;            /* empty */
        uint_fast64_t nv = (((old >> 32) + 1) << 32) | (uint_fast64_t)ctx->block_next[head];
        if (atomic_compare_exchange_weak_explicit(&ctx->block_free, &old, nv,
                memory_order_acquire, memory_order_acquire)) {
            atomic_fetch_add_explicit(&ctx->st_pool_grabs, 1, memory_order_relaxed);
            return (int32_t)head;
        }
    }
}
static void block_pool_return(dpumesh_ctx_t *ctx, int32_t id) {
    if (id < 0 || id >= ctx->n_blocks) return;
    atomic_fetch_add_explicit(&ctx->st_pool_returns, 1, memory_order_relaxed);
    uint_fast64_t old = atomic_load_explicit(&ctx->block_free, memory_order_relaxed);
    for (;;) {
        ctx->block_next[id] = (uint32_t)(old & 0xFFFFFFFFu);
        uint_fast64_t nv = (((old >> 32) + 1) << 32) | (uint_fast64_t)(uint32_t)id;
        if (atomic_compare_exchange_weak_explicit(&ctx->block_free, &old, nv,
                memory_order_release, memory_order_relaxed))
            break;
    }
    atomic_fetch_add_explicit(&ctx->pool_epoch, 1, memory_order_release);
    /* One returned block is one unit of capacity. Claim at most one valid waiter;
     * stale bits are discarded until either a live ARMED waiter is found or empty. */
    uint16_t port;
    while (pool_waiter_claim(ctx, &port))
        if (tx_wait_make_ready(ctx, port)) break;
}

/* Reset a slot's TX block chain for a fresh conn: cursors 0, no blocks held. The
 * first block is taken on the first tx_reserve. Existing su_seq/su_end/su_done
 * arrays are kept and reused. */
static void port_reset_tx(struct dmesh_port_slot *psl) {
    /* The previous conn's armed bit is cleared by dpumesh_free_port, which also
     * unbinds the EQ that owns it. */
    atomic_store_explicit(&psl->tx_deadline_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_error, 0, memory_order_relaxed);
    psl->tx_w = 0;
    atomic_store_explicit(&psl->tx_s, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_c, 0, memory_order_relaxed);
    psl->resv_len = 0;
    psl->resv_moff = 0;
    atomic_store_explicit(&psl->tx_f, 0, memory_order_relaxed);
    psl->tail_blk      = 0;
    psl->head_blk_next = 0;
    psl->nblk_owned    = 0;
    psl->nrec          = 0;
    for (int b = 0; b < TX_BLOCKS_PER_CONN; b++) {
        psl->pblk[b] = -1;
        atomic_store_explicit(&psl->blk_used[b], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&psl->su_head, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->su_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_wait_state, DMESH_TX_WAIT_IDLE,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->tx_wait_reason, DMESH_TX_WAIT_NONE,
                          memory_order_relaxed);
}

/* OWNER-only (live conn): recycle drained tail blocks into recyc, compact a
 * fully drained conn back to logical 0, and return surplus recycled blocks to
 * the pool. Called from reserve before the grow decision, so steady sliding
 * reuses drained blocks and only a net demand change grabs or returns. */
static void tx_refresh_blocks(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl) {
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t block_slots = (uint64_t)ctx->blocks_per_conn;
    uint64_t f = atomic_load_explicit(&psl->tx_f, memory_order_acquire);
    uint64_t f_blk = f / bs;
    while (psl->tail_blk < f_blk) {                        /* logical block fully freed → recycle */
        int s = (int)(psl->tail_blk % block_slots);
        if (psl->pblk[s] >= 0) { psl->recyc[psl->nrec++] = psl->pblk[s]; psl->pblk[s] = -1; }
        psl->tail_blk++;
    }
    /* Full-drain compaction: nothing live AND nothing in flight → recycle the head block
     * too and reset the chain to logical 0 (safe: su empty ⇒ no concurrent tx_f writer). */
    if (f == psl->tx_w &&
        atomic_load_explicit(&psl->su_head, memory_order_relaxed) ==
        atomic_load_explicit(&psl->su_tail, memory_order_acquire)) {
        int s = (int)(f_blk % block_slots);
        if (psl->pblk[s] >= 0) { psl->recyc[psl->nrec++] = psl->pblk[s]; psl->pblk[s] = -1; }
        psl->tx_w = 0;
        atomic_store_explicit(&psl->tx_s, 0, memory_order_relaxed);
        atomic_store_explicit(&psl->tx_c, 0, memory_order_relaxed);
        atomic_store_explicit(&psl->tx_f, 0, memory_order_relaxed);
        psl->tail_blk = 0;
        psl->head_blk_next = 0;
    }
    while (psl->nrec > ctx->recycle_reserve) {
        block_pool_return(ctx, psl->recyc[--psl->nrec]);
        psl->nblk_owned--;
    }
}

/* Return a CLOSED conn's remaining blocks once fully drained (tx_f == tx_w). Called by
 * free_port (owner, after publishing role=FREE) and tx_reclaim_ack (PE, on the last ACK).
 * role==FREE is loaded with acquire first, so the owner's final writes are visible;
 * the block_lock and the nblk_owned>0 recheck make exactly one caller return them.
 * Until then the port stays FREE-but-draining and the alloc paths skip it. */
static void try_return_blocks(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl) {
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_FREE) return;  /* live */
    if (psl->nblk_owned <= 0) return;                                              /* none/returned */
    if (psl->tx_w != atomic_load_explicit(&psl->tx_f, memory_order_acquire)) return; /* not drained */
    pthread_mutex_lock(&ctx->block_lock);
    if (psl->nblk_owned > 0 &&
        psl->tx_w == atomic_load_explicit(&psl->tx_f, memory_order_acquire)) {
        uint64_t block_slots = (uint64_t)ctx->blocks_per_conn;
        for (uint64_t k = psl->tail_blk; k < psl->head_blk_next; k++) {  /* all assigned blocks */
            int s = (int)(k % block_slots);
            if (psl->pblk[s] >= 0) { block_pool_return(ctx, psl->pblk[s]); psl->pblk[s] = -1; }
        }
        while (psl->nrec > 0) block_pool_return(ctx, psl->recyc[--psl->nrec]);
        psl->nblk_owned = 0;
    }
    pthread_mutex_unlock(&ctx->block_lock);
}

/* ---- Per-conn TX BYTE-RING over the block chain: reserve → commit → send → (ACK) free ---- */

/* Reserve one contiguous message in the connection's TX block chain. The owner
 * thread receives EAGAIN for capacity pressure or EINVAL for invalid state. */
uint8_t *dpumesh_tx_reserve(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len) {
    if (port == 0) { errno = EINVAL; return NULL; }
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t block_slots = (uint64_t)ctx->blocks_per_conn;
    if (len == 0 || (uint64_t)len > bs) { errno = EINVAL; return NULL; }  /* must fit a block */
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
    if (role != DMESH_ROLE_CLIENT && role != DMESH_ROLE_SERVER) {
        errno = EINVAL;
        return NULL;
    }
    int error_number = atomic_load_explicit(&psl->tx_error,
                                            memory_order_acquire);
    if (error_number != 0) { errno = error_number; return NULL; }
    if (psl->resv_len != 0) { errno = EINVAL; return NULL; } /* one outstanding alloc/QP */
    if (!psl->su_seq) {                                    /* lazy per-slot send-unit FIFO */
        uint16_t *seq = (uint16_t *)malloc((size_t)ctx->su_depth * sizeof(uint16_t));
        uint64_t *end = (uint64_t *)malloc((size_t)ctx->su_depth * sizeof(uint64_t));
        uint8_t *done = (uint8_t *)calloc((size_t)ctx->su_depth, sizeof(uint8_t));
        if (!seq || !end || !done) {
            free(seq);
            free(end);
            free(done);
            errno = ENOMEM;
            return NULL;
        }
        psl->su_seq = seq;
        psl->su_end = end;
        psl->su_done = done;
    }

    /* Probe the block window before mutating tx_w: on EAGAIN the conn's write head
     * is exactly where it was, so the caller's retry is a clean no-op. */
    uint64_t k   = psl->tx_w / bs;
    uint32_t off = (uint32_t)(psl->tx_w % bs);
    int      pad = ((uint64_t)off + len > bs);             /* won't fit → needs a fresh block */
    uint64_t need_k = pad ? k + 1 : k;
    int32_t reserved_phys = -1;
    if (psl->head_blk_next <= need_k) {
        /* Recycling reads cursors the PE thread owns. It may rewind the
         * chain, so re-probe after. */
        tx_refresh_blocks(ctx, psl);
        k      = psl->tx_w / bs;
        off    = (uint32_t)(psl->tx_w % bs);
        pad    = ((uint64_t)off + len > bs);
        need_k = pad ? k + 1 : k;
    }
    if (psl->head_blk_next <= need_k) {                    /* a new block must be backed */
        uint64_t b = psl->head_blk_next;
        /* A logical block may reuse its slot after the previous occupant drains. */
        int have = (b - psl->tail_blk < block_slots) &&
                   (psl->nrec > 0 || psl->nblk_owned < ctx->blocks_per_conn);
        if (!have) {
            atomic_fetch_add_explicit(&ctx->st_grow_waits, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&ctx->st_wait_window, 1, memory_order_relaxed);
            tx_wait_arm(ctx, psl, port, DMESH_TX_WAIT_QP_RECLAIM);
            errno = EAGAIN;
            return NULL;
        }
        /* Reserve shared capacity before tx_w moves for padding, so either EAGAIN
         * path is a no-op and the arm snapshot describes the failed head exactly.
         * A recycled block is already private to this QP and needs no grab. */
        if (psl->nrec == 0) {
            reserved_phys = block_pool_grab(ctx);
            if (reserved_phys < 0) {
                atomic_fetch_add_explicit(&ctx->st_grow_waits, 1,
                                          memory_order_relaxed);
                atomic_fetch_add_explicit(&ctx->st_wait_pool, 1,
                                          memory_order_relaxed);
                tx_wait_arm(ctx, psl, port, DMESH_TX_WAIT_SHARED_POOL);
                errno = EAGAIN;
                return NULL;
            }
        }
    }

    /* From here the reserve cannot fail for capacity. A spin-polling caller may have
     * reached this successful retry before consuming its queued TX_READY; erase that
     * obsolete one-shot before assigning bytes. */
    tx_wait_cancel(ctx, psl, port);

    if (pad) {                                             /* commit to the pad: we WILL succeed */
        atomic_store_explicit(&psl->blk_used[k % block_slots], off,
                              memory_order_release);       /* seal block k content end */
        psl->tx_w = (k + 1) * bs;                          /* pad to the next block boundary */
        k   = need_k;
        off = 0;
        atomic_fetch_add_explicit(&ctx->st_block_pads, 1, memory_order_relaxed);
    }
    /* Back every logical block up to k with a physical block, ONCE each (head_blk_next
     * tracks the highest assigned). The probe above cleared block head_blk_next; any
     * further ones are the same admission test, and a message spans at most one new
     * block, so this loop runs at most once past the probe. */
    while (psl->head_blk_next <= k) {
        uint64_t b = psl->head_blk_next;
        int bslot = (int)(b % block_slots);
        if (psl->nrec > 0) {                               /* reuse a recycled block (no pool op) */
            psl->pblk[bslot] = psl->recyc[--psl->nrec];
            atomic_fetch_add_explicit(&ctx->st_recycle_hits, 1, memory_order_relaxed);
        } else {
            /* The one possible shared grab was completed by the admission probe. */
            psl->pblk[bslot] = reserved_phys;
            reserved_phys = -1;
            psl->nblk_owned++;
        }
        atomic_store_explicit(&psl->blk_used[bslot], 0,
                              memory_order_relaxed);
        psl->head_blk_next = b + 1;
    }
    int s = (int)(k % block_slots);
    psl->resv_len = len;
    psl->resv_moff = (uint64_t)((size_t)psl->pblk[s] * (size_t)bs + off);
    return (uint8_t *)ctx->dma_buffer + psl->resv_moff;
}

/* Finalize `len` bytes (<= the reserved len) as committed message bytes, ready to ship.
 * Advances tx_w + tx_c and records the block's content end. Consumes the reserve — one
 * commit per dpumesh_tx_reserve. 0 = committed, -1 = no live reserve or len > it (a
 * caller-contract break; nothing is mutated). Owner thread. */
int dpumesh_tx_commit(dpumesh_ctx_t *ctx, uint16_t port,
                      const void *buf, uint32_t len) {
    if (port == 0 || buf == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct dmesh_port_slot *psl = &ctx->ports[port];
    int error_number = atomic_load_explicit(&psl->tx_error,
                                            memory_order_acquire);
    if (error_number != 0) { errno = error_number; return -1; }
    if (psl->nblk_owned <= 0) { errno = EINVAL; return -1; }
    /* Reject commits without an exact live reservation. */
    if (psl->resv_len == 0 || len == 0 || len > psl->resv_len ||
        buf != (const uint8_t *)ctx->dma_buffer + psl->resv_moff) {
        errno = EINVAL;
        return -1;
    }
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t k = psl->tx_w / bs;                           /* block the reserve placed the body in */
    psl->tx_w += len;
    atomic_store_explicit(
        &psl->blk_used[k % (uint64_t)ctx->blocks_per_conn],
        (uint32_t)(psl->tx_w - k * bs), memory_order_release);
    atomic_store_explicit(&psl->tx_c, psl->tx_w, memory_order_release);
    psl->resv_len = 0;                                     /* reserve consumed: one post per alloc */
    psl->resv_moff = 0;
    return 0;
}

/* Discard committed-but-UNSENT bytes (close-before-flush): rewind commit + write heads to
 * the send head. Shipped bytes (in flight) are untouched; abandoned blocks are returned
 * at close. Owner thread. */
void dpumesh_tx_discard_unsent(dpumesh_ctx_t *ctx, uint16_t port) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (psl->nblk_owned <= 0) return;
    uint64_t sent = atomic_load_explicit(&psl->tx_s, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_c, sent, memory_order_relaxed);
    psl->tx_w = sent;
    psl->resv_len = 0;
    psl->resv_moff = 0;
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t k = sent / bs;
    atomic_store_explicit(
        &psl->blk_used[k % (uint64_t)ctx->blocks_per_conn],
        (uint32_t)(sent - k * bs), memory_order_relaxed);
}

/* Return the next committed descriptor without advancing tx_s. Padded block tails
 * are skipped; dpumesh_tx_track records the reclaim boundary. */
static int dpumesh_tx_next_send_upto(dpumesh_ctx_t *ctx, uint16_t port,
                                     int flush_partial, uint64_t commit_limit,
                                     size_t *out_moff, uint32_t *out_len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t block_slots = (uint64_t)ctx->blocks_per_conn;
    for (;;) {
        uint64_t committed = atomic_load_explicit(&psl->tx_c,
                                                   memory_order_acquire);
        if (committed > commit_limit) committed = commit_limit;
        uint64_t sent = atomic_load_explicit(&psl->tx_s,
                                             memory_order_relaxed);
        if (sent >= committed) return 0;                   /* nothing committed to ship */
        uint64_t k = sent / bs;
        uint32_t off = (uint32_t)(sent % bs);
        uint32_t used = atomic_load_explicit(&psl->blk_used[k % block_slots],
                                             memory_order_acquire);
        if (off >= used) {                                 /* block k content exhausted → skip pad */
            atomic_store_explicit(&psl->tx_s, (k + 1) * bs,
                                  memory_order_relaxed);    /* jump to the next block start */
            continue;
        }
        uint64_t content_end = k * bs + (uint64_t)used;    /* content end within block k */
        uint64_t limit = (committed < content_end) ? committed : content_end;
        uint64_t avail = limit - sent;
        /* Normal post_send drains only complete wire slots. A short tail at the end
         * of a sealed physical block is the one exception: reserve padded past it and
         * committed bytes in a later block, so this tail can never grow and must go
         * first to preserve the byte stream's order. In the common case only the one
         * newest, still-fillable partial remains for an explicit flush. */
        size_t moff = (size_t)psl->pblk[k % block_slots] * (size_t)bs + off;
        uint32_t payload_cap = dpa_dma_payload_cap(moff,
                                                   (uint32_t)ctx->slot_size);
        if (!flush_partial && avail < (uint64_t)payload_cap &&
            committed <= content_end)
            return 0;
        uint32_t chunk = (avail < (uint64_t)payload_cap) ?
            (uint32_t)avail : payload_cap;
        *out_moff = moff;
        *out_len  = chunk;
        return 1;
    }
}

int dpumesh_tx_next_send(dpumesh_ctx_t *ctx, uint16_t port, int flush_partial,
                         size_t *out_moff, uint32_t *out_len) {
    return dpumesh_tx_next_send_upto(ctx, port, flush_partial, UINT64_MAX,
                                    out_moff, out_len);
}

/* Publish reclaim metadata before the forward descriptor. */
void dpumesh_tx_track(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq, uint32_t len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (!psl->su_seq) return;
    uint16_t h = atomic_load_explicit(&psl->su_head, memory_order_relaxed);
    size_t idx = (size_t)(h & (ctx->su_depth - 1));
    uint64_t sent = atomic_load_explicit(&psl->tx_s, memory_order_relaxed) + len;
    atomic_store_explicit(&psl->tx_s, sent, memory_order_relaxed);
    psl->su_seq[idx] = seq;
    psl->su_end[idx] = sent;                               /* end cursor after this unit */
    psl->su_done[idx] = 0;
    atomic_store_explicit(&psl->su_head, (uint_fast16_t)(h + 1), memory_order_release);
}

/* Undo the newest unpublished entry after enqueue failure. */
static int dpumesh_tx_untrack(dpumesh_ctx_t *ctx, uint16_t port,
                              uint16_t seq, uint32_t len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint16_t head = atomic_load_explicit(&psl->su_head, memory_order_relaxed);
    uint16_t tail = atomic_load_explicit(&psl->su_tail, memory_order_acquire);
    if (head == tail)
        return -1;
    uint16_t previous = (uint16_t)(head - 1u);
    size_t idx = (size_t)(previous & (ctx->su_depth - 1));
    uint64_t sent = atomic_load_explicit(&psl->tx_s, memory_order_relaxed);
    if (psl->su_seq[idx] != seq || psl->su_end[idx] != sent || sent < len)
        return -1;
    psl->su_done[idx] = 0;
    atomic_store_explicit(&psl->su_head, (uint_fast16_t)previous,
                          memory_order_release);
    atomic_store_explicit(&psl->tx_s, sent - len, memory_order_relaxed);
    return 0;
}

/* Arm a tail retained under a coalescing stamp but carrying no armed bit, once
 * the acknowledgement leaves the QP with nothing in flight. Runs on the PE
 * thread: it sets the multi-producer EQ bit and only reads the owner's cursors
 * and stamp. */
static void tx_arm_idle_tail(struct dmesh_port_slot *psl, uint16_t port,
                             uint16_t su_tail, uint16_t su_head)
{
    if (su_tail != su_head)
        return;                                    /* still in flight */
    uint64_t deadline = atomic_load_explicit(&psl->tx_deadline_ns,
                                             memory_order_relaxed);
    if (deadline == 0)
        return;                                    /* not coalescing: no tail */
    if (atomic_load_explicit(&psl->tx_s, memory_order_relaxed) >=
        atomic_load_explicit(&psl->tx_c, memory_order_acquire))
        return;                                    /* nothing retained */
    struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
    if (!eq || (role != DMESH_ROLE_CLIENT && role != DMESH_ROLE_SERVER))
        return;
    size_t word = (size_t)port >> 6;
    uint_fast64_t mask = (uint_fast64_t)1u << (port & 63u);
    if (atomic_load_explicit(&eq->tx_armed[word], memory_order_acquire) & mask)
        return;                                    /* the owner already armed it */
    eq_tx_armed_set(eq, port, deadline);
}

/* Apply one exact forward ACK. DMA completions can reorder when successive L7
 * messages choose different backend pods/egress engines. Mark the matching unit,
 * then advance tx_f only across the contiguous completed FIFO prefix; a later ACK
 * can never release an earlier unit whose DPU read is still in flight. */
static inline void tx_reclaim_ack(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq) {
    if (port == 0) return;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint16_t tail = atomic_load_explicit(&psl->su_tail, memory_order_relaxed);
    uint16_t head = atomic_load_explicit(&psl->su_head, memory_order_acquire);
    if (tail == head) return;                              /* nothing outstanding (su may be NULL) */
    /* head != tail ⇒ the owner shipped ⇒ FIFO arrays are allocated + visible (the
     * su_head acquire orders the owner's lazy malloc before this). FIFO seqs ascend, so
     * locate the exact sequence in O(1) from the tail sequence. Live depth is at most
     * 32768, making the 16-bit forward distance unambiguous. */
    uint16_t outstanding = (uint16_t)(head - tail);
    size_t tail_idx = (size_t)(tail & (ctx->su_depth - 1));
    uint16_t rel = (uint16_t)(seq - psl->su_seq[tail_idx]);
    if (rel >= outstanding)
        return;                                             /* stale, duplicate, or future */
    size_t ack_idx = (size_t)((uint16_t)(tail + rel) & (ctx->su_depth - 1));
    if (psl->su_seq[ack_idx] != seq)
        return;                                             /* gap/FIN: never guess */
    psl->su_done[ack_idx] = 1;

    uint64_t newf = 0;
    int popped = 0;
    while (tail != head) {
        size_t idx = (size_t)(tail & (ctx->su_depth - 1));
        if (!psl->su_done[idx])
            break;
        psl->su_done[idx] = 0;                              /* clean before slot reuse */
        newf = psl->su_end[idx];
        tail = (uint16_t)(tail + 1);
        popped++;
    }
    if (popped) {
        atomic_store_explicit(&psl->tx_f, newf, memory_order_release);
        atomic_store_explicit(&psl->su_tail, (uint_fast16_t)tail, memory_order_release);
        if (atomic_load_explicit(&psl->tx_wait_state, memory_order_acquire) ==
                DMESH_TX_WAIT_ARMED &&
            atomic_load_explicit(&psl->tx_wait_reason, memory_order_relaxed) ==
                DMESH_TX_WAIT_QP_RECLAIM &&
            tx_wait_qp_retryable(ctx, psl))
            (void)tx_wait_make_ready(ctx, port);
        tx_arm_idle_tail(psl, port, tail, head);
        try_return_blocks(ctx, psl);                       /* return blocks if this drained a CLOSED conn */
    }
}

/* Fail-safe bound, not a flow-control knob: a cell frees in microseconds while
 * any consumer exists, so only a ring with no consumer reaches it, and it
 * reports an error instead of hanging. */
#define RING_STALL_DEADLINE_SEC 5

int dpumesh_enqueue(dpumesh_ctx_t *ctx, const sw_descriptor_t *desc) {
    struct dma_desc *dma;
    uint32_t ring_slot;

    if (desc == NULL) {
        DOCA_LOG_ERR("ENQUEUE rejected: desc is NULL");
        return -1;
    }

    /* body_buf_slot is a byte offset in the shared TX mmap. Loopback is legal:
     * the DPU demultiplexes it by dst_port like any other destination. */
    if (desc->body_buf_slot < 0 ||
        (size_t)desc->body_buf_slot + desc->body_len > (size_t)ctx->num_slots * ctx->slot_size) {
        DOCA_LOG_ERR("ENQUEUE rejected: byte offset=%d + len=%u out of TX buffer (%zu)",
                     desc->body_buf_slot, desc->body_len,
                     (size_t)ctx->num_slots * ctx->slot_size);
        return -1;
    }

    if (desc->body_len > (uint32_t)ctx->slot_size) {
        DOCA_LOG_ERR("ENQUEUE rejected: body_len=%u exceeds slot_size=%d",
                     desc->body_len, ctx->slot_size);
        return -1;
    }
    if (desc->body_len != 0 &&
        desc->body_len > dpa_dma_payload_cap((uint64_t)desc->body_buf_slot,
                                             (uint32_t)ctx->slot_size)) {
        DOCA_LOG_ERR("ENQUEUE rejected: offset=%d len=%u exceeds aligned DMA window",
                     desc->body_buf_slot, desc->body_len);
        return -1;
    }

    /* Hash each connection to one forward ring, preserving its order while
     * spreading connections across K EUs. TX byte-ring admission provides flow control. */
    int ridx = dmesh_forward_ring(desc->src_port, ctx->k_rings);
    struct dma_ring *ring = ctx->dma_rings[ridx];

    if (__atomic_load_n(&ring->dead, __ATOMIC_ACQUIRE)) {
        errno = EIO;
        return -1;
    }

    /* A refused claim withdraws its ticket, keeping the published sequence
     * gapless. */
    uint64_t t;
    {
        struct timespec backoff = {0, 1000}; /* 1µs initial */
        struct timespec deadline; int have_dl = 0;
        while (!dma_ring_try_claim(ring, &t)) {
            if (__atomic_load_n(&ring->dead, __ATOMIC_ACQUIRE)) {
                errno = EIO;
                return -1;
            }
            uint64_t pr = __atomic_fetch_add(&ring->busy_probes, 1, __ATOMIC_RELAXED);
            if ((pr & 4095u) == 0)
                DOCA_LOG_WARN("DMA ring %d full (size=%u) [stuck x%llu]",
                              ridx, ring->size, (unsigned long long)(pr + 1));
            /* Armed lazily, so the uncontended path never pays a clock_gettime. */
            if (!have_dl) {
                clock_gettime(CLOCK_MONOTONIC, &deadline);
                deadline.tv_sec += RING_STALL_DEADLINE_SEC;
                have_dl = 1;
            } else {
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                if (now.tv_sec > deadline.tv_sec ||
                    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                    __atomic_store_n(&ring->dead, 1, __ATOMIC_RELEASE);
                    DOCA_LOG_ERR("DMA ring %d stalled >%ds (size=%u)",
                                 ridx, RING_STALL_DEADLINE_SEC, ring->size);
                    errno = EIO;
                    return -1;
                }
            }
            nanosleep(&backoff, NULL);
            if (backoff.tv_nsec < 50000) backoff.tv_nsec *= 2;
        }
    }
    ring_slot = (uint32_t)(t % ring->size);
    dma = &ring->descs[ring_slot];

    dma->mmap = ctx->dpa_mmap_handle;
    dma->addr = (uint64_t)ctx->dma_buffer + (size_t)desc->body_buf_slot;  /* byte offset */
    dma->size = desc->body_len;
    /* oriented endpoint tuple → DPA passthrough → DPU routes (dst_pod==BLANK).
     * src identity is stamped from the ctx (this node), not the caller's desc. */
    dma->seq         = desc->seq;
    dma->src_port    = desc->src_port;
    dma->dst_port    = desc->dst_port;
    dma->src_service = (int8_t)ctx->service_id;
    dma->dst_service = (int8_t)desc->dst_service;
    dma->dst_pod_id  = desc->dst_pod;
    dma->src_pod_id  = ctx->pod_id;

    dma_ring_publish_desc(ring, t);

    DOCA_LOG_DBG("ENQUEUE: seq=%u dst_svc=%d dst_pod=%d ring=%d slot=%u len=%u",
                 desc->seq, desc->dst_service, desc->dst_pod, ridx, ring_slot, desc->body_len);

    return 0;
}

/* ====================================================================
 * RX functions
 * ==================================================================== */

/* Pop one connection descriptor from the SPMC accept ring. Returns -1 when empty. */
int dpumesh_dequeue(dpumesh_ctx_t *ctx, sw_descriptor_t *desc) {
    return rxq_try_pop(ctx, desc) ? 0 : -1;
}

uint8_t *dpumesh_rx_buf(dpumesh_ctx_t *ctx, int slot) {
    /* slot carries the landing byte-offset (pos) into rx_dma_buffer. */
    if (slot < 0 || (size_t)slot >= ctx->rx_dma_buf_size) return NULL;
    return (uint8_t *)ctx->rx_dma_buffer + (size_t)slot;
}

void dpumesh_rx_free(dpumesh_ctx_t *ctx, int slot) {
    /* `slot` is the landing byte offset. Return its admission credit to the
     * matching ring once the consumer has read it. */
    rx_credit_return(ctx, slot);
}

/* ====================================================================
 * Query / info functions
 * ==================================================================== */

int dpumesh_get_slot_size(dpumesh_ctx_t *ctx) {
    return ctx->slot_size;
}

int dpumesh_get_block_size(dpumesh_ctx_t *ctx) {
    return ctx->block_size;
}

void dmesh_get_tx_stats(dmesh_channel_t *s, dmesh_tx_stats_t *out) {
    if (!s || !s->ctx || !out) return;
    dpumesh_ctx_t *ctx = s->ctx;
    out->pool_grabs   = atomic_load_explicit(&ctx->st_pool_grabs,   memory_order_relaxed);
    out->pool_returns = atomic_load_explicit(&ctx->st_pool_returns, memory_order_relaxed);
    out->recycle_hits = atomic_load_explicit(&ctx->st_recycle_hits, memory_order_relaxed);
    out->grow_waits   = atomic_load_explicit(&ctx->st_grow_waits,   memory_order_relaxed);
    out->block_pads   = atomic_load_explicit(&ctx->st_block_pads,   memory_order_relaxed);
}

/* Split of grow_waits by cause, for attributing a transmit stall. */
void dpumesh_get_wait_split(dpumesh_ctx_t *ctx, unsigned long long *window,
                            unsigned long long *pool) {
    if (!ctx) return;
    if (window)
        *window = atomic_load_explicit(&ctx->st_wait_window, memory_order_relaxed);
    if (pool)
        *pool = atomic_load_explicit(&ctx->st_wait_pool, memory_order_relaxed);
}

int dpumesh_get_pod_id(dpumesh_ctx_t *ctx) {
    return ctx->pod_id;
}

/* ====================================================================
 * Client-side API
 * ==================================================================== */

/* Allocate a host-unique port (>=1) and register it as a CLIENT or SERVER socket.
 * `user` is the app's conn handle (returned later by dmesh_next_ready) and `eq` the
 * event queue that owns it; both are stored before role is released, so the PE
 * observes a fully initialized slot. Returns 0 on exhaustion. */
uint16_t dpumesh_alloc_port(dpumesh_ctx_t *ctx, int role, void *user, struct dmesh_eq *eq) {
    pthread_mutex_lock(&ctx->port_lock);
    /* Client ports use a widening window below DMESH_UPORT_BASE. Inbox storage is
     * retained per visited port, and cursor rotation delays port-number reuse. */
    for (;;) {
        uint32_t span = ctx->port_span;
        if (ctx->next_port == 0 || ctx->next_port >= span) ctx->next_port = 1;
        for (uint32_t scanned = 0; scanned + 1 < span; scanned++) {
            uint32_t p = ctx->next_port;
            ctx->next_port = (p + 1 >= span) ? 1 : p + 1;           /* wrap in [1, span) */
            struct dmesh_port_slot *psl = &ctx->ports[p];
            if (psl->role != DMESH_ROLE_FREE || psl->nblk_owned > 0)
                continue;                                          /* live, or still draining */
            if (!psl->inbox) {
                psl->inbox = (sw_descriptor_t *)malloc((size_t)ctx->inbox_ring * sizeof(sw_descriptor_t));
                if (!psl->inbox) { pthread_mutex_unlock(&ctx->port_lock); return 0; }
                psl->inbox_ring = (uint32_t)ctx->inbox_ring;
            } else {
                /* Return the RX credits of deliveries that landed after the
                 * previous owner drained this inbox, before the head/tail
                 * reset discards them. */
                sw_descriptor_t d;
                while (inbox_pop(psl, &d)) rx_credit_return(ctx, d.body_buf_slot);
            }
            atomic_store_explicit(&psl->in_head, 0, memory_order_relaxed);
            atomic_store_explicit(&psl->in_tail, 0, memory_order_relaxed);
            psl->peer_pod   = DMESH_POD_BLANK;
            psl->peer_port  = 0;
            psl->rx_seq     = 0;
            psl->rx_next_pos = 0;
            psl->rx_seq_valid = 0;
            psl->user       = user;     /* visible before role (publish ordering below) */
            psl->eq         = eq;       /* ditto: the PE arms this EQ's list, not the ctx's */
            port_reset_tx(psl); /* fresh TX block-chain cursors */
            /* Publish role last: the PE sees the initialized inbox, cursors,
             * handle, EQ and chain before it can deliver here. */
            __atomic_store_n(&psl->role, (uint8_t)role, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&ctx->port_lock);
            return (uint16_t)p;
        }
        if (span >= DMESH_UPORT_BASE)
            break;                      /* swept the whole range: genuinely out of ports */
        /* Window full: widen it (the only path that allocates fresh inboxes) and
         * sweep the new region first. */
        ctx->port_span = (span * 2 > DMESH_UPORT_BASE) ? DMESH_UPORT_BASE : span * 2;
        ctx->next_port = span;
    }
    pthread_mutex_unlock(&ctx->port_lock);
    DOCA_LOG_ERR("dpumesh_alloc_port: no free ports");
    return 0;
}

/* Promote a pending server port to the accepting EQ exactly once. Returns the port
 * on success or zero when it is no longer pending. */
uint16_t dpumesh_accept_port(dpumesh_ctx_t *ctx, uint16_t port, void *user, struct dmesh_eq *eq) {
    if (port == 0) return 0;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    pthread_mutex_lock(&ctx->port_lock);
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_SERVER_PENDING) {
        pthread_mutex_unlock(&ctx->port_lock);
        return 0;   /* not pending (already accepted / freed / race) */
    }
    psl->user = user;
    psl->eq   = eq;             /* both visible before the role publish below */
    __atomic_store_n(&psl->role, DMESH_ROLE_SERVER, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ctx->port_lock);
    return port;
}

/* Release a conn. role=FREE (RELEASE) first → the PE drops further deliveries as
 * stale (and dmesh_next_ready skips any ready-list entry still pointing here); then
 * reclaim any undelivered inbound (their RX credits). The inbox ring is kept for
 * the slot's next reuse. */
void dpumesh_free_port(dpumesh_ctx_t *ctx, uint16_t port) {
    if (port == 0) return;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    /* Lifecycle changes also take port_lock, matching both client allocation and
     * PE-side SERVER_PENDING creation. */
    pthread_mutex_lock(&ctx->port_lock);
    /* Mark FREE, then return the TX blocks without blocking. With sends still
     * un-ACKed the blocks stay until the PE's last ACK returns them
     * (try_return_blocks); the alloc paths skip a FREE-but-draining port
     * (nblk_owned>0). */
    __atomic_store_n(&psl->role, DMESH_ROLE_FREE, __ATOMIC_RELEASE);
    tx_wait_cancel(ctx, psl, port);
    /* Drop both EQ-side records while the binding is still valid. */
    struct dmesh_eq *old_eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
    if (old_eq) {
        eq_tx_error_clear(old_eq, port);
        eq_tx_armed_clear(old_eq, port);
    }
    atomic_store_explicit(&psl->tx_deadline_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_error, 0, memory_order_release);
    psl->user = NULL;
    /* Unbind the EQ: arm_ready_after_push skips a NULL eq. */
    __atomic_store_n(&psl->eq, NULL, __ATOMIC_RELEASE);
    try_return_blocks(ctx, psl);
    if (psl->inbox) {
        sw_descriptor_t d;
        while (inbox_pop(psl, &d)) rx_credit_return(ctx, d.body_buf_slot);
    }
    /* Disarm so a recycled slot starts clean; dmesh_next_ready skips a stale
     * ready-list entry on role==FREE. */
    atomic_store_explicit(&psl->on_ready, 0u, memory_order_release);
    pthread_mutex_unlock(&ctx->port_lock);
}

/* Pop the next inbound message descriptor for a conn (CLIENT or SERVER — one
 * path). Returns 1 + fills *out, or 0 if the conn inbox is empty. The body is in
 * the shared RX mmap at out->body_buf_slot (a landing pos). */
int dpumesh_conn_recv(dpumesh_ctx_t *ctx, uint16_t port, sw_descriptor_t *out) {
    if (port == 0) return 0;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (!psl->inbox) return 0;
    if (inbox_pop(psl, out)) return 1;
    /* Inbox observed empty: disarm, then re-check under a seq_cst fence. This
     * pairs with the fence in arm_ready_after_push, so a message the PE enqueued
     * exactly as this conn drained is either seen here or re-pushed. */
    atomic_store_explicit(&psl->on_ready, 0u, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    if (inbox_pop(psl, out)) {
        /* A push raced in: re-arm and keep servicing. A later push then sees
         * on_ready==1 and leaves the drain to this consumer. */
        atomic_store_explicit(&psl->on_ready, 1u, memory_order_release);
        return 1;
    }
    return 0;
}

/* Pop the next conn with inbound data from THIS EQ's ready list and return its
 * app handle (the `user` registered at alloc). NULL when the list is drained. An
 * entry whose conn has since closed (role==FREE) is skipped. Single-consumer
 * (this EQ's thread); call it after waking on dmesh_eq_fd and drain each
 * returned conn to EAGAIN. */
void *dpumesh_next_ready(struct dmesh_eq *eq) {
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    uint16_t port;
    while (ready_pop(eq, &port)) {
        struct dmesh_port_slot *psl = &ctx->ports[port];
        uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
        /* Return only ACCEPTED conns. FREE is stale, and SERVER_PENDING is
         * drained by dmesh_accept, which is what sets its user handle. */
        if (role == DMESH_ROLE_CLIENT || role == DMESH_ROLE_SERVER)
            return psl->user;
    }
    return NULL;
}

/* Pop one automatically armed TX retry hint. The bitmap bit is removed first, then
 * READY->IDLE consumes the one-shot. If a direct retry already succeeded, its cancel
 * changed the state to IDLE and this stale bit is skipped. */
void *dpumesh_next_tx_ready(struct dmesh_eq *eq) {
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    uint16_t port;
    while (eq_tx_ready_pop(eq, &port)) {
        struct dmesh_port_slot *psl = &ctx->ports[port];
        uint_fast32_t expected = DMESH_TX_WAIT_READY;
        if (!atomic_compare_exchange_strong_explicit(&psl->tx_wait_state, &expected,
                                                      DMESH_TX_WAIT_IDLE,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire))
            continue;
        uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
        if ((role == DMESH_ROLE_CLIENT || role == DMESH_ROLE_SERVER) &&
            __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE) == eq)
            return psl->user;
    }
    return NULL;
}

/* Pop one asynchronous tail-publication failure. Removing the bitmap bit only
 * consumes the notification; dmesh_tx_call_begin keeps rejecting the QP with
 * its original sticky errno until the QP is destroyed. */
void *dpumesh_next_tx_error(struct dmesh_eq *eq) {
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    uint16_t port;
    while (eq_tx_error_pop(eq, &port)) {
        struct dmesh_port_slot *psl = &ctx->ports[port];
        uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
        if ((role == DMESH_ROLE_CLIENT || role == DMESH_ROLE_SERVER) &&
            __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE) == eq &&
            atomic_load_explicit(&psl->tx_error, memory_order_acquire) != 0)
            return psl->user;
    }
    return NULL;
}

/* ====================================================================
 * Connection lifecycle shared by the native and preload facades.
 * ==================================================================== */

/* Return the held RX-landing credit and clear the inbound view. */
static void conn_free_rx(dmesh_qp_t *c) {
    if (c->rx_slot >= 0) dpumesh_rx_free(c->ep->ctx, c->rx_slot);
    c->rx_slot = -1; c->rx_buf = NULL; c->rx_len = 0; c->rx_pos = 0;
}

/* Build the oriented tuple for one outbound descriptor of this conn: client →
 * service (the DPU selects a backend and pins the conn to it), or server → its
 * learned peer. `moff` = byte offset in the shared TX mmap, `len` = descriptor
 * length. seq++. Returns 0, or -1 (EBADMSG) on enqueue fault. */
static int emit_desc(dmesh_qp_t *c, size_t moff, uint32_t len,
                     uint32_t physical_advance) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    uint16_t next_seq = (uint16_t)(c->seq + 1);
    sw_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.body_buf_slot = (int32_t)moff;                 /* BYTE offset into the TX mmap */
    d.body_len      = len;
    d.src_port      = c->local_port;
    d.seq           = next_seq;
    d.dst_service   = c->dst_service;
    if (c->role == DMESH_ROLE_CLIENT) { d.dst_pod = DMESH_POD_BLANK; d.dst_port = DMESH_PORT_BLANK; }
    else                              { d.dst_pod = c->remote_pod;   d.dst_port = c->remote_port; }
    d.valid = 1;
    dpumesh_tx_track(ctx, c->local_port, next_seq, physical_advance);
    if (dpumesh_enqueue(ctx, &d) < 0) {
        (void)dpumesh_tx_untrack(ctx, c->local_port, next_seq,
                                 physical_advance);
        if (errno != EAGAIN) errno = EBADMSG;
        return -1;
    }
    c->seq = next_seq;
    return 0;
}

/* ===== Channel ===== */

dmesh_channel_t *dmesh_create_channel(void) {
    dmesh_channel_t *s = (dmesh_channel_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    /* Identity is injected, not declared: resolve $DPUMESH_SERVICE (a Kubernetes
     * Service name) to a service_id through the same table peers resolve through.
     * Unset = pure client (SVC_NONE). One table serves both directions. */
    int service_id = dmesh_config_identity();
    dpumesh_config_t cfg = DPUMESH_CONFIG_DEFAULT;
    if (dpumesh_init(&s->ctx, service_id, &cfg) != 0 || !s->ctx) {
        int saved_errno = errno != 0 ? errno : EIO;
        free(s);
        errno = saved_errno;
        return NULL;
    }
    s->pod_id     = dpumesh_get_pod_id(s->ctx);   /* DPU-assigned (valid after init) */
    s->slot_size  = dpumesh_get_slot_size(s->ctx);
    s->block_size = dpumesh_get_block_size(s->ctx);
    return s;
}

/* An EQ outliving its channel would point at a freed ctx, so every EQ must be
 * destroyed first. Returns 0, or -1 + EBUSY with nothing released. */
int dmesh_destroy_channel(dmesh_channel_t *s) {
    if (!s) return 0;
    if (s->ctx) {
        dpumesh_ctx_t *ctx = s->ctx;
        int live = 0;
        pthread_mutex_lock(&ctx->eq_lock);
        for (int i = 0; i < ctx->n_eqs; i++) if (ctx->eqs[i]) { live = 1; break; }
        pthread_mutex_unlock(&ctx->eq_lock);
        if (live) { errno = EBUSY; return -1; }
        dpumesh_destroy(ctx);
    }
    free(s);
    return 0;
}

int dmesh_pod_id(dmesh_channel_t *s)   { return s->pod_id; }
int dmesh_msg_max(dmesh_channel_t *s)  { return s->slot_size; }
int dmesh_post_max(dmesh_channel_t *s) { return s->block_size; }

/* ===== Event queue ===== */

/* Readiness is active at EQ creation. The eventfd is optional; polling remains
 * available when eventfd creation fails. */
dmesh_eq_t *dmesh_create_eq(dmesh_channel_t *ch) {
    if (!ch) { errno = EINVAL; return NULL; }
    dmesh_eq_t *eq = (dmesh_eq_t *)calloc(1, sizeof(*eq));
    if (!eq) { errno = ENOMEM; return NULL; }
    eq->accept_spare = (dmesh_qp_t *)calloc(1, sizeof(*eq->accept_spare));
    if (!eq->accept_spare) { free(eq); errno = ENOMEM; return NULL; }
    eq->ch         = ch;
    eq->notify_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&eq->tx_ready[i], (uint_fast64_t)0);
    atomic_init(&eq->tx_ready_count, (uint_fast32_t)0);
    eq->tx_ready_cursor = 0;
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&eq->tx_error[i], (uint_fast64_t)0);
    atomic_init(&eq->tx_error_count, (uint_fast32_t)0);
    eq->tx_error_cursor = 0;
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&eq->tx_armed[i], (uint_fast64_t)0);
    atomic_init(&eq->tx_armed_count, (uint_fast32_t)0);
    eq->tx_armed_cursor = 0;
    atomic_init(&eq->tx_earliest_ns, (uint_fast64_t)0);
    atomic_init(&eq->tx_due_hint, 0);
    atomic_init(&eq->ready_head, (uint_fast32_t)0);
    atomic_init(&eq->ready_tail, (uint_fast32_t)0);
    atomic_init(&eq->nqp, 0);
    atomic_init(&eq->wants_notify, 0);   /* poll-only until dmesh_eq_fd is called */
    atomic_init(&eq->suppress_notify, 0);

    dpumesh_ctx_t *ctx = ch->ctx;
    pthread_mutex_lock(&ctx->eq_lock);
    int idx = -1;
    for (int i = 0; i < ctx->n_eqs; i++) if (!ctx->eqs[i]) { idx = i; break; }
    if (idx < 0 && ctx->n_eqs < DMESH_MAX_EQ) idx = ctx->n_eqs++;
    if (idx >= 0) ctx->eqs[idx] = eq;
    pthread_mutex_unlock(&ctx->eq_lock);
    if (idx < 0) {   /* cap reached: an unregistered EQ would miss every accept */
        if (eq->notify_efd >= 0) close(eq->notify_efd);
        free(eq->accept_spare);
        free(eq);
        errno = EMFILE;
        return NULL;
    }
    eq->reg_idx = idx;
    return eq;
}

/* Unregister first, under eq_lock, so a concurrent accept-path notify_all_eqs
 * either saw this EQ before or never sees it again; then free. A live QP is
 * rejected with EBUSY. */
int dmesh_destroy_eq(dmesh_eq_t *eq) {
    if (!eq) return 0;
    if (atomic_load_explicit(&eq->nqp, memory_order_acquire) > 0) {
        errno = EBUSY;
        return -1;
    }
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    pthread_mutex_lock(&ctx->eq_lock);
    if (ctx->eqs[eq->reg_idx] == eq) ctx->eqs[eq->reg_idx] = NULL;
    pthread_mutex_unlock(&ctx->eq_lock);
    if (eq->notify_efd >= 0) close(eq->notify_efd);
    free(eq->accept_spare);
    free(eq);
    return 0;
}

/* Handing out the fd latches wants_notify — the PE starts writing the fd on ready
 * edges — and self-kicks once, so a conn armed while the EQ was poll-only still
 * leaves an edge for the caller's first sleep. The release store publishes the
 * flag to the PE thread before the kick. Idempotent. */
int dmesh_eq_fd(dmesh_eq_t *eq) {
    if (!eq) return -1;
    atomic_store_explicit(&eq->wants_notify, 1, memory_order_release);
    if (eq->notify_efd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(eq->notify_efd, &one, sizeof(one));
        (void)w;
    }
    return eq->notify_efd;
}

/* ===== Connection setup ===== */

dmesh_qp_t *dmesh_accept(dmesh_eq_t *eq) {
    dmesh_channel_t *s = eq->ch;
    /* Reserve the QP object before consuming the shared accept queue. */
    if (!eq->accept_spare) {
        eq->accept_spare = (dmesh_qp_t *)calloc(1, sizeof(*eq->accept_spare));
        if (!eq->accept_spare) { errno = ENOMEM; return NULL; }
    }
    sw_descriptor_t req;
    if (dpumesh_dequeue(s->ctx, &req) < 0 || !req.valid) {
        errno = EAGAIN;
        return NULL;
    }
    dmesh_qp_t *c = eq->accept_spare;
    eq->accept_spare = NULL;
    /* Promote the SERVER_PENDING slot the PE created at message-1 delivery,
     * whose inbox already holds any pipelined messages: attach this handle and
     * bind it to the accepting EQ, the only EQ dmesh_next_ready returns it on. */
    uint16_t ps = dpumesh_accept_port(s->ctx, req.dst_port, c, eq);
    if (ps == 0) {
        /* The slot stopped being pending between the queue push and this pop.
         * The QP object is untouched, so it stays the preallocated spare. */
        dpumesh_rx_free(s->ctx, req.body_buf_slot);
        eq->accept_spare = c;
        errno = EAGAIN;
        return NULL;
    }

    c->ep          = s;
    c->eq          = eq;
    c->role        = DMESH_ROLE_SERVER;
    c->local_port  = ps;                 /* == req.dst_port == uP */
    c->remote_pod  = req.src_pod;        /* learned peer (for replies + further sends) */
    c->remote_port = req.src_port;
    c->dst_service = req.src_service;
    c->seq         = 0;
    c->rx_slot     = req.body_buf_slot;  /* the first message (held) */
    c->rx_buf      = dpumesh_rx_buf(s->ctx, req.body_buf_slot);
    c->rx_len      = req.body_len;
    c->rx_pos      = 0;
    atomic_fetch_add_explicit(&eq->nqp, 1, memory_order_relaxed);
    return c;
}

/* Integer entry point (internal, dmesh_core.h) for the shim and for the
 * name-taking public wrapper below. Purely local — no round trip. */
dmesh_qp_t *dmesh_qp_open(dmesh_eq_t *eq, int dst_service_id) {
    dmesh_channel_t *s = eq->ch;
    dmesh_qp_t *c = (dmesh_qp_t *)calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; return NULL; }
    /* c = the port's handle, eq = the ready list its inbound edges arm */
    uint16_t pc = dpumesh_alloc_port(s->ctx, DMESH_ROLE_CLIENT, c, eq);
    if (pc == 0) { free(c); errno = ENOMEM; return NULL; }
    c->ep          = s;
    c->eq          = eq;
    c->role        = DMESH_ROLE_CLIENT;
    c->local_port  = pc;
    c->dst_service = (int16_t)dst_service_id;
    c->remote_pod  = DMESH_POD_BLANK;
    c->remote_port = DMESH_PORT_BLANK;
    c->seq         = 0;
    c->rx_slot     = -1;
    atomic_fetch_add_explicit(&eq->nqp, 1, memory_order_relaxed);
    return c;
}

/* Resolve the Kubernetes Service name and open the public QP. */
dmesh_qp_t *dmesh_create_qp(dmesh_eq_t *eq, const char *service_name) {
    if (!eq || !service_name) { errno = EINVAL; return NULL; }
    int svc = dmesh_resolve_name(service_name);
    if (svc < 0) return NULL;                 /* errno = ENOENT from resolve_name */
    return dmesh_qp_open(eq, svc);
}

dmesh_qp_t *dmesh_next_ready(dmesh_eq_t *eq) {
    return (dmesh_qp_t *)dpumesh_next_ready(eq);
}

/* ===== TX publication + teardown ===== */

static int dmesh_drain_tx_upto_locked(dmesh_qp_t *c, int flush_partial,
                                      uint64_t commit_limit) {
    if (!c) { errno = EINVAL; return -1; }
    dpumesh_ctx_t *ctx = c->ep->ctx;
    size_t moff; uint32_t len;
    while (dpumesh_tx_next_send_upto(ctx, c->local_port, flush_partial,
                                     commit_limit, &moff, &len)) {
        struct dmesh_port_slot *psl = &ctx->ports[c->local_port];
        uint64_t sent = atomic_load_explicit(&psl->tx_s,
                                              memory_order_relaxed);
        uint64_t committed = atomic_load_explicit(&psl->tx_c,
                                                   memory_order_acquire);
        uint64_t limit = committed < commit_limit ? committed : commit_limit;
        uint64_t original_end = sent + len;
        uint64_t padded_end = original_end;
        int padded = 0;

        if (original_end == limit && limit == committed &&
            psl->tx_w == committed) {
            padded_end = (original_end + DPA_DMA_COPY_ALIGN - 1u) &
                         ~(uint64_t)(DPA_DMA_COPY_ALIGN - 1u);
            if (padded_end > original_end) {
                uint64_t bs = (uint64_t)ctx->block_size;
                uint64_t block = (original_end - 1u) / bs;
                psl->tx_w = padded_end;
                atomic_store_explicit(
                    &psl->blk_used[block % (uint64_t)ctx->blocks_per_conn],
                    (uint32_t)(padded_end - block * bs), memory_order_release);
                atomic_store_explicit(&psl->tx_c, padded_end,
                                      memory_order_release);
                padded = 1;
            }
        }

        uint32_t advance = (uint32_t)(padded_end - sent);
        if (emit_desc(c, moff, len, advance) < 0) {
            if (padded) {
                uint64_t bs = (uint64_t)ctx->block_size;
                uint64_t block = (original_end - 1u) / bs;
                psl->tx_w = original_end;
                atomic_store_explicit(
                    &psl->blk_used[block % (uint64_t)ctx->blocks_per_conn],
                    (uint32_t)(original_end - block * bs), memory_order_release);
                atomic_store_explicit(&psl->tx_c, original_end,
                                      memory_order_release);
            }
            errno = EBADMSG;
            return -1;                                     /* bytes stay committed */
        }
    }
    return 0;
}

static int dmesh_drain_tx_locked(dmesh_qp_t *c, int flush_partial) {
    return dmesh_drain_tx_upto_locked(c, flush_partial, UINT64_MAX);
}

static int dmesh_tx_inflight_locked(const struct dmesh_port_slot *psl) {
    uint_fast16_t head =
        atomic_load_explicit(&psl->su_head, memory_order_acquire);
    uint_fast16_t tail =
        atomic_load_explicit(&psl->su_tail, memory_order_acquire);
    return head != tail;
}

/* Wait until every previously submitted unit has left DPU proxy custody before
 * publishing FIN. tx_reclaim_ack() runs on the independent PE thread and only
 * advances su_tail across the contiguous completed prefix, so an empty FIFO is
 * the exact data-before-FIN fence. Held under tx_gate to exclude another TX call.
 * On timeout the caller frees the local handle and enqueues no overtaking FIN. */
static int dmesh_wait_tx_reclaimed_locked(const struct dmesh_port_slot *psl) {
    uint64_t deadline = monotonic_ns() + TX_CLOSE_DRAIN_DEADLINE_NS;
    long wait_ns = TX_CLOSE_DRAIN_MIN_WAIT_NS;

    while (dmesh_tx_inflight_locked(psl)) {
        if (monotonic_ns() >= deadline) {
            errno = EBADMSG;
            return -1;
        }
        struct timespec wait = { .tv_sec = 0, .tv_nsec = wait_ns };
        (void)nanosleep(&wait, NULL);
        if (wait_ns < TX_CLOSE_DRAIN_MAX_WAIT_NS) {
            wait_ns *= 2;
            if (wait_ns > TX_CLOSE_DRAIN_MAX_WAIT_NS)
                wait_ns = TX_CLOSE_DRAIN_MAX_WAIT_NS;
        }
    }
    return 0;
}

/* Entry check for every public TX call: the port slot must still belong to this
 * handle and carry no latched fault. Takes the transmit gate on success; the
 * caller releases it. */
int dmesh_tx_qp_valid(dmesh_qp_t *c) {
    if (!c || !c->ep || !c->ep->ctx || c->local_port == 0) {
        errno = EINVAL;
        return -1;
    }
    struct dmesh_port_slot *psl = &c->ep->ctx->ports[c->local_port];
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
    if ((role != DMESH_ROLE_CLIENT && role != DMESH_ROLE_SERVER) ||
        psl->user != c) {
        errno = EINVAL;
        return -1;
    }
    int error_number = atomic_load_explicit(&psl->tx_error,
                                            memory_order_acquire);
    if (error_number != 0) {
        errno = error_number;
        return -1;
    }
    /* An un-posted dmesh_alloc still holds the gate for this caller's own
     * transmit call. */
    if (tx_call_open(psl)) {
        errno = EDEADLK;
        return -1;
    }
    tx_gate_acquire(psl);
    return 0;
}

/* True between a successful dmesh_alloc and its dmesh_post_send. */
int dmesh_tx_call_active(dmesh_qp_t *c) {
    if (!c || !c->ep || !c->ep->ctx || c->local_port == 0)
        return 0;
    return tx_call_open(&c->ep->ctx->ports[c->local_port]);
}

/* Release the transmit gate taken by dmesh_tx_qp_valid(). */
void dmesh_tx_call_done(dmesh_qp_t *c) {
    if (!c || !c->ep || !c->ep->ctx || c->local_port == 0)
        return;
    tx_gate_release(&c->ep->ctx->ports[c->local_port]);
}

/* Publish the units this commit completed, then place the newest partial.
 * Held under tx_gate. */
int dmesh_tx_after_commit(dmesh_qp_t *c) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    struct dmesh_port_slot *psl = &ctx->ports[c->local_port];
    uint64_t sent = atomic_load_explicit(&psl->tx_s, memory_order_relaxed);

    /* A retained tail accumulates until it can fill a transport unit. It is
     * released by its deadline, an explicit flush, allocation pressure, or
     * close. A tail it retains without an armed bit is armed by
     * tx_arm_idle_tail when the last acknowledgement leaves the QP idle. */
    if (atomic_load_explicit(&psl->tx_deadline_ns, memory_order_relaxed) != 0 &&
        psl->tx_w - sent < (uint64_t)ctx->slot_size)
        return 0;

    if (dmesh_drain_tx_locked(c, 0) != 0) {
        tx_disarm_tail(psl, c->local_port);
        return -1;
    }
    if (atomic_load_explicit(&psl->tx_s, memory_order_relaxed) >= psl->tx_w) {
        /* The armed bit means "a tail is waiting"; the deadline stamp means
         * "this stream is coalescing". Units still in flight keep the stamp,
         * so the retention rule stays in force. */
        struct dmesh_eq *eq = __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE);
        if (eq) eq_tx_armed_clear(eq, c->local_port);
        if (!dmesh_tx_inflight_locked(psl))
            atomic_store_explicit(&psl->tx_deadline_ns, 0,
                                  memory_order_relaxed);
        return 0;
    }
    uint64_t deadline = atomic_load_explicit(&psl->tx_deadline_ns,
                                             memory_order_relaxed);
    if (deadline == 0) {
        /* An idle stream has no outstanding unit for a successor to arrive
         * behind, so its first partial goes now. */
        if (!dmesh_tx_inflight_locked(psl))
            return dmesh_drain_tx_locked(c, 1);
    } else if (monotonic_ns() >= deadline) {
        /* A retained tail is released by its deadline alone. */
        tx_disarm_tail(psl, c->local_port);
        return dmesh_drain_tx_locked(c, 1);
    } else {
        return 0;                                /* keep coalescing */
    }
    tx_arm_tail(psl, c->local_port);
    return 0;
}

/* Publish a retained tail before returning capacity pressure to the caller. */
void dmesh_tx_pressure(dmesh_qp_t *c) {
    int saved_errno = errno;
    dpumesh_ctx_t *ctx = c->ep->ctx;
    struct dmesh_port_slot *psl = &ctx->ports[c->local_port];
    tx_disarm_tail(psl, c->local_port);
    if (atomic_load_explicit(&psl->tx_s, memory_order_relaxed) >=
        atomic_load_explicit(&psl->tx_c, memory_order_acquire)) {
        errno = saved_errno;
        return;
    }
    if (dmesh_drain_tx_locked(c, 1) != 0)
        tx_error_publish(psl, c->local_port, EBADMSG);
    errno = saved_errno;
}

/* Publish every retained tail on this EQ whose deadline expired. Runs on the
 * EQ's thread, which owns these QPs. */
void dpumesh_publish_due_tails(struct dmesh_eq *eq) {
    dpumesh_ctx_t *ctx = eq->ch->ctx;
    if (atomic_load_explicit(&eq->tx_armed_count, memory_order_acquire) == 0)
        return;
    /* The timer marks the EQ when a deadline may have passed. */
    if (!atomic_load_explicit(&eq->tx_due_hint, memory_order_acquire))
        return;
    atomic_store_explicit(&eq->tx_due_hint, 0, memory_order_release);
    uint64_t now = monotonic_ns();
    uint16_t port;
    while (eq_tx_armed_pop_due(eq, now, &port)) {
        struct dmesh_port_slot *psl = &ctx->ports[port];
        uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
        if (role != DMESH_ROLE_CLIENT && role != DMESH_ROLE_SERVER) {
            tx_gate_release(psl);
            continue;
        }
        dmesh_qp_t *c = (dmesh_qp_t *)psl->user;
        if (c && __atomic_load_n(&psl->eq, __ATOMIC_ACQUIRE) == eq &&
            atomic_load_explicit(&psl->tx_error, memory_order_acquire) == 0 &&
            atomic_load_explicit(&psl->tx_s, memory_order_relaxed) <
                atomic_load_explicit(&psl->tx_c, memory_order_acquire) &&
            dmesh_drain_tx_locked(c, 1) != 0)
            tx_error_publish(psl, port, EBADMSG);
        tx_gate_release(psl);
    }
    eq_tx_armed_refresh(eq);
}

/* Nanoseconds until a retained tail on this EQ comes due, -1 when none is
 * retained. Bounds an event loop's own poll timeout. */
int64_t dmesh_eq_next_deadline_ns(dmesh_eq_t *eq) {
    if (!eq) return -1;
    return eq_tx_armed_wait_ns(eq, monotonic_ns());
}

int dmesh_flush(dmesh_qp_t *c) {
    if (dmesh_tx_qp_valid(c) != 0) return -1;
    struct dmesh_port_slot *psl = &c->ep->ctx->ports[c->local_port];
    tx_disarm_tail(psl, c->local_port);
    int result = dmesh_drain_tx_locked(c, 1);
    tx_gate_release(psl);
    return result;
}

int dmesh_tx_inflight(dmesh_qp_t *c) {
    if (!c || !c->ep || !c->ep->ctx || c->local_port == 0) return 0;
    struct dmesh_port_slot *psl = &c->ep->ctx->ports[c->local_port];
    return dmesh_tx_inflight_locked(psl);
}

/* Ship this conn's FIN. Idempotent: fin_sent latches. Independent of
 * peer_closed — a received FIN does not close this half, and the DPU's upstream
 * teardown fans out from this FIN alone. */
static int dmesh_send_fin_locked(dmesh_qp_t *c) {
    if (c->fin_sent) return 0;
    dpumesh_ctx_t *ctx = c->ep->ctx;
    uint16_t next_seq = (uint16_t)(c->seq + 1);
    sw_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.body_buf_slot = 0;                                   /* 0-length FIN: offset unused */
    d.body_len      = 0;                                   /* FIN marker (0-length) */
    d.src_port      = c->local_port;
    d.seq           = next_seq;
    d.dst_service   = c->dst_service;
    d.dst_pod       = c->remote_pod;                       /* the learned peer conn */
    d.dst_port      = c->remote_port;
    d.valid         = 1;
    /* Latch only after enqueue succeeds. A failed attempt must be observable and
     * must not suppress a later close path from trying again. */
    if (dpumesh_enqueue(ctx, &d) < 0) {
        if (errno != EAGAIN) errno = EBADMSG;
        return -1;
    }
    c->seq = next_seq;
    c->fin_sent = 1;
    return 0;
}

int dmesh_send_fin(dmesh_qp_t *c) {
    if (dmesh_tx_qp_valid(c) != 0) return -1;
    struct dmesh_port_slot *psl = &c->ep->ctx->ports[c->local_port];
    tx_disarm_tail(psl, c->local_port);
    int result = dmesh_drain_tx_locked(c, 1);
    if (result == 0) result = dmesh_wait_tx_reclaimed_locked(psl);
    if (result == 0) result = dmesh_send_fin_locked(c);
    tx_gate_release(psl);
    return result;
}

static int dmesh_release_qp(dmesh_qp_t *c, int graceful) {
    if (!c) return 0;
    int close_result = 0, close_errno = 0;
    int fin_ordered = 1;
    dpumesh_ctx_t *ctx = c->ep->ctx;
    struct dmesh_port_slot *psl = &ctx->ports[c->local_port];
    if (c->eq && c->eq->drain_cur == c) c->eq->drain_cur = NULL; /* poll_eq resume cursor */
    /* An open transmit call already holds the gate; its reservation is
     * discarded below. */
    if (!tx_call_open(psl))
        tx_gate_acquire(psl);
    tx_disarm_tail(psl, c->local_port);
    if (graceful && dmesh_drain_tx_locked(c, 1) != 0) {
        close_result = -1;
        close_errno = errno;
    }
    /* Abort always discards the buffered tail. Graceful close reaches this with no
     * unsent committed bytes unless its flush failed; a live, un-posted reservation
     * is never application data owned by the transport and is discarded either way. */
    dpumesh_tx_discard_unsent(ctx, c->local_port);
    /* A data ACK releases DPU proxy custody rather than reporting a DMA copy, so
     * an empty submitted FIFO is the stream-order fence that keeps the
     * zero-copy FIN behind the payload. */
    if (dmesh_wait_tx_reclaimed_locked(psl) != 0) {
        fin_ordered = 0;
        if (close_result == 0) {
            close_result = -1;
            close_errno = errno;
        }
    }
    /* An established conn always closes its half (dmesh_send_fin self-guards a
     * second one). A CLIENT that never sent (seq==0) has no peer and no DPU-side
     * conn to tear down. */
    if (fin_ordered && (c->role == DMESH_ROLE_SERVER || c->seq > 0)) {
        if (dmesh_send_fin_locked(c) != 0 && close_result == 0) {
            close_result = -1;
            close_errno = errno;
        }
    }
    conn_free_rx(c);                                       /* return the held RX credit */
    tx_gate_release(psl);
    if (c->local_port) dpumesh_free_port(ctx, c->local_port);
    if (c->eq) atomic_fetch_sub_explicit(&c->eq->nqp, 1, memory_order_release);
    free(c);
    if (close_result != 0) errno = close_errno;
    return close_result;
}

int dmesh_destroy_qp(dmesh_qp_t *c) {
    return dmesh_release_qp(c, 1);
}

int dmesh_abort_qp(dmesh_qp_t *c) {
    return dmesh_release_qp(c, 0);
}
