/*
 * dmesh_core.c - DPUmesh DOCA transport layer implementation (raw core engine)
 *
 * NVIDIA DOCA (Comch + DMA) host-side backend for the DPUmesh transport.
 * Provides the host-side raw buffer API declared in dmesh_core.h: TX/RX
 * slot pools, descriptor SQ enqueue/dequeue, and a connection-oriented
 * port table (no request/response matching) over the DPU control + DMA
 * data path.
 */

#include "dmesh_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>      /* tx_reserve / lifecycle report EAGAIN|EINVAL|EBADMSG */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>        /* close() for the host epoll RX path */
#include <sys/epoll.h>     /* event-driven host PE progress (DPUMESH_HOST_EPOLL) */
#include <sys/eventfd.h>   /* per-CQ readiness eventfd for native-epoll integration */

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
#include "doca/comch_consumer.h"
#include "doca/comch_common.h"
#include "doca/comch_msgq.h"
#include "doca/dpa_common.h"

DOCA_LOG_REGISTER(DPUMESH_DOCA);

static const char *doca_err_str(doca_error_t rc) {
    return doca_error_get_descr(rc);
}

static void cleanup_ctx(struct dpumesh_ctx *ctx);

/* ====================================================================
 * dpumesh_ctx — internal state
 * ==================================================================== */

/* Accept queue between the PE thread (producer — a NEW conn's first message
 * lands here) and dmesh_accept (consumer). Sized larger than worst-case
 * concurrent new-conn bursts so the PE thread never has to drop on enqueue. */
#define RX_QUEUE_SIZE 65536

/* Per-conn TX send-unit FIFO depth: the max number of shipped-but-un-ACKed
 * descriptors a single connection may keep outstanding. Each shipped descriptor
 * records (seq -> end cursor) in su_seq/su_end[i]; a BATCH_FWD_ACK(port,seq) pops
 * the FIFO front and advances the conn's free cursor. Power of two (masked).
 *
 * ENFORCED by dpumesh_tx_reserve's send-unit admission, which is the ONLY thing
 * bounding it — the block window cannot, since one small message costs a whole FIFO
 * slot but a sliver of a block. Overrunning it would overwrite a live entry and let
 * tx_reclaim_ack free bytes the DPU is still DMA-reading. */
#define TX_SU_DEPTH 64u

/* ELASTIC per-conn TX buffer (block chain). The shared TX mmap is carved into
 * fixed BLOCKS of DPUMESH_TX_BLOCK bytes (= the max contiguous message = the
 * allocation unit). A conn grabs blocks on demand (grow, up to DPUMESH_TX_MAXB)
 * and returns drained ones — keeping a cushion of DPUMESH_TX_H recycled blocks
 * for reuse (grow/shrink hysteresis) — so its footprint tracks in-flight demand
 * instead of a fixed region. DMESH_TX_MAXB_CAP bounds the per-conn block arrays. */
#define DPUMESH_TX_BLOCK_DEFAULT  65536   /* 64 KB */
#define DPUMESH_TX_MAXB_DEFAULT   4        /* max blocks/conn (4 × 64 KB = 256 KB in-flight cap) */
#define DPUMESH_TX_H_DEFAULT      1        /* recycled-block cushion (hysteresis) */
#define DMESH_TX_MAXB_CAP         8        /* compile-time cap for per-conn block arrays */

/* Connection table (connection-oriented, full-duplex — NOT request/response).
 * Index = local port [1,65535]; port 0 = BLANK (a fresh-connection message → the
 * accept queue). Each allocated port IS a connection (like a socket fd): it owns a
 * peer (pod,port), an inbound message queue, and an optional per-conn readiness
 * eventfd. The PE thread routes every inbound by dst_port → that conn's inbox and
 * wakes that conn's fd. There is NO request↔response matching: a conn just delivers
 * whatever arrives, in send order per conn, until close. Allocated from one
 * host-unique pool so client and server ports never collide (loopback-safe).
 * DMESH_PORT_SPACE lives in dmesh_core.h — struct dmesh_cq's ready ring is sized by it. */
/* Per-conn inbound queue depth (descriptors only — bodies stay in the shared RX
 * mmap, referenced by pos). Lazily malloc'd per LIVE conn (not 65536× pre-alloc),
 * power of two. The DEPTH is sized at init (ctx->inbox_ring, copied into each slot's
 * psl->inbox_ring at malloc) to the DPU's per-region reverse-credit budget
 * rq = rq_depth/K = num_slots*slot_size/DPUMESH_SLOT_SIZE / K (comch_common.c). At
 * that depth the inbox CANNOT overflow before the DPU's own credit runs out — which
 * back-pressures cleanly — so the silent inbox-full drop in rx_deliver_desc is
 * unreachable in steady use. Below is the FLOOR (also the depth for tiny configs
 * where rq < 256). Prior builds hard-coded 256, which is 8× below the default
 * rq (=2048), so a hot conn could overflow it and lose messages silently. */
#define DMESH_INBOX_RING_MIN  256u

/* Starting width of the client-port window (dpumesh_alloc_port). Sets both the floor on
 * port-number reuse distance and the floor on how many per-port inboxes a process can
 * accumulate; it doubles on demand, so this only has to cover the common case. */
#define DMESH_PORT_SPAN_MIN   256u
/* Field grouping is by MUTATOR THREAD, not by role, to kill false sharing between the
 * PE (producer) and the conn's owning app thread (consumer). The two run on different
 * cores under hw pinning / real deployments, and a producer store that lands on the same
 * 64B line as a consumer store ping-pongs the line on every message. So: owner-local +
 * read-mostly setup fields first; then a producer-write line; then the shared arm flag on
 * its own line (both sides write it, so it must not sit with either side's private line);
 * then a consumer-write line. Each _cl_* pad opens a fresh cache line. All fields are
 * preserved; only their placement changed (the struct is calloc'd and accessed by name). */
struct dmesh_port_slot {
    uint8_t          role;            /* FREE / CLIENT / SERVER */
    int16_t          peer_pod;        /* established peer pod, DMESH_POD_BLANK = not yet learned */
    uint16_t         peer_port;       /* established peer port, 0 = not yet learned */
    void            *user;            /* app's conn handle (returned by dmesh_next_ready);
                                       * set BEFORE role is published so the PE never enqueues
                                       * a port whose handle isn't visible yet. */
    struct dmesh_cq *cq;              /* the CQ owning this conn: the ONE ready list its
                                       * edges are pushed to, and the ONE fd they wake.
                                       * Published with `user`, before role; cleared at
                                       * free_port, so the PE never arms a dead conn. */
    /* Inbound SPSC ring: PE thread = sole producer (in_tail), the conn's owning
     * app thread = sole consumer (in_head). Lock-free. inbox==NULL until alloc. */
    sw_descriptor_t *inbox;           /* malloc'd ring[inbox_ring]; NULL until alloc */
    uint32_t         inbox_ring;      /* this inbox's depth (power of two = ctx->inbox_ring),
                                       * stamped at malloc so inbox_push/pop stay self-contained */
    /* Per-conn TX BYTE-RING over an ELASTIC CHAIN of blocks from the shared pool.
     * FOUR monotonic LOGICAL byte cursors span the chain (byte offset in the chain =
     * cursor; logical block index k = cursor / block_size; offset-in-block = cursor %
     * block_size; physical block = pblk[k % maxb]):
     *   tx_w  alloc/write head — where the next message body is written / alloc'd (owner)
     *   tx_c  commit           — bytes finalized as whole messages, ready to ship (owner)
     *   tx_s  send             — bytes a descriptor was posted for (owner, at flush)
     *   tx_f  free             — bytes ACKed by the DPU, reclaimable (PE thread, atomic)
     * Invariant tx_f <= tx_s <= tx_c <= tx_w. A message never straddles a block (≤
     * block_size; padded to the next block if it won't fit) so each is CONTIGUOUS in
     * one physical block. blk_used[k%maxb] = committed content end offset in block k
     * (send skips the pad). Blocks are grabbed on demand (grow, up to maxb) and drained
     * blocks recycled via recyc[] (depth ≤ cushion_h) then returned to the pool (shrink);
     * nblk_owned = pblk-live + recyc count (≤ maxb, >0 == holds buffer / draining).
     * tail_blk = oldest live logical block index. su_seq/su_end = per-conn send-unit FIFO
     * (lazily malloc'd, kept per slot) mapping shipped seq -> end cursor for ACK reclaim.
     * The whole block chain is OWNER-thread-local while live; the PE only advances tx_f
     * (atomic). On close (role=FREE) the PE returns the remaining blocks (try_return_blocks). */
    uint64_t         tx_w, tx_c, tx_s;      /* owner-thread logical cursors */
    uint32_t         resv_len;              /* live dpumesh_tx_reserve len (owner); 0 = no reserve
                                             * outstanding. tx_commit rejects a len past it (ring
                                             * overrun) and clears it (one post per alloc). */
    uint64_t         tail_blk;              /* oldest live logical block index (owner) */
    uint64_t         head_blk_next;         /* next logical block index needing a physical block
                                             * (blocks [tail_blk, head_blk_next) are backed) */
    int32_t          pblk[DMESH_TX_MAXB_CAP];    /* logical k%maxb -> physical block id; -1 = none */
    int32_t          recyc[DMESH_TX_MAXB_CAP];   /* drained blocks held for reuse (owner) */
    uint32_t         blk_used[DMESH_TX_MAXB_CAP];/* committed content end offset in block k%maxb */
    int              nblk_owned;            /* physical blocks held (pblk-live + recyc); ≤ maxb */
    int              nrec;                  /* recyc depth (owner) */
    uint16_t        *su_seq;                /* [TX_SU_DEPTH] shipped seq (lazy malloc, per slot) */
    uint64_t        *su_end;                /* [TX_SU_DEPTH] shipped end cursor (lazy malloc, per slot) */

    /* ---- PRODUCER (PE thread) cache line: fields the PE mutates every message ---- */
    char _cl_prod[64];
    atomic_uint_fast32_t in_tail;           /* inbound SPSC producer (PE) */
    atomic_uint_fast64_t tx_f;              /* PE-thread logical cursor (ACK reclaim) */
    atomic_uint_fast16_t su_tail;           /* send-unit FIFO tail (PE writes/release, owner reads) */

    /* ---- shared ARM flag on its own line (both threads write it) ---- */
    /* Ready-list ARM flag (0=not on ready list & not being serviced; 1=on the list
     * OR a consumer is draining it). The PE arms it 0->1 (and ready_pushes) after a
     * push; the consumer disarms it 1->0 when conn_recv drains the inbox empty, then
     * re-checks under a seq_cst fence. This replaces the racy "arm on the inbox
     * empty->non-empty edge" test: that read a head the consumer concurrently
     * advanced, so a push landing exactly as the consumer stopped draining could be
     * enqueued but never put on the ready list — stranding the conn forever (a
     * lost-edge / Dekker race). The flag + paired fences close it and also bound the
     * conn to at most ONE ready-list entry per drain cycle (no duplicate churn). Its
     * own line so an arm/disarm never dirties either side's private cursor line. */
    char _cl_arm[64];
    atomic_uint_fast32_t on_ready;    /* PE arms; consumer (conn_recv) disarms */

    /* ---- CONSUMER (owner app thread) cache line: fields the owner mutates ---- */
    char _cl_cons[64];
    atomic_uint_fast32_t in_head;     /* inbound SPSC consumer (app) */
    atomic_uint_fast16_t su_head;     /* send-unit FIFO head (owner writes/release, PE reads) */
    char _cl_end[64];                 /* isolate this slot's consumer line from the next slot */
};
/* Ready-list SPSC ops (monotonic counters; PE producer, CQ thread consumer). Each
 * list carries conn PORTS; dmesh_next_ready maps each to its slot->user. Provably
 * never full (≤ live conns < DMESH_PORT_SPACE; the on_ready flag admits each at most
 * once between drains), but the guard keeps a stray push from corrupting indices. */
