#ifndef DPU_WORKER_H
#define DPU_WORKER_H

#include "object.h"

/* ====== DPU Worker ====== */

void run_dpu_worker(struct objects *objs);

/* SG-DMA egress hooks. */

/* Select a live backend for an unpinned L4 stream. Returns -1 if unroutable;
 * the caller owns pinning and treats backend loss as terminal. */
int32_t dpu_route_l4(struct objects *objs, int16_t svc);

/* Collect ready backend pod IDs for a service. */
int collect_live_hosts(struct objects *objs, int16_t svc, int32_t *out);

/* Accumulate one TX_ACK into src_pod's batch (custody release to the sender). */
void batch_or_send_tx_ack(struct objects *objs, struct pod_state *src_pod,
                          uint16_t port, uint16_t seq);

/* Flush a pod's accumulated REV_DONE batch as one message (idle/full flush). */
void flush_rev_done_batch(struct objects *objs, struct pod_state *pod);

/* Wake the main Comch emitter. */
void dpu_wake_main(struct objects *objs);

#endif /* DPU_WORKER_H */
