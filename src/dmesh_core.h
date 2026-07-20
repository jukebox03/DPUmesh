/*
 * dmesh_core.h — DPUmesh transport core. INTERNAL: NOT a public API.
 *
 * This header lives in src/ and is deliberately NOT installed. It is the
 * substrate both public surfaces sit on, and nothing outside this repo may
 * include it:
 *
 *   <dpumesh/dmesh.h>   the native API   -> src/dmesh_api.c
 *   the POSIX socket ABI (LD_PRELOAD)    -> src/dmesh_preload.c
 *                                           (+ src/dmesh_compat.h, the socket
 *                                            byte-stream semantics it needs)
 *
 * The two surfaces are SIBLINGS: neither is built on the other, both call in
 * here. See design/API.md.
 *
 * It was public (include/dpumesh/dmesh_core.h) until the API consolidation.
 * Being reachable was an accident of <dpumesh/dmesh.h> #including it, which put
 * every dpumesh_* symbol in every client's namespace.
 *
 * Connection lifecycle (dmesh_create_qp/close/accept/next_ready) is
 * implemented in dmesh_core.c and shared by both surfaces: it is transport, not
 * a façade — nothing about it is socket- or verbs-specific.
 */

#ifndef DMESH_CORE_H
#define DMESH_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include <dpumesh/dmesh.h>        /* dmesh_channel_t / dmesh_qp_t + the public protos
                                   * implemented in dmesh_core.c (lifecycle) */
#include <dpumesh/dmesh_common.h>

/* ====== Default constants ====== */
#define DPUMESH_SLOT_SIZE_DEFAULT       8192            /* 8KB */
/* Slot pool size (host TX byte-ring + host RX). num_slots × slot_size MUST equal
 * DPU_BUFFER_SIZE so in-flight bytes inside the DPU staging stay bounded (TX
 * byte-ring occupancy; RX slot admission). */
#define DPUMESH_NUM_SLOTS_DEFAULT       4096
/* The host→DPU descriptor ring depth is NOT configurable: it is the wire-ABI
 * constant DMA_RING_SIZE (dpumesh/dmesh_common.h), which the host and the DPA
 * kernel must agree on at build time. */

/* ====== Configuration ====== */
typedef struct {
    int num_slots;        /* slots per pool (0 = default) */
    int slot_size;        /* bytes per slot (0 = default) */
} dpumesh_config_t;

#define DPUMESH_CONFIG_DEFAULT { 0, 0 }

/* ====== SwDescriptor (host-internal RX/TX descriptor, packed) ====== */
/* Host-internal descriptor (NOT a wire layout): the façade builds it for
 * dpumesh_enqueue (translated to dma_desc) and dpumesh_dequeue fills it from a
 * delivered completion. Carries the oriented endpoint tuple — see design/API.md §5/§6. */
typedef struct {
    int32_t  body_buf_slot;         /* TX slot (send) | RX landing byte-offset (recv) */
    uint32_t body_len;
    /* ---- oriented endpoint tuple ---- */
    int16_t  src_pod;               /* sender pod (always concrete) */
    uint16_t src_port;              /* sender port */
    int16_t  src_service;           /* sender's own service (= ep service_id); SVC_NONE if none */
    int16_t  dst_service;           /* peer service (routing input when dst_pod==BLANK) */
    int16_t  dst_pod;               /* dest pod; DMESH_POD_BLANK(-1) -> DPU resolves dst_service */
    uint16_t dst_port;              /* dest port; DMESH_PORT_BLANK(0) -> accept queue */
    uint16_t seq;                   /* per-conn sequence (match key with port) */
    int8_t   valid;
} sw_descriptor_t;

/* ====== Opaque context ====== */
typedef struct dpumesh_ctx dpumesh_ctx_t;

/* Connection table index space: port [1,65535] IS a connection; 0 = BLANK. */
#define DMESH_PORT_SPACE  65536
/* Max CQs per channel (= max parallel RX consumers). Each costs a ready ring +
 * an eventfd, so the cap is what bounds the accept-path notify fan-out. */
