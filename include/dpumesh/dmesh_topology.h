#ifndef DMESH_TOPOLOGY_H
#define DMESH_TOPOLOGY_H

#include <stdint.h>

/*
 * One ownership rule spans the whole data path:
 *
 *   port -> forward ring -> DPA EU -> ARM worker
 *
 * F is the number of forward rings per pod, N the number of DPA EUs, and A the
 * number of homogeneous ARM data workers.  A valid topology requires F and N
 * to be multiples of A.  The EU mapping below then guarantees
 *
 *   dmesh_dpa_eu_for_ring(...) % A == ring % A
 *
 * while still distributing each owner's rings across all of its N/A EUs.
 */
static inline int
dmesh_forward_ring(uint16_t port, int forward_rings)
{
    return forward_rings > 1 ? (int)((uint32_t)port % (uint32_t)forward_rings) : 0;
}

static inline int
dmesh_worker_for_port(uint16_t port, int arm_workers)
{
    return arm_workers > 1 ? (int)((uint32_t)port % (uint32_t)arm_workers) : 0;
}

static inline int
dmesh_worker_for_ring(int ring, int arm_workers)
{
    return arm_workers > 1 ? ring % arm_workers : 0;
}

static inline int
dmesh_topology_valid(int forward_rings, int dpa_eus, int arm_workers)
{
    return forward_rings >= 1 && dpa_eus >= forward_rings &&
           arm_workers >= 1 && arm_workers <= forward_rings &&
           forward_rings % arm_workers == 0 &&
           dpa_eus % arm_workers == 0;
}

static inline int
dmesh_dpa_eu_for_ring(int pod_id, int forward_rings, int ring,
                      int dpa_eus, int arm_workers)
{
    if (!dmesh_topology_valid(forward_rings, dpa_eus, arm_workers) ||
        ring < 0 || ring >= forward_rings)
        return -1;

    int owner = dmesh_worker_for_ring(ring, arm_workers);
    int owner_ring = ring / arm_workers;
    int rings_per_owner = forward_rings / arm_workers;
    int eus_per_owner = dpa_eus / arm_workers;
    int eu_group = (pod_id * rings_per_owner + owner_ring) % eus_per_owner;
    if (eu_group < 0)
        eu_group += eus_per_owner;
    return owner + arm_workers * eu_group;
}

#endif /* DMESH_TOPOLOGY_H */
