#include "dpa.h"

#include <doca_error.h>
#include <doca_log.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_comch_msgq.h>
#include <doca_buf_array.h>
#include <doca_mmap.h>

#include "object.h"
#include "dpa_common.h"
#include "comch_common.h"
#include "dpu_worker.h"
#include <dpumesh/dmesh_common.h>
#include <dpumesh/dmesh_topology.h>
#include "ring.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

DOCA_LOG_REGISTER(DPA);

#ifdef DOCA_ARCH_DPU

/* Kernel function declaration (resolved from dpa_kernel.a stubs, DPU only) */
extern doca_dpa_func_t run_dma_manager;
extern doca_dpa_func_t run_dma_yield_helper;
extern doca_dpa_func_t thread_init_rpc;

extern struct doca_dpa_app *DPU_mesh_dpa_app;
#endif

/* Current ARM data worker ID. */
__thread int dpu_worker_id = 0;

/* DPU-only DPA MsgQ callbacks. */
#ifdef DOCA_ARCH_DPU

/*
 * Callback invoked once a message is received from DPA successfully
 *
 * @recv_task [in]: The receive task
 * @task_user_data [in]: Task user data
 * @ctx_user_data [in]: Consumer context user data
 */
static void dmesh_doca_dpa_msgq_recv_cb(struct doca_comch_consumer_task_post_recv *recv_task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	doca_error_t result;
    uint32_t data_len;

	struct objects *objs = ctx_user_data.ptr;
	struct doca_task *task = doca_comch_consumer_task_post_recv_as_task(recv_task);
    struct dmesh_doca_dpa_msgq *recv_msgq = task_user_data.ptr;
    if (recv_msgq != NULL)
        __atomic_fetch_sub(&recv_msgq->recv_posted, 1, __ATOMIC_RELAXED);

    struct dpu_data_worker *worker_state =
        &objs->data_workers[dpu_worker_id];
    dpu_comp_queue_t *q = &worker_state->queue;
    struct doca_task **defrecv = worker_state->deferred_recv;
    int *ndef = &worker_state->num_deferred_recv;

    data_len = doca_comch_consumer_task_post_recv_get_imm_data_len(recv_task);

    /* DPA sends comch_dma_comp_msg directly (20 bytes; HW imm-data max 32) rather
     * than the full comch_msg union, so read raw bytes and dispatch by the leading
     * type field. */
    uint8_t *raw = (uint8_t *)doca_comch_consumer_task_post_recv_get_imm_data(recv_task);

    if (raw == NULL) {
        DOCA_LOG_ERR("DPA MsgQ recv callback entered with NULL imm data (len=%u)", data_len);
        goto resubmit_recv_task;
    }

    /* Type field is the first byte (uint8_t in the packed comch_dma_comp_msg).
     * Any imm payload of ours has at least the 1-byte type at offset 0. */
    if (data_len < 1) {
        DOCA_LOG_ERR("DPA MsgQ recv: imm data too short for type field (len=%u)", data_len);
        goto resubmit_recv_task;
    }

    enum dpa_msg_type msg_type = (enum dpa_msg_type)raw[0];

    switch (msg_type) {
        case DPA_MSG_FWD_DONE: {
            if (data_len < sizeof(struct comch_dma_comp_msg)) {
                DOCA_LOG_ERR("DPA MsgQ recv: DMA_COMPLETED too short (len=%u, need=%zu)",
                             data_len, sizeof(struct comch_dma_comp_msg));
                break;
            }
            struct comch_dma_comp_msg *comp_msg = (struct comch_dma_comp_msg *)raw;
            int32_t src_pod_id = comp_msg->src_pod_id;
            int32_t dst_pod_id = comp_msg->dst_pod_id;   /* may be DMESH_POD_BLANK → resolve */
            uint16_t seq = comp_msg->seq;

            /* Find the source pod's local DMA buffer. pod_data_ready ACQUIRE-loads
             * dma_ready so the dma_buffer/handle reads below see the
             * RELEASE-published setup fields. */
            struct pod_state *src_pod = find_pod_by_id(objs, src_pod_id);
            if (!src_pod || !pod_data_ready(src_pod) || !src_pod->dma_buffer ||
                __atomic_load_n(&src_pod->dma_generation, __ATOMIC_ACQUIRE) !=
                    comp_msg->generation) {
                /* Normal during teardown: DEL_ACK fences the DMA itself, but an
                 * older FWD_DONE can already be queued on ARM. Generation makes
                 * this safe even if the pod_id slot has since been reused. */
                break;
            }

            /* Body is the entire DMA payload — no in-band header.
             * length / pos / pod ids / endpoint tuple travel via comp_msg. */
            uint32_t payload_len = comp_msg->length;
            uint32_t body_offset = comp_msg->pos;

            /* Publish the completion to the owning data worker. */
            dpu_comp_entry_t entry;
            entry.src_pod_id = src_pod_id;
            entry.dst_pod_id = dst_pod_id;
            entry.dst_service = comp_msg->dst_service;
            entry.src_port = comp_msg->src_port;
            entry.dst_port = comp_msg->dst_port;
            entry.seq = seq;
            entry.length = payload_len;

            /* Zero-copy: record buffer offset instead of heap-copying.
             * End-node slot-based admission keeps in-flight bytes ≤ buf_size
             * so DPA cannot lap unconsumed data. */
            entry.buf_offset = body_offset;
            entry.generation = comp_msg->generation;
            /* Derive src_pod index directly from the ACQUIRE-gated pointer
             * resolved above (avoids an unguarded re-scan of pods[]). */
            entry.pod_idx = (int)(src_pod - objs->pods);

            if (comp_queue_enqueue(q, &entry) != 0) {
                /* Throttled: the first drop, then one in 65536 with a running
                 * total. Shared by the A worker PE threads without atomics: a lost
                 * increment only mis-throttles a diagnostic. */
                static uint64_t cq_full_drops;
                if ((cq_full_drops++ & 0xFFFFu) == 0)
                    DOCA_LOG_ERR("Completion queue full, dropping (total %llu) seq=%u (src=%d, dst=%d)",
                                 (unsigned long long)cq_full_drops, seq, src_pod_id, dst_pod_id);
                /* zero-copy: no heap data to free */
            }
            break;
        }
        case DPA_MSG_RING_ADD_ACK: {
            if (data_len != sizeof(struct dpa_ring_ack_msg)) {
                DOCA_LOG_ERR("DPA MsgQ recv: RING_ADD_ACK bad size (len=%u, need=%zu)",
                             data_len, sizeof(struct dpa_ring_ack_msg));
                break;
            }
            const struct dpa_ring_ack_msg *ack =
                (const struct dpa_ring_ack_msg *)raw;
            struct pod_state *pod = find_pod_by_id(objs, ack->pod_id);
            if (pod == NULL || !__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE)) {
                DOCA_LOG_WARN("Ignoring ADD_ACK for dead pod=%d eu=%u gen=%u",
                              ack->pod_id, ack->eu_index, ack->generation);
                break;
            }
            uint32_t generation = __atomic_load_n(&pod->dma_generation,
                                                   __ATOMIC_ACQUIRE);
            uint32_t expected = __atomic_load_n(&pod->dpa_add_expected_mask,
                                                 __ATOMIC_ACQUIRE);
            uint32_t bit = ack->eu_index < 32 ? (1u << ack->eu_index) : 0;
            if (ack->generation != generation || bit == 0 || (expected & bit) == 0) {
                DOCA_LOG_WARN("Ignoring stale/unexpected ADD_ACK pod=%d eu=%u gen=%u current=%u mask=0x%x",
                              ack->pod_id, ack->eu_index, ack->generation,
                              generation, expected);
                break;
            }
            if (ack->status != DPA_RING_ACK_OK)
                __atomic_store_n(&pod->dpa_add_ack_failed, 1, __ATOMIC_RELEASE);
            __atomic_fetch_or(&pod->dpa_add_ack_mask, bit, __ATOMIC_ACQ_REL);
            dpu_wake_main(objs);
            break;
        }
        case DPA_MSG_RING_DEL_ACK: {
            if (data_len != sizeof(struct dpa_ring_ack_msg)) {
                DOCA_LOG_ERR("DPA MsgQ recv: RING_DEL_ACK bad size (len=%u, need=%zu)",
                             data_len, sizeof(struct dpa_ring_ack_msg));
                break;
            }
            const struct dpa_ring_ack_msg *ack =
                (const struct dpa_ring_ack_msg *)raw;
            struct pod_state *pod = NULL;
            int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
            for (int i = 0; i < n; i++) {
                struct pod_state *candidate = &objs->pods[i];
                if (__atomic_load_n(&candidate->cleanup_pending,
                                    __ATOMIC_ACQUIRE) &&
                    candidate->pod_id == ack->pod_id &&
                    __atomic_load_n(&candidate->dma_generation,
                                    __ATOMIC_ACQUIRE) == ack->generation) {
                    pod = candidate;
                    break;
                }
            }
            if (pod == NULL) {
                DOCA_LOG_WARN("Ignoring stale DEL_ACK pod=%d eu=%u gen=%u",
                              ack->pod_id, ack->eu_index, ack->generation);
                break;
            }
            uint32_t expected = __atomic_load_n(&pod->dpa_del_expected_mask,
                                                 __ATOMIC_ACQUIRE);
            uint32_t bit = ack->eu_index < 32 ? (1u << ack->eu_index) : 0;
            if (bit == 0 || (expected & bit) == 0 ||
                ack->status != DPA_RING_ACK_OK) {
                DOCA_LOG_WARN("Ignoring invalid DEL_ACK pod=%d eu=%u gen=%u status=%u mask=0x%x",
                              ack->pod_id, ack->eu_index, ack->generation,
                              ack->status, expected);
                break;
            }
            __atomic_fetch_or(&pod->dpa_del_ack_mask, bit, __ATOMIC_ACQ_REL);
            dpu_wake_main(objs);
            break;
        }
        case DPA_MSG_WAKE:
            break;
        default:
            DOCA_LOG_ERR("Received unknown message type: %u", msg_type);
            break;
    }

