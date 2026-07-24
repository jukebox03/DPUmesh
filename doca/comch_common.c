#include "comch_common.h"

#include <doca_log.h>
#include <doca_error.h>
#include <doca_mmap.h>

#include "object.h"
#include "comch_client.h"
#include "comch_server.h"
#include "dpa.h"
#include "dpa_common.h"
#include <dpumesh/dmesh_common.h>
DOCA_LOG_REGISTER(COMCH_COMMON);


doca_error_t
export_mmap_to_remote(struct objects *objs, struct doca_mmap *mmap, void *buffer, size_t buf_size, enum mmap_type mmap_type)
{
    doca_error_t result;
    struct dmesh_mmap_msg *msg;
    const void *export_desc;
	size_t export_desc_len;
    char export_msg[4096];

    result = doca_mmap_export_pci(mmap, objs->dev, &export_desc, &export_desc_len);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to export local mmap to DPU: %s", doca_error_get_descr(result));
        return result;
    }

    DOCA_LOG_INFO("Successfully exported local mmap to DPU, export descriptor length: %zu bytes",
                  export_desc_len);

    /* Bound the export descriptor against the fixed staging buffer before the
     * memcpy below, so an oversized descriptor cannot smash the stack. */
    if (export_desc_len > sizeof(export_msg) - sizeof(struct dmesh_mmap_msg)) {
        DOCA_LOG_ERR("export_desc_len=%zu exceeds staging buffer capacity %zu",
                     export_desc_len, sizeof(export_msg) - sizeof(struct dmesh_mmap_msg));
        return DOCA_ERROR_TOO_BIG;
    }

    msg = (struct dmesh_mmap_msg *)export_msg;
    msg->type = DMESH_MSG_MMAP_EXPORT;
    msg->mmap_type = mmap_type;
    msg->host_addr = (void *)htonq((uint64_t)buffer);
    msg->buf_size = htonq((uint64_t)buf_size);
    msg->export_desc_len = htonq(export_desc_len);
    memcpy(msg->export_desc, export_desc, export_desc_len);
    
    /* Send export descriptor to DPU via comch (host→DPU only). */
    return client_send_msg(objs, (const char *)msg, sizeof(struct dmesh_mmap_msg) + export_desc_len);
}

doca_error_t
process_mmap_msg(struct objects *objs, struct doca_comch_connection *conn,
                 struct dmesh_mmap_msg *mmap_msg, size_t msg_len)
{
	doca_error_t result;
	struct doca_mmap *imported_mmap = NULL;
	if (msg_len < sizeof(struct dmesh_mmap_msg)) {
		DOCA_LOG_ERR("MMAP message shorter than its fixed header: %zu", msg_len);
		return DOCA_ERROR_INVALID_VALUE;
	}
	void *remote_addr = (void *)ntohq((uint64_t)mmap_msg->host_addr);
	size_t buf_size = ntohq(mmap_msg->buf_size);
	size_t export_desc_len = ntohq(mmap_msg->export_desc_len);
	if (export_desc_len == 0 ||
	    export_desc_len != msg_len - sizeof(struct dmesh_mmap_msg)) {
		DOCA_LOG_ERR("Invalid MMAP descriptor length: desc=%zu message=%zu header=%zu",
		             export_desc_len, msg_len, sizeof(struct dmesh_mmap_msg));
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (remote_addr == NULL || buf_size == 0) {
		DOCA_LOG_ERR("Invalid remote mmap metadata: remote_addr=%p buf_size=%zu",
		             remote_addr, buf_size);
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("remote_addr: %p, buf_size: %zu, export_desc_len: %zu",
		      remote_addr, buf_size, export_desc_len);

#ifdef DOCA_ARCH_DPU
	/* DPU side: store per-pod */
	struct pod_state *pod = find_pod_by_connection(objs, conn);
	if (!pod) {
		DOCA_LOG_ERR("process_mmap_msg: no pod found for connection");
		return DOCA_ERROR_NOT_FOUND;
	}
	if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE)) {
		DOCA_LOG_ERR("process_mmap_msg: connection has not registered a pod");
		return DOCA_ERROR_BAD_STATE;
	}
	if (pod->init_result != DMESH_POD_INIT_PENDING) {
		DOCA_LOG_ERR("Pod %d: MMAP arrived after terminal init result=%d",
		             pod->pod_id, pod->init_result);
		return DOCA_ERROR_BAD_STATE;
	}

