/*
 * dmesh_core.c - DPUmesh DOCA transport layer implementation (raw core engine)
 *
 * NVIDIA DOCA (Comch + DMA) host-side backend for the DPUmesh transport.
 * Provides the host-side raw buffer API declared in dmesh_core.h: TX/RX
 * slot pools, descriptor SQ enqueue/dequeue, and a connection-oriented
 * port table (no request/response matching) over the DPU control + DMA
 * data path.
 */

#include <dpumesh/dmesh_core.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>        /* close() for the host epoll RX path */
#include <sys/epoll.h>     /* event-driven host PE progress (DPUMESH_HOST_EPOLL) */
#include <sys/eventfd.h>   /* readiness eventfd for native-epoll integration (dpumesh_get_event_fd) */

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
 * records (seq -> end cursor) in su_seq/su_end[region*TX_SU_DEPTH + i]; a
 * BATCH_FWD_ACK(port,seq) pops the FIFO front and advances the conn's free cursor.
 * Power of two (masked). Bounds in-flight descriptors per conn (backpressure). */
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
 * host-unique pool so client and server ports never collide (loopback-safe). */
#define DMESH_PORT_SPACE  65536
/* Per-conn inbound queue depth (descriptors only — bodies stay in the shared RX
 * mmap, referenced by pos). Lazily malloc'd per LIVE conn (not 65536× pre-alloc).
 * Power of two. On overflow (app drains too slowly) the landing is reclaimed
 * (message dropped) + the shared RX credit still bounds total in-flight bytes. */
#define DMESH_INBOX_RING  256u
struct dmesh_port_slot {
    uint8_t          role;            /* FREE / CLIENT / SERVER */
    int16_t          peer_pod;        /* established peer pod, DMESH_POD_BLANK = not yet learned */
    uint16_t         peer_port;       /* established peer port, 0 = not yet learned */
    void            *user;            /* app's conn handle (returned by dmesh_next_ready);
                                       * set BEFORE role is published so the PE never enqueues
                                       * a port whose handle isn't visible yet. */
    /* Inbound SPSC ring: PE thread = sole producer (in_tail), the conn's owning
     * app thread = sole consumer (in_head). Lock-free. inbox==NULL until alloc. */
    sw_descriptor_t *inbox;           /* malloc'd ring[DMESH_INBOX_RING] */
    atomic_uint_fast32_t in_head;     /* consumer (app) */
    atomic_uint_fast32_t in_tail;     /* producer (PE) */
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
    atomic_uint_fast64_t tx_f;              /* PE-thread logical cursor (ACK reclaim) */
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
    atomic_uint_fast16_t su_head;           /* send-unit FIFO head (owner writes/release, PE reads) */
    atomic_uint_fast16_t su_tail;           /* send-unit FIFO tail (PE writes/release, owner reads) */
};
/* Ready-list SPSC ops (monotonic counters; PE producer, app consumer). The list
 * carries conn PORTS; dmesh_next_ready maps each to its slot->user. Provably never
 * full (≤ live conns < DMESH_PORT_SPACE; the inbox 0->1 edge admits each at most
 * once between drains), but the guard keeps a stray push from corrupting indices. */