static inline void ready_push(struct dmesh_cq *cq, uint16_t port);
static inline int  ready_pop(struct dmesh_cq *cq, uint16_t *port);
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
/* One cell of the lock-free bounded SPMC RX ring (Vyukov's bounded-MPMC cell/seq
 * design, producer side specialized to a single producer). `seq` carries the
 * turn-stamp: the producer may write cell i only when seq==enq_pos; a consumer
 * may read it only when seq==deq_pos+1. */
struct rxq_cell {
    sw_descriptor_t desc;
    atomic_uint_fast32_t seq;
};

struct dpumesh_ctx {
    char worker_id[128];
    int  pod_id;             /* this node's address; -1 until the DPU assigns it at register */
    int  num_slots;
    int  slot_size;
    int  k_rings;              /* K = forward rings per pod (EU-sharding); 1 = legacy */
    int  inbox_ring;           /* per-conn inbound descriptor ring depth (pow2), sized to the
                                * DPU per-region reverse-credit budget so the inbox-full drop
                                * (rx_deliver_desc) is unreachable in steady use. */
    /* DOCA objects */
    struct objects doca_objs;
    void *dma_buffer;          /* Host TX buffer (PCI mmap, CPU→DPU source) */
    /* K forward descriptor rings (EU-sharding). dpumesh_enqueue conn-shards
     * across them (ring = src_port % K). Posting is LOCK-FREE MPSC — a Vyukov
     * bounded queue lives in each ring (enq_pos ticket + per-slot seq[]); no
     * per-ring mutex. K=1 = legacy. */
    struct dma_ring *dma_rings[MAX_EU_PER_POD];
    /* Reverse credit region size = rx_dma_buf_size / k_rings. The DPA reports an
     * absolute landing pos; ring_idx = pos / rx_region_size selects which ring's
     * credit slot to return. K=1 → region = whole buffer → ring_idx always 0. */
    size_t rx_region_size;
    doca_dpa_dev_mmap_t dpa_mmap_handle;  /* DPA handle for local mmap (used in TX descriptors) */

    /* Host RX buffer (PCI mmap, DPU→CPU destination) */
    void *rx_dma_buffer;
    struct doca_mmap *rx_dma_mmap;
    size_t rx_dma_buf_size;

    /* Persistent buffer for initial registration to avoid stack UAF */
    struct dmesh_register_msg reg_msg;

    /* TX buffer management — ELASTIC per-conn block chains over a SHARED lock-free
     * block pool. The shared TX mmap (num_slots * slot_size = DPU_BUFFER_SIZE bytes) is
     * carved into n_blocks blocks of block_size bytes. A conn grabs blocks on demand
     * (grow, up to maxb) and returns drained ones (keeping a cushion of cushion_h for
     * reuse — grow/shrink hysteresis), so per-conn footprint tracks in-flight demand
     * instead of a fixed region. block_size = the max contiguous message. The pool is a
     * lock-free Treiber free-list of block ids (block_free = tag<<32 | head; head ==
     * n_blocks == empty; block_next[] links). The per-conn block chain + send-unit FIFO
     * live in each dmesh_port_slot and are OWNER-thread-local while the conn is live. */
    int   block_size;          /* bytes per block (= max contiguous message = alloc unit) */
    int   n_blocks;            /* number of blocks = num_slots*slot_size / block_size */
    int   maxb;                /* max blocks a conn may own (per-conn in-flight cap) */
    int   cushion_h;           /* recycled-block cushion depth (grow/shrink hysteresis) */
    atomic_uint_fast64_t block_free;   /* Treiber head: (tag<<32) | head_index */
    uint32_t *block_next;      /* [n_blocks]: free-list links */
    pthread_mutex_t block_lock;    /* close-path block return (exactly-once handoff, cold) */
    int block_lock_initialized;
    /* Elastic-pool event counters (diagnostics; relaxed atomics — events are the
     * RARE paths: steady sliding touches none of these except recycle_hits once
     * per drained block). Read via dmesh_get_tx_stats (public). */
    atomic_ullong st_pool_grabs;    /* shared-pool CAS pops (conn grow / first block) */
    atomic_ullong st_pool_returns;  /* shared-pool CAS pushes (shrink / close drain) */
    atomic_ullong st_recycle_hits;  /* grow served from the conn's recyc[] (no pool op) */
    atomic_ullong st_grow_waits;    /* backoff sleeps in reserve (window full / pool empty) */
    atomic_ullong st_block_pads;    /* message didn't fit the block tail → pad + next block */
    /* RX-side drop counters (observability). Logged at teardown. inbox_drops should stay 0
     * now the inbox is sized to the reverse-credit budget (see DMESH_INBOX_RING_MIN);
     * a non-zero value means a conn overflowed anyway (slow drain past the credit budget). */
    atomic_ullong st_rx_inbox_drops;   /* established/pending conn inbox full → message dropped */
    atomic_ullong st_rx_accept_drops;  /* accept queue full → NEW conn dropped */

    /* RX descriptor queue — lock-free bounded SPMC ring (1 producer = PE
     * thread, N consumers = server workers). dpumesh_dequeue spin-polls it
     * (pure lock-free + adaptive backoff); a native epoll_wait() on the readiness
     * eventfd is the idle-sleep path. No mutex/cond on the RX landing path. */
    struct rxq_cell *rx_ring;          /* RX_QUEUE_SIZE cells (power of two) */
    /* rx_enq (producer-private, written every request) and rx_deq (CAS-hammered
     * by every worker) sit on separate cachelines: otherwise the PE's per-request
     * rx_enq store false-shares the line the workers CAS, bouncing it and
     * inflating CAS-retry CPU. */
    char _rx_pad0[64];
    atomic_uint_fast32_t rx_enq;       /* producer position (PE only) */
    char _rx_pad1[64];
    atomic_uint_fast32_t rx_deq;       /* consumer position (workers CAS) */
    char _rx_pad2[64];

    /* PE progress thread */
    pthread_t pe_tid;
    volatile int pe_running;

    /* CQ registry. Each CQ owns its readiness eventfd + ready list; an ESTABLISHED
     * conn's delivery wakes only its own CQ (psl->cq), which is what lets N threads
     * receive in parallel. This registry exists for the ONE delivery that has no conn
     * yet: a NEW conn goes on the shared accept queue, so EVERY CQ is notified and
     * whichever one accepts it owns it. Cold path (once per conn), hence the mutex —
     * it also makes dmesh_destroy_cq's unregister race-free against the PE. */
    struct dmesh_cq *cqs[DMESH_MAX_CQ];
    int              n_cqs;            /* high-water mark of cqs[]; slots may be NULL */
    pthread_mutex_t  cq_lock;
    int              cq_lock_initialized;

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
 * PE progress thread — drives DOCA progress engine
 * ==================================================================== */

/* Adaptive-poll tuning: spin this many empty iterations before the first
 * back-off (catches brief inter-request gaps with no latency cost), then sleep
 * a short, capped interval per empty iteration (yields the core during
 * sustained idle). */
#define PE_IDLE_SPIN     2048
#define PE_BACKOFF_NS    20000   /* 20 us */

static void *pe_progress_fn(void *arg) {
    dpumesh_ctx_t *ctx = (dpumesh_ctx_t *)arg;
    struct doca_pe *pe = ctx->doca_objs.pe;

    /* Sleep on the PE notification fd instead of spinning (baked ON). The host
     * comch PE receives ONLY real completions (REV_DONE / TX_ACK from the DPU),
     * each of which raises the notification fd — there is NO silent-wakeup path
     * here (unlike the DPU forward ring), so epoll is safe and cuts this thread's
     * idle CPU to ~0 (p99 tail win). If the epoll setup below fails it falls back
     * to the adaptive-spin loop. */
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
            while (doca_pe_progress(pe)) { /* drain all ready completions */ }

            /* Arm, then re-check once to close the drain→arm race: a completion
             * landing between the last drain and the arm must not be stranded. */
            (void)doca_pe_request_notification(pe);
            if (doca_pe_progress(pe)) {
                (void)doca_pe_clear_notification(pe, pfd);
                continue;
            }
            /* Block until a completion raises the fd, or 200 ms so a pe_running=0
             * shutdown is observed promptly (vs a busy spin). */
            struct epoll_event evs[1];
            (void)epoll_wait(ep, evs, 1, 200);
            (void)doca_pe_clear_notification(pe, pfd);
        }
        close(ep);
        return NULL;
    }

    /* ===== Fallback: adaptive spin + nanosleep (baked default) ===== */
    uint32_t idle = 0;
    while (ctx->pe_running) {
        uint8_t did = 0;
        if (pe)
            did |= doca_pe_progress(pe);
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

/* Return one unit of reverse-DMA admission credit to the DPA (it polls this
 * counter at the extra slot past the dma_ring; see dpumesh_rx_free). */
static inline void rx_credit_return(dpumesh_ctx_t *ctx, int pos)
{
    /* Per-ring credit: the reverse region a landing fell in maps 1:1 to a forward
     * ring (disjoint regions, ring j owns [j*R,(j+1)*R)). Return credit to that
     * ring's slot so the DPA EU owning rev ring j sees its own freed count. */
    int idx = (ctx->rx_region_size > 0) ? (int)((size_t)pos / ctx->rx_region_size) : 0;
    if (idx < 0 || idx >= ctx->k_rings) idx = 0;
    struct dma_ring *r = ctx->dma_rings[idx];
    if (r && r->descs) {
        volatile uint64_t *credit = (volatile uint64_t *)(r->descs + r->size);
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

/* Wake the thread blocked in a vanilla epoll_wait() on ONE CQ's readiness fd.
 * Per-delivery write (no coalescing) → cannot lose a wakeup; the eventfd is a plain
 * counter, drained by one read() per epoll wakeup. Safe to call from the PE thread.
 *
 * Gated on wants_notify: a CQ whose fd was never handed out (dmesh_cq_fd) has no thread
 * asleep on it, so the write() would only burn a syscall (and, under fair pinning, the
 * app's own core — the PE thread is a sibling of the app thread). The ready list is
 * armed regardless (arm_ready_after_push), so gating drops ONLY the redundant wakeup:
 * a poller still sees the conn. The acquire load pairs with the release store +
 * self-kick in dmesh_cq_fd so a caller that starts polling and only later sleeps on the
 * fd cannot miss an already-armed conn. */
static inline void cq_notify(struct dmesh_cq *cq)
{
    if (cq->notify_efd >= 0 &&
        atomic_load_explicit(&cq->wants_notify, memory_order_acquire)) {
        uint64_t one = 1;
        ssize_t w = write(cq->notify_efd, &one, sizeof(one));
        (void)w;
    }
}

/* Wake EVERY CQ — only for the shared accept queue, whose conns have no CQ yet, so
 * any consumer may claim them. Cold (once per new conn); the lock also keeps a
 * concurrent dmesh_destroy_cq from freeing a CQ under us. */
static void notify_all_cqs(dpumesh_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->cq_lock);
    for (int i = 0; i < ctx->n_cqs; i++)
        if (ctx->cqs[i]) cq_notify(ctx->cqs[i]);
    pthread_mutex_unlock(&ctx->cq_lock);
}

/* Ready-list SPSC: PE pushes a ready conn's port; that conn's CQ thread pops it via
 * dmesh_next_ready. Monotonic counters (count = tail-head); ring index masks the
 * power-of-two DMESH_PORT_SPACE. */
static inline void ready_push(struct dmesh_cq *cq, uint16_t port) {
    uint_fast32_t t = atomic_load_explicit(&cq->ready_tail, memory_order_relaxed);
    uint_fast32_t h = atomic_load_explicit(&cq->ready_head, memory_order_acquire);
    if (t - h >= DMESH_PORT_SPACE) return;   /* provably never full; guard anyway */
    cq->ready_ring[t & (DMESH_PORT_SPACE - 1)] = port;
    atomic_store_explicit(&cq->ready_tail, t + 1, memory_order_release);
}
static inline int ready_pop(struct dmesh_cq *cq, uint16_t *port) {
    uint_fast32_t h = atomic_load_explicit(&cq->ready_head, memory_order_relaxed);
    uint_fast32_t t = atomic_load_explicit(&cq->ready_tail, memory_order_acquire);
    if (h == t) return 0;                                    /* empty */
    *port = cq->ready_ring[h & (DMESH_PORT_SPACE - 1)];
    atomic_store_explicit(&cq->ready_head, h + 1, memory_order_release);
    return 1;
}

/* Deliver one inbound descriptor (model B — the DPU owns every connection).
 * Demux purely by the local dst_port (like a TCP demux resolving to a socket):
 *   - a LIVE conn      → its inbound SPSC ring + the READY LIST + channel fd. This
 *                        covers a client's reply on its own port (dst_port < BASE)
 *                        AND an established server conn on its DPU-assigned uP.
 *   - FREE + uP (>=BASE)→ a NEW server conn (the DPU created an upstream to me) →
 *                        accept queue; dmesh_accept binds port = dst_port = uP.
 *   - FREE + low port  → stale (conn closed) → reclaim the landing credit.
 * dst_port==BLANK never reaches a host: a client addresses a service and the DPU
 * always resolves to a concrete port before delivering. Bodies stay in the shared
 * RX mmap (pos); only the descriptor is queued. */
/* Per-conn TX region lifecycle + FIFO reclaim (defined with the TX functions below).
 * port_reset_tx is used by the PE (SERVER_PENDING) above its definition. */
static void port_reset_tx(struct dmesh_port_slot *psl);
static inline void tx_reclaim_ack(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq);

/* Ready-list arming (producer side, PE thread). Call AFTER a successful inbox_push
 * on a NON-pending conn: if the conn wasn't already armed/being-serviced, put it on
 * ITS CQ's ready list and wake THAT CQ. The seq_cst fence orders the inbox tail-store
 * (in inbox_push) before the on_ready load, pairing with the fence in
 * dpumesh_conn_recv so a push can never be lost off the ready list (Dekker). Skips
 * SERVER_PENDING (re-read here since a concurrent dmesh_accept may have promoted the
 * slot after the caller's role load) — a pending conn is drained by dmesh_accept —
 * and a conn whose CQ binding is gone (freed slot). Arming is unconditional
 * otherwise: readiness is live from dmesh_create_cq, never gated on dmesh_cq_fd. */
static inline void arm_ready_after_push(struct dmesh_port_slot *psl, uint16_t dport) {
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) == DMESH_ROLE_SERVER_PENDING) return;
    struct dmesh_cq *cq = __atomic_load_n(&psl->cq, __ATOMIC_ACQUIRE);
    if (!cq) return;
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_exchange_explicit(&psl->on_ready, 1u, memory_order_acq_rel) == 0) {
        ready_push(cq, dport);
        cq_notify(cq);
    }
}

