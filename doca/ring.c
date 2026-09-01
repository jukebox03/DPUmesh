#include "ring.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <doca_log.h>
#include "dpa_common.h"
#include "object.h"
#include "buffer.h"
#include "comch_common.h"

DOCA_LOG_REGISTER(RING);

int setup_dma_ring(struct objects *objs, size_t size, struct dma_ring **out_ring)
{
    doca_error_t result;
    struct dma_ring *ring;

    /* calloc, NOT malloc: the fields below are initialized by hand, so any field added
     * to struct dma_ring later would otherwise silently inherit heap garbage. */
    ring = (struct dma_ring *)calloc(1, sizeof(struct dma_ring));
    if (!ring)
        return DOCA_ERROR_NO_MEMORY;
    *out_ring = ring;
    ring->size = size;          /* logical ring size (host wraps at this) */
    ring->memfd = -1;
    ring->enq_pos = 0;
    ring->descs = NULL;
    ring->ctrl = NULL;
    ring->busy_probes = 0;
    ring->dead = 0;

    /* Slots [0,size) are descriptors, followed by the RX-credit and
     * consumer-head cache lines. */
    size_t alloc_slots = ring->size + DMA_RING_EXTRA_SLOTS;
    ring->map_bytes = alloc_slots * sizeof(struct dma_desc);
    result = alloc_memfd_buffer_and_set_mmap(&ring->mmap, objs->dev,
                           (void **)&ring->descs,
                           ring->map_bytes,
                           DOCA_ACCESS_FLAG_PCI_READ_WRITE, &ring->memfd);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to allocate DMA resources: %s", doca_error_get_descr(result));
        free(ring);
        *out_ring = NULL;
        return result;
    }

    memset(ring->descs, 0, alloc_slots * sizeof(struct dma_desc));
    ring->ctrl = (struct dma_ring_ctrl *)&ring->descs[DMA_RING_CTRL_SLOT(ring->size)];

    /* export mmap to DPU (covers all alloc_slots) */
    result = export_mmap_to_remote(objs, ring->mmap,
                                   ring->descs,
                                   alloc_slots * sizeof(struct dma_desc),
                                   DMA_RING);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to export mmap and buffer to DPU: %s", doca_error_get_descr(result));
        destroy_mmap_and_unmap_buffer(ring->mmap, ring->descs,
                                      ring->map_bytes, ring->memfd);
        free(ring);
        *out_ring = NULL;
        return result;
    }
    return 0;
}

int setup_rev_ring(struct objects *objs, struct rev_ring **out_ring)
{
    struct rev_ring *ring = (struct rev_ring *)calloc(1, sizeof(*ring));
    if (!ring)
        return DOCA_ERROR_NO_MEMORY;

    ring->size = DMA_REV_RING_SIZE;
    ring->memfd = -1;
    ring->map_bytes = DMA_REV_RING_BYTES;
    doca_error_t result = alloc_memfd_buffer_and_set_mmap(
        &ring->mmap, objs->dev, (void **)&ring->entries,
        ring->map_bytes, DOCA_ACCESS_FLAG_PCI_READ_WRITE, &ring->memfd);
    if (result != DOCA_SUCCESS) {
        free(ring);
        return result;
    }
    memset(ring->entries, 0, DMA_REV_RING_BYTES);
    ring->ctrl = (struct dmesh_rev_ring_ctrl *)
        ((uint8_t *)ring->entries +
         (size_t)ring->size * sizeof(*ring->entries));

    result = export_mmap_to_remote(objs, ring->mmap, ring->entries,
                                   DMA_REV_RING_BYTES, DMA_REV_RING);
    if (result != DOCA_SUCCESS) {
        destroy_mmap_and_unmap_buffer(ring->mmap, ring->entries,
                                      ring->map_bytes, ring->memfd);
        free(ring);
        return result;
    }
    *out_ring = ring;
    return DOCA_SUCCESS;
}

int attach_dma_ring(int fd, size_t size, struct dma_ring **out_ring)
{
    if (fd < 0 || size == 0 || out_ring == NULL)
        return -1;
    struct dma_ring *ring = calloc(1, sizeof(*ring));
    if (ring == NULL)
        return -1;
    ring->size = (uint32_t)size;
    ring->memfd = fd;
    ring->map_bytes = (size + DMA_RING_EXTRA_SLOTS) * sizeof(struct dma_desc);
    ring->descs = mmap(NULL, ring->map_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    if (ring->descs == MAP_FAILED) {
        close(fd);
        free(ring);
        return -1;
    }
    ring->ctrl = (struct dma_ring_ctrl *)&ring->descs[DMA_RING_CTRL_SLOT(size)];
    *out_ring = ring;
    return 0;
}

int attach_rev_ring(int fd, struct rev_ring **out_ring)
{
    if (fd < 0 || out_ring == NULL)
        return -1;
    struct rev_ring *ring = calloc(1, sizeof(*ring));
    if (ring == NULL)
        return -1;
    ring->size = DMA_REV_RING_SIZE;
    ring->memfd = fd;
    ring->map_bytes = DMA_REV_RING_BYTES;
    ring->entries = mmap(NULL, ring->map_bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (ring->entries == MAP_FAILED) {
        close(fd);
        free(ring);
        return -1;
    }
    ring->ctrl = (struct dmesh_rev_ring_ctrl *)
        ((uint8_t *)ring->entries +
         (size_t)ring->size * sizeof(*ring->entries));
    *out_ring = ring;
    return 0;
}

void detach_dma_ring(struct dma_ring *ring)
{
    if (ring == NULL)
        return;
    if (ring->descs != NULL)
        munmap(ring->descs, ring->map_bytes);
    if (ring->memfd >= 0)
        close(ring->memfd);
    free(ring);
}

void detach_rev_ring(struct rev_ring *ring)
{
    if (ring == NULL)
        return;
    if (ring->entries != NULL)
        munmap(ring->entries, ring->map_bytes);
    if (ring->memfd >= 0)
        close(ring->memfd);
    free(ring);
}