#define DMESH_MAX_CQ      64
#define DMESH_TX_READY_WORDS (DMESH_PORT_SPACE / 64)

/* ====== Completion queue (public handle dmesh_cq_t; layout internal) ======
 *
 * ONE CQ = ONE CONSUMER THREAD. Everything a thread needs to receive lives here,
 * so CQs share nothing on the RX path — that is the whole point of the object.
 * A conn binds to the CQ that created (dmesh_create_qp) or accepted it, and the PE
 * pushes its ready-edges to THAT cq's list only.
 *
 * The accept queue is deliberately NOT here: it stays ctx-wide and SPMC, so every
 * CQ may accept and whichever one wins owns the conn (SO_REUSEPORT-like spread,
 * no extra machinery). A new conn therefore notifies EVERY CQ — cold path, once
 * per conn. */
struct dmesh_cq {
    dmesh_channel_t   *ch;
    struct dmesh_qp *accept_spare; /* preallocated before consuming an accept descriptor;
                                    * prevents OOM from stranding SERVER_PENDING */
    struct dmesh_qp *vq_cur;   /* poll_cq resume cursor: the conn whose inbox was only
                                  * partially drained because the caller's wc[] filled (its
                                  * inbox never went empty, so the ready list holds no fresh
                                  * edge — this is the only path back to it). Owned by this
                                  * CQ's thread; dmesh_destroy_qp clears it on a matching conn. */
    int                notify_efd;  /* this CQ's readiness fd; ALWAYS armed (created live —
                                     * dmesh_cq_fd only hands it out, it never enables it). */
    /* eventfd-write gate. The PE writes notify_efd on every inbox empty->non-empty edge
     * so a thread blocked in epoll_wait on the fd wakes — one write() syscall per edge on
     * the PE thread. A caller that only ever POLLS (never sleeps on the fd, e.g. the RPC
     * bench and the verbs client) has nobody to wake, so that syscall is pure waste — and
     * under fair pinning the PE thread shares the app's core, so it also steals the app's
     * cycles. wants_notify latches 1 the first time dmesh_cq_fd hands the fd out (the
     * caller has DECLARED it may sleep); until then cq_notify skips the write. The ready
     * list is armed unconditionally either way, so a poller never misses a conn — only the
     * redundant wakeup is elided. Set-with-self-kick in dmesh_cq_fd closes the arm race. */
    atomic_int         wants_notify;
    int                reg_idx;     /* slot in ctx->cqs[], for destroy */
    atomic_int         nqp;         /* live QPs bound here. Enforces ibv_destroy_cq's EBUSY
                                     * rule instead of only documenting it: a conn outliving
                                     * its CQ reports completions into freed memory. Atomic
                                     * because dmesh_create_qp may run off the CQ's thread;
                                     * touched only at conn create/destroy, never on the
                                     * data path. */
    /* TX readiness is multi-producer: the PE can reclaim a QP while any owner thread
     * can return a surplus block to the channel pool. A bit per port therefore replaces
     * the RX list's SPSC assumption. At most one bit is live per automatically armed QP. */
    atomic_uint_fast64_t tx_ready[DMESH_TX_READY_WORDS];
    atomic_uint_fast32_t tx_ready_count;
    uint32_t             tx_ready_cursor; /* CQ-consumer round-robin word cursor */
    /* PE-published READY LIST for this CQ's conns. The PE pushes a conn's port the
     * moment its inbox goes empty->non-empty; the CQ thread drains it via
     * dmesh_next_ready instead of scanning conns or holding a per-conn fd. SPSC: PE =
     * sole producer (ready_tail), this CQ's thread = sole consumer (ready_head). Sized
     * to the port space so it never overflows (each live conn appears at most once —
     * the on_ready flag dedups). */
    char _rl_pad0[64];
    atomic_uint_fast32_t ready_head;   /* consumer (this CQ's thread) */
    char _rl_pad1[64];
    atomic_uint_fast32_t ready_tail;   /* producer (PE) */
    char _rl_pad2[64];
    uint16_t ready_ring[DMESH_PORT_SPACE];
};