resubmit_recv_task:
    /* Backpressure: if comp_queue is nearly full, defer recv task resubmission
     * so DPA sees consumer_empty and pauses; the owning data worker's PE pass
     * resubmits when the queue drops below BP_LOW. On submit failure also stash
     * for that pass to retry rather than losing the task.
     *
     * A floor of receives stays posted through soft backpressure: the DPA sends
     * ring ACKs on this same channel and drops them when it has no receive, so
     * the control path stays sendable while forwarding is throttled. Only
     * genuine queue exhaustion (BP_HARD) withholds the floor. */
    uint32_t usage = comp_queue_usage(q);
    int below_floor = recv_msgq != NULL &&
                      __atomic_load_n(&recv_msgq->recv_posted,
                                      __ATOMIC_RELAXED) < DPA_CTRL_RECV_FLOOR;
    if ((usage >= COMP_QUEUE_BP_HARD ||
         (usage >= COMP_QUEUE_BP_HIGH && !below_floor)) &&
        *ndef < MAX_DEFERRED_RECV) {
        defrecv[(*ndef)++] = task;
    } else {
        result = doca_task_submit(task);
        if (result == DOCA_SUCCESS && recv_msgq != NULL)
            __atomic_fetch_add(&recv_msgq->recv_posted, 1, __ATOMIC_RELAXED);
        if (result != DOCA_SUCCESS) {
            if (*ndef < MAX_DEFERRED_RECV) {
                defrecv[(*ndef)++] = task;
                DOCA_LOG_WARN("DPA MsgQ recv resubmit failed: %s; deferred",
                              doca_error_get_name(result));
            } else {
                DOCA_LOG_ERR("DPA MsgQ recv resubmit failed and deferred list full: %s",
                             doca_error_get_name(result));
            }
        }
    }
}

/*
 * Callback invoked once consumer encounters a receive error
 *
 * @recv_task [in]: The receive task
 * @task_user_data [in]: Task user data
 * @ctx_user_data [in]: Consumer context user data
 */
static void dmesh_doca_dpa_msgq_recv_error_cb(struct doca_comch_consumer_task_post_recv *recv_task,
					     union doca_data task_user_data,
					     union doca_data ctx_user_data)
{
	struct doca_task *task = doca_comch_consumer_task_post_recv_as_task(recv_task);
	doca_error_t status = doca_task_get_status(task);
	struct dmesh_doca_dpa_msgq *recv_msgq = task_user_data.ptr;
	if (recv_msgq != NULL)
		__atomic_fetch_sub(&recv_msgq->recv_posted, 1, __ATOMIC_RELAXED);

	DOCA_LOG_ERR("DPA MsgQ recv ERROR callback: status=%s(%d)",
	             doca_error_get_descr(status), (int)status);

	/* Resubmit to keep the recv task alive — do not free. */
	doca_error_t resubmit = doca_task_submit(task);
	if (resubmit == DOCA_SUCCESS && recv_msgq != NULL)
		__atomic_fetch_add(&recv_msgq->recv_posted, 1, __ATOMIC_RELAXED);
	if (resubmit != DOCA_SUCCESS) {
		struct objects *objs = ctx_user_data.ptr;
		struct dpu_data_worker *worker_state =
			&objs->data_workers[dpu_worker_id];
		if (worker_state->num_deferred_recv < MAX_DEFERRED_RECV) {
			worker_state->deferred_recv[worker_state->num_deferred_recv++] = task;
			DOCA_LOG_WARN("DPA MsgQ recv resubmit after error deferred: %s",
			              doca_error_get_name(resubmit));
		} else {
			DOCA_LOG_ERR("DPA MsgQ recv error task lost: deferred list full");
		}
	}
}
/*
 * Callback invoked once a message is sent to DPA successfully
 *
 * @send_task [in]: The send task
 * @task_user_data [in]: Task user data
 * @ctx_user_data [in]: Producer context user data
 */