static void rx_deliver_desc(dpumesh_ctx_t *ctx, const sw_descriptor_t *desc, int slot)
{
    uint16_t dport = desc->dst_port;
    struct dmesh_port_slot *psl = &ctx->ports[dport];
    uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);

    /* (1) a LIVE or PENDING conn → its inbound ring. A SERVER_PENDING conn (created
     * by the PE at message-1 delivery, not yet accepted) also takes the inbox so
     * pipelined messages 2..P coalesce here instead of re-hitting the accept queue.
     * The ready list is only for ACCEPTED conns (a pending conn is drained by
     * dmesh_accept, not next_ready). */
    if (role != DMESH_ROLE_FREE) {
        int r = inbox_push(psl, desc);
        if (r == 0) {
            /* inbox full (app draining too slowly) → drop + reclaim the landing. */
            atomic_fetch_add_explicit(&ctx->st_rx_inbox_drops, 1, memory_order_relaxed);
            DOCA_LOG_ERR("RX deliver: conn %u inbox full, dropping seq=%u", dport, desc->seq);
            rx_credit_return(ctx, slot);
        } else {
            /* Arm the owning CQ's ready list (flag-based, race-free — see
             * arm_ready_after_push). A SERVER_PENDING slot promoted concurrently is
             * handled inside the helper via its own role re-read; a still-pending slot
             * is skipped (drained by dmesh_accept). Single-producer (PE thread). */
            arm_ready_after_push(psl, dport);
        }
        return;
    }

    /* (2) FREE + dst_port is a DPU-assigned upstream id → a NEW server conn. Create
     * a PENDING port slot NOW (so further messages for this uP coalesce into its
     * inbox), then push message 1 to the accept queue so dmesh_accept returns it
     * first. dst_port==BLANK never reaches a host (the DPU always resolves). */
    if (dport >= DMESH_UPORT_BASE) {
        pthread_mutex_lock(&ctx->port_lock);
        if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_FREE) {
            /* raced live between the initial load and the lock → coalesce to inbox */
            pthread_mutex_unlock(&ctx->port_lock);
            int r = inbox_push(psl, desc);
            if (r == 0) { atomic_fetch_add_explicit(&ctx->st_rx_inbox_drops, 1, memory_order_relaxed);
                          rx_credit_return(ctx, slot); }
            else arm_ready_after_push(psl, dport);
            return;
        }
        if (psl->nblk_owned > 0) {
            /* FREE but the prior conn's TX blocks are still draining (this uP was
             * reused before its chain drained) → can't create a conn on it yet;
             * drop (the client retries). Rare — uPs cycle slowly. */
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
        psl->user      = NULL;
        psl->cq        = NULL;   /* no owner until a CQ accepts it (dmesh_accept binds) */
        port_reset_tx(psl);   /* fresh block-chain cursors; TX blocks grabbed LAZILY on first reply */
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
        notify_all_cqs(ctx);       /* no owner yet → every CQ may accept it */
        return;
    }

    /* (3) FREE + a low (client) port → stale (conn closed) → reclaim the landing. */
    rx_credit_return(ctx, slot);
}

/*
 * Parse + deliver one BATCH_REV_DONE entry at rx_dma_buffer[pos] whose body
 * length is dma_len. Per-message metadata (the oriented endpoint tuple) is
 * taken from the entry — NOT from the DMA payload, which is the body itself
 * (no in-band header). Returns 0 on success, -1 on malformed/undeliverable.
 */
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

static void rx_data_hook(void *hook_ctx, const uint8_t *data, uint32_t len) {
    dpumesh_ctx_t *ctx = (dpumesh_ctx_t *)hook_ctx;
    /* Dispatch on the 1-byte type at offset 0. The only DPU->Host messages this
     * hook receives are BATCH_FWD_ACK and BATCH_REV_DONE (the non-batched FWD_ACK/
     * REV_DONE types no longer exist); both carry a uint8_t type as their first
     * field and values stay < 256, so a single byte read is sufficient. */
    uint8_t mtype = data[0];


    if (mtype == DMESH_MSG_BATCH_FWD_ACK) {
        /* Batched TX_ACK: reclaim each (port,seq)'s bytes from the conn's TX
         * byte-ring (FIFO tail-reclaim). One message → K frees. */
        if (len < 4) {
            DOCA_LOG_ERR("BATCH_TX_ACK: too short (len=%u)", len);
            return;
        }
        const struct dmesh_batch_tx_ack_msg *b = (const struct dmesh_batch_tx_ack_msg *)data;
        uint32_t n = b->count;
        if (n > BATCH_TXACK_MAX) n = BATCH_TXACK_MAX;
        if (len < 4u + 4u * n) {
            DOCA_LOG_ERR("BATCH_TX_ACK: len=%u short for count=%u", len, n);
            return;
        }
        /* Each ACK advances the conn's TX ring tail (FIFO). A miss (seq !=
         * tail's) = an already-freed / duplicate / dropped-under-overload ACK →
         * harmless no-op. */
        for (uint32_t i = 0; i < n; i++)
            tx_reclaim_ack(ctx, b->acks[i].port, b->acks[i].seq);
        return;
    }


    if (mtype == DMESH_MSG_BATCH_REV_DONE) {
        /* Batched reverse-DMA notification: deliver each entry. One reaped comch
         * msg → K deliveries, so the single PE thread reaps 1/K — the 2-pod lever. */
        if (len < 4) {
            DOCA_LOG_ERR("BATCH_REV_DONE: too short (len=%u)", len);
            return;
        }
        const struct dmesh_batch_rev_done_msg *b = (const struct dmesh_batch_rev_done_msg *)data;
        uint32_t n = b->count;
        if (n > BATCH_REVDONE_MAX) n = BATCH_REVDONE_MAX;
        if (len < 4u + 16u * n) {
            DOCA_LOG_ERR("BATCH_REV_DONE: len=%u short for count=%u", len, n);
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            /* Bounds/size validation happens inside process_rx_dma_entry. */
            const struct dmesh_rev_done_entry *e = &b->entries[i];
            if (process_rx_dma_entry(ctx, e) != 0)
                DOCA_LOG_WARN("BATCH_REV_DONE: process_rx_dma_entry failed pos=%u len=%u",
                              e->pos, e->length);
        }
        return;
    }
}

