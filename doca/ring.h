#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

struct dma_desc;
struct doca_dev;
struct doca_mmap;
struct objects;

struct dma_ring {
    struct doca_mmap *mmap;
    uint32_t size;
    struct dma_desc *descs;
    /* Lock-free MPSC forward ring. enq_pos assigns tickets; per-slot sequences and
     * desc.valid guard publication, DPA consumption, and reuse. */
    uint64_t  enq_pos;
    uint64_t *seq;
    /* "ring busy" WARN rate-limit state (best-effort under lock-free contention;
     * a racy probe count only mis-throttles a diagnostic, never corrupts). */
    uint64_t busy_probes;
    /* One-way latch set when a slot wait times out; subsequent enqueues fail fast. */
    int dead;
};

/* Create + export one host→DPU forward descriptor ring (with the +1 credit
 * slot). EU-sharding allocates K of these; each is exported as DMA_RING and the
 * DPU pairs them in arrival order. */
int setup_dma_ring(struct objects *objs, size_t size, struct dma_ring **out_ring);
#endif /* RING_H */
