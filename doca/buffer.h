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

doca_error_t
alloc_buffer_and_set_thread_safe_mmap(struct doca_mmap **mmap,
                                      struct doca_dev *dev,
                                      void **buffer, size_t buffer_size,
                                      uint32_t access_mask);
						
doca_error_t
destroy_mmap_and_free_buffer(struct doca_mmap *mmap, void *buffer);

#endif // BUFFER_H