static void init_config(dpumesh_ctx_t *ctx, const dpumesh_config_t *config, int service_id) {
    const char *env_val;

    /* num_slots (32 MB / 8 KB = 4096) and slot_size (8 KB, the DPA dma_copy limit)
     * are baked; a programmatic config override still wins. */
    ctx->num_slots = (config && config->num_slots > 0) ? config->num_slots
                                                       : DPUMESH_NUM_SLOTS_DEFAULT;
    ctx->slot_size = (config && config->slot_size > 0) ? config->slot_size
                                                       : DPUMESH_SLOT_SIZE_DEFAULT;

    /* K = forward rings per pod (EU-sharding). Deploy option DPUMESH_RINGS_PER_POD
     * (default 2) — MUST match the DPU's K (forward rings pair 1:1; a mismatch stalls
     * dma_ready). A conn pins to ONE ring (src_port % K); conns spread across K.
     * K ≤ N (=4 on the DPU); values > MAX_EU_PER_POD are clamped. */
    ctx->k_rings = DPUMESH_RINGS_PER_POD_DEFAULT;
    { const char *ke = getenv("DPUMESH_RINGS_PER_POD");
      if (ke && *ke) { int v = atoi(ke);
                       if (v >= 1 && v <= MAX_EU_PER_POD) ctx->k_rings = v; } }

    /* Per-conn inbox depth = the DPU's per-region reverse-credit budget, so a hot conn
     * cannot overflow its inbox before the DPU's credit runs out (which back-pressures
     * cleanly instead of the SILENT drop in rx_deliver_desc). The DPU caps in-flight
     * landings per region at rq = rq_depth/K, rq_depth = host_rx_bytes/DPUMESH_SLOT_SIZE
     * = num_slots*slot_size/DPUMESH_SLOT_SIZE (comch_common.c). Round up to a power of two
     * (the ring is masked), floor at DMESH_INBOX_RING_MIN, cap so one conn's inbox stays
     * bounded. DPUMESH_INBOX_RING overrides the computed depth (still pow2 + floored). */
    {
        uint64_t rq_depth = (uint64_t)ctx->num_slots * (uint64_t)ctx->slot_size
                            / (uint64_t)DPUMESH_SLOT_SIZE;
        uint32_t k = ctx->k_rings > 0 ? (uint32_t)ctx->k_rings : 1u;
        uint64_t want = rq_depth / k;
        if ((env_val = getenv("DPUMESH_INBOX_RING")) != NULL && atoi(env_val) > 0)
            want = (uint64_t)atoi(env_val);
        if (want > 65536u) want = 65536u;                  /* cap ≈ 65536 × 24B = 1.5MB/conn */
        uint32_t r = DMESH_INBOX_RING_MIN;
        while ((uint64_t)r < want) r <<= 1;                /* round up to pow2, >= floor */
        ctx->inbox_ring = (int)r;
    }

    /* ELASTIC TX blocks. block_size (default 64 KB, >= slot_size) = the max contiguous
     * message = the allocation unit; the num_slots*slot_size TX buffer holds n_blocks =
     * (num_slots*slot_size)/block_size of them in a shared pool. maxb = per-conn block
     * cap (default 4 => 256 KB in-flight/conn); cushion_h = recycled-block cushion
     * (grow/shrink hysteresis, default 1). Env: DPUMESH_TX_BLOCK / _TX_MAXB / _TX_H. */
    int bsz = DPUMESH_TX_BLOCK_DEFAULT;
    if ((env_val = getenv("DPUMESH_TX_BLOCK")) != NULL && atoi(env_val) > 0)
        bsz = atoi(env_val);
    if (bsz < ctx->slot_size) bsz = ctx->slot_size;    /* a block must hold >= 1 wire slot */
    size_t txbytes = (size_t)ctx->num_slots * (size_t)ctx->slot_size;
    if ((size_t)bsz > txbytes) bsz = (int)txbytes;
    /* ...and at most TX_SU_DEPTH wire slots. This one is load-bearing: a message may be
     * block_size long and carves into ceil(block_size/slot_size) descriptors, so without
     * this clamp a single max-size message could need more FIFO than exists and
     * dpumesh_tx_reserve's send-unit admission would refuse it forever. */
    if ((size_t)bsz > (size_t)TX_SU_DEPTH * (size_t)ctx->slot_size)
        bsz = (int)((size_t)TX_SU_DEPTH * (size_t)ctx->slot_size);
    ctx->block_size = bsz;
    ctx->n_blocks   = (int)(txbytes / (size_t)bsz);
    if (ctx->n_blocks < 1) ctx->n_blocks = 1;

    int mb = DPUMESH_TX_MAXB_DEFAULT;
    if ((env_val = getenv("DPUMESH_TX_MAXB")) != NULL && atoi(env_val) > 0)
        mb = atoi(env_val);
    if (mb < 1) mb = 1;
    if (mb > DMESH_TX_MAXB_CAP) mb = DMESH_TX_MAXB_CAP;
    if (mb > ctx->n_blocks) mb = ctx->n_blocks;        /* can't own more than the whole pool */
    ctx->maxb = mb;

    int hh = DPUMESH_TX_H_DEFAULT;
    if ((env_val = getenv("DPUMESH_TX_H")) != NULL && atoi(env_val) >= 0)
        hh = atoi(env_val);
    if (hh < 0) hh = 0;
    if (hh > mb) hh = mb;                              /* cushion can't exceed the per-conn cap */
    ctx->cushion_h = hh;

    /* This node's service id (what it advertises: the DPU stores it on our pod_state
     * and DERIVES the service's backend set by scanning pods[] for it — there is no
     * service->backend table). SVC_NONE = client-only. Comes SOLELY from the caller,
     * which resolved it from $DPUMESH_SERVICE via the registry (dmesh_create_channel).
     * The DPUMESH_SERVICE_ID int override was DELETED (NAMING.md §2): an int identity
     * surviving next to a name is the two-sources-of-identity defect this removes.
     * The pod_id (this node's address) is NO LONGER host-chosen — the DPU assigns it
     * at registration (see init_control_path). It stays -1 until POD_ASSIGNED arrives. */
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

static doca_error_t wait_for_pod_init_result(dpumesh_ctx_t *ctx) {
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000 };
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int32_t init_result = __atomic_load_n(&ctx->doca_objs.pod_init_result,
                                              __ATOMIC_ACQUIRE);
        if (init_result == DMESH_POD_INIT_READY) {
            DOCA_LOG_INFO("DPU pod is data-ready: pod_id=%d", ctx->pod_id);
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
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (init_elapsed_ms(&start, &now) >= DPUMESH_POD_INIT_TIMEOUT_MS) {
            DOCA_LOG_ERR("Timed out after %d ms waiting for DPU pod readiness "
                         "(pod_id=%d, rings=%d)",
                         DPUMESH_POD_INIT_TIMEOUT_MS, ctx->pod_id, ctx->k_rings);
            return DOCA_ERROR_TIME_OUT;
        }
    }
}

static doca_error_t init_control_path(dpumesh_ctx_t *ctx) {
    doca_error_t result;

    fprintf(stderr, "[dpumesh] Connecting comch client...\n");
    result = init_comch_ctrl_path_client("DPUMesh", &ctx->doca_objs, true);
    if (result != DOCA_SUCCESS) return result;

    /* Register with pod_id = -1: the DPU allocates our pod_id and replies with
     * DMESH_MSG_POD_ASSIGNED. Block until it arrives, progressing the PE by hand
     * (the PE thread isn't started until the end of dpumesh_init). */
    __atomic_store_n(&ctx->doca_objs.assigned_pod_id, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->doca_objs.pod_init_result, DMESH_POD_INIT_PENDING,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->doca_objs.pod_quiesced, 0, __ATOMIC_RELEASE);
    ctx->reg_msg.type = DMESH_MSG_POD_REGISTER;
    ctx->reg_msg.pod_id = -1;                    /* DPU assigns this node's address */
    ctx->reg_msg.service_id = ctx->service_id;   /* DPU: pods[our slot].service_id = this (the LB set is derived from it) */

    result = client_send_msg(&ctx->doca_objs, (const char *)&ctx->reg_msg, sizeof(ctx->reg_msg));
    if (result != DOCA_SUCCESS) return result;
    DOCA_LOG_INFO("Sent REGISTER to DPU: service_id=%d (awaiting pod_id)", ctx->service_id);

    /* Wait for phase 1 (address assignment). A registration failure may arrive
     * as POD_INIT_RESULT before POD_ASSIGNED, so check both atomics. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000 };   /* 10 us */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
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
        nanosleep(&ts, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (init_elapsed_ms(&start, &now) >= DPUMESH_POD_INIT_TIMEOUT_MS)
            break;
    }
    if (assigned < 0) {
        DOCA_LOG_ERR("Timed out after %d ms waiting for DPU pod_id assignment "
                     "(service_id=%d)", DPUMESH_POD_INIT_TIMEOUT_MS, ctx->service_id);
        return DOCA_ERROR_TIME_OUT;
    }
    ctx->pod_id = assigned;
    DOCA_LOG_INFO("DPU assigned pod_id=%d (service_id=%d)", ctx->pod_id, ctx->service_id);
    return DOCA_SUCCESS;
}

static doca_error_t init_datapath(dpumesh_ctx_t *ctx) {
    doca_error_t result;

    /* Host datapath consumer intentionally NOT created: its ID is never advertised
     * to the DPU and the host PE thread never progresses consumer_pe, so it would
     * only waste ~2 GiB (CC_DATA_PATH_MSG_SIZE × CC_DATA_PATH_TASK_NUM). CPU→DPU
     * uses the DMA ring; DPU→CPU uses reverse DMA; DPU→Host control (BATCH_FWD_ACK /
     * BATCH_REV_DONE) arrives on the comch control-path client (objs->pe). */

    /* K forward descriptor rings, exported in order as DMA_RING (sent BEFORE
     * DMA_BUFFER so the DPU's setup trigger sees all K before pairing). */
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

    /* Allocate Host RX DMA buffer (PCI mmap, DPA writes DPU→CPU data here).
     * Partitioned into k_rings disjoint reverse regions (rx_region_size each). */
    ctx->rx_dma_buf_size = buf_size;
    ctx->rx_region_size = ctx->rx_dma_buf_size / (ctx->k_rings > 0 ? ctx->k_rings : 1);
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
    int prc = pthread_mutex_init(&ctx->cq_lock, NULL);
    if (prc != 0) { errno = prc; goto fail; }
    ctx->cq_lock_initialized = 1;

    init_config(ctx, config, service_id);

    /* These limits are data-plane ABI, not tuning preferences. The DPA copies at
     * most 8 KiB per descriptor, TX byte offsets mirror into a fixed 32 MiB DPU
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
     * to n_blocks (the empty sentinel). Per-conn send-unit FIFOs (su_seq/su_end) are
     * lazily malloc'd per port slot (kept for the slot's life). */
    ctx->block_next = (uint32_t *)malloc((size_t)ctx->n_blocks * sizeof(uint32_t));
    if (!ctx->block_next) { errno = ENOMEM; goto fail; }
    for (int i = 0; i < ctx->n_blocks; i++)
        ctx->block_next[i] = (uint32_t)(i + 1);          /* last -> n_blocks (empty sentinel) */
    atomic_init(&ctx->block_free, (uint_fast64_t)0);     /* tag 0, head index 0 */
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
     * role=FREE, nblk_owned=0 (holds no TX blocks), su NULL, cursors 0. pblk[] must
     * start -1 (0 is a valid block id); a conn grabs its first block LAZILY on the
     * first write (no eager borrow at connect/accept). */
    ctx->ports = (struct dmesh_port_slot *)calloc(DMESH_PORT_SPACE, sizeof(struct dmesh_port_slot));
    if (!ctx->ports) { errno = ENOMEM; goto fail; }
    for (uint32_t p = 0; p < DMESH_PORT_SPACE; p++)
        for (int b = 0; b < DMESH_TX_MAXB_CAP; b++)
            ctx->ports[p].pblk[b] = -1;
    prc = pthread_mutex_init(&ctx->port_lock, NULL);
    if (prc != 0) { errno = prc; goto fail; }
    ctx->port_lock_initialized = 1;
    ctx->next_port = 1;
    ctx->port_span = DMESH_PORT_SPAN_MIN;

    ctx->doca_objs.rx_data_hook = rx_data_hook;
    ctx->doca_objs.rx_hook_ctx = ctx;

    ctx->pe_running = 1;
    prc = pthread_create(&ctx->pe_tid, NULL, pe_progress_fn, ctx);
    if (prc != 0) {
        ctx->pe_running = 0;   /* cleanup must not join a never-created thread */
        errno = prc;
        goto fail;
    }

    DOCA_LOG_INFO("DPUmesh DOCA initialized: worker=%s pod_id=%d inbox_ring=%d (rq/K, was 256)",
                  ctx->worker_id, ctx->pod_id, ctx->inbox_ring);

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

/* Graceful remote-resource barrier. Keep the normal PE progress thread alive
 * while waiting: Comch notification delivery is owned by that thread for the
 * lifetime of a running channel, and switching the already-armed PE to ad-hoc
 * progress on the destroying thread can strand POD_QUIESCED. If initialization
 * failed before the PE thread was started, this function instead progresses the
 * PE synchronously. Keep every exported mmap alive until the DPU confirms that
 * DPA rings, DPA producer DMAs, ARM SG-DMAs and imported handles are gone. */
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
    doca_error_t result = client_send_msg(objs, (const char *)&msg, sizeof(msg));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_WARN("POD_UNREGISTER send failed for pod_id=%d: %s",
                      pod_id, doca_error_get_name(result));
        return -1;
    }

    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000 };
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (!ctx->pe_running && objs->pe != NULL)
            (void)doca_pe_progress(objs->pe);
        if (__atomic_load_n(&objs->pod_quiesced, __ATOMIC_ACQUIRE)) {
            DOCA_LOG_INFO("DPU remote resources quiesced: pod_id=%d", pod_id);
            return 0;
        }
        result = require_running_control_path(ctx);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_WARN("Control path stopped while awaiting POD_QUIESCED: %s",
                          doca_error_get_name(result));
            return -1;
        }
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (init_elapsed_ms(&start, &now) >= DPUMESH_POD_CLEANUP_TIMEOUT_MS) {
            DOCA_LOG_WARN("Timed out after %d ms waiting for POD_QUIESCED "
                          "(pod_id=%d); falling back to disconnect cleanup",
                          DPUMESH_POD_CLEANUP_TIMEOUT_MS, pod_id);
            return -1;
        }
    }
}

