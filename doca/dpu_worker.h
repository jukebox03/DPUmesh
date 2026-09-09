#ifndef DPU_WORKER_H
#define DPU_WORKER_H

#include <signal.h>

#include "object.h"
#include "topology.h"

/* ====== DPU Worker ====== */

/* Set by SIGTERM and SIGINT. The worker loop leaves on it and then drains and
 * releases hardware; nothing ends the process from the handler. */
extern volatile sig_atomic_t dmesh_dpu_stop;

/* Runs the control loop until that request, then tears the runtime down.
 * Returns 0 only when every resource was released. */
int run_dpu_worker(struct objects *objs);

/* SG-DMA egress hooks. */

/* Select a live backend for an unpinned L4 stream. Returns -1 if unroutable;
 * the caller owns pinning and treats backend loss as terminal. */
int32_t dpu_route_l4(struct objects *objs, int16_t svc);

/* Collect ready backend pod IDs for a service. */
int collect_live_hosts(struct objects *objs, int16_t svc, int32_t *out);

/* 1 when the held generation names a reachable backend for `svc` on another
 * node. This is what separates "no live host" from "host not ready": the first
 * is routable across the boundary and the second is not routable at all. A
 * Service with no local replica is served, not poisoned: locality is a
 * preference, which is what topology-aware routing treats it as everywhere
 * else. */
int dmesh_service_has_remote(struct objects *objs, int16_t svc);

/* Wake the main control and doorbell loop. */
void dpu_wake_main(struct objects *objs);

/* Coalesce host reverse-ring wake requests by host sleep epoch. */
void dpu_request_host_doorbell(struct objects *objs, struct pod_state *pod,
                               uint64_t epoch);

#endif /* DPU_WORKER_H */
