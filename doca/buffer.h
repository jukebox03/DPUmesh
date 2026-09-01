#ifndef BUFFER_H
#define BUFFER_H

#include <doca_mmap.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_error.h>

#define CACHE_ALIGN 128 /* dma_copy requires 128B-aligned addresses */

doca_error_t
alloc_buffer_and_set_mmap(struct doca_mmap **mmap, struct doca_dev *dev,
                        void **buffer, size_t buffer_size, uint32_t access_mask);

/* Host broker allocation: the returned MAP_SHARED memfd remains open so the
 * exact pages registered with DOCA can be passed to an unprivileged workload.
 * The file is shape sealed (grow/shrink/seal); contents intentionally remain
 * writable by the workload and DPU. */
doca_error_t
alloc_memfd_buffer_and_set_mmap(struct doca_mmap **mmap, struct doca_dev *dev,
                               void **buffer, size_t buffer_size,
                               uint32_t access_mask, int *out_fd);

doca_error_t
alloc_buffer_and_set_thread_safe_mmap(struct doca_mmap **mmap,
                                      struct doca_dev *dev,
                                      void **buffer, size_t buffer_size,
                                      uint32_t access_mask);
						
doca_error_t
destroy_mmap_and_free_buffer(struct doca_mmap *mmap, void *buffer);

doca_error_t
destroy_mmap_and_unmap_buffer(struct doca_mmap *mmap, void *buffer,
                              size_t buffer_size, int fd);

#endif // BUFFER_H