static void cleanup_ctx(dpumesh_ctx_t *ctx) {
    if (!ctx) return;

    /* QPs/channels are already gone, so no new application traffic can appear.
     * Receive the remote teardown ACK on the same notification/progress thread
     * that has owned the Comch PE throughout the channel lifetime. */
    (void)request_remote_pod_quiesce(ctx);

    if (ctx->pe_running) {
        ctx->pe_running = 0;
        pthread_join(ctx->pe_tid, NULL);
    }

    /* PE thread joined → no more cq_notify() writers. Surviving CQs (an app that
     * dropped the channel without destroying them) are reaped here. */
    for (int i = 0; i < ctx->n_cqs; i++) {
        struct dmesh_cq *cq = ctx->cqs[i];
        if (!cq) continue;
        if (cq->notify_efd >= 0) close(cq->notify_efd);
        ctx->cqs[i] = NULL;
        free(cq);
    }
    if (ctx->cq_lock_initialized) {
        pthread_mutex_destroy(&ctx->cq_lock);
        ctx->cq_lock_initialized = 0;
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
            if (ctx->ports[p].su_seq) { free(ctx->ports[p].su_seq); ctx->ports[p].su_seq = NULL; }
            if (ctx->ports[p].su_end) { free(ctx->ports[p].su_end); ctx->ports[p].su_end = NULL; }
        }
    }

    /* Destroy EVERY host mmap while the DOCA device is still open. The old path
     * freed only RX and leaked K ring mmaps plus the TX mmap, leaving dev_close
     * permanently IN_USE. destroy_mmap_and_free_buffer deliberately keeps memory
     * allocated if DOCA refuses the mmap destroy, avoiding DMA into freed memory. */
    if (ctx->rx_dma_mmap && ctx->rx_dma_buffer) {
        if (destroy_mmap_and_free_buffer(ctx->rx_dma_mmap,
                                         ctx->rx_dma_buffer) == DOCA_SUCCESS) {
            ctx->rx_dma_mmap = NULL;
            ctx->rx_dma_buffer = NULL;
        }
    }

    for (int j = 0; j < MAX_EU_PER_POD; j++) {
        struct dma_ring *ring = ctx->dma_rings[j];
        if (!ring) continue;
        if (ring->mmap && ring->descs &&
            destroy_mmap_and_free_buffer(ring->mmap, ring->descs) == DOCA_SUCCESS) {
            ring->mmap = NULL;
            ring->descs = NULL;
        }
        free(ring->seq);
        ring->seq = NULL;
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
    DOCA_LOG_INFO("Destroying DPUmesh context: worker=%s", ctx->worker_id);
    {   /* RX-drop summary (should be 0/0 — inbox sized to the reverse-credit budget) */
        unsigned long long idr = atomic_load_explicit(&ctx->st_rx_inbox_drops, memory_order_relaxed);
        unsigned long long adr = atomic_load_explicit(&ctx->st_rx_accept_drops, memory_order_relaxed);
        if (idr || adr)
            DOCA_LOG_WARN("RX drops at teardown: inbox_full=%llu accept_full=%llu (MESSAGES LOST)", idr, adr);
        else
            DOCA_LOG_INFO("RX drops at teardown: inbox_full=0 accept_full=0");
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
            return;
    }
}

/* Reset a slot's TX block-chain to a fresh (empty) conn: cursors 0, no blocks held.
 * NO block is grabbed here — the first block is taken LAZILY on the first tx_reserve.
 * su_seq/su_end (if already malloc'd for this slot) are kept and reused. */
static void port_reset_tx(struct dmesh_port_slot *psl) {
    psl->tx_w = psl->tx_c = psl->tx_s = 0;
    atomic_store_explicit(&psl->tx_f, 0, memory_order_relaxed);
    psl->tail_blk      = 0;
    psl->head_blk_next = 0;
    psl->nblk_owned    = 0;
    psl->nrec          = 0;
    for (int b = 0; b < DMESH_TX_MAXB_CAP; b++) psl->pblk[b] = -1;
    atomic_store_explicit(&psl->su_head, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->su_tail, 0, memory_order_relaxed);
}

/* OWNER-only (live conn): recycle drained tail blocks into recyc, compact a fully-
 * drained conn back to logical 0, and shrink surplus recyc (> cushion_h) to the pool.
 * Called from reserve before the grow decision, so steady sliding reuses drained
 * blocks (0 pool ops) and only a net demand change grabs/returns. */
static void tx_refresh_blocks(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl) {
    uint64_t bs = (uint64_t)ctx->block_size, maxb = (uint64_t)ctx->maxb;
    uint64_t f = atomic_load_explicit(&psl->tx_f, memory_order_acquire);
    uint64_t f_blk = f / bs;
    while (psl->tail_blk < f_blk) {                        /* logical block fully freed → recycle */
        int s = (int)(psl->tail_blk % maxb);
        if (psl->pblk[s] >= 0) { psl->recyc[psl->nrec++] = psl->pblk[s]; psl->pblk[s] = -1; }
        psl->tail_blk++;
    }
    /* Full-drain compaction: nothing live AND nothing in flight → recycle the head block
     * too and reset the chain to logical 0 (safe: su empty ⇒ no concurrent tx_f writer). */
    if (f == psl->tx_w &&
        atomic_load_explicit(&psl->su_head, memory_order_relaxed) ==
        atomic_load_explicit(&psl->su_tail, memory_order_acquire)) {
        int s = (int)(f_blk % maxb);
        if (psl->pblk[s] >= 0) { psl->recyc[psl->nrec++] = psl->pblk[s]; psl->pblk[s] = -1; }
        psl->tx_w = psl->tx_c = psl->tx_s = 0;
        atomic_store_explicit(&psl->tx_f, 0, memory_order_relaxed);
        psl->tail_blk = 0;
        psl->head_blk_next = 0;
    }
    while (psl->nrec > ctx->cushion_h) {                   /* shrink: return surplus to the pool */
        block_pool_return(ctx, psl->recyc[--psl->nrec]);
        psl->nblk_owned--;
    }
}

/* Return a CLOSED conn's remaining blocks once fully drained (tx_f == tx_w). Called by
 * free_port (owner, after publishing role=FREE) and tx_reclaim_ack (PE, on the last ACK).
 * role==FREE (acquire) FIRST so the owner's final writes are visible; the block_lock +
 * nblk_owned>0 recheck make exactly one caller return them. Until then the port stays
 * FREE-but-draining (nblk_owned>0) and the alloc paths skip it (no reuse pre-drain). */
static void try_return_blocks(dpumesh_ctx_t *ctx, struct dmesh_port_slot *psl) {
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_FREE) return;  /* live */
    if (psl->nblk_owned <= 0) return;                                              /* none/returned */
    if (psl->tx_w != atomic_load_explicit(&psl->tx_f, memory_order_acquire)) return; /* not drained */
    pthread_mutex_lock(&ctx->block_lock);
    if (psl->nblk_owned > 0 &&
        psl->tx_w == atomic_load_explicit(&psl->tx_f, memory_order_acquire)) {
        uint64_t maxb = (uint64_t)ctx->maxb;
        for (uint64_t k = psl->tail_blk; k < psl->head_blk_next; k++) {  /* all assigned blocks */
            int s = (int)(k % maxb);
            if (psl->pblk[s] >= 0) { block_pool_return(ctx, psl->pblk[s]); psl->pblk[s] = -1; }
        }
        while (psl->nrec > 0) block_pool_return(ctx, psl->recyc[--psl->nrec]);
        psl->nblk_owned = 0;
    }
    pthread_mutex_unlock(&ctx->block_lock);
}

/* ---- Per-conn TX BYTE-RING over the block chain: reserve → commit → send → (ACK) free ---- */

/* Reserve `len` CONTIGUOUS bytes (ONE message, <= block_size) at this conn's write head,
 * returning a pointer into the shared TX mmap to fill (then dpumesh_tx_commit). A message
 * that would straddle the current block's end pads the tail and starts a fresh block, so
 * each message is contiguous in one physical block. Grabs a block on demand (GROW, up to
 * maxb) — reusing a recycled block first, else the shared pool.
 *
 * NEVER BLOCKS — the ibv_post_send contract. NULL + errno=EAGAIN when the conn's block
 * window is full (its own TX_ACKs will free one; retry later), NULL + errno=EINVAL for a
 * permanent argument/state error. Owner thread only. */
uint8_t *dpumesh_tx_reserve(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len) {
    if (port == 0 || port >= DMESH_PORT_SPACE) { errno = EINVAL; return NULL; }
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint64_t bs = (uint64_t)ctx->block_size, maxb = (uint64_t)ctx->maxb;
    if (len == 0 || (uint64_t)len > bs) { errno = EINVAL; return NULL; }  /* must fit a block */
    if (!psl->su_seq) {                                    /* lazy per-slot send-unit FIFO */
        psl->su_seq = (uint16_t *)malloc(TX_SU_DEPTH * sizeof(uint16_t));
        psl->su_end = (uint64_t *)malloc(TX_SU_DEPTH * sizeof(uint64_t));
        if (!psl->su_seq || !psl->su_end) { errno = ENOMEM; return NULL; }
    }

    /* SEND-UNIT ADMISSION. The block window does NOT bound the FIFO: dpumesh_tx_next_send
     * carves [tx_s, tx_c), which a post-per-flush leaves exactly one message long, so a
     * 64 B message costs 1/1024 of a block but a WHOLE FIFO slot. Gate on the FIFO itself.
     *
     * Reserve this message's whole carve — ceil(len/slot_size) descriptors — not one: the
     * flush that follows emits all of them with no further admission point. Over-counts
     * for a DMESH_SEND_MORE batch (whose carve coalesces across messages), which is the
     * safe direction. The block_size <= TX_SU_DEPTH * slot_size clamp at init makes the
     * worst-case single message always fit an empty FIFO, so this cannot deadlock. */
    uint32_t need_su = (len + (uint32_t)ctx->slot_size - 1) / (uint32_t)ctx->slot_size;
    uint16_t suh = atomic_load_explicit(&psl->su_head, memory_order_relaxed);
    uint16_t sut = atomic_load_explicit(&psl->su_tail, memory_order_acquire);
    if ((uint32_t)(uint16_t)(suh - sut) + need_su > TX_SU_DEPTH) {
        atomic_fetch_add_explicit(&ctx->st_grow_waits, 1, memory_order_relaxed);
        errno = EAGAIN;
        return NULL;
    }

    tx_refresh_blocks(ctx, psl);                           /* recycle / compact / shrink first */

    /* Probe the block window BEFORE mutating tx_w: on EAGAIN the conn's write head must
     * be exactly where it was, so the caller's retry is a clean no-op. (Padding first and
     * failing after would strand the padded tail.) */
    uint64_t k   = psl->tx_w / bs;
    uint32_t off = (uint32_t)(psl->tx_w % bs);
    int      pad = ((uint64_t)off + len > bs);             /* won't fit → needs a fresh block */
    uint64_t need_k = pad ? k + 1 : k;
    if (psl->head_blk_next <= need_k) {                    /* a new block must be backed */
        uint64_t b = psl->head_blk_next;
        /* Block b is admissible only once b-maxb has drained: that both caps in-flight
         * bytes per conn and keeps slot b%maxb from aliasing a still-live block. */
        int have = (b - psl->tail_blk < maxb) &&
                   (psl->nrec > 0 || psl->nblk_owned < ctx->maxb);
        if (!have) {
            atomic_fetch_add_explicit(&ctx->st_grow_waits, 1, memory_order_relaxed);
            errno = EAGAIN;
            return NULL;
        }
    }

    if (pad) {                                             /* commit to the pad: we WILL succeed */
        psl->blk_used[k % maxb] = off;                     /* seal block k content end */
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
        int bslot = (int)(b % maxb);
        if (b - psl->tail_blk >= maxb) { errno = EAGAIN; return NULL; }
        if (psl->nrec > 0) {                               /* reuse a recycled block (no pool op) */
            psl->pblk[bslot] = psl->recyc[--psl->nrec];
            atomic_fetch_add_explicit(&ctx->st_recycle_hits, 1, memory_order_relaxed);
        } else {
            int32_t phys = (psl->nblk_owned < ctx->maxb) ? block_pool_grab(ctx) : -1;
            if (phys < 0) {                                /* pool momentarily empty */
                atomic_fetch_add_explicit(&ctx->st_grow_waits, 1, memory_order_relaxed);
                errno = EAGAIN;
                return NULL;
            }
            psl->pblk[bslot] = phys;
            psl->nblk_owned++;
        }
        psl->blk_used[bslot] = 0;
        psl->head_blk_next = b + 1;
    }
    psl->resv_len = len;                                   /* commit clamps to this (ring-overrun guard) */
    int s = (int)(k % maxb);
    return (uint8_t *)ctx->dma_buffer + (size_t)psl->pblk[s] * (size_t)bs + off;
}

