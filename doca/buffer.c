#define _GNU_SOURCE
#include "buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <doca_dev.h>
#include <doca_mmap.h>

DOCA_LOG_REGISTER(BUFFER);

static doca_error_t
alloc_buffer_and_set_mmap_impl(struct doca_mmap **mmap_obj, struct doca_dev *dev,
                               void **buffer, size_t buffer_size,
                               uint32_t access_mask, int thread_safe,
                               int use_memfd, int *out_fd)
{
    doca_error_t result;
    int ret;
    int memfd = -1;

    result = doca_mmap_create(mmap_obj);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create local mmap - %s",
                doca_error_get_name(result));
        return result;
    }

    if (thread_safe) {
        result = doca_mmap_enable_thread_safety(*mmap_obj);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to enable mmap thread safety - %s",
                         doca_error_get_name(result));
            goto destroy_mmap;
        }
    }

    result = doca_mmap_add_dev(*mmap_obj, dev);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to add device to mmap - %s",
                doca_error_get_name(result));
        goto destroy_mmap;
    }

    result = doca_mmap_set_permissions(*mmap_obj, access_mask);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set mmap permissions - %s",
                doca_error_get_name(result));
        goto destroy_mmap;
    }

    if (use_memfd) {
        memfd = memfd_create("dpumesh-dma", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (memfd < 0 || ftruncate(memfd, (off_t)buffer_size) != 0) {
            ret = errno;
            result = DOCA_ERROR_NO_MEMORY;
            DOCA_LOG_ERR("Failed to create memfd DMA buffer - %s", strerror(ret));
            goto close_memfd;
        }
        *buffer = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       memfd, 0);
        if (*buffer == MAP_FAILED) {
            *buffer = NULL;
            result = DOCA_ERROR_NO_MEMORY;
            DOCA_LOG_ERR("Failed to map memfd DMA buffer - %s", strerror(errno));
            goto close_memfd;
        }
        if (fcntl(memfd, F_ADD_SEALS,
                  F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
            result = DOCA_ERROR_IO_FAILED;
            DOCA_LOG_ERR("Failed to shape-seal memfd DMA buffer - %s",
                         strerror(errno));
            goto free_buffer;
        }
    } else {
        ret = posix_memalign(buffer, CACHE_ALIGN, buffer_size);
        if (ret != 0) {
            result = DOCA_ERROR_NO_MEMORY;
            DOCA_LOG_ERR("Failed to allocate aligned memory for buffer - %s",
                    strerror(ret));
            goto destroy_mmap;
        }
    }
	
	memset(*buffer, 0, buffer_size);

    result = doca_mmap_set_memrange(*mmap_obj, *buffer, buffer_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set mmap memrange - %s",
                doca_error_get_name(result));
        goto free_buffer;
    }
    result = doca_mmap_start(*mmap_obj);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start mmap - %s",   
                doca_error_get_name(result));
        goto free_buffer;
    }

    if (out_fd != NULL)
        *out_fd = memfd;
    return DOCA_SUCCESS;

free_buffer:
    if (use_memfd)
        munmap(*buffer, buffer_size);
    else
        free(*buffer);
    *buffer = NULL;
close_memfd:
    if (memfd >= 0)
        close(memfd);
destroy_mmap:
    doca_mmap_destroy(*mmap_obj);
    *mmap_obj = NULL;

    return result;
}

doca_error_t
alloc_buffer_and_set_mmap(struct doca_mmap **mmap, struct doca_dev *dev,
                          void **buffer, size_t buffer_size,
                          uint32_t access_mask)
{
    return alloc_buffer_and_set_mmap_impl(mmap, dev, buffer, buffer_size,
                                          access_mask, 0, 0, NULL);
}

doca_error_t
alloc_memfd_buffer_and_set_mmap(struct doca_mmap **mmap, struct doca_dev *dev,
                               void **buffer, size_t buffer_size,
                               uint32_t access_mask, int *out_fd)
{
    if (out_fd == NULL)
        return DOCA_ERROR_INVALID_VALUE;
    *out_fd = -1;
    return alloc_buffer_and_set_mmap_impl(mmap, dev, buffer, buffer_size,
                                          access_mask, 0, 1, out_fd);
}

doca_error_t
alloc_buffer_and_set_thread_safe_mmap(struct doca_mmap **mmap,
                                      struct doca_dev *dev,
                                      void **buffer, size_t buffer_size,
                                      uint32_t access_mask)
{
    return alloc_buffer_and_set_mmap_impl(mmap, dev, buffer, buffer_size,
                                          access_mask, 1, 0, NULL);
}


doca_error_t
destroy_mmap_and_unmap_buffer(struct doca_mmap *mmap, void *buffer,
                              size_t buffer_size, int fd)
{
    doca_error_t result = doca_mmap_destroy(mmap);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to destroy mmap - %s",
                     doca_error_get_name(result));
        return result;
    }
    if (buffer != NULL && buffer_size != 0)
        (void)munmap(buffer, buffer_size);
    if (fd >= 0)
        close(fd);
    return DOCA_SUCCESS;
}

doca_error_t
destroy_mmap_and_free_buffer(struct doca_mmap *mmap, void *buffer)
{
    doca_error_t result;
    
    result = doca_mmap_destroy(mmap);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to destroy mmap - %s",
                doca_error_get_name(result));
        return result;
    }

    free(buffer);

    return DOCA_SUCCESS;
}
