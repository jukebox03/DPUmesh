/*
 * dmesh.h — the DPUmesh native API. RDMA-verbs-shaped, zero-copy both ways.
 *
 * This is ONE of the two surfaces DPUmesh offers, and the only one you program
 * against directly:
 *
 *   1. THIS header — the native API. Zero-copy send AND receive; no memcpy, no
 *      syscall and no thread on the data path. Every call is a caller-thread
 *      re-arrangement of the internal transport.
 *   2. The LD_PRELOAD shim (libdmesh_preload.so) — the POSIX socket ABI, for
 *      UNMODIFIED binaries. You never call it; it impersonates libc.
 *
 * Both sit directly on the internal core (src/dmesh_core.h, NOT installed).
 * Neither depends on the other. See design/API_REDESIGN.md.
 *
 * WHY VERBS-SHAPED AND NOT SOCKET-SHAPED — it is faster, not just more familiar.
 * DPUmesh's substrate already IS RDMA-shaped: a pre-registered mmap, a DMA
 * engine, credit admission, completions. read(2) semantics MANDATE a copy into
 * caller memory, so a socket-shaped API cannot be zero-copy — the copy is the
 * price of the POSIX contract, and only the shim should pay it.
 *
 * OBJECT MAPPING (ibverbs -> DPUmesh):
 *   context + PD                      ->  dmesh_channel_t (folded)
 *   ibv_create_cq (+ comp_channel)    ->  dmesh_create_cq  -> dmesh_cq_t
 *   ibv_create_qp (cq-bound)          ->  dmesh_create_qp    -> dmesh_qp_t
 *     RC (connected, ordered)         ->  dmesh_create_qp + dmesh_pin_route
 *     service-addressed (reliable,    ->  dmesh_create_qp alone (the DPU LBs it to a
 *     datagram-ish; closest to IB RD      backend; STICKY per conn by DEFAULT, so
 *     or Mellanox DC — NOT UD, which      replies keep send order. Per-message LB is
 *     is unreliable and needs an AH)      opt-in per service, and only then may
 *                                         replies interleave across backends.)
 *   rdma_cm CONNECT_REQUEST           ->  DMESH_WC_CONN_REQ completion
 *   ibv_post_send (+ WR list)         ->  dmesh_post_send (+ DMESH_SEND_MORE)
 *   ibv_poll_cq                       ->  dmesh_poll_cq
 *   SRQ recv-buffer repost            ->  dmesh_wc_release
 *   ibv_req_notify_cq + comp_channel  ->  dmesh_cq_fd + drain-then-poll (below)
 *
 * The split mirrors how a real RDMA program is written: rdma_cm for connection
 * setup, ibv_* for the data path.
 *
 * MEMORY MODEL — registered pool, NOT ibv_reg_mr:
 *   There is no registration of arbitrary app memory (the DPU DMAs only from the
 *   mmap exported at init; per-buffer registration would put an address-
 *   translation lookup on the DPU hot path — the one thing an RDMA NIC does in
 *   hardware). SEND buffers come from the conn's pre-registered TX byte-ring:
 *   dmesh_alloc(c, len) IS the "registered buffer" allocator.
 *
 * DELIBERATE DEVIATIONS FROM VERBS (stated, not hidden):
 *   - No one-sided READ/WRITE, no rkey. Arbitrary remote-address translation
 *     needs a lookup table on the DPU datapath. Out of scope by design.
 *   - No ibv_reg_mr. See MEMORY MODEL.
 *   - context + PD folded into dmesh_channel_t: PD is meaningless without
 *     arbitrary reg_mr, so there are no MRs for it to scope. The CQ is NOT
 *     folded in — it is the one object carrying a concurrency constraint, and
 *     folding it made RX single-consumer for the whole process.
 *   - No send completions, signaled or otherwise. Unsignaled is the verbs
 *     default and the high-performance idiom; here it is the only mode, so the
 *     signal that SQ space freed is simply that a later dmesh_alloc succeeds.
 *
 * SEND CONTRACT (the price of zero-copy + O(1) reclaim — the TX ring is FIFO):
 *   - At most ONE outstanding (alloc'd but not yet posted) buffer per conn.
 *     A second dmesh_alloc of the SAME len before dmesh_post_send returns the same
 *     buffer; re-allocing with a DIFFERENT len is undefined (it may reposition the
 *     reserve) — this is the "one outstanding alloc" contract, not a resize primitive.
 *   - Post order == alloc order == wire order on a conn (like a verbs SQ, where
 *     wire order is post order; here the buffer IS the queue slot).
 *   - Ownership transfers to the transport AT POST (no send completion; reclaim
 *     is internal on the DPU's TX_ACK). Do not touch the buffer after posting.
 *   - alloc length cap = dmesh_post_max (one contiguous reserve). A post of
 *     len <= dmesh_msg_max (slot_size) arrives as exactly ONE RECV completion;
 *     a larger post (<= post_max) is legal but is delivered as MULTIPLE
 *     <= slot_size RECV completions, in order on a server/pinned conn — the
 *     receiver reassembles (app framing, as with a byte stream).
 *   - Different conns are fully independent (per-conn rings).
 *
 * BACKPRESSURE — dmesh_alloc NEVER BLOCKS:
 *   It returns NULL + errno=EAGAIN when there is no ring space right now. It does
 *   NOT sleep — you already have exactly ONE wait point (the CQ, via dmesh_cq_fd),
 *   and a second one inside alloc would both contradict that shape and stall every
 *   other conn on your thread. On EAGAIN: do other work and retry — a later alloc
 *   succeeds once the DPU's TX_ACKs free space.
 *
 *   EAGAIN has TWO causes, and you can neither tell them apart nor need to:
 *     - this conn hit its own in-flight ceiling (DPUMESH_TX_MAXB blocks), OR
 *     - the process-wide block pool is momentarily empty because OTHER conns hold it.
 *   The second is not your conn's doing and NO amount of per-conn accounting prevents
 *   it. EAGAIN is therefore a normal resource condition, NOT a caller error — never
 *   treat it as fatal. (This is exactly where the analogy to ibv_post_send's ENOMEM
 *   stops: verbs sends from YOUR memory, so only the descriptor count is finite and
 *   overrunning it IS your bug. Here the buffer itself is a finite shared resource.)
 *
 *   It DOES occur (dmesh_get_tx_stats): measured 2026-07-16 on the default deploy,
 *   grow_waits = 0 at 64 B / 1 KB but 554-662 at 8 KB x conc 32. Payload- and DPU-
 *   config-dependent — treat EAGAIN as live code, not a theoretical branch. See
 *   "Handling EAGAIN" on dmesh_alloc below.
 *
 * RECV MODEL — SRQ + credit:
 *   The transport owns the landing buffers; there is no per-QP post_recv. Each
 *   DMESH_WC_RECV holds one RX credit until you dmesh_wc_release() it — release
 *   promptly, credits bound RX admission (like reposting to an SRQ). Releasing
 *   is valid even after the conn is closed.
 *
 * SCALING — ONE CQ PER THREAD, exactly as in verbs:
 *   A CQ is single-consumer: poll it from ONE thread only. That is not a
 *   limitation, it is why you make several. Each dmesh_cq_t owns its own ready
 *   list and its own completion fd, and a conn belongs to the CQ that created
 *   (dmesh_create_qp) or accepted (DMESH_WC_CONN_REQ) it — so N threads with N CQs
 *   receive genuinely in parallel, sharing nothing on the RX path. One CQ for
 *   the whole process serializes every thread behind one ready list.
 *
 *   Inbound conns are the exception that needs no machinery: the accept queue is
 *   channel-wide and multi-consumer, so every CQ may poll it and whichever one
 *   accepts a conn owns it thereafter — SO_REUSEPORT-style distribution for free.
 *
 * EVENT LOOP (per CQ; single-consumer):
 *   dmesh_cq_t *cq = dmesh_create_cq(ch);
 *   int fd = dmesh_cq_fd(cq);                // register in a vanilla epoll
 *   on EPOLLIN:
 *     uint64_t u; read(fd, &u, 8);           // drain the counter
 *     dmesh_wc_t wc[64]; int n;
 *     while ((n = dmesh_poll_cq(cq, wc, 64)) > 0)
 *       for (int i = 0; i < n; i++) switch (wc[i].opcode) {
 *         case DMESH_WC_CONN_REQ: break;   // new conn = wc[i].qp, already
 *                                          // usable; refuse by dmesh_destroy_qp
 *         case DMESH_WC_RECV:     handle(wc[i].buf, wc[i].len);
 *                                 dmesh_wc_release(ch, &wc[i]); break;
 *         case DMESH_WC_RECV_FIN: dmesh_destroy_qp(wc[i].qp); break;
 *       }
 *   Always poll to 0 before sleeping on the fd again (edge-triggered rule, same
 *   as ibv_req_notify_cq re-arm). Completions for ONE conn are in order; across
 *   conns there is no order.
 *
 * SEND PATH:
 *   void *b = dmesh_alloc(c, len);           // NULL+EAGAIN = SQ full, retry later
 *   ...fill b in place (zero-copy)...
 *   dmesh_post_send(c, b, len, 0, 0);        // post + doorbell
 *   // batching (verbs WR-list equivalent): N x post_send(.., DMESH_SEND_MORE)
 *   // then one dmesh_flush(c) — or a final post_send without the flag.
 */