	int kmax = objs->k_rings > 0 ? objs->k_rings : 1;
	if (mmap_msg->mmap_type == DMA_RING) {
		/* The protocol is deliberately ordered: exactly K rings, then TX, then
		 * RX. This turns a host/DPU K mismatch into an explicit failure instead
		 * of an indefinitely pending pod. */
		if (pod->ring_mmap_count >= kmax || pod->remote_mmap != NULL ||
		    pod->host_rx_mmap != NULL) {
			DOCA_LOG_ERR("Pod %d: out-of-order/extra DMA_RING (count=%d k=%d)",
				     pod->pod_id, pod->ring_mmap_count, kmax);
			return DOCA_ERROR_INVALID_VALUE;
		}
	} else if (mmap_msg->mmap_type == DMA_BUFFER) {
		if (pod->ring_mmap_count != kmax || pod->remote_mmap != NULL ||
		    pod->host_rx_mmap != NULL) {
			DOCA_LOG_ERR("Pod %d: out-of-order/duplicate TX mmap (rings=%d/%d)",
			             pod->pod_id, pod->ring_mmap_count, kmax);
			return DOCA_ERROR_INVALID_VALUE;
		}
	} else if (mmap_msg->mmap_type == DMA_HOST_RX_BUFFER) {
		if (pod->ring_mmap_count != kmax || pod->remote_mmap == NULL ||
		    pod->host_rx_mmap != NULL) {
			DOCA_LOG_ERR("Pod %d: out-of-order/duplicate RX mmap (rings=%d/%d tx=%p)",
			             pod->pod_id, pod->ring_mmap_count, kmax,
			             (void *)pod->remote_mmap);
			return DOCA_ERROR_INVALID_VALUE;
		}
	} else {
		DOCA_LOG_ERR("Invalid mmap type received: %d", mmap_msg->mmap_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Validate the imported range before giving it to DOCA/DPA. The ring shape is
	 * wire ABI; TX offsets mirror into a fixed DPU staging buffer; RX is split
	 * into K regions at 8 KiB credit granularity. */
	if (((uintptr_t)remote_addr & 127u) != 0) {
		DOCA_LOG_ERR("Pod %d: mmap base is not 128-byte aligned: %p",
		             pod->pod_id, remote_addr);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (mmap_msg->mmap_type == DMA_RING &&
	    buf_size !=
		(DMA_RING_SIZE + DMA_RING_EXTRA_SLOTS) * sizeof(struct dma_desc)) {
		DOCA_LOG_ERR("Pod %d: invalid ring bytes=%zu expected=%zu", pod->pod_id,
		             buf_size,
		             (DMA_RING_SIZE + DMA_RING_EXTRA_SLOTS) *
				sizeof(struct dma_desc));
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (mmap_msg->mmap_type == DMA_BUFFER && buf_size > DPU_BUFFER_SIZE) {
		DOCA_LOG_ERR("Pod %d: TX mmap bytes=%zu exceed DPU staging=%u",
		             pod->pod_id, buf_size, (unsigned)DPU_BUFFER_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (mmap_msg->mmap_type == DMA_HOST_RX_BUFFER &&
	    (buf_size != pod->remote_buf_size ||
	     buf_size < (size_t)kmax * DPUMESH_SLOT_SIZE ||
	     buf_size % ((size_t)kmax * DPUMESH_SLOT_SIZE) != 0)) {
		DOCA_LOG_ERR("Pod %d: invalid RX bytes=%zu (TX=%zu K=%d slot=%d)",
		             pod->pod_id, buf_size, pod->remote_buf_size, kmax,
		             DPUMESH_SLOT_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_mmap_create_from_export(NULL, mmap_msg->export_desc,
					      export_desc_len,
					      objs->dev,
					      &imported_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create remote mmap from export desc: %s",
			     doca_error_get_name(result));
		return result;
	}

	if (mmap_msg->mmap_type == DMA_HOST_RX_BUFFER) {
		pod->host_rx_mmap = imported_mmap;
		pod->host_rx_addr = remote_addr;
		pod->host_rx_buf_size = buf_size;
		/* rq_depth derived from host_rx buffer size: num_slots × slot_size. */
		pod->rq_depth = (uint32_t)(buf_size / DPUMESH_SLOT_SIZE);
		DOCA_LOG_INFO("Pod %d: Host RX buffer stored (addr=%p, size=%zu, rq_depth=%u)",
			      pod->pod_id, remote_addr, buf_size, pod->rq_depth);
	} else if (mmap_msg->mmap_type == DMA_BUFFER) {
		pod->remote_mmap = imported_mmap;
		pod->remote_addr = remote_addr;
		pod->remote_buf_size = buf_size;
	} else { /* DMA_RING */
		pod->ring_mmaps[pod->ring_mmap_count] = imported_mmap;
		/* Save the host base used by proxy egress credit reads. */
		pod->ring_host_addrs[pod->ring_mmap_count] = remote_addr;
		pod->ring_mmap_count++;
	}

	DOCA_LOG_INFO("Pod %d: mmap_type=%d stored (ring_mmaps=%d/%d, remote_mmap=%p, host_rx_mmap=%p)",
		      pod->pod_id, mmap_msg->mmap_type,
		      pod->ring_mmap_count, kmax,
		      (void *)pod->remote_mmap, (void *)pod->host_rx_mmap);

	/* Start per-pod DMA setup when DPU initialization and every required host
	 * mapping are ready. Pending mappings are handled by the worker setup pass. */
	if (objs->dpu_ready && pod->ring_mmap_count == kmax && pod->remote_mmap &&
	    pod->host_rx_mmap && !pod->dma_ready) {
		result = setup_pod_dma(objs, pod);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("setup_pod_dma failed for pod %d: %s",
				     pod->pod_id, doca_error_get_descr(result));
			return result;
		}
	}

#else
		/* The host reverse path lands in ctx->rx_dma_buffer. */
	(void)objs; (void)conn; (void)result; (void)imported_mmap;
	(void)remote_addr; (void)buf_size; (void)export_desc_len;
#endif

	return DOCA_SUCCESS;
}