static void dmesh_doca_dpa_msgq_send_cb(struct doca_comch_producer_task_send *send_task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	(void)ctx_user_data;

    free(task_user_data.ptr);

	struct doca_task *task = doca_comch_producer_task_send_as_task(send_task);
	doca_task_free(task);
}

/*
 * Callback invoked once producer encounters a send error
 *
 * @send_task [in]: The send task
 * @task_user_data [in]: Task user data
 * @ctx_user_data [in]: Producer context user data
 */
static void dmesh_doca_dpa_msgq_send_error_cb(struct doca_comch_producer_task_send *send_task,
					     union doca_data task_user_data,
					     union doca_data ctx_user_data)
{
    (void)ctx_user_data;

    struct doca_task *task = doca_comch_producer_task_send_as_task(send_task);
    DOCA_LOG_ERR("Failed to send msg");
    free(task_user_data.ptr);
    doca_task_free(task);
}

#endif /* DOCA_ARCH_DPU — DPA msgQ task callbacks */

/* Handle a DPA Comch message-queue context state transition. */
void dmesh_doca_dpa_comch_msgq_ctx_state_changed_cb(const union doca_data user_data,
							  struct doca_ctx *ctx,
							  enum doca_ctx_states prev_state,
							  enum doca_ctx_states next_state)
{
	(void)user_data;
	(void)prev_state;
	(void)ctx;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
        DOCA_LOG_ERR("DPA comch msgQ state is idle.");
		break;
    case DOCA_CTX_STATE_STARTING:
        break;
    case DOCA_CTX_STATE_RUNNING:
        break;
	case DOCA_CTX_STATE_STOPPING:
	default:
		break;
	}
}

#ifdef DOCA_ARCH_DPU

doca_error_t
init_dpa_objects(struct objects *objs)
{
    doca_error_t result;

    /* Direct initialization uses the default N and K values. */
    if (objs->num_dpa_threads <= 0)
        objs->num_dpa_threads = DPA_THREADS_DEFAULT;
    if (objs->k_rings <= 0) {
        int k = DPUMESH_RINGS_PER_POD_DEFAULT;
        if (k > objs->num_dpa_threads) k = objs->num_dpa_threads;
        if (k > MAX_EU_PER_POD) k = MAX_EU_PER_POD;
        objs->k_rings = k;
    }
    DOCA_LOG_INFO("DPA multi-EU: num_dpa_threads=%d k_rings=%d (MAX_DPA_RINGS=%d)",
                  objs->num_dpa_threads, objs->k_rings, MAX_DPA_RINGS);

    /* Allocate every EU-thread container up to the array cap; only
     * num_dpa_threads of them, finalized by auto-detect after doca_dpa_start,
     * are created and used. */
    for (int k = 0; k < MAX_DPA_EU; k++) {
        if (!objs->dpa_threads[k]) {
            objs->dpa_threads[k] = calloc(1, sizeof(struct dmesh_doca_dpa_thread));
            if (!objs->dpa_threads[k]) {
                DOCA_LOG_ERR("Failed to allocate memory for dpa_threads[%d]", k);
                return DOCA_ERROR_NO_MEMORY;
            }
        }
        if (!objs->dpa_comches[k]) {
            objs->dpa_comches[k] = calloc(1, sizeof(struct dmesh_doca_dpa_comch));
            if (!objs->dpa_comches[k]) {
                DOCA_LOG_ERR("Failed to allocate memory for dpa_comches[%d]", k);
                return DOCA_ERROR_NO_MEMORY;
            }
        }
    }

    /* One DPA device shared by all EU threads (handles are device-level). */
    result = doca_dpa_create(objs->dev, &objs->dpa);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create DOCA DPA with error = %s", doca_error_get_name(result));
        return result;
    }

    result = doca_dpa_set_app(objs->dpa, DPU_mesh_dpa_app);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set DPA application with error = %s", doca_error_get_name(result));
        goto destroy_dpa;
    }

    /* DPA log level kept at ERROR: INFO emits per-DMA / per-trigger lines that
     * flood the DPU log on the hot path. */
    result = doca_dpa_set_log_level(objs->dpa, DOCA_DPA_DEV_LOG_LEVEL_ERROR);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_WARN("Failed to set DPA log level: %s", doca_error_get_name(result));
    }

    result = doca_dpa_start(objs->dpa);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start DOCA DPA with error = %s", doca_error_get_name(result));
        goto destroy_dpa;
    }

    /* Select N before creating EU threads and mapping pod rings. */
    if (objs->dpa_threads_auto) {
        unsigned int avail = 0;
        doca_error_t er = doca_dpa_get_total_num_eus_available(objs->dpa, &avail);
        int n = (er == DOCA_SUCCESS && avail >= 1) ? (int)avail : objs->num_dpa_threads;
        if (n > DPA_THREADS_AUTO_CAP) n = DPA_THREADS_AUTO_CAP;
        if (n < 1) n = 1;
        objs->num_dpa_threads = n;
        DOCA_LOG_WARN("DPA auto-detect: %u EUs available -> N=%d "
                      "(auto cap=%d, explicit cap=%d)",
                      avail, objs->num_dpa_threads,
                      DPA_THREADS_AUTO_CAP, MAX_DPA_EU);
    }

    int A = objs->n_data_workers >= 1 ? objs->n_data_workers : 1;
    if (objs->k_rings > objs->num_dpa_threads) {
        DOCA_LOG_ERR("Invalid topology: K=%d forward rings exceeds N=%d DPA EUs. "
                     "Host and DPU K cannot be clamped independently.",
                     objs->k_rings, objs->num_dpa_threads);
        result = DOCA_ERROR_INVALID_VALUE;
        goto destroy_dpa;
    }
    /* Align N to the ARM-worker ownership groups. */
    int aligned_n = objs->num_dpa_threads -
                    (objs->num_dpa_threads % A);
    if (aligned_n != objs->num_dpa_threads) {
        DOCA_LOG_WARN("Topology alignment: N %d -> %d so N %% A(%d) == 0",
                      objs->num_dpa_threads, aligned_n, A);
        objs->num_dpa_threads = aligned_n;
    }
    if (!dmesh_topology_valid(objs->k_rings, objs->num_dpa_threads, A)) {
        DOCA_LOG_ERR("Invalid connection-affine topology N/K/A=%d/%d/%d",
                     objs->num_dpa_threads, objs->k_rings, A);
        result = DOCA_ERROR_INVALID_VALUE;
        goto destroy_dpa;
    }
    DOCA_LOG_WARN("Connection-affine topology active: N/K/A=%d/%d/%d "
                  "(EU%%A == ring%%A == port%%A)",
                  objs->num_dpa_threads, objs->k_rings, A);

    /* Point every EU thread struct at the shared device. */
    for (int k = 0; k < objs->num_dpa_threads; k++)
        objs->dpa_threads[k]->dpa = objs->dpa;

    return DOCA_SUCCESS;