#ifndef DMESH_H
#define DMESH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "dmesh_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The internal transport context. Opaque here BY DESIGN: the core
 * (src/dmesh_core.h) is not a public API and is not installed. */
struct dpumesh_ctx;

/* ===== Handles ===== */

/* Channel — one per process. context + PD, folded. */
typedef struct dmesh_channel {
    struct dpumesh_ctx *ctx;
    int            pod_id;
    int            slot_size;   /* max body per RECV completion (wire DMA cap) */
    int            block_size;  /* max contiguous message = alloc/post cap */
    uint32_t       next_group;  /* GLOBAL rolling route-affinity id source (atomic across
                                 * all conns -> 1..255): concurrent large messages get
                                 * DISTINCT groups. A per-conn counter would collide —
                                 * every conn's first large message would pick id 1. */
} dmesh_channel_t;

/* Completion queue — one per polling THREAD. Opaque: it owns the ready list the
 * PE thread pushes to, which is single-producer/single-consumer and internal. */
typedef struct dmesh_cq dmesh_cq_t;

/* Connection — a persistent full-duplex link to one peer. The QP. */
typedef struct dmesh_qp {
    dmesh_channel_t *ep;
    dmesh_cq_t      *cq;       /* the CQ this conn's completions land on (the QP's CQ,
                                * fixed at connect/accept): its creator, and the only
                                * thread allowed to poll them. */
    void     *user_data;       /* APP-OWNED (like a verbs qp_context / epoll data.ptr):
                                * you set it, the transport never touches it. */
    int       role;            /* DMESH_ROLE_CLIENT (connect) | DMESH_ROLE_SERVER (accept) */

    /* addressing */
    uint16_t  local_port;      /* my port (this conn's id) */
    int16_t   dst_service;     /* peer service (the service connected to / caller's) */
    int16_t   remote_pod;      /* SERVER: the DPU-facing peer pod (learned at accept).
                                * CLIENT: always DMESH_POD_BLANK (Model B never pins). */
    uint16_t  remote_port;     /* SERVER: the peer uP (learned at accept); CLIENT: 0. */
    uint8_t   peer_closed;     /* received the peer's FIN -> EOF (sticky) */
    uint16_t  seq;             /* per-conn OUTBOUND message counter */

    /* inbound view (rx_slot>=0 => one message is held on the conn; the compat
     * layer's partial-read cursor lives here too) */
    int            rx_slot;    /* landing byte-offset in host RX buffer; -1 = none */
    const uint8_t *rx_buf;
    uint32_t       rx_len;
    uint32_t       rx_pos;

    /* Outbound bytes live in the per-conn TX byte-ring (keyed by local_port);
     * the conn holds no TX buffer state of its own. */

    /* CONNECTION-level route affinity (dmesh_pin_route): 0 = no affinity group (default).
     * NOT the same as "per-message LB": the DPU is conn-STICKY by default, so a group-less
     * conn already keeps its first backend. The group MATTERS when the service opted into
     * per-message LB (DPUMESH_LB_PER_REQUEST_SVC) — then it is the only thing pinning the
     * conn. Non-zero = a route_group stamped on EVERY outbound message of this conn (and
     * its FIN), so the DPU pins the whole conn to the ONE backend picked for its first
     * message — socket-like total order on the conn, whatever the service's LB policy.
     * Group ids are a per-channel rolling 255-space, so unrelated
     * conns/channels reuse a byte; the DPU keys its pin table by (dst_service, id), so
     * a collision can only merge SAME-SERVICE traffic onto one backend (balance skew,
     * ordering intact) — it can never redirect a conn to another service's backend. */
    uint8_t   pin_group;
} dmesh_qp_t;

