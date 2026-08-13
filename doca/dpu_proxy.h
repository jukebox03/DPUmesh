#ifndef DPU_PROXY_H
#define DPU_PROXY_H

/* ARM byte-stream proxy. A service selects, per connection, whether its payload
 * is forwarded at L4 or handed to the L7 layer behind
 * linkerd/include/dmesh_l7.h. */

#include <stdint.h>

struct objects;

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

/* Create the engine. It is the sole DPU→host reverse path. */
int px_init(struct objects *objs);

/* Process one forward completion on its connection owner. */
int px_process_forward(struct objects *objs, int worker_id,
                       void *entry /* dpu_comp_entry_t* */);

/* Resume connections stalled by egress allocation. */
int px_drain_stalled(struct objects *objs, int worker_id);

/* Progress one ARM worker's SG-DMA engine. */
enum px_progress_state px_worker_drain(struct objects *objs, int worker_id);

/* Bind the calling ARM thread to one proxy worker. */
void px_bind_worker(struct objects *objs, int worker_id);

/* The ARM worker an L7 request must be processed on, or -1 when the completion
 * is not one the L7 layer carries. The layer's session state lives on one
 * worker, so its requests are routed there rather than by port. */
int px_l7_request_owner(struct objects *objs, int32_t dst_pod_id,
                        int16_t dst_service);

/* Report the L7 audit counters: fallbacks by cause, custody violations and
 * shared-pool lock traffic. Rate-limited internally, and silent while nothing
 * has changed. */
void px_l7_stats_report(struct objects *objs, int worker_id);

/* SG-DMA completion notification handle, armed while a worker is parked. */
int px_worker_notification_fd(struct objects *objs, int worker_id);
int px_worker_arm_notification(struct objects *objs, int worker_id);
void px_worker_clear_notification(struct objects *objs, int worker_id, int fd);

#ifndef DMESH_L7_RUNTIME_OWNER
/* C-driven L7 lifecycle used by the null backend. */
int px_l7_attach_worker(struct objects *objs, int worker_id);
int px_l7_step_worker(struct objects *objs, int worker_id);
void px_l7_detach_worker(struct objects *objs, int worker_id);
#endif

/* True only after the egress owner has stopped submitting for this dead pod,
 * every destination DMA/credit read has completed, all lane queues are empty,
 * and no worker completion still names the slot. The control path uses
 * this as the ARM half of POD_QUIESCED before destroying imported host mmaps. */
int px_pod_reclaim_ready(struct objects *objs, int pod_idx);


#endif /* DPU_PROXY_H */