destroy_dpa:
    doca_dpa_destroy(objs->dpa);
    objs->dpa = NULL;
    return result;
}

doca_error_t
dmesh_doca_dpa_thread_create(struct dmesh_doca_dpa_thread *dpa_thread, int eu_id)
{
    doca_error_t result;
    struct doca_dpa_eu_affinity *data_affinity = NULL;
    struct doca_dpa_eu_affinity *yield_affinity = NULL;

    result = doca_dpa_mem_alloc(dpa_thread->dpa, sizeof(struct dpa_thread_arg), &dpa_thread->arg);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to alloc dpa mem: %s",
            doca_error_get_descr(result));
        return result;
    }

    result = doca_dpa_thread_create(dpa_thread->dpa, &dpa_thread->thread);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create dpa thread: %s",
            doca_error_get_descr(result));
        return result;
    }

    result = doca_dpa_thread_set_func_arg(dpa_thread->thread, run_dma_manager, dpa_thread->arg);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set DPA thread func: %s",
            doca_error_get_descr(result));
        return result;
    }

    /* The handoff below requires exact affinity: with a relaxed one the helper
     * could notify the data thread before that thread released its EU. */
    if (eu_id < 0)
        return DOCA_ERROR_INVALID_VALUE;
    result = doca_dpa_eu_affinity_create(dpa_thread->dpa, &data_affinity);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_eu_affinity_set(data_affinity,
                                          (unsigned int)eu_id);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_thread_set_affinity(dpa_thread->thread,
                                              data_affinity);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Required data-thread affinity for EU %d failed: %s",
                     eu_id, doca_error_get_descr(result));
        return result;
    }

    result = doca_dpa_thread_start(dpa_thread->thread);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start DPA thread: %s",
            doca_error_get_descr(result));
        return result;
    }

    /* The helper wakes this data thread only after the helper itself has been
     * scheduled on their shared physical EU. */
    result = doca_dpa_notification_completion_create(
        dpa_thread->dpa, dpa_thread->thread,
        &dpa_thread->resume_completion);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_notification_completion_start(
            dpa_thread->resume_completion);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_notification_completion_get_dpa_handle(
            dpa_thread->resume_completion, &dpa_thread->resume_handle);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create data resume completion for EU %d: %s",
                     eu_id, doca_error_get_descr(result));
        return result;
    }

    result = doca_dpa_thread_create(dpa_thread->dpa,
                                    &dpa_thread->yield_thread);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_thread_set_func_arg(
            dpa_thread->yield_thread, run_dma_yield_helper,
            dpa_thread->resume_handle);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_eu_affinity_create(dpa_thread->dpa,
                                             &yield_affinity);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_eu_affinity_set(yield_affinity,
                                          (unsigned int)eu_id);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_thread_set_affinity(dpa_thread->yield_thread,
                                              yield_affinity);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_thread_start(dpa_thread->yield_thread);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create same-EU yield helper for EU %d: %s",
                     eu_id, doca_error_get_descr(result));
        return result;
    }

    result = doca_dpa_notification_completion_create(
        dpa_thread->dpa, dpa_thread->yield_thread,
        &dpa_thread->yield_completion);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_notification_completion_start(
            dpa_thread->yield_completion);
    if (result == DOCA_SUCCESS)
        result = doca_dpa_notification_completion_get_dpa_handle(
            dpa_thread->yield_completion, &dpa_thread->yield_handle);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create yield-helper completion for EU %d: %s",
                     eu_id, doca_error_get_descr(result));
        return result;
    }

    /* Make the helper activatable. doca_dpa_thread_run does not execute a
     * kernel — it only lets a notification schedule one — so the helper stays
     * dormant until the data thread notifies it on the way out of this EU. */
    result = doca_dpa_thread_run(dpa_thread->yield_thread);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to prime yield helper for EU %d: %s", eu_id,
                     doca_error_get_descr(result));
        return result;
    }

    return DOCA_SUCCESS;
}

doca_error_t
dmesh_doca_dpa_msgq_create(const struct dmesh_doca_dpa_msgq_create_attr *attr,
                            struct dmesh_doca_dpa_msgq *msgq)
{
    doca_error_t result;
    struct doca_ctx *consumer_ctx;
    struct doca_ctx *producer_ctx;
    uint32_t consumer_id;

    memset(msgq, 0, sizeof(*msgq));