static inline void ready_push(dpumesh_ctx_t *ctx, uint16_t port);
static inline int  ready_pop(dpumesh_ctx_t *ctx, uint16_t *port);
/* Inbound SPSC ring ops (monotonic counters; count = tail-head). */
static inline int inbox_push(struct dmesh_port_slot *psl, const sw_descriptor_t *d) {
    uint_fast32_t t = atomic_load_explicit(&psl->in_tail, memory_order_relaxed);
    uint_fast32_t h = atomic_load_explicit(&psl->in_head, memory_order_acquire);
    if (t - h >= DMESH_INBOX_RING) return 0;                 /* full */
    psl->inbox[t & (DMESH_INBOX_RING - 1)] = *d;
    atomic_store_explicit(&psl->in_tail, t + 1, memory_order_release);
    return (t == h) ? 2 : 1;   /* 2 = empty→non-empty transition (edge-trigger the fd) */
}
static inline int inbox_pop(struct dmesh_port_slot *psl, sw_descriptor_t *out) {
    uint_fast32_t h = atomic_load_explicit(&psl->in_head, memory_order_relaxed);
    uint_fast32_t t = atomic_load_explicit(&psl->in_tail, memory_order_acquire);
    if (h == t) return 0;                                    /* empty */
    *out = psl->inbox[h & (DMESH_INBOX_RING - 1)];
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
    /* Elastic-pool event counters (diagnostics; relaxed atomics — events are the
     * RARE paths: steady sliding touches none of these except recycle_hits once
     * per drained block). Read via dpumesh_get_tx_pool_stats. */
    atomic_ullong st_pool_grabs;    /* shared-pool CAS pops (conn grow / first block) */
    atomic_ullong st_pool_returns;  /* shared-pool CAS pushes (shrink / close drain) */
    atomic_ullong st_recycle_hits;  /* grow served from the conn's recyc[] (no pool op) */
    atomic_ullong st_grow_waits;    /* backoff sleeps in reserve (window full / pool empty) */
    atomic_ullong st_block_pads;    /* message didn't fit the block tail → pad + next block */

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

    /* Readiness eventfd for native-epoll integration. Lazily enabled by
     * dpumesh_get_event_fd(): once enabled, rx_deliver_desc writes the eventfd on
     * each user-visible delivery (new conn -> accept RX ring, or established conn -> its inbox) so a
     * caller blocked in a vanilla epoll_wait() on this fd wakes up. The PE thread
     * itself already sleeps on the DOCA PE notification fd (DPUMESH_HOST_EPOLL), so
     * the whole chain is notification-driven, not busy-poll. -1 = not created. */
    int notify_efd;
    volatile int notify_enabled;

    /* Endpoint port table + allocator (oriented-tuple demux). */
    struct dmesh_port_slot *ports;     /* [DMESH_PORT_SPACE] */
    pthread_mutex_t port_lock;
    uint32_t next_port;                /* bump cursor, wraps within [1, DMESH_UPORT_BASE) */
    int32_t service_id;                /* this node's service id (SVC_NONE if client-only) */

    /* PE-published READY LIST (single channel-eventfd model). The PE pushes a
     * conn's port the moment its inbox goes empty->non-empty; the app drains this
     * list via dmesh_next_ready() instead of scanning every conn or holding a
     * per-conn fd. SPSC: PE = sole producer (ready_tail), the app event-loop =
     * sole consumer (ready_head). Sized to the port space so it never overflows
     * (each live conn appears at most once — the 0->1 edge dedups). */
    char _rl_pad0[64];
    atomic_uint_fast32_t ready_head;   /* consumer (app) */
    char _rl_pad1[64];
    atomic_uint_fast32_t ready_tail;   /* producer (PE) */
    char _rl_pad2[64];
    uint16_t ready_ring[DMESH_PORT_SPACE];
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

/* Wake a caller blocked in a vanilla epoll_wait() on the readiness eventfd.
 * No-op until dpumesh_get_event_fd() enables it. Per-delivery write (no
 * coalescing) → cannot lose a wakeup; the eventfd is a plain counter, drained by
 * one read() per epoll wakeup. Safe to call from the PE thread. */
static inline void dpumesh_notify(dpumesh_ctx_t *ctx)
{
    if (ctx->notify_enabled && ctx->notify_efd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(ctx->notify_efd, &one, sizeof(one));
        (void)w;
    }
}

/* Ready-list SPSC: PE pushes a ready conn's port; the app event-loop pops it via
 * dmesh_next_ready. Monotonic counters (count = tail-head); ring index masks the
 * power-of-two DMESH_PORT_SPACE. */
static inline void ready_push(dpumesh_ctx_t *ctx, uint16_t port) {
    uint_fast32_t t = atomic_load_explicit(&ctx->ready_tail, memory_order_relaxed);
    uint_fast32_t h = atomic_load_explicit(&ctx->ready_head, memory_order_acquire);
    if (t - h >= DMESH_PORT_SPACE) return;   /* provably never full; guard anyway */
    ctx->ready_ring[t & (DMESH_PORT_SPACE - 1)] = port;
    atomic_store_explicit(&ctx->ready_tail, t + 1, memory_order_release);
}
static inline int ready_pop(dpumesh_ctx_t *ctx, uint16_t *port) {
    uint_fast32_t h = atomic_load_explicit(&ctx->ready_head, memory_order_relaxed);
    uint_fast32_t t = atomic_load_explicit(&ctx->ready_tail, memory_order_acquire);
    if (h == t) return 0;                                    /* empty */
    *port = ctx->ready_ring[h & (DMESH_PORT_SPACE - 1)];
    atomic_store_explicit(&ctx->ready_head, h + 1, memory_order_release);
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
            DOCA_LOG_ERR("RX deliver: conn %u inbox full, dropping seq=%u", dport, desc->seq);
            rx_credit_return(ctx, slot);
        } else if (r == 2 && ctx->notify_enabled) {
            /* Re-read role: a concurrent dmesh_accept may have promoted this slot
             * SERVER_PENDING→SERVER AFTER our initial load (line above), in which
             * case this empty→non-empty edge MUST wake the app — using the stale
             * PENDING snapshot would strand the conn until its inbox overflows. If
             * still PENDING, skip (the accept path drains it). Still on the PE
             * thread, so ready_push stays single-producer. */
            uint8_t rnow = __atomic_load_n(&psl->role, __ATOMIC_ACQUIRE);
            if (rnow != DMESH_ROLE_SERVER_PENDING) {
                ready_push(ctx, dport);
                dpumesh_notify(ctx);
            }
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
            if (r == 0) rx_credit_return(ctx, slot);
            else if (r == 2 && ctx->notify_enabled) { ready_push(ctx, dport); dpumesh_notify(ctx); }
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
            psl->inbox = (sw_descriptor_t *)malloc(DMESH_INBOX_RING * sizeof(sw_descriptor_t));
            if (!psl->inbox) { pthread_mutex_unlock(&ctx->port_lock); rx_credit_return(ctx, slot); return; }
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
        port_reset_tx(psl);   /* fresh block-chain cursors; TX blocks grabbed LAZILY on first reply */
        __atomic_store_n(&psl->role, DMESH_ROLE_SERVER_PENDING, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&ctx->port_lock);

        uint_fast32_t pos = atomic_load_explicit(&ctx->rx_enq, memory_order_relaxed);
        struct rxq_cell *c = &ctx->rx_ring[pos & (RX_QUEUE_SIZE - 1)];
        uint_fast32_t cseq = atomic_load_explicit(&c->seq, memory_order_acquire);
        if ((int_fast32_t)(cseq - pos) != 0) {
            __atomic_store_n(&psl->role, DMESH_ROLE_FREE, __ATOMIC_RELEASE);   /* roll back (no block borrowed) */
            DOCA_LOG_ERR("RX deliver: accept queue full, dropping new conn uP=%u", dport);
            rx_credit_return(ctx, slot);
            return;
        }
        c->desc = *desc;
        atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
        atomic_store_explicit(&ctx->rx_enq, pos + 1, memory_order_relaxed);
        dpumesh_notify(ctx);                       /* endpoint/accept fd */
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

    /* This node's service id (what it advertises; DPU sets service_table[service_id]
     * = the assigned pod_id). SVC_NONE = client-only. Comes from the caller;
     * DPUMESH_SERVICE_ID env overrides. The pod_id (this node's address) is NO
     * LONGER host-chosen — the DPU assigns it at registration (see
     * init_control_path). It stays -1 until DMESH_MSG_POD_ASSIGNED arrives. */
    if ((env_val = getenv("DPUMESH_SERVICE_ID")) != NULL)
        ctx->service_id = atoi(env_val);
    else
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

static doca_error_t init_control_path(dpumesh_ctx_t *ctx) {
    doca_error_t result;

    fprintf(stderr, "[dpumesh] Connecting comch client...\n");
    result = init_comch_ctrl_path_client("DPUMesh", &ctx->doca_objs, true);
    if (result != DOCA_SUCCESS) return result;

    /* Register with pod_id = -1: the DPU allocates our pod_id and replies with
     * DMESH_MSG_POD_ASSIGNED. Block until it arrives, progressing the PE by hand
     * (the PE thread isn't started until the end of dpumesh_init). */
    __atomic_store_n(&ctx->doca_objs.assigned_pod_id, -1, __ATOMIC_RELEASE);
    ctx->reg_msg.type = DMESH_MSG_POD_REGISTER;
    ctx->reg_msg.pod_id = -1;                    /* DPU assigns this node's address */
    ctx->reg_msg.service_id = ctx->service_id;   /* DPU: service_table[service_id]=assigned pod_id */

    result = client_send_msg(&ctx->doca_objs, (const char *)&ctx->reg_msg, sizeof(ctx->reg_msg));
    if (result != DOCA_SUCCESS) return result;
    DOCA_LOG_INFO("Sent REGISTER to DPU: service_id=%d (awaiting pod_id)", ctx->service_id);

    /* Wait for the assignment. Bounded (~2 s) so a lost reply fails init instead
     * of wedging forever. The reply normally lands in microseconds. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000 };   /* 10 us */
    int32_t assigned = -1;
    for (int i = 0; i < 200000; i++) {
        assigned = __atomic_load_n(&ctx->doca_objs.assigned_pod_id, __ATOMIC_ACQUIRE);
        if (assigned >= 0) break;
        if (ctx->doca_objs.pe) doca_pe_progress(ctx->doca_objs.pe);
        nanosleep(&ts, NULL);
    }
    if (assigned < 0) {
        DOCA_LOG_ERR("Timed out waiting for DPU pod_id assignment (service_id=%d)", ctx->service_id);
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
        DOCA_LOG_WARN("Failed to export Host RX buffer to DPU: %s", doca_err_str(result));
    }

    return DOCA_SUCCESS;
}

int dpumesh_init(dpumesh_ctx_t **out, int service_id,
                 const dpumesh_config_t *config) {
    dpumesh_ctx_t *ctx = (dpumesh_ctx_t *)calloc(1, sizeof(dpumesh_ctx_t));
    if (!ctx) return -1;
    ctx->notify_efd = -1;   /* before any goto fail: cleanup must not close fd 0 */

    init_config(ctx, config, service_id);

    if (init_doca_device(ctx) != DOCA_SUCCESS) goto fail;
    if (init_control_path(ctx) != DOCA_SUCCESS) goto fail;
    if (init_datapath(ctx) != DOCA_SUCCESS) goto fail;

    /* Shared lock-free Treiber block pool. block_next[i] = i+1 threads the free-list;
     * block_free head starts at index 0 (all n_blocks free, tag 0); the last block links
     * to n_blocks (the empty sentinel). Per-conn send-unit FIFOs (su_seq/su_end) are
     * lazily malloc'd per port slot (kept for the slot's life). */
    ctx->block_next = (uint32_t *)malloc((size_t)ctx->n_blocks * sizeof(uint32_t));
    if (!ctx->block_next) goto fail;
    for (int i = 0; i < ctx->n_blocks; i++)
        ctx->block_next[i] = (uint32_t)(i + 1);          /* last -> n_blocks (empty sentinel) */
    atomic_init(&ctx->block_free, (uint_fast64_t)0);     /* tag 0, head index 0 */
    pthread_mutex_init(&ctx->block_lock, NULL);

    /* Lock-free SPMC RX ring: seq[i] = i (cell i first writable at enq
     * position i), enq = deq = 0. */
    ctx->rx_ring = (struct rxq_cell *)malloc((size_t)RX_QUEUE_SIZE * sizeof(struct rxq_cell));
    if (!ctx->rx_ring) goto fail;
    for (uint32_t i = 0; i < RX_QUEUE_SIZE; i++)
        atomic_init(&ctx->rx_ring[i].seq, (uint_fast32_t)i);
    atomic_init(&ctx->rx_enq, (uint_fast32_t)0);
    atomic_init(&ctx->rx_deq, (uint_fast32_t)0);

    /* Readiness eventfd (non-blocking, close-on-exec). Non-fatal if it fails —
     * dpumesh_get_event_fd() then returns -1 and the caller falls back to polling. */
    ctx->notify_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ctx->notify_enabled = 0;

    /* Endpoint port table + allocator (oriented-tuple demux). calloc → every slot
     * role=FREE, nblk_owned=0 (holds no TX blocks), su NULL, cursors 0. pblk[] must
     * start -1 (0 is a valid block id); a conn grabs its first block LAZILY on the
     * first write (no eager borrow at connect/accept). */
    ctx->ports = (struct dmesh_port_slot *)calloc(DMESH_PORT_SPACE, sizeof(struct dmesh_port_slot));
    if (!ctx->ports) goto fail;
    for (uint32_t p = 0; p < DMESH_PORT_SPACE; p++)
        for (int b = 0; b < DMESH_TX_MAXB_CAP; b++)
            ctx->ports[p].pblk[b] = -1;
    pthread_mutex_init(&ctx->port_lock, NULL);
    ctx->next_port = 1;

    ctx->doca_objs.rx_data_hook = rx_data_hook;
    ctx->doca_objs.rx_hook_ctx = ctx;

    ctx->pe_running = 1;
    if (pthread_create(&ctx->pe_tid, NULL, pe_progress_fn, ctx) != 0) {
        ctx->pe_running = 0;   /* cleanup must not join a never-created thread */
        goto fail;
    }

    DOCA_LOG_INFO("DPUmesh DOCA initialized: worker=%s pod_id=%d", ctx->worker_id, ctx->pod_id);

    *out = ctx;
    return 0;

fail:
    cleanup_ctx(ctx);
    return -1;
}

static void cleanup_ctx(dpumesh_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->pe_running) {
        ctx->pe_running = 0;
        pthread_join(ctx->pe_tid, NULL);
    }
    /* PE thread joined → no more dpumesh_notify() writers; safe to close. */
    if (ctx->notify_efd >= 0) { close(ctx->notify_efd); ctx->notify_efd = -1; }

    /* Free resources BEFORE destroying locks they depend on. */

    /* Per-conn TX block chains need no drain at teardown — in-flight bytes die with the
     * ctx. (PE thread joined above → no concurrent reserve/reclaim.) Free the shared
     * block pool + per-port send-unit FIFOs. */
    if (ctx->block_next) { free(ctx->block_next); ctx->block_next = NULL; }
    pthread_mutex_destroy(&ctx->block_lock);
    if (ctx->ports) {
        for (uint32_t p = 0; p < DMESH_PORT_SPACE; p++) {
            if (ctx->ports[p].su_seq) { free(ctx->ports[p].su_seq); ctx->ports[p].su_seq = NULL; }
            if (ctx->ports[p].su_end) { free(ctx->ports[p].su_end); ctx->ports[p].su_end = NULL; }
        }
    }

    /* Destroy DMA landing-zone mmap + buffer (Host RX DMA buffer).
     * Must happen before cleanup_objects destroys the device. */
    if (ctx->rx_dma_mmap) {
        doca_mmap_destroy(ctx->rx_dma_mmap);
        ctx->rx_dma_mmap = NULL;
    }
    if (ctx->rx_dma_buffer) {
        free(ctx->rx_dma_buffer);
        ctx->rx_dma_buffer = NULL;
    }

    cleanup_objects(&ctx->doca_objs);

    for (int j = 0; j < MAX_EU_PER_POD; j++)
        if (ctx->dma_rings[j]) { free(ctx->dma_rings[j]->seq); ctx->dma_rings[j]->seq = NULL; }
    if (ctx->rx_ring) free(ctx->rx_ring);
    if (ctx->ports) { free(ctx->ports); ctx->ports = NULL; pthread_mutex_destroy(&ctx->port_lock); }

    free(ctx);
}

void dpumesh_destroy(dpumesh_ctx_t *ctx) {
    if (!ctx) return;
    DOCA_LOG_INFO("Destroying DPUmesh context: worker=%s", ctx->worker_id);
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

/* Reserve `len` CONTIGUOUS bytes (ONE message, ≤ block_size) at this conn's write head,
 * returning a pointer into the shared TX mmap to fill (then dpumesh_tx_commit). A message
 * that would straddle the current block's end pads the tail and starts a fresh block, so
 * each message is contiguous in one physical block. Grabs a block on demand (GROW, up to
 * maxb) — reusing a recycled block first — else busy-spins (own ACKs free a block). NULL
 * if len==0 or len>block_size. Owner thread only. */
uint8_t *dpumesh_tx_reserve(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return NULL;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint64_t bs = (uint64_t)ctx->block_size, maxb = (uint64_t)ctx->maxb;
    if (len == 0 || (uint64_t)len > bs) return NULL;      /* one message must fit a block */
    if (!psl->su_seq) {                                    /* lazy per-slot send-unit FIFO */
        psl->su_seq = (uint16_t *)malloc(TX_SU_DEPTH * sizeof(uint16_t));
        psl->su_end = (uint64_t *)malloc(TX_SU_DEPTH * sizeof(uint64_t));
        if (!psl->su_seq || !psl->su_end) return NULL;
    }
    tx_refresh_blocks(ctx, psl);                           /* recycle / compact / shrink first */
    uint64_t k   = psl->tx_w / bs;
    uint32_t off = (uint32_t)(psl->tx_w % bs);
    if ((uint64_t)off + len > bs) {                        /* won't fit → pad + move to next block */
        psl->blk_used[k % maxb] = off;                     /* seal block k content end */
        psl->tx_w = (k + 1) * bs;                          /* pad to the next block boundary */
        k   = psl->tx_w / bs;                              /* = old k + 1 */
        off = 0;
        atomic_fetch_add_explicit(&ctx->st_block_pads, 1, memory_order_relaxed);
    }
    /* Back every logical block up to k with a physical block, ONCE each (head_blk_next
     * tracks the highest assigned). For each new block b, WAIT until block b-maxb has
     * drained (the live window stays ≤ maxb blocks) so slot b%maxb is free — this is the
     * per-conn in-flight cap AND it prevents reusing a still-live earlier block. Reuse a
     * recycled block first (no pool op), else grab from the shared pool. */
    struct timespec backoff = {0, 1000};
    while (psl->head_blk_next <= k) {
        uint64_t b = psl->head_blk_next;
        int bslot = (int)(b % maxb);
        for (;;) {
            if (b - psl->tail_blk < (uint64_t)maxb) {      /* block b-maxb drained → slot b%maxb free */
                if (psl->nrec > 0) {                        /* reuse a recycled block (no pool op) */
                    psl->pblk[bslot] = psl->recyc[--psl->nrec];
                    atomic_fetch_add_explicit(&ctx->st_recycle_hits, 1, memory_order_relaxed);
                    break;
                }
                if (psl->nblk_owned < ctx->maxb) {         /* GROW: grab from the shared pool */
                    int32_t phys = block_pool_grab(ctx);
                    if (phys >= 0) { psl->pblk[bslot] = phys; psl->nblk_owned++; break; }
                }
            }
            atomic_fetch_add_explicit(&ctx->st_grow_waits, 1, memory_order_relaxed);
            nanosleep(&backoff, NULL);                      /* window full OR pool empty → wait */
            if (backoff.tv_nsec < 50000) backoff.tv_nsec *= 2;
            tx_refresh_blocks(ctx, psl);                    /* an ACK may have drained a block */
        }
        psl->blk_used[bslot] = 0;
        psl->head_blk_next = b + 1;
    }
    int s = (int)(k % maxb);
    return (uint8_t *)ctx->dma_buffer + (size_t)psl->pblk[s] * (size_t)bs + off;
}

/* Finalize `len` bytes (<= the reserved len) as committed message bytes, ready to ship.
 * Advances tx_w + tx_c and records the block's content end. Owner thread. */
void dpumesh_tx_commit(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len) {
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (psl->nblk_owned <= 0) return;
    uint64_t bs = (uint64_t)ctx->block_size;
    uint64_t k = psl->tx_w / bs;                           /* block the reserve placed the body in */
    psl->tx_w += len;
    psl->tx_c  = psl->tx_w;
    psl->blk_used[k % (uint64_t)ctx->maxb] = (uint32_t)(psl->tx_w - k * bs);  /* content end in block k */
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
 * pad). Skips a padded block tail transparently. BLOCKS if the send-unit FIFO is full.
 * 1 if one, 0 if nothing. Does NOT advance tx_s (call dpumesh_tx_sent). Owner thread. */
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
        /* Back-pressure on the send-unit FIFO (bounds in-flight descriptors per conn). */
        struct timespec backoff = {0, 1000};
        while ((uint16_t)(atomic_load_explicit(&psl->su_head, memory_order_relaxed) -
                          atomic_load_explicit(&psl->su_tail, memory_order_acquire)) >= (uint16_t)TX_SU_DEPTH) {
            nanosleep(&backoff, NULL);
            if (backoff.tv_nsec < 50000) backoff.tv_nsec *= 2;
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

/* Reclaim on BATCH_FWD_ACK(port,seq): pop the send-unit FIFO front when its seq matches
 * (FIFO — a conn ships + ACKs in seq order) and advance the free cursor tx_f to that
 * unit's end. The PE advances tx_f/su_tail ONLY; the block chain is owner-managed while
 * live, so this returns blocks only for a CLOSED conn (try_return_blocks). */
static inline void tx_reclaim_ack(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    uint16_t tail = atomic_load_explicit(&psl->su_tail, memory_order_relaxed);
    uint16_t head = atomic_load_explicit(&psl->su_head, memory_order_acquire);
    if (tail == head) return;                              /* nothing outstanding (su may be NULL) */
    /* head != tail ⇒ the owner shipped ⇒ su_seq/su_end are allocated + visible (the
     * su_head acquire orders the owner's lazy malloc before this). */
    size_t idx = (size_t)(tail & (TX_SU_DEPTH - 1));
    if (psl->su_seq[idx] == seq) {
        atomic_store_explicit(&psl->tx_f, psl->su_end[idx], memory_order_release);
        atomic_store_explicit(&psl->su_tail, (uint_fast16_t)(tail + 1), memory_order_release);
        try_return_blocks(ctx, psl);                       /* return blocks if this drained a CLOSED conn */
    }
}

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
    uint64_t t = __atomic_fetch_add(&ring->enq_pos, 1, __ATOMIC_RELAXED);
    ring_slot = (uint32_t)(t % ring->size);
    dma = &ring->descs[ring_slot];
    {
        struct timespec backoff = {0, 1000}; /* 1µs initial */
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
    dma->route_group = desc->route_group;   /* route-affinity key (0 = normal per-message LB) */

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

/* Poll-mode dequeue tuning: spin this many empty racy-checks before yielding
 * the core; at load the queue is rarely empty so the backoff is never reached
 * (no wakeup, no context switch). Mirrors the PE adaptive-poll pattern. */
#define RX_POLL_SPIN       1024
#define RX_POLL_BACKOFF_NS 20000   /* 20 us */

int dpumesh_dequeue(dpumesh_ctx_t *ctx, sw_descriptor_t *desc, int timeout_ms) {
    {
        /* Lock-free spin-poll on the Vyukov RX ring; back off after sustained
         * idle. timeout_ms: 0 = non-blocking try, >0 = poll to deadline, <0 =
         * poll forever. There is NO cond-blocking path — the façade always polls. */
        struct timespec deadline; int have_dl = 0;
        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec  += timeout_ms / 1000;
            deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
            have_dl = 1;
        }
        uint32_t idle = 0;
        for (;;) {
            if (rxq_try_pop(ctx, desc))
                return 0;
            if (timeout_ms == 0) return -1;            /* non-blocking try */
            if (++idle >= RX_POLL_SPIN) {
                if (have_dl) {
                    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                    if (now.tv_sec > deadline.tv_sec ||
                        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                        return -1;
                }
                struct timespec t = {0, RX_POLL_BACKOFF_NS};
                nanosleep(&t, NULL);
            }
        }
    }
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

void dpumesh_get_tx_pool_stats(dpumesh_ctx_t *ctx, dpumesh_tx_pool_stats_t *out) {
    if (!ctx || !out) return;
    out->pool_grabs   = atomic_load_explicit(&ctx->st_pool_grabs,   memory_order_relaxed);
    out->pool_returns = atomic_load_explicit(&ctx->st_pool_returns, memory_order_relaxed);
    out->recycle_hits = atomic_load_explicit(&ctx->st_recycle_hits, memory_order_relaxed);
    out->grow_waits   = atomic_load_explicit(&ctx->st_grow_waits,   memory_order_relaxed);
    out->block_pads   = atomic_load_explicit(&ctx->st_block_pads,   memory_order_relaxed);
}

/* Enable + return the readiness eventfd: a real fd that becomes readable
 * whenever an inbound request/response is delivered, so a caller can wait on it
 * with a VANILLA epoll/poll/select instead of busy-polling dequeue/conn_recv.
 * The PE thread (notification-driven under DPUMESH_HOST_EPOLL=1) writes it on each
 * delivery. Drain it with a single read() of a uint64_t per wakeup, then collect
 * ready work via dpumesh_dequeue(0)/dpumesh_conn_recv(). Returns -1 if the
 * eventfd could not be created. Idempotent; level-triggered-friendly. */
int dpumesh_get_event_fd(dpumesh_ctx_t *ctx) {
    if (!ctx || ctx->notify_efd < 0) return -1;
    ctx->notify_enabled = 1;
    return ctx->notify_efd;
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
 * `user` is the app's conn handle (returned later by dmesh_next_ready); it is
 * stored BEFORE role is published so the PE, which may deliver + enqueue this port
 * the instant it sees role!=FREE, never hands back a NULL handle. The role is
 * published with RELEASE so the PE's ACQUIRE-load sees a fully-initialized slot.
 * Returns 0 on exhaustion. */
uint16_t dpumesh_alloc_port(dpumesh_ctx_t *ctx, int role, void *user) {
    pthread_mutex_lock(&ctx->port_lock);
    /* Model B: host CLIENT conns use [1, DMESH_UPORT_BASE); accepted SERVER conns
     * get their port from the DPU-assigned upstream id via
     * dpumesh_alloc_port_specific. Capping the round-robin here keeps the two
     * ranges disjoint so a loopback host never collides in its ports[] table. */
    if (ctx->next_port == 0 || ctx->next_port >= DMESH_UPORT_BASE) ctx->next_port = 1;
    for (uint32_t scanned = 0; scanned < DMESH_UPORT_BASE - 1; scanned++) {
        uint32_t p = ctx->next_port;
        ctx->next_port = (p + 1 >= DMESH_UPORT_BASE) ? 1 : p + 1;  /* wrap in [1, UPORT_BASE) */
        struct dmesh_port_slot *psl = &ctx->ports[p];
        if (psl->role == DMESH_ROLE_FREE && psl->nblk_owned <= 0) { /* skip FREE-but-draining */
            /* Per-port inbound ring is allocated once and KEPT for the lifetime of
             * the process (reused when this port number is reallocated) — never
             * freed mid-run, so a stale PE delivery can't use-after-free it. */
            if (!psl->inbox) {
                psl->inbox = (sw_descriptor_t *)malloc(DMESH_INBOX_RING * sizeof(sw_descriptor_t));
                if (!psl->inbox) { pthread_mutex_unlock(&ctx->port_lock); return 0; }
            } else {
                /* A straggler reply may have landed in this slot's inbox AFTER the
                 * previous owner's dmesh_close drained it (close/deliver race).
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
            port_reset_tx(psl);         /* fresh block-chain cursors; first block grabbed LAZILY on first write */
            /* Publish role LAST (RELEASE) so the PE's ACQUIRE-load sees the fully
             * initialized inbox/head/tail/user/chain before it can deliver here. */
            __atomic_store_n(&psl->role, (uint8_t)role, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&ctx->port_lock);
            return (uint16_t)p;
        }
    }
    pthread_mutex_unlock(&ctx->port_lock);
    DOCA_LOG_ERR("dpumesh_alloc_port: no free ports");
    return 0;
}

/* Allocate a SPECIFIC port slot (model B accept): the DPU already assigned this
 * connection's id (`p`, a uP in [DMESH_UPORT_BASE, 65535)), and the backend must
 * bind exactly that port so the DPU can address it. Returns `p` on success, or 0
 * if the slot is already live (a duplicate pre-accept message — caller drops it;
 * coalescing of pre-accept bursts is a later stage). Mirrors dpumesh_alloc_port's
 * publication ordering (init inbox/head/tail/user BEFORE RELEASE-storing role). */
uint16_t dpumesh_alloc_port_specific(dpumesh_ctx_t *ctx, uint16_t p, int role, void *user) {
    if (p == 0 || p >= DMESH_PORT_SPACE) return 0;
    pthread_mutex_lock(&ctx->port_lock);
    struct dmesh_port_slot *psl = &ctx->ports[p];
    if (psl->role != DMESH_ROLE_FREE || psl->nblk_owned > 0) {
        pthread_mutex_unlock(&ctx->port_lock);
        return 0;   /* already live, or FREE-but-draining (blocks not yet returned) */
    }
    if (!psl->inbox) {
        psl->inbox = (sw_descriptor_t *)malloc(DMESH_INBOX_RING * sizeof(sw_descriptor_t));
        if (!psl->inbox) { pthread_mutex_unlock(&ctx->port_lock); return 0; }
    } else {
        /* Drain a straggler delivery from a prior owner (close/deliver race) so its
         * RX credit is returned, not discarded by the head/tail reset below. */
        sw_descriptor_t d;
        while (inbox_pop(psl, &d)) rx_credit_return(ctx, d.body_buf_slot);
    }
    atomic_store_explicit(&psl->in_head, 0, memory_order_relaxed);
    atomic_store_explicit(&psl->in_tail, 0, memory_order_relaxed);
    psl->peer_pod  = DMESH_POD_BLANK;
    psl->peer_port = 0;
    psl->user      = user;
    port_reset_tx(psl);   /* fresh block-chain cursors; first block grabbed LAZILY on first reply */
    __atomic_store_n(&psl->role, (uint8_t)role, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ctx->port_lock);
    return p;
}

/* Promote a PE-created SERVER_PENDING slot to a live SERVER conn (model B accept):
 * attach the app's conn handle so dmesh_next_ready starts returning it. The PE
 * already allocated the inbox + set the peer + published SERVER_PENDING (message 1
 * rode the accept queue; any messages 2..P are already coalesced in the inbox).
 * Returns `port` on success, 0 if the slot is not pending. */
uint16_t dpumesh_accept_port(dpumesh_ctx_t *ctx, uint16_t port, void *user) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return 0;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    pthread_mutex_lock(&ctx->port_lock);
    if (__atomic_load_n(&psl->role, __ATOMIC_ACQUIRE) != DMESH_ROLE_SERVER_PENDING) {
        pthread_mutex_unlock(&ctx->port_lock);
        return 0;   /* not pending (already accepted / freed / race) */
    }
    psl->user = user;
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
    try_return_blocks(ctx, psl);
    if (psl->inbox) {
        sw_descriptor_t d;
        while (inbox_pop(psl, &d)) rx_credit_return(ctx, d.body_buf_slot);
    }
}

/* Pop the next inbound message descriptor for a conn (CLIENT or SERVER — one
 * path). Returns 1 + fills *out, or 0 if the conn inbox is empty. The body is in
 * the shared RX mmap at out->body_buf_slot (a landing pos). */
int dpumesh_conn_recv(dpumesh_ctx_t *ctx, uint16_t port, sw_descriptor_t *out) {
    if (port == 0 || port >= DMESH_PORT_SPACE) return 0;
    struct dmesh_port_slot *psl = &ctx->ports[port];
    if (!psl->inbox) return 0;
    return inbox_pop(psl, out);
}

/* Pop the next conn that has inbound, from the PE-published ready list, and return
 * its app handle (the `user` registered at alloc). NULL when the list is drained.
 * No scan: the PE put exactly the ready conns here. A list entry whose conn has
 * since closed (role==FREE) is skipped — its port may even have been recycled, but
 * round-robin allocation makes that astronomically distant; either way a stale
 * entry only ever costs one extra empty drain. Single-consumer (the event loop);
 * call it after waking on dmesh_get_event_fd, drain each returned conn to EAGAIN. */
void *dpumesh_next_ready(dpumesh_ctx_t *ctx) {
    uint16_t port;
    while (ready_pop(ctx, &port)) {
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

