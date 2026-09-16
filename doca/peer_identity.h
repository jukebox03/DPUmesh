#ifndef DMESH_PEER_IDENTITY_H
#define DMESH_PEER_IDENTITY_H

#include <stdint.h>
#include <string.h>
#include "workload_limits.h"

/* Retained from paired-host REGISTER, never filled from application input.
 * This is a host representation, not a wire structure. dma_generation fences
 * local DMA callbacks; it does not replace this registration incarnation. */
struct dmesh_peer_registration {
    uint8_t daemon[DMESH_DAEMON_INCARNATION_SIZE];
    uint32_t slot;
    uint64_t generation;
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
};

struct dmesh_peer_pair {
    char local_uid[DMESH_POD_UID_MAX];
    char remote_uid[DMESH_POD_UID_MAX];
    struct dmesh_peer_registration local;
    struct dmesh_peer_registration remote;
    /* Certified by the control/crypto owner after the lane activation barrier.
     * Zero on an outbound request, immutable after channel authentication. */
    uint8_t association[16];
    uint64_t lane_id, lane_generation;
};

static inline int dmesh_peer_registration_valid(const struct dmesh_peer_registration *r)
{
    uint8_t daemon = 0, nonce = 0;
    for (size_t i = 0; i < sizeof(r->daemon); i++) daemon |= r->daemon[i];
    for (size_t i = 0; i < sizeof(r->nonce); i++) nonce |= r->nonce[i];
    return r->generation && daemon && nonce;
}

static inline int dmesh_peer_registration_equal(const struct dmesh_peer_registration *a,
                                               const struct dmesh_peer_registration *b)
{
    /* Do not compare padding in a C structure. */
    return a->slot == b->slot && a->generation == b->generation &&
        !memcmp(a->daemon, b->daemon, sizeof(a->daemon)) &&
        !memcmp(a->nonce, b->nonce, sizeof(a->nonce));
}
#endif