/* ====== Lifecycle ====== */
/* service_id = the service this node advertises (SVC_NONE for a pure client).
 * The node's pod_id is assigned by the DPU at registration (dpumesh_get_pod_id
 * returns it after init). config = NULL uses defaults. */
int  dpumesh_init(dpumesh_ctx_t **ctx, int service_id,
                  const dpumesh_config_t *config);
void dpumesh_destroy(dpumesh_ctx_t *ctx);

/* ====== Name / identity resolution (src/dmesh_resolve.c) ======
 * ONE file-backed table serving BOTH façades. The integer service_id is born
 * here and appears in no public header (NAMING.md §4). The public name-taking
 * constructors in <dpumesh/dmesh.h> are thin wrappers over dmesh_qp_open below:
 *   dmesh_create_channel(void)      = dpumesh_init(dmesh_config_identity())
 *   dmesh_create_qp(cq, name)       = dmesh_qp_open(cq, dmesh_resolve_name(name))
 * The shim is NOT a client of the public API — it resolves by ClusterIP:port and
 * calls dmesh_qp_open with the integer directly. */
int dmesh_config_load(const char *path);          /* NULL → $DPUMESH_CONFIG or /etc/dpumesh/registry; idempotent, load-once */
int dmesh_config_listen_port(void);               /* $DPUMESH_PORT, -1 = not a server */
int dmesh_config_identity(void);                  /* resolve $DPUMESH_SERVICE, SVC_NONE if unset/unknown */
int dmesh_resolve_name(const char *name);         /* name → svc, -1 + ENOENT */
int dmesh_resolve_addr(uint32_t ip_net, uint16_t port_host);  /* ClusterIP:port → svc, -1 = not meshed */

/* Integer entry point for the CLIENT QP, shared by the shim and the public
 * name-taking wrapper. The public dmesh_create_qp(cq, name) lives in
 * dmesh_core.c and calls this after resolve_name. */
dmesh_qp_t *dmesh_qp_open(dmesh_cq_t *cq, int dst_service_id);

/* ====== Query configured values ====== */
int dpumesh_get_slot_size(dpumesh_ctx_t *ctx);
/* Max contiguous message = the per-conn TX block size (the reserve/alloc length cap). */
int dpumesh_get_block_size(dpumesh_ctx_t *ctx);

/* TX pool counters: dmesh_tx_stats_t / dmesh_get_tx_stats, in <dpumesh/dmesh.h>
 * (public — grow_waits is the observable counterpart of dmesh_alloc's EAGAIN). */

/* ====== Info ====== */
int         dpumesh_get_pod_id(dpumesh_ctx_t *ctx);
const char *dpumesh_get_worker_id(dpumesh_ctx_t *ctx);

/* ====== Raw Buffer API ====== */

/* Pop one NEW-connection descriptor off the accept ring. NON-BLOCKING: 0 + *desc,
 * or -1 when empty. Readiness comes from any CQ's fd (dmesh_cq_fd): the ring is
 * SPMC, so every CQ is woken and may pop. */
int dpumesh_dequeue(dpumesh_ctx_t *ctx, sw_descriptor_t *desc);

/* Get pointer to RX buffer data for a slot (zero-copy read). */
uint8_t *dpumesh_rx_buf(dpumesh_ctx_t *ctx, int slot);

/* Free an RX buffer slot after reading. */
void dpumesh_rx_free(dpumesh_ctx_t *ctx, int slot);