    result = doca_comch_msgq_create(attr->dev, &msgq->msgq);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create comch msgq - %s",
                doca_error_get_name(result));
        return result;
    }
    
    result = doca_comch_msgq_set_max_num_consumers(msgq->msgq, 1);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set max num consumers - %s",
                doca_error_get_name(result));
        return result;
    }

    result = doca_comch_msgq_set_max_num_producers(msgq->msgq, 1);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set max num producers - %s",
                doca_error_get_name(result));
        return result;
    }
    
    /* if true, DPA is consumer */
    if (attr->is_send) {
        result = doca_comch_msgq_set_dpa_consumer(msgq->msgq, attr->dpa);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set dpa consumer - %s",
                    doca_error_get_name(result));
            return result;
        }
    } else {
        /* else, DPA is producer */
        result = doca_comch_msgq_set_dpa_producer(msgq->msgq, attr->dpa);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set dpa producer - %s",
                    doca_error_get_name(result));
            return result;
        }
    }
    
    result = doca_comch_msgq_start(msgq->msgq);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start msgq - %s",
                doca_error_get_name(result));
        return result;
    }
    
    result = doca_comch_msgq_consumer_create(msgq->msgq, &msgq->consumer);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create msgq consumer - %s",
                doca_error_get_name(result));
        return result;
    }

    result = doca_comch_consumer_get_id(msgq->consumer, &consumer_id);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get msgq consumer id - %s",
                doca_error_get_name(result));
        return result;
    }
    msgq->target_consumer_id = consumer_id;

    consumer_ctx = doca_comch_consumer_as_ctx(msgq->consumer);
    /* DPU→DPA direction: must fit the largest message (DPA_MSG_RING_ADD, _RING_DEL, etc.) */
    result = doca_comch_consumer_set_imm_data_len(msgq->consumer, sizeof(struct comch_msg));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set imm data len to %zu - %s",
                sizeof(struct comch_msg), doca_error_get_name(result));
        return result;
    }
    
    if (attr->is_send) {
        /* consumer on DPA */
        result = doca_ctx_set_datapath_on_dpa(consumer_ctx, attr->dpa);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set consumer datapath on dpa - %s",
                    doca_error_get_name(result));
            return result;
        }
        result = doca_comch_consumer_set_completion(msgq->consumer, attr->consumer_comp, 0);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set consumer completion - %s",
                    doca_error_get_name(result));
            return result;
        }
        result = doca_comch_consumer_set_dev_max_num_recv(msgq->consumer, attr->max_num_msg);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set consumer max # of recv messages - %s",
                    doca_error_get_name(result));
            return result;
        }
    } else {
        /* consumer on DPU */
        union doca_data ctx_user_data;
        ctx_user_data.ptr = attr->ctx_user_data;
        result = doca_ctx_set_user_data(consumer_ctx, ctx_user_data);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set consumer ctx user data - %s",
                    doca_error_get_name(result));
            return result;
        }
        result = doca_ctx_set_state_changed_cb(consumer_ctx, attr->ctx_state_changed_cb);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set state changed cb - %s",
                    doca_error_get_name(result));
            return result;
        }
        result = doca_pe_connect_ctx(attr->pe, consumer_ctx);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to connect consumer to pe - %s",
                    doca_error_get_name(result));
            return result;
        }
        result = doca_comch_consumer_task_post_recv_set_conf(msgq->consumer,
                                        dmesh_doca_dpa_msgq_recv_cb,
                                        dmesh_doca_dpa_msgq_recv_error_cb,
                                        attr->max_num_msg);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set consumer task config - %s",
                    doca_error_get_name(result));
            return result;
        }
    }

    result = doca_ctx_start(consumer_ctx);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start consumer ctx - %s", 
                doca_error_get_name(result));
        return result;
    }

    result = doca_comch_msgq_producer_create(msgq->msgq, &msgq->producer);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create msgq producer - %s", 
                doca_error_get_name(result));
        return result;
    }
    producer_ctx = doca_comch_producer_as_ctx(msgq->producer);
    if (attr->is_send) {
        /* producer on DPU */
        union doca_data ctx_user_data;
        ctx_user_data.ptr = attr->ctx_user_data;
        result = doca_ctx_set_user_data(producer_ctx, ctx_user_data);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set producer ctx user data - %s", 
                    doca_error_get_name(result));
            return result;
        }
        result = doca_ctx_set_state_changed_cb(producer_ctx, attr->ctx_state_changed_cb);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set state changed cb - %s", 
                    doca_error_get_name(result));
            return result;
        }
        result = doca_pe_connect_ctx(attr->pe, producer_ctx);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to connect producer to pe - %s", 
                    doca_error_get_name(result));
            return result;
        }
        result = doca_comch_producer_task_send_set_conf(msgq->producer,
                                dmesh_doca_dpa_msgq_send_cb,
                                dmesh_doca_dpa_msgq_send_error_cb,
                                attr->max_num_msg);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set producer task config - %s", 
                    doca_error_get_name(result));
            return result;
        }
    } else {
        /* producer on DPA */
        result = doca_ctx_set_datapath_on_dpa(producer_ctx, attr->dpa);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set producer datapath on dpa - %s", 
                    doca_error_get_name(result));
            return result;
        }
        result = doca_comch_producer_set_dev_max_num_send(msgq->producer, attr->max_num_msg);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set producer max # of send messages - %s", 
                    doca_error_get_name(result));
            return result;
        }
        result = doca_comch_producer_dpa_completion_attach(msgq->producer, attr->producer_comp);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to attach producer dpa completion - %s", 
                    doca_error_get_name(result));
            return result;
        }
    }
    result = doca_ctx_start(producer_ctx);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start producer ctx - %s",
                doca_error_get_name(result));
        return result;
    }

    if (attr->is_send == false) {
        for (uint32_t idx = 0; idx < attr->max_num_msg; idx++) {
            struct doca_comch_consumer_task_post_recv *recv_task;
            result = doca_comch_consumer_task_post_recv_alloc_init(msgq->consumer, NULL, &recv_task);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to alloc recv task at idx=%u: %s", idx, doca_error_get_name(result));
                return result;
            }
            union doca_data recv_user_data = { .ptr = msgq };
            doca_task_set_user_data(
                doca_comch_consumer_task_post_recv_as_task(recv_task),
                recv_user_data);
            result = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(recv_task));
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to submit recv task at idx=%u: %s", idx, doca_error_get_name(result));
                return result;
            }
            __atomic_fetch_add(&msgq->recv_posted, 1, __ATOMIC_RELAXED);
        }
    }

    return DOCA_SUCCESS;
}

doca_error_t
dmesh_doca_dpa_comch_create(struct objects *objs, int idx)
{
    struct dmesh_doca_dpa_comch *comch = objs->dpa_comches[idx];
    struct dmesh_doca_dpa_thread *dpa_thread = objs->dpa_threads[idx];
    doca_error_t result;

    memset(comch, 0, sizeof(*comch));

    result = doca_comch_consumer_completion_create(&(comch->consumer_comp));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create consumer completion - %s",
                doca_error_get_name(result));
        return result;
    }
    
    result = doca_comch_consumer_completion_set_max_num_recv(comch->consumer_comp,
            CC_DPA_MAX_MSG_NUM);    
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set max num recv - %s",
            doca_error_get_name(result));
        return result;
    }

    /* Must match the consumer's imm_data_len (consumer <= completion required by DOCA).
     * DPA actually sends only sizeof(comch_dma_comp_msg) bytes, but the buffer
     * must be large enough for the consumer's configured imm size. */
    result = doca_comch_consumer_completion_set_imm_data_len(comch->consumer_comp, sizeof(struct comch_msg));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set completion imm data len to %zu - %s",
            sizeof(struct comch_msg),
            doca_error_get_name(result));
        return result;
        }
        
    result = doca_comch_consumer_completion_set_dpa_thread(comch->consumer_comp, dpa_thread->thread);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set dpa thread - %s",
            doca_error_get_name(result));
        return result;
    }

    result = doca_comch_consumer_completion_start(comch->consumer_comp);
    if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start consumer completion - %s",
			     doca_error_get_name(result));
        return result;
    }

    result = doca_dpa_completion_create(dpa_thread->dpa, CC_DPA_MAX_MSG_NUM, &comch->producer_comp);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create producer completion - %s",
                doca_error_get_name(result));
        return result;
    }
    result = doca_dpa_completion_set_thread(comch->producer_comp, dpa_thread->thread);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set dpa thread to producer completion - %s",
                doca_error_get_name(result));
        return result;
    }
    result = doca_dpa_completion_start(comch->producer_comp);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start producer completion - %s",
                doca_error_get_name(result));
        return result;
    }

    return DOCA_SUCCESS;
}