/* ===== Completions ===== */

typedef enum {
    DMESH_WC_RECV     = 1,   /* one whole inbound message; holds an RX credit */
    DMESH_WC_RECV_FIN = 2,   /* peer closed the conn (EOF); no credit held */
    DMESH_WC_CONN_REQ = 3,   /* new inbound conn (server side); no credit held */
} dmesh_wc_opcode_t;

typedef struct dmesh_wc {
    dmesh_qp_t       *qp;       /* the QP this completion belongs to (== the handle from
                                 * dmesh_create_qp / CONN_REQ; use qp->user_data for
                                 * your per-conn context) */
    dmesh_wc_opcode_t opcode;
    const uint8_t    *buf;      /* RECV: points INTO the RX mmap (zero-copy); valid
                                 * until dmesh_wc_release. Else NULL. */
    uint32_t          len;      /* RECV: message length (<= slot_size). Else 0. */
    int32_t           rx_slot;  /* internal release token; -1 = nothing held */
} dmesh_wc_t;

/* ===== Channel lifecycle (ibv_open_device + PD) ===== */

/* Identity is INJECTED, not declared: the node's own service is the k8s Service
 * name in $DPUMESH_SERVICE (a webhook writes it from the pod's labels; unset = a
 * pure client). The transport interns it to a service_id through the same table
 * it resolves peers with — no integer crosses this surface (NAMING.md). The
 * node's pod_id (its address) is ASSIGNED BY THE DPU at register — the caller
 * never picks it; dmesh_pod_id() returns the assigned value. NULL on init failure. */