/* Finalize `len` bytes (<= the reserved len) as committed message bytes, ready to ship.
 * Advances tx_w + tx_c and records the block's content end. Consumes the reserve — one
 * commit per dpumesh_tx_reserve. 0 = committed, -1 = no live reserve or len > it (a
 * caller-contract break; nothing is mutated). Owner thread. */
int dpumesh_tx_commit(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (psl->nblk_owned <= 0) return -1;
    /* REJECT, never clamp. Clamping to resv_len looks defensive but is worse than the
     * bug it hides: resv_len is the LAST reserve's length, so a commit with no live
     * reserve (post without alloc, or a second post of one alloc) silently passed and
     * shipped whatever bytes happened to sit at the write head — uninitialised ring
     * memory, on the wire, with no error anywhere. */
    if (psl->resv_len == 0 || len > psl->resv_len) return -1;
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t k = psl->tx_w / bs;                           /* block the reserve placed the body in */
    psl->tx_w += len;
    psl->tx_c  = psl->tx_w;
    psl->blk_used[k % (uint64_t)ctx->maxb] = (uint32_t)(psl->tx_w - k * bs);  /* content end in block k */
    psl->resv_len = 0;                                     /* reserve consumed: one post per alloc */
    return 0;
}

/* Discard committed-but-UNSENT bytes (close-before-flush): rewind commit + write heads to
 * the send head. Shipped bytes (in flight) are untouched; abandoned blocks are returned
 * at close. Owner thread. */
void dpumesh_tx_discard_unsent(dpumesh_ctx_t *ctx, uint16_t port) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (psl->nblk_owned <= 0) return;
    psl->tx_c = psl->tx_s;
    psl->tx_w = psl->tx_s;
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t k = psl->tx_s / bs;
    psl->blk_used[k % (uint64_t)ctx->maxb] = (uint32_t)(psl->tx_s - k * bs);
}

/* Get the next descriptor to ship from [tx_s, tx_c): its byte offset in the shared mmap
 * (*out_moff) and length (*out_len, <= slot_size, never crossing a block boundary or a
 * pad). Skips a padded block tail transparently. 1 if one, 0 if nothing. Does NOT advance
 * tx_s (call dpumesh_tx_sent). Owner thread.
 *
 * INFALLIBLE on capacity: dpumesh_tx_reserve admitted this message only after reserving
 * its whole carve in the send-unit FIFO, so every descriptor it yields is guaranteed a
 * slot. Hence no capacity check (and no backoff loop) here. */
int dpumesh_tx_next_send(dpumesh_ctx_t *ctx, uint16_t port, size_t *out_moff, uint32_t *out_len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (psl->nblk_owned <= 0) return 0;
    uint64_t bs = (uint64_t)ctx->block_size, maxb = (uint64_t)ctx->maxb;
    for (;;) {
        if (psl->tx_s >= psl->tx_c) return 0;              /* nothing committed to ship */
        uint64_t k = psl->tx_s / bs;
        uint32_t off = (uint32_t)(psl->tx_s % bs);
        uint32_t used = psl->blk_used[k % maxb];
        if (off >= used) {                                 /* block k content exhausted → skip pad */
            psl->tx_s = (k + 1) * bs;                       /* jump to the next block start */
            continue;
        }
        uint64_t content_end = k * bs + (uint64_t)used;    /* content end within block k */
        uint64_t limit = (psl->tx_c < content_end) ? psl->tx_c : content_end;
        uint64_t avail = limit - psl->tx_s;
        uint32_t chunk = (avail < (uint64_t)ctx->slot_size) ? (uint32_t)avail : (uint32_t)ctx->slot_size;
        *out_moff = (size_t)psl->pblk[k % maxb] * (size_t)bs + off;
        *out_len  = chunk;
        return 1;
    }
}

/* Record a shipped descriptor's (seq -> end cursor) in the per-conn send-unit FIFO and
 * advance the send head. A BATCH_FWD_ACK(port,seq) later pops it (FIFO) to advance tx_f.
 * `len` = the descriptor length (0 for a FIN — holds no bytes). Owner thread. */
void dpumesh_tx_sent(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq, uint32_t len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (psl->nblk_owned <= 0 || !psl->su_seq) return;
    uint16_t h = atomic_load_explicit(&psl->su_head, memory_order_relaxed);
    size_t idx = (size_t)(h & (TX_SU_DEPTH - 1));
    psl->tx_s += len;
    psl->su_seq[idx] = seq;
    psl->su_end[idx] = psl->tx_s;                          /* end cursor after this unit */
    atomic_store_explicit(&psl->su_head, (uint_fast16_t)(h + 1), memory_order_release);
}

/* Reclaim on BATCH_FWD_ACK(port,seq): advance the free cursor tx_f past every send-unit
 * the ACK implies is done. The PE advances tx_f/su_tail ONLY; the block chain is
 * owner-managed while live, so this returns blocks only for a CLOSED conn.
 *
 * CUMULATIVE, not exact-front: forward completion is in-order per conn (its messages all
 * ride ONE forward ring, drained by one EU), so an ACK for `seq` means every earlier
 * still-outstanding unit also completed. We therefore pop every FIFO entry whose seq is
 * at-or-before `seq` within the ≤TX_SU_DEPTH outstanding window, not just an exact-front
 * match. This TOLERATES A DROPPED intermediate ACK (the DPU drops one on pending_txack
 * overflow) — the old exact-front match would then never advance and wedge the conn's TX
 * forever. A stale/duplicate ACK falls outside the window and pops nothing (no-op). */
static inline void tx_reclaim_ack(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint16_t tail = atomic_load_explicit(&psl->su_tail, memory_order_relaxed);
    uint16_t head = atomic_load_explicit(&psl->su_head, memory_order_acquire);
    if (tail == head) return;                              /* nothing outstanding (su may be NULL) */
    /* head != tail ⇒ the owner shipped ⇒ su_seq/su_end are allocated + visible (the
     * su_head acquire orders the owner's lazy malloc before this). FIFO seqs ascend, so
     * the popped set is a contiguous prefix ending at-or-before `seq`. */
    uint64_t newf = 0;
    int popped = 0;
    while (tail != head) {
        size_t idx = (size_t)(tail & (TX_SU_DEPTH - 1));
        /* su_seq[idx] within (seq-TX_SU_DEPTH, seq]? distance wraps large if it is a
         * FUTURE seq (> seq) or a stale ACK (< tail's seq) → stop. */
        if ((uint16_t)(seq - psl->su_seq[idx]) >= TX_SU_DEPTH) break;
        newf = psl->su_end[idx];
        tail = (uint16_t)(tail + 1);
        popped++;
    }
    if (popped) {
        atomic_store_explicit(&psl->tx_f, newf, memory_order_release);
        atomic_store_explicit(&psl->su_tail, (uint_fast16_t)tail, memory_order_release);
        try_return_blocks(ctx, psl);                       /* return blocks if this drained a CLOSED conn */
    }
}

/* Fail-safe, NOT a flow-control knob: a cell frees in microseconds while any consumer
 * exists, so only a ring with NO consumer can reach this. Turns an unbounded hang
 * into a loud error. */
#define RING_STALL_DEADLINE_SEC 5

int dpumesh_enqueue(dpumesh_ctx_t *ctx, const sw_descriptor_t *desc) {
    struct dma_desc *dma;
    uint32_t ring_slot;

    if (desc == NULL) {
        DOCA_LOG_ERR("ENQUEUE rejected: desc is NULL");
        return -1;
    }

    /* Loopback (dst == self) is NOW supported: demux is by dst_port (client vs
     * server socket), not by req_id origin, so a self-routed request and its reply
     * are distinguished even on the same host. The DPU may also route a service to
     * the sender's own pod. No reject here. */

    /* body_buf_slot now carries a BYTE OFFSET into the shared TX mmap (per-conn
     * byte-ring), not a slot index. Bounds-check the offset + length. */
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

    /* EU-sharding: round-robin this request across the K forward rings, so the
     * pod's traffic spreads over K EUs. Each ring has its own lock (single-
     * producer per ring). K=1 → always ring 0 (legacy single-ring path).
     *
     * Flow control: end-to-end via byte-ring admission only. dpumesh_tx_reserve
     * has already back-pressured the writer on the conn's own un-ACKed bytes, and
     * num_slots × slot_size = DPU_BUFFER_SIZE, so total in-flight bytes inside DPU's
     * buffer can never exceed buffer size. DPU/DPA do no FC of their own. */
    /* conn-sharding: a connection's messages ALL use one ring (hashed by the
     * sender's local port), so they stay FIFO on one EU → per-conn send order is
     * preserved; different conns spread across the K rings/EUs (throughput kept). */
    int ridx = (int)((unsigned)desc->src_port % (unsigned)ctx->k_rings);
    struct dma_ring *ring = ctx->dma_rings[ridx];

    /* Lock-free MPSC claim (Vyukov bounded queue) — replaces the per-ring mutex.
     * enq_pos hands out a monotonic ticket t; the producer owns slot t%size for
     * generation t. It may write when the cell is free for t: seq==t (first use /
     * already reclaimed) OR the previous occupant (ticket t-size) has PUBLISHED
     * (seq==t-size+1) AND the DPA has CONSUMED it (valid==0) → reclaim. A stalled
     * producer leaves seq unadvanced, so a lapping producer WAITS here instead of
     * overwriting the slot — generation-safe with just the `valid` flag, no lock,
     * DPA untouched. A cell not yet free is real backpressure (capped backoff). */
    /* Fail fast on a ring already declared dead — do NOT burn a ticket on it. */
    if (__atomic_load_n(&ring->dead, __ATOMIC_ACQUIRE)) {
        DOCA_LOG_ERR("ENQUEUE rejected: DMA ring %d is dead (no DPA consumer)", ridx);
        return -1;
    }

    uint64_t t = __atomic_fetch_add(&ring->enq_pos, 1, __ATOMIC_RELAXED);
    ring_slot = (uint32_t)(t % ring->size);
    dma = &ring->descs[ring_slot];
    {
        struct timespec backoff = {0, 1000}; /* 1µs initial */
        struct timespec deadline; int have_dl = 0;
        for (;;) {
            uint64_t s = __atomic_load_n(&ring->seq[ring_slot], __ATOMIC_ACQUIRE);
            if (s == t) break;                                   /* free for me */
            if (t >= ring->size && s == t - ring->size + 1 && dma->valid == 0) {
                /* prev generation published + DPA-consumed → reclaim this cell */
                __atomic_store_n(&ring->seq[ring_slot], t, __ATOMIC_RELEASE);
                break;
            }
            /* prev generation not yet published+consumed → ring full here
             * (throttled WARN, best-effort under concurrency) */
            uint64_t pr = __atomic_fetch_add(&ring->busy_probes, 1, __ATOMIC_RELAXED);
            if ((pr & 4095u) == 0)
                DOCA_LOG_WARN("DMA ring %d busy at slot=%u (size=%u) [stuck x%llu]",
                              ridx, ring_slot, ring->size, (unsigned long long)(pr + 1));
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
                    DOCA_LOG_ERR("DMA ring %d STALLED >%ds at slot=%u (size=%u): no DPA "
                                 "consumer is draining it — ring marked dead, failing enqueue",
                                 ridx, RING_STALL_DEADLINE_SEC, ring_slot, ring->size);
                    return -1;
                }
            }
            nanosleep(&backoff, NULL);
            if (backoff.tv_nsec < 50000) backoff.tv_nsec *= 2;
        }
    }

    /* TX byte lifetime: the façade calls dpumesh_tx_sent right after this enqueue
     * to record the shipped descriptor (seq -> end cursor) in the conn's send-unit
     * FIFO; the DPU's BATCH_FWD_ACK advances the conn's free cursor (rx_data_hook ->
     * tx_reclaim_ack). Both legs (client request / server reply) are identical —
     * request-vs-response is not a wire flag, just the oriented tuple. */

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

    __sync_synchronize();
    dma->valid = 1;                                     /* publish to DPA */
    /* Vyukov: mark this cell PRODUCED so the next generation (ticket t+size) can
     * reclaim it once the DPA consumes this descriptor (valid→0). Release-ordered
     * AFTER valid=1, so a reclaiming producer that observes this seq also sees
     * valid's effect (never a stale pre-consume 0). */
    __atomic_store_n(&ring->seq[ring_slot], t + 1, __ATOMIC_RELEASE);

    DOCA_LOG_DBG("ENQUEUE: seq=%u dst_svc=%d dst_pod=%d ring=%d slot=%u len=%u",
                 desc->seq, desc->dst_service, desc->dst_pod, ridx, ring_slot, desc->body_len);

    return 0;
}