/* Fill a DPA thread argument with shared Comch handles and no initial rings. */
static doca_error_t
dmesh_fill_dpa_thread_arg(struct objects *objs, int idx, struct dpa_thread_arg *arg)
{
    doca_error_t result;
    struct dmesh_doca_dpa_comch *comch = objs->dpa_comches[idx];
    doca_dpa_dev_comch_consumer_completion_t dpa_consumer_comp;
    doca_dpa_dev_completion_t dpa_producer_comp;
    doca_dpa_dev_comch_producer_t dpa_producer;
    doca_dpa_dev_comch_consumer_t dpa_consumer;
    uint32_t send_consumer_id;
    uint32_t recv_consumer_id;

    result = doca_comch_consumer_completion_get_dpa_handle(comch->consumer_comp, &dpa_consumer_comp);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get consumer completion DPA handle: %s", doca_error_get_name(result));
        return result;
    }
    result = doca_dpa_completion_get_dpa_handle(comch->producer_comp, &dpa_producer_comp);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get producer completion DPA handle: %s", doca_error_get_name(result));
        return result;
    }
    result = doca_comch_consumer_get_dpa_handle(comch->send.consumer, &dpa_consumer);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get consumer DPA handle: %s", doca_error_get_name(result));
        return result;
    }
    result = doca_comch_consumer_get_id(comch->send.consumer, &send_consumer_id);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get send.consumer ID: %s", doca_error_get_name(result));
        return result;
    }
    result = doca_comch_consumer_get_id(comch->recv.consumer, &recv_consumer_id);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get recv.consumer ID: %s", doca_error_get_name(result));
        return result;
    }

    result = doca_comch_producer_get_dpa_handle(comch->recv.producer, &dpa_producer);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get producer DPA handle: %s", doca_error_get_name(result));
        return result;
    }

    memset(arg, 0, sizeof(*arg));
    arg->eu_index = (uint32_t)idx;   /* stamped into every ring ACK so ARM can attribute it */
    arg->dpa_consumer_comp = dpa_consumer_comp;
    arg->dpa_producer_comp = dpa_producer_comp;
    arg->dpa_consumer = dpa_consumer;
    arg->dpa_producer = dpa_producer;
    arg->yield_notification = objs->dpa_threads[idx]->yield_handle;
    arg->dpu_consumer_id = recv_consumer_id;
    arg->num_rings = 0;  /* rings added dynamically via setup_pod_dma */

    return DOCA_SUCCESS;
}

/* Nonblocking send: returns immediately on submit failure, no PE progress, no
 * retry. Hot-path DPU→DPA wake signals use this directly — a missed trigger is
 * recoverable by the next successful send. The message travels in a copy the
 * completion callback frees, so the caller's buffer is free at return. */
doca_error_t
dmesh_doca_dpa_msgq_send_try(struct dmesh_doca_dpa_msgq *msgq, void *msg, uint32_t msg_size)
{
    void *payload = malloc(msg_size);
    if (payload == NULL)
        return DOCA_ERROR_NO_MEMORY;
    memcpy(payload, msg, msg_size);

    struct doca_comch_producer_task_send *send_task;
    doca_error_t result =
        doca_comch_producer_task_send_alloc_init(msgq->producer, NULL,
                                                 payload, msg_size,
                                                 msgq->target_consumer_id,
                                                 &send_task);
    if (result != DOCA_SUCCESS) {
        free(payload);
        return result;
    }

    struct doca_task *task = doca_comch_producer_task_send_as_task(send_task);
    union doca_data user_data = { .ptr = payload };
    doca_task_set_user_data(task, user_data);

    result = doca_task_submit(task);
    if (result != DOCA_SUCCESS) {
        free(payload);
        doca_task_free(task);
    }
    return result;
}

/* The same send on the control path (thread start, RING_ADD), where a refusal
 * is a fault worth naming rather than a hint the next send recovers. This can
 * run inside the main PE's Comch callback, so it must not recursively progress
 * that PE. */
doca_error_t
dmesh_doca_dpa_msgq_send(struct dmesh_doca_dpa_msgq *msgq, void *msg, uint32_t msg_size)
{
    doca_error_t result = dmesh_doca_dpa_msgq_send_try(msgq, msg, msg_size);
    if (result != DOCA_SUCCESS)
        DOCA_LOG_ERR("DPA MsgQ send failed: %s (msg_size=%u)",
                     doca_error_get_name(result), msg_size);
    return result;
}

/*
 * Create a DPA buffer array for a specific mmap (per-pod version).
 */
doca_error_t
setup_dpa_buf_array_pod(struct objects *objs, size_t num_elem,
                        struct doca_mmap *mmap, struct doca_buf_arr **out_buf_arr)
{
    doca_error_t result;

    result = doca_buf_arr_create(num_elem, out_buf_arr);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create buffer array: %s", doca_error_get_descr(result));
        return result;
    }

    result = doca_buf_arr_set_target_dpa(*out_buf_arr, objs->dpa);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set buffer array target DPA: %s", doca_error_get_descr(result));
        goto destroy_buf_arr;
    }

    result = doca_buf_arr_set_params(*out_buf_arr, mmap, sizeof(struct dma_desc), 0);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set buffer array params: %s", doca_error_get_descr(result));
        goto destroy_buf_arr;
    }

    result = doca_buf_arr_start(*out_buf_arr);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start buffer array: %s", doca_error_get_descr(result));
        goto destroy_buf_arr;
    }

    return DOCA_SUCCESS;

destroy_buf_arr:
    doca_buf_arr_destroy(*out_buf_arr);
    *out_buf_arr = NULL;
    return result;
}

/*
 * Fill ring info for a specific pod.
 */
static doca_error_t
dmesh_fill_dpa_ring_info(struct objects *objs, struct pod_state *pod, int j,
                         struct dpa_ring_info *ring_info)
{
    doca_error_t result;
    doca_dpa_dev_buf_arr_t dpa_buf_arr;
    doca_dpa_dev_mmap_t host_mmap, dpu_mmap;

    /* Ring j uses buf_arrs[j], the shared host TX mmap, and the pod staging base.
     * region_off is fixed at zero by the wire ABI. */
    result = doca_buf_arr_get_dpa_handle(pod->buf_arrs[j], &dpa_buf_arr);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get buf array DPA handle: %s", doca_error_get_name(result));
        return result;
    }

    result = doca_mmap_dev_get_dpa_handle(pod->remote_mmap, objs->dev, &host_mmap);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get host mmap DPA handle: %s", doca_error_get_name(result));
        return result;
    }

    result = doca_mmap_dev_get_dpa_handle(pod->local_mmap, objs->dev, &dpu_mmap);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get DPU mmap DPA handle: %s", doca_error_get_name(result));
        return result;
    }

    ring_info->buf_arr = dpa_buf_arr;
    ring_info->buf_arr_size = DMA_RING_SIZE;
    ring_info->host_mmap = host_mmap;
    ring_info->host_addr = (uint64_t)pod->remote_addr;
    ring_info->dpu_mmap = dpu_mmap;
    ring_info->dpu_addr = (uint64_t)pod->dma_buffer;  /* pod staging base */
    ring_info->region_off = 0;                        /* wire-ABI constant */
    ring_info->host_buf_size = (uint32_t)pod->remote_buf_size;
    ring_info->pod_id = pod->pod_id;

    return DOCA_SUCCESS;
}