/* Per-conn TX BYTE-RING (the SQ). A conn owns a contiguous byte region; messages pack
 * at byte granularity. Lifecycle: reserve -> (fill) -> commit -> ship.
 *
 * dpumesh_tx_reserve(port, len): reserve len CONTIGUOUS bytes (ONE message, <= the TX
 *   block size) at the write head and return a pointer into TX DMA memory to fill
 *   (zero-copy). Grabs a block on demand (grow, elastic pool).
 *
 *   NEVER BLOCKS. The configured block window is bounded so its complete worst-case
 *   slot carve fits the send-unit FIFO; reserve therefore admits ring bytes only.
 *
 *   NULL + errno:
 *     EAGAIN  either the per-conn block window (maxb) is exhausted or the shared
 *             physical block pool is empty. The reserve automatically arms one
 *             CQ TX_READY retry hint; QP ACK reclaim resolves the first cause and
 *             a shared block return resolves the second. st_grow_waits counts both.
 *     EINVAL  len == 0, len > block_size, or the port is not a live conn.
 *
 * dpumesh_tx_commit(port, buf, len): validate the exact reserved pointer and finalize
 *   len (<= reserved) bytes as committed batching input.
 * dpumesh_tx_discard_unsent(port): drop committed-but-unsent bytes (close-before-flush).
 * dpumesh_tx_next_send(port, flush_partial, &moff, &len): select the next descriptor
 *   from the committed range as (mmap byte offset, len<=slot_size). With
 *   flush_partial=0, only a full slot is returned, except that a short tail sealed by
 *   a physical TX-block boundary must be emitted before later blocks can preserve
 *   byte order. With flush_partial=1, the current trailing partial is returned too.
 *   1 if one, 0 if none. Cannot fail on capacity (the whole block window fits the
 *   FIFO). Does NOT advance the send head — enqueue it, then call dpumesh_tx_sent.
 * dpumesh_tx_sent(port, seq, len): record the shipped descriptor (seq->end) + advance
 *   the send head, so a BATCH_FWD_ACK(port,seq) reclaims it.
 * dpumesh_tx_sq_depth(): the per-conn in-flight descriptor cap (TX_SU_DEPTH). */
uint8_t *dpumesh_tx_reserve(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len);
int      dpumesh_tx_commit(dpumesh_ctx_t *ctx, uint16_t port,
                           const void *buf, uint32_t len);
void     dpumesh_tx_discard_unsent(dpumesh_ctx_t *ctx, uint16_t port);
int      dpumesh_tx_next_send(dpumesh_ctx_t *ctx, uint16_t port, int flush_partial,
                              size_t *out_moff, uint32_t *out_len);
void     dpumesh_tx_sent(dpumesh_ctx_t *ctx, uint16_t port, uint16_t seq, uint32_t len);

/* Enqueue a descriptor to TX SQ. Returns 0 on success, -1 on failure. */
int dpumesh_enqueue(dpumesh_ctx_t *ctx, const sw_descriptor_t *desc);

/* ====== Connection API (connection-oriented, full-duplex — no RPC matching) ======
 *
 * A "port" IS a connection (like a socket fd): it owns a peer, an inbound message
 * queue. Inbound is routed by dst_port
 * to the conn's inbox; there is NO request↔response matching. */

/* DMESH_ROLE_* live in <dpumesh/dmesh_common.h> (shared with the DPU side). */

/* Allocate a host-unique conn port (>=1) as CLIENT or SERVER (allocates its inbound
 * ring); 0 on exhaustion. `user` is the app's conn handle, returned later by
 * dpumesh_next_ready; `cq` is the completion queue this conn's ready-edges go to.
 * Both are stored before the port goes live, so a ready-list entry never dereferences
 * NULL. Release with dpumesh_free_port (reclaims undelivered inbound credits). */
uint16_t dpumesh_alloc_port(dpumesh_ctx_t *ctx, int role, void *user, struct dmesh_cq *cq);
/* Promote a PE-created DMESH_ROLE_SERVER_PENDING slot to a live SERVER conn: attach
 * the app's conn handle `user` and bind it to the accepting `cq`. Returns `port` on
 * success, 0 if the slot is not pending (already accepted / freed / race). */
