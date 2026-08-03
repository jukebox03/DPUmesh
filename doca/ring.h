#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

#include "dpa_common.h"
#include "comch_common.h"

struct doca_dev;
struct doca_mmap;
struct objects;

struct dma_ring {
    struct doca_mmap *mmap;
    uint32_t size;
    struct dma_desc *descs;
    struct dma_ring_ctrl *ctrl;
    /* Lock-free MPSC forward ring. enq_pos assigns tickets; each descriptor's
     * generation sequence publishes that ticket to the DPA without gaps. */
    uint64_t  enq_pos;
    /* "ring full" WARN rate-limit state (best-effort under lock-free contention;
     * a racy probe count only mis-throttles a diagnostic, never corrupts). */
    uint64_t busy_probes;
    int dead;
};

struct rev_ring {
    struct doca_mmap *mmap;
    uint32_t size;
    struct dmesh_rev_ring_entry *entries;
    struct dmesh_rev_ring_ctrl *ctrl;
    uint64_t head;
};

/* Claim the next free ticket. Full-ring speculative tickets withdraw in reverse
 * order, so a refused claim leaves the published sequence gapless. */
static inline int
dma_ring_try_claim(struct dma_ring *ring, uint64_t *out_ticket)
{
    uint64_t ticket = __atomic_fetch_add(&ring->enq_pos, 1,
                                         __ATOMIC_RELAXED);
    for (;;) {
        uint64_t head = __atomic_load_n(&ring->ctrl->consumer_head,
                                        __ATOMIC_ACQUIRE);
        if (ticket - head < ring->size) {
            *out_ticket = ticket;
            return 1;
        }
        uint64_t expected = ticket + 1;
        if (__atomic_compare_exchange_n(&ring->enq_pos, &expected, ticket, 1,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED))
            return 0;
    }
}

/* Publish one completed MPSC ticket after its descriptor payload. */
static inline void
dma_ring_publish_desc(struct dma_ring *ring, uint64_t ticket)
{
    struct dma_desc *desc = &ring->descs[ticket % ring->size];
    __atomic_store_n(&desc->publish_seq, ticket + 1, __ATOMIC_RELEASE);
}

/* Create and export one host→DPU forward descriptor ring. */
int setup_dma_ring(struct objects *objs, size_t size, struct dma_ring **out_ring);

/* Create and export one DPU→host reverse completion ring. */
int setup_rev_ring(struct objects *objs, struct rev_ring **out_ring);
#endif /* RING_H */
