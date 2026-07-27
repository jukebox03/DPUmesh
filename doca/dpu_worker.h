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

/* Wake the main control and doorbell loop. */
void dpu_wake_main(struct objects *objs);

/* Coalesce host reverse-ring wake requests by host sleep epoch. */
void dpu_request_host_doorbell(struct objects *objs, struct pod_state *pod,
                               uint64_t epoch);

#endif /* DPU_WORKER_H */