/* Forward-ring EU owner. */
static inline int
dpa_eu_for_ring(const struct objects *objs, int pod_id, int ring)
{
    return dmesh_dpa_eu_for_ring(pod_id, objs->k_rings, ring,
                                 objs->num_dpa_threads,
                                 objs->n_data_workers);
}

/* Create and publish a pod's DMA buffers, ring metadata, and DPA thread state. */
doca_error_t
setup_pod_dma(struct objects *objs, struct pod_state *pod)
{
    doca_error_t result;

    int N = objs->num_dpa_threads;
    int K = objs->k_rings > 0 ? objs->k_rings : 1;
    if (K > N) K = N;
    int L = objs->n_data_workers > 0 ? objs->n_data_workers : 1;
    /* The host is told L and shards its credit returns by it, so a geometry the
     * egress engine cannot honour must fail the pod instead of degrading. */
    if (L > K || K % L != 0) {
        DOCA_LOG_ERR("pod %d: unusable landing geometry K=%d L=%d",
                     pod->pod_id, K, L);
        return DOCA_ERROR_INVALID_VALUE;
    }
    pod->k_rings = K;
    pod->landing_stripes = L;
    __atomic_store_n(&pod->egress_quiesced_mask, 0, __ATOMIC_RELEASE);
    for (int a = 0; a < MAX_ARM_WORKERS; a++)
        __atomic_store_n(&pod->egress_inflight_worker[a].v, 0,
                         __ATOMIC_RELEASE);
    __atomic_store_n(&pod->egress_pending_emit, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pod->proxy_source_refs, 0, __ATOMIC_RELEASE);

    /* Every target EU must ACK this exact incarnation. Publish the expected
     * bitmap before any EU can answer; K<=N makes the mapping injective. */
    uint32_t generation = __atomic_add_fetch(&pod->dma_generation, 1,
                                              __ATOMIC_ACQ_REL);
    uint32_t expected_mask = 0;
    for (int j = 0; j < K; j++)
        expected_mask |= 1u << dpa_eu_for_ring(objs, pod->pod_id, j);
    __atomic_store_n(&pod->dpa_add_ack_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pod->dpa_add_ack_failed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pod->dpa_setup_complete, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pod->dpa_add_expected_mask, expected_mask, __ATOMIC_RELEASE);
    pod->dpa_add_last_send_ns = 0;

    /* One staging buffer per pod mirrors host TX offsets across all forward rings.
     * The 128-byte tail covers the 128 B copy rounding
     * (dpa_dma_aligned_copy_len) at the buffer boundary. */
    /* Allocated once per slot and reused across incarnations: it is DPU-local, so
     * a reconnecting pod lands in the same staging. It is never freed — it is the
     * egress SG-DMA read source, and destroying it faults the engine's shared
     * doca_dma ctx (see pods_remove_connection). */
    /* On reuse it is not memset either: stale bytes are unreachable, since the
     * DPA only lands at offsets the new pod's own descriptors name. */
    if (pod->local_mmap == NULL) {
        result = alloc_buffer_and_set_thread_safe_mmap(
            &pod->local_mmap, objs->dev, &pod->dma_buffer,
            DPU_BUFFER_SIZE + 128,
            DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
                DOCA_ACCESS_FLAG_PCI_READ_WRITE);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("setup_pod_dma: alloc buffer failed for pod %d: %s",
                         pod->pod_id, doca_error_get_descr(result));
            return result;
        }
    }
    /* (The per-pod DPU staging buffer is NOT exported to the host: the host never
     * reads it — the SG-DMA egress lands into the receiver's own rx_dma_buffer.) */

    /* Map each worker's K/A rings across its N/A EUs. The first ring starts
     * the EU thread; additional rings use ADD_RING. */
    for (int j = 0; j < K; j++) {
        int k_j = dpa_eu_for_ring(objs, pod->pod_id, j);
        result = setup_dpa_buf_array_pod(
            objs, DMA_RING_SIZE + DMA_RING_EXTRA_SLOTS,
            pod->ring_mmaps[j], &pod->buf_arrs[j]);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("setup_pod_dma: buf_arr[%d] failed for pod %d: %s",
                         j, pod->pod_id, doca_error_get_descr(result));
            return result;
        }

        struct dpa_ring_info ring_info;
        result = dmesh_fill_dpa_ring_info(objs, pod, j, &ring_info);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("setup_pod_dma: fill ring info[%d] failed for pod %d: %s",
                         j, pod->pod_id, doca_error_get_descr(result));
            return result;
        }

        struct dmesh_doca_dpa_thread *dpa_thread = objs->dpa_threads[k_j];
        if (!objs->dpa_thread_running[k_j]) {
            struct dpa_thread_arg arg;
            result = dmesh_fill_dpa_thread_arg(objs, k_j, &arg);
            if (result != DOCA_SUCCESS)
                return result;
            arg.rings[0] = ring_info;
            arg.ring_generation[0] = generation;
            arg.num_rings = 1;
            arg.initial_generation = generation;
            result = doca_dpa_h2d_memcpy(objs->dpa, dpa_thread->arg,
                                          &arg, sizeof(struct dpa_thread_arg));
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("setup_pod_dma: h2d_memcpy failed (EU %d): %s",
                             k_j, doca_error_get_descr(result));
                return result;
            }
            uint64_t rpc_ret;
            result = doca_dpa_rpc(objs->dpa, thread_init_rpc, &rpc_ret,
                                  arg.dpa_consumer, (uint32_t)CC_DPA_MAX_MSG_NUM);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("setup_pod_dma: thread_init_rpc failed (EU %d): %s",
                             k_j, doca_error_get_descr(result));
                return result;
            }
            result = doca_dpa_thread_run(dpa_thread->thread);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("setup_pod_dma: dpa_thread_run failed (EU %d): %s",
                             k_j, doca_error_get_descr(result));
                return result;
            }
            objs->dpa_thread_running[k_j] = 1;

            struct comch_msg trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.type = DPA_MSG_WAKE;
            result = dmesh_doca_dpa_msgq_send(&objs->dpa_comches[k_j]->send,
                                               &trigger, sizeof(trigger));
            if (result != DOCA_SUCCESS)
                DOCA_LOG_WARN("Trigger msg to EU %d failed: %s", k_j, doca_error_get_descr(result));
        } else {
            struct comch_add_ring_msg add_msg;
            memset(&add_msg, 0, sizeof(add_msg));
            add_msg.type = DPA_MSG_RING_ADD;
            add_msg.generation = generation;
            add_msg.ring = ring_info;
            result = dmesh_doca_dpa_msgq_send(&objs->dpa_comches[k_j]->send,
                                               &add_msg, sizeof(add_msg));
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("setup_pod_dma: send ADD_RING to EU %d failed: %s",
                             k_j, doca_error_get_descr(result));
                return result;
            }
        }
    }

    /* Reverse direction (DPU→host) is the ARM SG-DMA egress engine (dpu_proxy.c):
     * it reads segments out of this pod's staging and DMAs them into the receiver's
     * host RX buffer directly — there is no DPA reverse ring to wire up here. The
     * host RX mmap is imported lazily (comch_common.c); the egress uses it once
     * present (pod->host_rx_mmap / host_rx_addr). */

    /* Do NOT publish dma_ready here. ADD_RING send completion only means the ARM
     * submitted the task. dpu_finalize_pending_pod_inits publishes readiness after
     * every target EU returns an ADD_ACK for `generation`. RELEASE makes all setup
     * fields above visible to that main-thread ACQUIRE. */
    {
        struct timespec sent_at;
        clock_gettime(CLOCK_MONOTONIC, &sent_at);
        pod->dpa_add_last_send_ns =
            (uint64_t)sent_at.tv_sec * 1000000000ull + (uint64_t)sent_at.tv_nsec;
    }
    __atomic_store_n(&pod->dpa_setup_complete, 1, __ATOMIC_RELEASE);
    return DOCA_SUCCESS;
}