uint16_t dpumesh_accept_port(dpumesh_ctx_t *ctx, uint16_t port, void *user, struct dmesh_cq *cq);
void     dpumesh_free_port(dpumesh_ctx_t *ctx, uint16_t port);

/* Pop the next inbound message descriptor for a conn (CLIENT or SERVER — one path).
 * Returns 1 + fills *out (body at *out->body_buf_slot in the shared RX mmap; free
 * via dpumesh_rx_free after reading), or 0 if the conn inbox is empty. */
int dpumesh_conn_recv(dpumesh_ctx_t *ctx, uint16_t port, sw_descriptor_t *out);

/* Pop the next READY conn ON THIS CQ (one whose inbox went empty→non-empty since you
 * last drained it) and return the `user` handle registered at alloc; NULL when
 * drained. The CQ's fd (dmesh_cq_fd) wakes you; this names the conns to service
 * WITHOUT scanning every conn or holding a per-conn fd. Drain each returned conn to
 * EAGAIN (edge-triggered re-arm). Single-consumer (this CQ's thread). */
void *dpumesh_next_ready(struct dmesh_cq *cq);
/* Pop one QP whose automatically armed dmesh_alloc(EAGAIN) became retryable. */
void *dpumesh_next_tx_ready(struct dmesh_cq *cq);

/* ====== Connection lifecycle — INTERNAL, shared by both surfaces ======
 *
 * These are transport, not façade: nothing here is socket- or verbs-specific, so
 * they live in dmesh_core.c and both src/dmesh_api.c and src/dmesh_preload.c call
 * them. The public half of the lifecycle (dmesh_create_channel / dmesh_create_qp /
 * dmesh_destroy_qp) is declared in <dpumesh/dmesh.h> and implemented in the same
 * file; only the three below stay internal. */

/* Pop the next NEW inbound connection off the channel-wide accept queue and BIND it
 * to `cq`: allocate a SERVER conn that learns its peer (pod,port) and holds the first
 * message body (c->rx_slot). NULL+EAGAIN if none pending; NULL+ENOMEM on alloc failure
 * (the message is dropped, its RX credit reclaimed). The native API folds this into
 * dmesh_poll_cq as DMESH_WC_CONN_REQ; the shim drives it from its dispatcher thread.
 * The queue is SPMC, so several CQs may call this concurrently — each conn goes to
 * exactly one of them, which owns it from then on. */
dmesh_qp_t *dmesh_accept(dmesh_cq_t *cq);

/* Pop the next conn that has inbound, from THIS CQ's ready list (the PE puts ready
 * conns here, so there is NO scan and NO per-conn fd). Returns the SAME conn handle
 * created at accept/connect, or NULL when drained. Single-consumer. */
dmesh_qp_t *dmesh_next_ready(dmesh_cq_t *cq);

/* Submit every complete wire slot currently committed on this QP, leaving only the
 * newest fillable partial slot buffered. This is the post_send fast path; unlike the
 * public dmesh_flush it does not force that trailing partial. */
int dmesh_flush_full(dmesh_qp_t *c);

/* Send a FIN — a zero-length message to the established peer. It rides the SAME
 * conn-shard ring as this conn's data (src_port), so it arrives AFTER every prior
 * message. The peer's PE delivers it as a 0-length descriptor -> the peer sees EOF ->
 * the peer closes -> its port slot is reclaimed. It holds NO ring bytes (no send-unit /
 * reclaim), but it still rides the shared host->DPU forward ring via dpumesh_enqueue, so
 * — like any enqueue — it is normally instant yet CAN back off briefly under ring
 * contention (up to RING_STALL_DEADLINE_SEC before publication): close is therefore not
 * strictly non-blocking. A ring wedged past that deadline returns -1/EBADMSG and does
 * not latch fin_sent, so the failure is observable. Called by graceful close and
 * abort teardown; also used by the shim's shutdown(SHUT_WR). */
int dmesh_send_fin(dmesh_qp_t *c);

#ifdef __cplusplus
}
#endif

#endif /* DMESH_CORE_H */
