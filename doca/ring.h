#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

#include "dpa_common.h"

struct doca_dev;
struct doca_mmap;
struct objects;

struct dma_ring {
    struct doca_mmap *mmap;
    uint32_t size;
    struct dma_desc *descs;
    struct dma_ring_ctrl *ctrl;
    /* Lock-free MPSC forward ring. enq_pos assigns tickets; each descriptor's
     * generation sequence publishes that ticket to the DPA. */
    uint64_t  enq_pos;
    /* "ring busy" WARN rate-limit state (best-effort under lock-free contention;
     * a racy probe count only mis-throttles a diagnostic, never corrupts). */
    uint64_t busy_probes;
    /* One-way latch set when a slot wait times out; subsequent enqueues fail fast. */
    int dead;
};

/* Publish one completed MPSC ticket after its descriptor payload. */
static inline void
dma_ring_publish_desc(struct dma_ring *ring, uint64_t ticket)
{
    struct dma_desc *desc = &ring->descs[ticket % ring->size];
    __atomic_store_n(&desc->publish_seq, ticket + 1, __ATOMIC_RELEASE);
}

/* Create and export one host→DPU forward descriptor ring. */
int setup_dma_ring(struct objects *objs, size_t size, struct dma_ring **out_ring);
#endif /* RING_H */