int
progress_setup_pod_dma(struct objects *objs, struct pod_state *pod)
{
    uint32_t expected = __atomic_load_n(&pod->dpa_add_expected_mask,
                                         __ATOMIC_ACQUIRE);
    uint32_t acked = __atomic_load_n(&pod->dpa_add_ack_mask,
                                      __ATOMIC_ACQUIRE);
    if ((acked & expected) == expected)
        return 1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull +
                      (uint64_t)now.tv_nsec;
    if (pod->dpa_add_last_send_ns != 0 &&
        now_ns - pod->dpa_add_last_send_ns < 10000000ull)
        return 0;
    pod->dpa_add_last_send_ns = now_ns;

    int K = pod->k_rings;
    int N = objs->num_dpa_threads;
    if (K > N) K = N;
    uint32_t generation = __atomic_load_n(&pod->dma_generation,
                                           __ATOMIC_ACQUIRE);
    for (int j = 0; j < K; j++) {
        int eu = dpa_eu_for_ring(objs, pod->pod_id, j);
        uint32_t bit = 1u << eu;
        if ((expected & bit) == 0 || (acked & bit) != 0)
            continue;

        struct comch_add_ring_msg add_msg;
        memset(&add_msg, 0, sizeof(add_msg));
        add_msg.type = DPA_MSG_RING_ADD;
        add_msg.generation = generation;
        doca_error_t result = dmesh_fill_dpa_ring_info(objs, pod, j,
                                                       &add_msg.ring);
        if (result != DOCA_SUCCESS)
            return -1;
        result = dmesh_doca_dpa_msgq_send_try(
            &objs->dpa_comches[eu]->send, &add_msg, sizeof(add_msg));
        if (result != DOCA_SUCCESS && result != DOCA_ERROR_AGAIN) {
            DOCA_LOG_ERR("RING_ADD retry to EU %d failed (pod_id=%d): %s",
                         eu, pod->pod_id, doca_error_get_descr(result));
            return -1;
        }
    }
    return 0;
}

void
teardown_pod_dma(struct objects *objs, struct pod_state *pod)
{
    /* k_rings is stamped by setup_pod_dma; 0 means this pod never got that far
     * (registered but disconnected before its mmaps arrived) → no rings to drop. */
    int K = pod->k_rings;
    int N = objs->num_dpa_threads;
    __atomic_store_n(&pod->dpa_del_expected_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pod->dpa_del_ack_mask, 0, __ATOMIC_RELEASE);
    pod->dpa_del_last_send_ns = 0;
    if (K <= 0 || N <= 0 || pod->pod_id < 0)
        return;
    if (K > N) K = N;

    uint32_t expected = 0;
    for (int j = 0; j < K; j++) {
        int k_j = dpa_eu_for_ring(objs, pod->pod_id, j);
        if (objs->dpa_thread_running[k_j])
            expected |= 1u << k_j;
    }
    __atomic_store_n(&pod->dpa_del_expected_mask, expected, __ATOMIC_RELEASE);
    (void)progress_teardown_pod_dma(objs, pod);
}

int
progress_teardown_pod_dma(struct objects *objs, struct pod_state *pod)
{
    uint32_t expected = __atomic_load_n(&pod->dpa_del_expected_mask,
                                         __ATOMIC_ACQUIRE);
    uint32_t acked = __atomic_load_n(&pod->dpa_del_ack_mask,
                                      __ATOMIC_ACQUIRE);
    if ((acked & expected) == expected)
        return 1;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull +
                      (uint64_t)now.tv_nsec;
    /* An ACK can be lost if the ARM receive pool was temporarily empty. DEL is
     * idempotent, so retry unacked EUs at 10 ms without filling their MsgQs. */
    if (pod->dpa_del_last_send_ns != 0 &&
        now_ns - pod->dpa_del_last_send_ns < 10000000ull)
        return 0;
    pod->dpa_del_last_send_ns = now_ns;

    int K = pod->k_rings;
    int N = objs->num_dpa_threads;
    if (K > N) K = N;
    uint32_t generation = __atomic_load_n(&pod->dma_generation,
                                           __ATOMIC_ACQUIRE);
    for (int j = 0; j < K; j++) {
        int eu = dpa_eu_for_ring(objs, pod->pod_id, j);
        uint32_t bit = 1u << eu;
        if ((expected & bit) == 0 || (acked & bit) != 0)
            continue;
        struct comch_add_ring_msg del_msg;
        memset(&del_msg, 0, sizeof(del_msg));
        del_msg.type = DPA_MSG_RING_DEL;
        del_msg.generation = generation;
        del_msg.ring.pod_id = pod->pod_id;
        doca_error_t result = dmesh_doca_dpa_msgq_send_try(
            &objs->dpa_comches[eu]->send, &del_msg, sizeof(del_msg));
        if (result != DOCA_SUCCESS && result != DOCA_ERROR_AGAIN)
            DOCA_LOG_WARN("RING_DEL retry to EU %d failed (pod_id=%d): %s",
                          eu, pod->pod_id, doca_error_get_descr(result));
    }
    return 0;
}

#endif /* DOCA_ARCH_DPU */
