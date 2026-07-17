#ifndef DPU_WORKER_H
#define DPU_WORKER_H

#include "object.h"

/* ====== DPU Worker ====== */

void run_dpu_worker(struct objects *objs);

/* ====== Exposed to the SG-DMA egress engine (dpu_proxy.c) ======
 * These live in dpu_worker.c; the egress engine calls them so request/reply
 * egress reuses the SAME batched TX_ACK / REV_DONE accumulators and the SAME
 * L4 default route. */

/* L4 default route: load-balance over the service's live backends. Holds no affinity
 * of its own — the caller (px_resolve_backend) owns stickiness. Used for a DEFERred
 * request seg. Returns a live pod_id or -1 (unroutable). */
int32_t dpu_route_l4(struct objects *objs, int16_t svc);

/* Collect the live backend pod_ids advertising service `svc` (derived from pods[]:
 * registered + service_id==svc + dma_ready). Fills out[0..n) (caller sizes >= MAX_PODS)
 * and returns n (0 = no healthy backend). The L7 hook is shown this set as its
 * cluster endpoints (dpu_proxy.c fills dmesh_l7_ctx.hosts from it). */
int collect_live_hosts(struct objects *objs, int16_t svc, int32_t *out);

/* Accumulate one TX_ACK into src_pod's batch (custody release to the sender). */
void batch_or_send_tx_ack(struct objects *objs, struct pod_state *src_pod,
                          uint16_t port, uint16_t seq);

/* Flush a pod's accumulated REV_DONE batch as one message (idle/full flush). */
void flush_rev_done_batch(struct objects *objs, struct pod_state *pod);

/* Wake the (event-driven) main loop if it is parked in epoll. Egress workers call
 * this after an SG-DMA batch completes so main emits the REV_DONE promptly at low
 * load. No-op when the reaper is off (reaper_wake_fd < 0). */
void dpu_wake_main(struct objects *objs);

#endif /* DPU_WORKER_H */