dmesh_channel_t *dmesh_create_channel(void);

/* Release the channel + all DOCA resources. Safe on NULL. */
void dmesh_destroy_channel(dmesh_channel_t *s);

int dmesh_pod_id(dmesh_channel_t *s);      /* this node's DPU-assigned pod_id */
int dmesh_msg_max(dmesh_channel_t *s);     /* max length arriving as ONE RECV (slot_size) */
int dmesh_post_max(dmesh_channel_t *s);    /* max length of one alloc/post (block size) */

/* ===== Completion queue lifecycle (ibv_create_cq + comp_channel) ===== */

/* Make a CQ on this channel. Create ONE PER POLLING THREAD: each owns its own ready
 * list and completion fd, so threads on distinct CQs never touch shared RX state (see
 * SCALING). Readiness is live from this call — never conditional on dmesh_cq_fd.
 * NULL+ENOMEM on OOM, NULL+EMFILE past the per-channel CQ cap. */
dmesh_cq_t *dmesh_create_cq(dmesh_channel_t *ch);

/* Release a CQ. DESTROY ITS CONNS FIRST (ibv_destroy_cq's EBUSY rule): a conn
 * outliving its CQ has nowhere to report completions. Safe on NULL. */
void dmesh_destroy_cq(dmesh_cq_t *cq);

/* This CQ's completion-channel fd (ibv_req_notify_cq + comp_channel): readable when a
 * new conn or any inbound message is pending ON THIS CQ. Vanilla epoll/poll; drain one
 * uint64_t on wake, then dmesh_poll_cq to 0.
 *
 * PURELY OPTIONAL — it is the idle-sleep path, not a prerequisite. dmesh_poll_cq is
 * complete on its own, so a spin-polling client never has to call this. */
int dmesh_cq_fd(dmesh_cq_t *cq);

/* ===== Connection setup (rdma_cm) ===== */

