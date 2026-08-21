#include "buffer.h"

#include <doca_dev.h>
#include <doca_mmap.h>

DOCA_LOG_REGISTER(BUFFER);

static doca_error_t
alloc_buffer_and_set_mmap_impl(struct doca_mmap **mmap, struct doca_dev *dev,
                               void **buffer, size_t buffer_size,
                               uint32_t access_mask, int thread_safe)
{
    doca_error_t result;
    int ret;

    result = doca_mmap_create(mmap);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create local mmap - %s",
                doca_error_get_name(result));
        return result;
    }

    if (thread_safe) {
        result = doca_mmap_enable_thread_safety(*mmap);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to enable mmap thread safety - %s",
                         doca_error_get_name(result));
            goto destroy_mmap;
        }
    }

    result = doca_mmap_add_dev(*mmap, dev);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to add device to mmap - %s",
                doca_error_get_name(result));
        goto destroy_mmap;
    }

    result = doca_mmap_set_permissions(*mmap, access_mask);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set mmap permissions - %s",
                doca_error_get_name(result));
        goto destroy_mmap;
    }

    ret = posix_memalign(buffer, CACHE_ALIGN, buffer_size);
    if (ret != 0) {
        result = DOCA_ERROR_NO_MEMORY;
        DOCA_LOG_ERR("Failed to allocate aligned memory for buffer - %s",
                strerror(ret));
        goto destroy_mmap;
    }
	
	memset(*buffer, 0, buffer_size);

    result = doca_mmap_set_memrange(*mmap, *buffer, buffer_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set mmap memrange - %s",
                doca_error_get_name(result));
        goto free_buffer;
    }
    result = doca_mmap_start(*mmap);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start mmap - %s",   
                doca_error_get_name(result));
        goto free_buffer;
    }

    return DOCA_SUCCESS;

free_buffer:
    free(*buffer);
    *buffer = NULL;
destroy_mmap:
    doca_mmap_destroy(*mmap);
    *mmap = NULL;

    return result;
}

doca_error_t
alloc_buffer_and_set_mmap(struct doca_mmap **mmap, struct doca_dev *dev,
                          void **buffer, size_t buffer_size,
                          uint32_t access_mask)
{
    return alloc_buffer_and_set_mmap_impl(mmap, dev, buffer, buffer_size,
                                          access_mask, 0);
}

doca_error_t
alloc_buffer_and_set_thread_safe_mmap(struct doca_mmap **mmap,
                                      struct doca_dev *dev,
                                      void **buffer, size_t buffer_size,
                                      uint32_t access_mask)
{
    return alloc_buffer_and_set_mmap_impl(mmap, dev, buffer, buffer_size,
                                          access_mask, 1);
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