/* ====================================================================
 * RX functions
 * ==================================================================== */

/* Pop one NEW-connection descriptor off the Vyukov accept ring. Non-blocking: 0 +
 * *desc, or -1 when empty. Readiness comes from any CQ's fd (every CQ is notified —
 * the ring is SPMC), so there is no timeout/blocking form — the old one polled with a
 * nanosleep backoff and no caller ever passed a non-zero timeout. */
int dpumesh_dequeue(dpumesh_ctx_t *ctx, sw_descriptor_t *desc) {
    return rxq_try_pop(ctx, desc) ? 0 : -1;
}

uint8_t *dpumesh_rx_buf(dpumesh_ctx_t *ctx, int slot) {
    /* slot carries the landing byte-offset (pos) into rx_dma_buffer. */
    if (slot < 0 || (size_t)slot >= ctx->rx_dma_buf_size) return NULL;
    return (uint8_t *)ctx->rx_dma_buffer + (size_t)slot;
}

void dpumesh_rx_free(dpumesh_ctx_t *ctx, int slot) {
    /* `slot` is the landing byte offset (pos), not a pool index — return the
     * admission credit to the matching ring so the DPA can reuse that position
     * (AFTER the consumer read it). */
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

int dpumesh_get_pod_id(dpumesh_ctx_t *ctx) {
    return ctx->pod_id;
}

const char *dpumesh_get_worker_id(dpumesh_ctx_t *ctx) {
    return ctx->worker_id;
}

/* ====================================================================
 * Client-side API
 * ==================================================================== */

/* Allocate a host-unique port (>=1) and register it as a CLIENT or SERVER socket.
 * `user` is the app's conn handle (returned later by dmesh_next_ready) and `cq` the
 * completion queue that owns it; both are stored BEFORE role is published so the PE,
 * which may deliver + enqueue this port the instant it sees role!=FREE, never hands
 * back a NULL handle or arms an unbound conn. The role is published with RELEASE so
 * the PE's ACQUIRE-load sees a fully-initialized slot. Returns 0 on exhaustion. */
uint16_t dpumesh_alloc_port(dpumesh_ctx_t *ctx, int role, void *user, struct dmesh_cq *cq) {
    pthread_mutex_lock(&ctx->port_lock);
    /* Model B: host CLIENT conns use [1, DMESH_UPORT_BASE); accepted SERVER conns get
     * their port from the DPU-assigned upstream id via dpumesh_alloc_port_specific.
     * Capping the sweep here keeps the two ranges disjoint so a loopback host never
     * collides in its own ports[] table.
     *
     * The cursor sweeps a WINDOW of that range, widened only when concurrency needs it.
     * A port's inbox is allocated on first use and then kept for the process's life — it
     * cannot be freed, since the PE pushes into it WITHOUT port_lock (rx_deliver_desc),
     * so freeing one would race a live delivery. A cursor sweeping all 32767 client ports
     * therefore ends up allocating 32767 inboxes (~1.5 GB at the default depth) for a
     * client that merely churns a few connections. The window caps that at port_span.
     *
     * Sweeping (rather than reusing the lowest free port) still matters: reusing a
     * just-closed port invites a reply already in flight at FIN time to land on the NEW
     * conn, so port numbers are aged out across the whole window. */
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
                /* A straggler reply may have landed in this slot's inbox AFTER the
                 * previous owner's dmesh_destroy_qp drained it (close/deliver race).
                 * Return those RX credits now, before the head/tail reset discards
                 * them (else the DPA reverse-admission credit slowly leaks). */
                sw_descriptor_t d;
                while (inbox_pop(psl, &d)) rx_credit_return(ctx, d.body_buf_slot);
            }
            atomic_store_explicit(&psl->in_head, 0, memory_order_relaxed);
            atomic_store_explicit(&psl->in_tail, 0, memory_order_relaxed);
            psl->peer_pod   = DMESH_POD_BLANK;
            psl->peer_port  = 0;
            psl->user       = user;     /* visible before role (publish ordering below) */
            psl->cq         = cq;       /* ditto: the PE arms this CQ's list, not the ctx's */
            port_reset_tx(psl);         /* fresh block-chain cursors; first block grabbed LAZILY on first write */
            /* Publish role LAST (RELEASE) so the PE's ACQUIRE-load sees the fully
             * initialized inbox/head/tail/user/cq/chain before it can deliver here. */
            __atomic_store_n(&psl->role, (uint8_t)role, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&ctx->port_lock);
            return (uint16_t)p;
        }
        if (span >= DMESH_UPORT_BASE)
            break;                      /* swept the whole range: genuinely out of ports */
        /* Window full → widen it (the only thing that allocates fresh inboxes) and sweep
         * the new region first. */
        ctx->port_span = (span * 2 > DMESH_UPORT_BASE) ? DMESH_UPORT_BASE : span * 2;
        ctx->next_port = span;
    }
    pthread_mutex_unlock(&ctx->port_lock);
    DOCA_LOG_ERR("dpumesh_alloc_port: no free ports");
    return 0;
}

/* Promote a PE-created SERVER_PENDING slot to a live SERVER conn (model B accept):
 * attach the app's conn handle so dmesh_next_ready starts returning it, and bind the
 * conn to the CQ that accepted it — from here its ready-edges go to that CQ alone.
 * The PE already allocated the inbox + set the peer + published SERVER_PENDING
 * (message 1 rode the accept queue; any messages 2..P are already coalesced in the
 * inbox). The port_lock makes the pending->SERVER promote exactly-once, so when
 * several CQs accept concurrently only one binds the conn.
 * Returns `port` on success, 0 if the slot is not pending. */
uint16_t dpumesh_accept_port(dpumesh_ctx_t *ctx, uint16_t port, void *user, struct dmesh_cq *cq) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return 0;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    pthread_mutex_lock(&ctx->port_lock);
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_SERVER_PENDING) {
        pthread_mutex_unlock(&ctx->port_lock);
        return 0;   /* not pending (already accepted / freed / race) */
    }
    psl->user = user;
    psl->cq   = cq;             /* both visible before the role publish below */
    __atomic_store_n(&psl->role, DMESH_ROLE_SERVER, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ctx->port_lock);
    return port;
}

/* Release a conn. role=FREE (RELEASE) first → the PE drops further deliveries as
 * stale (and dmesh_next_ready skips any ready-list entry still pointing here); then
 * reclaim any undelivered inbound (their RX credits). The inbox ring is kept for
 * the slot's next reuse. */
void dpumesh_free_port(dpumesh_ctx_t *ctx, uint16_t port) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    /* Mark FREE first, then try to return the TX blocks — NON-BLOCKING. If sends are
     * still un-ACKed the blocks are NOT returned here; the PE's reclaim returns them
     * on the last ACK (try_return_blocks). Until then the port is FREE-but-draining
     * (nblk_owned>0) and the alloc paths skip it, so its blocks are never reused
     * mid-DMA. Close never blocks the app thread. */
    __atomic_store_n(&psl->role, DMESH_ROLE_FREE, __ATOMIC_RELEASE);
    psl->user = NULL;
    /* Unbind the CQ (RELEASE) too: arm_ready_after_push skips a NULL cq, so the PE
     * stops touching a ready list whose owner may be destroyed next. */
    __atomic_store_n(&psl->cq, NULL, __ATOMIC_RELEASE);
    try_return_blocks(ctx, psl);
    if (psl->inbox) {
        sw_descriptor_t d;
        while (inbox_pop(psl, &d)) rx_credit_return(ctx, d.body_buf_slot);
    }
    /* Disarm so a recycled slot starts clean (a stale ready-list entry for this port
     * is skipped by dmesh_next_ready on role==FREE). */
    atomic_store_explicit(&psl->on_ready, 0u, memory_order_release);
}

/* Pop the next inbound message descriptor for a conn (CLIENT or SERVER — one
 * path). Returns 1 + fills *out, or 0 if the conn inbox is empty. The body is in
 * the shared RX mmap at out->body_buf_slot (a landing pos). */
int dpumesh_conn_recv(dpumesh_ctx_t *ctx, uint16_t port, sw_descriptor_t *out) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return 0;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (!psl->inbox) return 0;
    if (inbox_pop(psl, out)) return 1;
    /* Inbox observed empty → we are about to report "done draining" to the caller,
     * which will stop revisiting this conn until the ready list names it again.
     * Disarm, then RE-CHECK under a seq_cst fence: this pairs with the fence in
     * arm_ready_after_push so a message the PE enqueued exactly as we drained cannot
     * be stranded off the ready list. Either this re-check sees it, or the PE's
     * arm sees on_ready==0 and (re-)pushes the conn — never neither. */
    atomic_store_explicit(&psl->on_ready, 0u, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    if (inbox_pop(psl, out)) {
        /* A push raced in. We are servicing again, so re-arm: a LATER push will then
         * see on_ready==1 and rely on us to drain it (and we disarm+recheck again on
         * the next empty). Harmless if the PE also already re-armed + pushed us (the
         * duplicate ready-list visit just drains empty once). */
        atomic_store_explicit(&psl->on_ready, 1u, memory_order_release);
        return 1;
    }
    return 0;
}

/* Pop the next conn that has inbound, from THIS CQ's PE-published ready list, and
 * return its app handle (the `user` registered at alloc). NULL when the list is
 * drained. No scan: the PE put exactly the ready conns here. A list entry whose conn
 * has since closed (role==FREE) is skipped — its port may even have been recycled,
 * but round-robin allocation makes that astronomically distant; either way a stale
 * entry only ever costs one extra empty drain. Single-consumer (this CQ's thread);
 * call it after waking on dmesh_cq_fd, drain each returned conn to EAGAIN. */
void *dpumesh_next_ready(struct dmesh_cq *cq) {
    dpumesh_ctx_t *ctx = cq->ch->ctx;
    uint16_t port;
    while (ready_pop(cq, &port)) {
        struct dmesh_port_slot *psl = &ctx->ports[port];
        uint8_t role = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
        /* Return only ACCEPTED conns. Skip FREE (closed since enqueued → stale) and
         * SERVER_PENDING (not yet accepted → drained by dmesh_accept, and its user
         * handle isn't set yet). */
        if (role == DMESH_ROLE_CLIENT || role == DMESH_ROLE_SERVER)
            return psl->user;
    }
    return NULL;
}


/* ====================================================================
 * Connection lifecycle
 *
 * This section used to live in src/dmesh.c behind the name "socket façade".
 * It never was one: nothing below is socket- or verbs-specific — it is the
 * transport's own notion of a connection (create, address, learn a peer, emit a
 * descriptor, tear down). Both public surfaces (<dpumesh/dmesh.h> and the
 * LD_PRELOAD shim) call straight in here, and neither is built on the other.
 * ==================================================================== */

/* Return the held RX-landing credit and clear the inbound view. */
static void conn_free_rx(dmesh_qp_t *c) {
    if (c->rx_slot >= 0) dpumesh_rx_free(c->ep->ctx, c->rx_slot);
    c->rx_slot = -1; c->rx_buf = NULL; c->rx_len = 0; c->rx_pos = 0;
}

/* Build the oriented tuple for one outbound descriptor of this conn (client →
 * service, which the DPU LBs to a backend and then STICKS the conn to; or server →
 * its learned peer). `moff` = byte offset in the shared TX mmap, `len` = descriptor
 * length. seq++. Returns 0, or -1 (EBADMSG) on enqueue fault. */