/* ibv_create_qp(): make a CLIENT QP bound to `cq` and addressed to a logical SERVICE
 * by its k8s Service NAME — the same string the preloaded app hands getaddrinfo()
 * (NAMING.md §1b). The transport resolves it to a service_id at point of use; no
 * integer wire address is ever cached in app code. NULL+ENOENT if the name is not in
 * the registry. This is NOT rdma_connect: it is PURELY LOCAL and does NO round trip —
 * nothing is signalled to the peer, and the QP is established (peer learned) only on
 * its first inbound. NULL+ENOMEM on OOM.
 *
 * The conn's completions land on `cq` — the thread that polls `cq` is the one that
 * services this conn. There is NO dmesh_accept: an inbound conn arrives as a
 * DMESH_WC_CONN_REQ completion (RDMA_CM_EVENT_CONNECT_REQUEST), already usable, on
 * whichever CQ polled it out of the shared accept queue. */
dmesh_qp_t *dmesh_create_qp(dmesh_cq_t *cq, const char *service_name);

/* Pin this connection's outbound routing to ONE backend — datagram-ish -> RC.
 * Claims a route-affinity group from the channel's global id source and stamps it on
 * every subsequent message (and the FIN): the DPU routes the first message by normal
 * LB, records the pick, and every later message reuses it — so replies arrive in send
 * order, like a socket. Call it right after dmesh_create_qp, before any post. Idempotent.
 * Meaningless on a SERVER conn (it already sends to its learned peer). */
void dmesh_pin_route(dmesh_qp_t *c);

/* GRACEFUL close, then destroy — it is both halves, unlike ibv_destroy_qp. Drops any
 * posted-but-unflushed bytes, then (if established) SENDS A FIN so the peer sees EOF
 * (DMESH_WC_RECV_FIN) and the peer + DPU-owned upstream tear down. Shipped-but-un-ACKed
 * bytes are reclaimed by their own TX_ACKs. Reclaims held RX credit, then FREES the qp.
 * Safe on NULL. Returns 0.
 *
 * The qp is INVALID after this. One poll_cq batch can carry a CONN_REQ plus that qp's
 * landed messages, so destroying mid-batch dangles the later wc[] entries that name it
 * — defer closes to a post-batch sweep. */
int dmesh_destroy_qp(dmesh_qp_t *c);

/* ===== Send (ibv_post_send) ===== */

/* Defer the doorbell: the message is committed to the conn's SQ but not shipped; the
 * next dmesh_post_send WITHOUT this flag (or a dmesh_flush) ships everything committed
 * so far in order — the verbs WR-list batching pattern. */
#define DMESH_SEND_MORE      0x1u

/* Reserve `len` CONTIGUOUS bytes in this conn's pre-registered TX ring and return a
 * pointer into transport DMA memory to fill DIRECTLY (no memcpy). Then dmesh_post_send.
 *
 * NEVER BLOCKS. Returns NULL with errno:
 *   EAGAIN   no ring space right now — EITHER this conn hit its in-flight ceiling
 *            OR the shared block pool is momentarily empty (other conns hold it).
 *            A normal resource condition, NOT a caller error. This is the ONLY
 *            transient error.
 *   EINVAL   len == 0, len > dmesh_post_max(), or the conn is not established.
 *
 * Handling EAGAIN — pick by your program's shape, and do NOT die on it:
 *   - Single-conn / request-response: retry in place, draining the CQ each pass:
 *       while (!(b = dmesh_alloc(qp, len))) {
 *           if (errno != EAGAIN) goto dead;
 *           dmesh_poll_cq(cq, wc, N);          // keep the CQ moving while you wait
 *       }
 *   - Reactor (one thread, many conns): do NOT spin like that — it starves the other
 *     conns. Park the body on that conn and re-post it from your loop; see
 *     bench/echo_dpumesh.c. Note the loop must then poll rather than sleep while a
 *     reply is parked (nothing wakes cq_fd when the SQ drains), so keep the parked
 *     set exact: a conn you have given up on MUST NOT read as still-pending, or your
 *     loop spins at 100% forever.
 *
 * There is no separate "commit": dmesh_post_send finalizes the bytes. */
void *dmesh_alloc(dmesh_qp_t *c, uint32_t len);

