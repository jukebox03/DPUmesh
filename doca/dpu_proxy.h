#ifndef DPU_PROXY_H
#define DPU_PROXY_H

/* ARM byte-stream proxy. A service selects, per connection, whether its payload
 * is forwarded at L4 or handed to the L7 layer behind
 * linkerd/include/dmesh_l7.h. */

#include <stdint.h>

struct objects;
struct dmesh_peer_channel;
struct dmesh_peer_table;
struct dmesh_peer_transport;

enum px_progress_state {
    PX_PROGRESS_IDLE = 0,
    PX_PROGRESS_PENDING,
    PX_PROGRESS_PROGRESSED,
};

/* Return a ready pin, or -1 when the stream is terminal. */
static inline int32_t dmesh_l4_pinned_backend(int32_t pinned_backend,
                                              int backend_ready) {
    return pinned_backend >= 0 && backend_ready ? pinned_backend : -1;
}

/* The destination-side admission rule, as a decision over its inputs.
 *
 * `verdict` is the policy answer: 1 admits, 0 refuses, negative when no
 * verdict is available. `strict` is whether the generation grades the
 * destination Service as protected, and `caller_strict` the same for the
 * calling Service.
 *
 * Three rules meet here. A served verdict decides on its own — a policy that
 * refused is a decision, not an absence. Where no verdict is available, a
 * protected destination refuses rather than admitting an unauthenticated
 * stream, and an ungraded or relaxed one carries it, which is what a Service
 * outside the protected set is graded for. And a protected caller reaching an
 * unprotected callee cannot authenticate the callee, so that call stands only
 * where the destination's own policy admitted it explicitly.
 *
 * Returns 1 to admit, 0 to refuse, and sets *mixed when the third rule is what
 * refused it. */
static inline int dmesh_inbound_admits(int verdict, int strict, int caller_strict,
                                       int *mixed) {
    if (mixed)
        *mixed = 0;
    if (verdict >= 0)
        return verdict;
    if (strict)
        return 0;
    if (caller_strict) {
        if (mixed)
            *mixed = 1;
        return 0;
    }
    return 1;
}

/* The proxy's view of one direction of a connection. */
typedef struct dmesh_proxy_conn {
    int32_t  src_pod;
    uint16_t src_port;
    int      is_reply;
    int16_t  dst_service;
    int32_t  peer_pod;
    uint16_t peer_port;
} dmesh_proxy_conn;

/* ---- engine lifecycle / hooks (called from dpu_worker.c) ---- */

/* Create the engine: the SG-DMA DPU→host data path. */
int px_init(struct objects *objs);

/* Re-derive the interned-id → L7 mode table from the DPUMESH_L7_SVC and
 * DPUMESH_L7_OPAQUE_SVC name lists against the held generation. Called by
 * px_init and after every topology adoption. Returns -1 when one Service is
 * named by two lists. */
int px_l7_resolve_modes(struct objects *objs);

/* Process one forward completion on its connection owner. */
int px_process_forward(struct objects *objs, int worker_id,
                       void *entry /* dpu_comp_entry_t* */);

/* Resume connections stalled by egress allocation. */
int px_drain_stalled(struct objects *objs, int worker_id);

/* Progress one ARM worker's SG-DMA engine. */
enum px_progress_state px_worker_drain(struct objects *objs, int worker_id);

/* Constant-time worker state copied by the embedded runtime's maintenance
 * pass. This is intentionally narrower than the proxy's internal structures:
 * it exposes enough state to classify a progress loss without turning the
 * private queue layout into an ABI. */
struct px_worker_stats {
    uint64_t dma_tasks_inflight;
    uint64_t dma_retry_batches;
    uint64_t ack_release_depth;
    uint64_t stalled_connections;
    uint64_t remote_fin_pending;
    uint32_t dma_stalled;
    uint32_t emit_pending;
    uint32_t ack_retry_pending;
};
int px_worker_stats(struct objects *objs, int worker_id,
                    struct px_worker_stats *out);

/* Bind the calling ARM thread to one proxy worker. */
void px_bind_worker(struct objects *objs, int worker_id);

/* The ARM worker an L7 request must be processed on, or -1 when the completion
 * keeps the normal port owner. A selected-worker L7 layer returns its one
 * owner; DPUMESH_L7_LINKERD_WORKER=all returns -1 because every worker has
 * local session state. */
int px_l7_request_owner(struct objects *objs, int32_t dst_pod_id,
                        int16_t dst_service);

/* Report the L7 audit counters: fallbacks by cause and custody violations,
 * plus the engine's drain/DMA counters under DPUMESH_PERF_STATS. Rate-limited
 * internally, and silent while nothing has changed. */
void px_l7_stats_report(struct objects *objs, int worker_id);
/* Peer refusals and poisoned connections, counted by reason.
 * `dmesh_peer_ops.event` reaches this through `px_peer_event_cb`. */
void px_peer_event(struct objects *objs, const char *reason);
void px_peer_stats_report(struct objects *objs, int worker_id);
/* Release one extent whose destination was remote, now that its STREAM_ACK
 * says the bytes landed. `dmesh_peer_ops.release` reaches this through
 * `px_peer_release_cb`. */
void px_peer_release(struct objects *objs, uint8_t kind, void *cookie, uint32_t bytes);
/* Reset peer channels the adopted generation no longer binds, or binds to a
 * different static key. Runs on the control thread on every adoption. */
void px_peer_generation_changed(struct objects *objs);
/* Attach the assumed RDMA transport to one data worker. The lower layer owns
 * its context and accepted connection objects; this layer owns authenticated
 * stream state and all proxy custody above them. */
int px_peer_configure(struct objects *objs, int worker_id,
                      const struct dmesh_peer_transport *transport,
                      void *transport_ctx);
/* The table `px_peer_configure` bound that transport into, which is where the
 * transport has to be told inbound connections land. NULL until it is bound. */
struct dmesh_peer_table *px_peer_table(struct objects *objs, int worker_id);
/* Undo `px_peer_configure`: close what the table holds and leave it without a
 * transport, which is how a worker that never had one looks. The caller still
 * owns the transport context and frees it after this returns. */
void px_peer_detach(struct objects *objs, int worker_id);
/* Close the channels that have been idle past DMESH_CHANNEL_IDLE_NS. Called on
 * the worker's own maintenance cadence, not per pass. */
void px_peer_evict_idle(struct objects *objs, int worker_id);
struct dmesh_peer_channel *
px_peer_accept(struct objects *objs, int worker_id, const char *node_name,
               uint32_t incarnation, void *conn,
               const uint8_t peer_key[32]);
/* Adopt the generation's `protected=` grading. Runs on the control thread when
 * a generation is adopted, so a data worker's decision is a byte read. */
void px_protection_refresh(struct objects *objs);

/* SG-DMA completion notification handle, armed while a worker is parked. */
int px_worker_notification_fd(struct objects *objs, int worker_id);
int px_worker_arm_notification(struct objects *objs, int worker_id);
void px_worker_clear_notification(struct objects *objs, int worker_id, int fd);

/* True only after the egress owner has stopped submitting for this dead pod,
 * every destination DMA/credit read has completed, all lane queues are empty,
 * and no worker completion still names the slot. The control path uses
 * this as the ARM half of POD_QUIESCED before destroying imported host mmaps. */
int px_pod_reclaim_ready(struct objects *objs, int pod_idx);


#endif /* DPU_PROXY_H */