static int emit_desc(dmesh_qp_t *c, size_t moff, uint32_t len) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    c->seq++;
    sw_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.body_buf_slot = (int32_t)moff;                 /* BYTE offset into the TX mmap */
    d.body_len      = len;
    d.src_port      = c->local_port;
    d.seq           = c->seq;
    d.dst_service   = c->dst_service;
    if (c->role == DMESH_ROLE_CLIENT) { d.dst_pod = DMESH_POD_BLANK; d.dst_port = DMESH_PORT_BLANK; }
    else                              { d.dst_pod = c->remote_pod;   d.dst_port = c->remote_port; }
    d.valid = 1;
    if (dpumesh_enqueue(ctx, &d) < 0) { errno = EBADMSG; return -1; }
    return 0;
}

/* ===== Channel ===== */

dmesh_channel_t *dmesh_create_channel(void) {
    dmesh_channel_t *s = (dmesh_channel_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    /* Identity is injected, not declared: resolve $DPUMESH_SERVICE (a k8s Service
     * name) to a service_id through the same table peers resolve through. Unset =
     * pure client (SVC_NONE). One table serves both directions (NAMING.md §2). */
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

/* Same EBUSY rule one level up: a CQ outliving its channel points at a freed ctx, and
 * tearing the transport down under a live CQ is the same use-after-free as tearing a CQ
 * down under a live QP. Destroy the CQs first (which their own EBUSY makes you destroy
 * the QPs first). Returns 0, or -1 + EBUSY with nothing released. */
int dmesh_destroy_channel(dmesh_channel_t *s) {
    if (!s) return 0;
    if (s->ctx) {
        dpumesh_ctx_t *ctx = s->ctx;
        int live = 0;
        pthread_mutex_lock(&ctx->cq_lock);
        for (int i = 0; i < ctx->n_cqs; i++) if (ctx->cqs[i]) { live = 1; break; }
        pthread_mutex_unlock(&ctx->cq_lock);
        if (live) { errno = EBUSY; return -1; }
        dpumesh_destroy(ctx);
    }
    free(s);
    return 0;
}

int dmesh_pod_id(dmesh_channel_t *s)   { return s->pod_id; }
int dmesh_msg_max(dmesh_channel_t *s)  { return s->slot_size; }
int dmesh_post_max(dmesh_channel_t *s) { return s->block_size; }

/* ===== Completion queue ===== */

/* Readiness is armed HERE, at creation — never lazily on the first dmesh_cq_fd. The
 * ready list is not an optional extra: it is how dmesh_poll_cq finds an ESTABLISHED
 * conn's inbound (a NEW conn's first message rides the separate accept queue, which is
 * why the failure mode was so asymmetric — connects worked, replies vanished). Arming
 * on first cq_fd meant a caller that only ever POLLS, and never sleeps on the fd,
 * silently received nothing on established conns. That is a footgun, not an
 * optimization: every client of this API polls. The eventfd is the idle-sleep path
 * only, so its creation failing is NOT fatal (notify_efd = -1 → poll still delivers). */
dmesh_cq_t *dmesh_create_cq(dmesh_channel_t *ch) {
    if (!ch) { errno = EINVAL; return NULL; }
    dmesh_cq_t *cq = (dmesh_cq_t *)calloc(1, sizeof(*cq));
    if (!cq) { errno = ENOMEM; return NULL; }
    cq->ch         = ch;
    cq->notify_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    atomic_init(&cq->ready_head, (uint_fast32_t)0);
    atomic_init(&cq->ready_tail, (uint_fast32_t)0);
    atomic_init(&cq->nqp, 0);
    atomic_init(&cq->wants_notify, 0);   /* poll-only until dmesh_cq_fd is called */

    dpumesh_ctx_t *ctx = ch->ctx;
    pthread_mutex_lock(&ctx->cq_lock);
    int idx = -1;
    for (int i = 0; i < ctx->n_cqs; i++) if (!ctx->cqs[i]) { idx = i; break; }
    if (idx < 0 && ctx->n_cqs < DMESH_MAX_CQ) idx = ctx->n_cqs++;
    if (idx >= 0) ctx->cqs[idx] = cq;
    pthread_mutex_unlock(&ctx->cq_lock);
    if (idx < 0) {   /* cap reached: an unregistered CQ would miss every accept */
        if (cq->notify_efd >= 0) close(cq->notify_efd);
        free(cq);
        errno = EMFILE;
        return NULL;
    }
    cq->reg_idx = idx;
    return cq;
}

/* Unregister FIRST (under cq_lock, so a concurrent accept-path notify_all_cqs either
 * saw us before or never sees us again), then free. Conns still bound here would have
 * nowhere to report, so ibv_destroy_cq's EBUSY rule is CHECKED, not just documented:
 * freeing under a live QP leaves the PE arming a ready list in freed memory. */
int dmesh_destroy_cq(dmesh_cq_t *cq) {
    if (!cq) return 0;
    if (atomic_load_explicit(&cq->nqp, memory_order_acquire) > 0) {
        errno = EBUSY;
        return -1;
    }
    dpumesh_ctx_t *ctx = cq->ch->ctx;
    pthread_mutex_lock(&ctx->cq_lock);
    if (ctx->cqs[cq->reg_idx] == cq) ctx->cqs[cq->reg_idx] = NULL;
    pthread_mutex_unlock(&ctx->cq_lock);
    if (cq->notify_efd >= 0) close(cq->notify_efd);
    free(cq);
    return 0;
}

/* Handing out the fd is the caller DECLARING it may sleep on it, so latch wants_notify
 * (the PE starts writing the fd on ready edges) and self-kick once: any conn armed while
 * the CQ was still poll-only (wants_notify==0, no wakeup written) is already on the ready
 * list but left no edge on the fd, so without this the caller's first epoll_wait could
 * block despite pending work. The release store publishes the flag to the PE thread
 * before the kick. Idempotent: repeated calls just re-kick (one spurious wakeup). */
int dmesh_cq_fd(dmesh_cq_t *cq) {
    if (!cq) return -1;
    atomic_store_explicit(&cq->wants_notify, 1, memory_order_release);
    if (cq->notify_efd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(cq->notify_efd, &one, sizeof(one));
        (void)w;
    }
    return cq->notify_efd;
}

/* ===== Connection setup ===== */

dmesh_qp_t *dmesh_accept(dmesh_cq_t *cq) {
    dmesh_channel_t *s = cq->ch;
    sw_descriptor_t req;
    if (dpumesh_dequeue(s->ctx, &req) < 0 || !req.valid) {
        errno = EAGAIN;
        return NULL;
    }
    dmesh_qp_t *c = (dmesh_qp_t *)calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; dpumesh_rx_free(s->ctx, req.body_buf_slot); return NULL; }
    /* Model B: the PE created a SERVER_PENDING slot at message-1 delivery (port =
     * req.dst_port = uP, with any pipelined messages 2..P already coalesced in its
     * inbox). Promote it to a live SERVER conn, attach THIS handle, and bind it to the
     * CQ that won it — dmesh_next_ready then returns it on this CQ only. */
    uint16_t ps = dpumesh_accept_port(s->ctx, req.dst_port, c, cq);
    if (ps == 0) { dpumesh_rx_free(s->ctx, req.body_buf_slot); free(c); errno = ENOMEM; return NULL; }

    c->ep          = s;
    c->cq          = cq;
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
    atomic_fetch_add_explicit(&cq->nqp, 1, memory_order_relaxed);
    return c;
}

/* Integer entry point (internal, dmesh_core.h): the shim and the name-taking public
 * wrapper below both land here. Purely local — no round trip. */
dmesh_qp_t *dmesh_qp_open(dmesh_cq_t *cq, int dst_service_id) {
    dmesh_channel_t *s = cq->ch;
    dmesh_qp_t *c = (dmesh_qp_t *)calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; return NULL; }
    /* c = the port's handle, cq = the ready list its inbound edges arm */
    uint16_t pc = dpumesh_alloc_port(s->ctx, DMESH_ROLE_CLIENT, c, cq);
    if (pc == 0) { free(c); errno = ENOMEM; return NULL; }
    c->ep          = s;
    c->cq          = cq;
    c->role        = DMESH_ROLE_CLIENT;
    c->local_port  = pc;
    c->dst_service = (int16_t)dst_service_id;
    c->remote_pod  = DMESH_POD_BLANK;
    c->remote_port = DMESH_PORT_BLANK;
    c->seq         = 0;
    c->rx_slot     = -1;
    atomic_fetch_add_explicit(&cq->nqp, 1, memory_order_relaxed);
    return c;
}

/* Public constructor (NAMING.md §4): resolve the k8s Service NAME to a service_id
 * at point of use, then open. Two lines, and the integer is never exposed. This
 * lives here (not dmesh_api.c, which is the verbs data path only) because the
 * public lifecycle already lives in this file, shared with the shim. */
dmesh_qp_t *dmesh_create_qp(dmesh_cq_t *cq, const char *service_name) {
    if (!cq || !service_name) { errno = EINVAL; return NULL; }
    int svc = dmesh_resolve_name(service_name);
    if (svc < 0) return NULL;                 /* errno = ENOENT from resolve_name */
    return dmesh_qp_open(cq, svc);
}

dmesh_qp_t *dmesh_next_ready(dmesh_cq_t *cq) {
    return (dmesh_qp_t *)dpumesh_next_ready(cq);
}

/* ===== Doorbell + teardown ===== */

int dmesh_flush(dmesh_qp_t *c) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    size_t moff; uint32_t len;
    while (dpumesh_tx_next_send(ctx, c->local_port, &moff, &len)) {
        if (emit_desc(c, moff, len) < 0) return -1;          /* EBADMSG; bytes stay committed */
        dpumesh_tx_sent(ctx, c->local_port, c->seq, len);    /* c->seq = the seq emit_desc used */
    }
    return 0;
}

/* Ship this conn's FIN. IDEMPOTENT: fin_sent latches, so every caller can just ask
 * for a FIN without first proving nobody else sent one. Independent of peer_closed —
 * receiving the peer's FIN does not close OUR half (TCP does not conflate them, and
 * the DPU's upstream teardown fans out from this FIN alone). */
void dmesh_send_fin(dmesh_qp_t *c) {
    if (c->fin_sent) return;
    c->fin_sent = 1;
    dpumesh_ctx_t *ctx = c->ep->ctx;
    c->seq++;
    sw_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.body_buf_slot = 0;                                   /* 0-length FIN: offset unused */
    d.body_len      = 0;                                   /* FIN marker (0-length) */
    d.src_port      = c->local_port;
    d.seq           = c->seq;
    d.dst_service   = c->dst_service;
    d.dst_pod       = c->remote_pod;                       /* the learned peer conn */
    d.dst_port      = c->remote_port;
    d.valid         = 1;
    /* Best-effort: rides after all prior data (seq order). Holds NO ring bytes, so it
     * needs no send-unit / reclaim — its TX_ACK is a harmless FIFO no-op. */
    dpumesh_enqueue(ctx, &d);
}

int dmesh_destroy_qp(dmesh_qp_t *c) {
    if (!c) return 0;
    dpumesh_ctx_t *ctx = c->ep->ctx;
    if (c->cq && c->cq->vq_cur == c) c->cq->vq_cur = NULL; /* poll_cq resume cursor */
    dpumesh_tx_discard_unsent(ctx, c->local_port);         /* buffered, never flushed → drop */
    /* Established conns ALWAYS close their half (dmesh_send_fin self-guards against a
     * second one). A CLIENT that never sent has no peer and no DPU-side conn — nothing
     * to tear down, so seq==0 skips. */
    if (c->role == DMESH_ROLE_SERVER || c->seq > 0)
        dmesh_send_fin(c);
    conn_free_rx(c);                                       /* return the held RX credit */
    if (c->local_port) dpumesh_free_port(ctx, c->local_port);
    if (c->cq) atomic_fetch_sub_explicit(&c->cq->nqp, 1, memory_order_release);
    free(c);
    return 0;
}