/* Post one message previously filled in the buffer returned by dmesh_alloc(c).
 * `buf` MUST be the pointer of the conn's most recent (un-posted) dmesh_alloc and
 * `len` <= that alloc's length (contract, not checked — the ring position is implied).
 * `wr_id` is reserved for future send-completion support and is ignored. Returns 0,
 * or -1 with errno:
 *   EINVAL   NULL conn / len == 0 (a FIN is dmesh_destroy_qp, never a 0-length post)
 *   EBADMSG  descriptor enqueue fault at the doorbell (close the conn)
 * Ownership of the bytes transfers to the transport; do not touch buf after. */
int dmesh_post_send(dmesh_qp_t *c, const void *buf, uint32_t len,
                    uint64_t wr_id, unsigned flags);

/* Ring the doorbell for everything posted with DMESH_SEND_MORE. Nothing pending is a
 * no-op. 0 = shipped; -1 EBADMSG = descriptor fault (close the conn).
 *
 * Cannot run out of PER-CONN queue space — dmesh_alloc reserved the send-unit slot up
 * front, so a posted message is always carveable. It CAN, however, back off briefly on
 * the SHARED host->DPU descriptor ring, which is a different (global, all-conns)
 * resource that saturates only when the DPA falls behind the host; a ring wedged past
 * a deadline fails with EBADMSG rather than hanging. So: dmesh_alloc never blocks,
 * dmesh_flush is not quite in that class. Post with DMESH_SEND_MORE and flush from a
 * point in your loop where a rare microsecond-scale wait is acceptable. */
int dmesh_flush(dmesh_qp_t *c);

/* ===== Diagnostics ===== */

/* Elastic TX block-pool counters (cumulative since init). Every field counts a RARE
 * path — the steady-state alloc/post/ACK cycle increments nothing except recycle_hits
 * once per drained block:
 *   pool_grabs   shared-pool CAS pops (a conn growing / taking its first block)
 *   pool_returns shared-pool CAS pushes (shrink surplus / close-drain return)
 *   recycle_hits grow served from the conn's own recycled blocks (no pool op)
 *   grow_waits   dmesh_alloc calls that hit the in-flight ceiling and returned EAGAIN
 *   block_pads   message didn't fit the current block tail -> pad + fresh block
 *
 * grow_waits is the OBSERVABLE COUNTERPART OF EAGAIN: it is how you find out whether
 * your send loop ever actually hits backpressure. The ring grows on demand, so a
 * non-zero grow_waits means the conn reached its in-flight cap, not that memory ran
 * out. Sample it around a workload; if it stays 0, backpressure never happened. */
typedef struct dmesh_tx_stats {
    unsigned long long pool_grabs;
    unsigned long long pool_returns;
    unsigned long long recycle_hits;
    unsigned long long grow_waits;
    unsigned long long block_pads;
} dmesh_tx_stats_t;
void dmesh_get_tx_stats(dmesh_channel_t *s, dmesh_tx_stats_t *out);

/* ===== Completion queue (ibv_poll_cq) ===== */

/* Harvest up to nwc completions into wc[]. NON-BLOCKING; returns the count (0 =
 * drained), or -1 with errno=EINVAL on bad args. Single-consumer: call from the ONE
 * thread that owns this CQ (make more CQs to poll in parallel — see SCALING). Reports
 * only conns bound to THIS cq, plus any new conn it takes off the shared accept queue.
 * Delivers, in this priority: the resumed partially-drained conn from the previous
 * call, then new conns (one DMESH_WC_CONN_REQ each, immediately followed by that
 * conn's already-landed messages), then every ready conn's inbox drained to empty.
 * A conn cut off by wc[] filling up resumes FIRST on the next call, so per-conn
 * ordering always holds and no message is lost. */
int dmesh_poll_cq(dmesh_cq_t *cq, dmesh_wc_t *wc, int nwc);

/* Return a DMESH_WC_RECV completion's RX credit (≈ reposting the buffer to the SRQ).
 * Idempotent (wc->rx_slot is cleared); a no-op for FIN/CONN_REQ entries. Valid after
 * the conn closed — the credit is channel-level, not conn-level. */
void dmesh_wc_release(dmesh_channel_t *s, dmesh_wc_t *wc);

#ifdef __cplusplus
}
#endif

#endif /* DMESH_H */
