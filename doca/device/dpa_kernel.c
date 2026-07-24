#include "doca_dpa_dev.h"
#include "doca_dpa_dev_comch_msgq.h"
#include "doca_dpa_dev_buf.h"
#include "dpaintrin.h"
#include "dpa_common.h"

/* Spin iterations waiting for DPU consumer to resubmit recv tasks before
 * dropping the descriptor (consumer stalled). */
#define DMA_CONSUMER_EMPTY_WAIT_LOOPS  0x800000

/* Max DMA size for doca_dpa_dev_comch_producer_dma_copy.
 * HW supports up to 8KB per single call with 128B-aligned addresses. */
#define DPA_DMA_COPY_MAX  8192
/* The DPU staging slot is DPUMESH_SLOT_SIZE bytes; each FORWARD dma_copy (host →
 * DPU staging) is capped at DPA_DMA_COPY_MAX. If the cap exceeded the canonical slot
 * size a copy could overflow the staging slot, so couple them at compile time.
 * (The DPA is forward-only — DPU→host egress is the ARM SG-DMA engine, dpu_proxy.c —
 * so no reverse DMA is involved here.) */
_Static_assert(DPA_DMA_COPY_MAX <= DPUMESH_SLOT_SIZE,
               "DPA_DMA_COPY_MAX must not exceed DPUMESH_SLOT_SIZE");

/* Alignment requirements for doca_dpa_dev_comch_producer_dma_copy:
 * - Source and destination addresses: 64B aligned
 *   (ensured by CACHE_ALIGN=128 buffer allocation + 128B-aligned offsets)
 * - Transfer size per call: 128B aligned, max 8KB */
#define DMA_COPY_SIZE_ALIGN  128
#define ALIGN_UP_128(x)  (((x) + (DMA_COPY_SIZE_ALIGN - 1)) & ~(uint32_t)(DMA_COPY_SIZE_ALIGN - 1))

/* Max consecutive descriptors drained from one ring per process_*_desc call.
 * Bounds per-ring work so a busy ring can't starve the others within a single
 * drain_all_rings inner iter. */
#define RING_BATCH_CAP  32

/*
 * RPC for initializing DPA IO thread called before running the thread
 *
 * @consumer [in]: The DPA Comch consumer
 * @num_msg [in]: Number of consumer recv credits to acknowledge
 * @return: always returns 0
 */

__dpa_rpc__ uint64_t thread_init_rpc(doca_dpa_dev_comch_consumer_t consumer, uint32_t num_msg)
{
    DOCA_DPA_DEV_LOG_INFO("recv thread init RPC, num_msg: %u\n", num_msg);
	doca_dpa_dev_comch_consumer_ack(consumer, num_msg);

	return 0;
}

/* An ADD ACK is an init correctness barrier, not a data completion. Wait only
 * for a posted ARM receive and then issue a flushed immediate. If ARM has no
 * receive after the bounded wait, the host-side 30 s init timeout fails closed. */
#define RING_ACK_CONSUMER_WAIT_LOOPS 100000u
static int send_ring_ack(struct dpa_thread_arg *thread_arg, uint8_t type,
                         int32_t pod_id, uint32_t generation, uint8_t status)
{
    struct dpa_ring_ack_msg ack = {
        .type = type,
        .pod_id = (int8_t)pod_id,
        .status = status,
        .eu_index = (uint8_t)thread_arg->eu_index,
        .generation = generation,
    };
    uint32_t wait = 0;
    while (doca_dpa_dev_comch_producer_is_consumer_empty(
               thread_arg->dpa_producer, thread_arg->dpu_consumer_id) == 1 &&
           ++wait < RING_ACK_CONSUMER_WAIT_LOOPS) {
    }
    if (wait >= RING_ACK_CONSUMER_WAIT_LOOPS) {
        DOCA_DPA_DEV_LOG_INFO("RING_ACK type=%u: DPU consumer unavailable pod=%d eu=%u gen=%u\n",
                              type, pod_id, thread_arg->eu_index, generation);
        return 0;
    }
    doca_dpa_dev_comch_producer_post_send_imm_only(
        thread_arg->dpa_producer, thread_arg->dpu_consumer_id,
        (const uint8_t *)&ack, sizeof(ack), DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
    return 1;
}

static void handle_dpu_msg(struct dpa_thread_arg *thread_arg, const struct comch_msg *msg)
{
    switch(msg->type) {
        case DPA_MSG_RING_ADD: {
            struct comch_add_ring_msg *add_msg = (struct comch_add_ring_msg *)msg;
            /* A pod has at most ONE ring per EU, so a same-pod_id entry is a stale
             * duplicate: overwrite in place rather than accrete. Defensive — RING_DEL
             * on disconnect should mean this never fires. */
            uint32_t slot = thread_arg->num_rings;
            for (uint32_t r = 0; r < thread_arg->num_rings; r++) {
                if (thread_arg->rings[r].pod_id == add_msg->ring.pod_id) {
                    slot = r;
                    DOCA_DPA_DEV_LOG_INFO("ADD_RING: replacing stale entry for pod_id=%d at r=%u\n",
                                          add_msg->ring.pod_id, r);
                    break;
                }
            }
            if (slot < MAX_DPA_RINGS) {
                thread_arg->rings[slot] = add_msg->ring;
                thread_arg->ring_generation[slot] = add_msg->generation;
                thread_arg->consumer_head[slot] = 0;
                if (slot == thread_arg->num_rings)
                    thread_arg->num_rings++;
                DOCA_DPA_DEV_LOG_INFO("Added ring: pod_id=%d, r=%u, num_rings=%u\n",
                                     add_msg->ring.pod_id, slot, thread_arg->num_rings);
                send_ring_ack(thread_arg, DPA_MSG_RING_ADD_ACK,
                              add_msg->ring.pod_id, add_msg->generation,
                              DPA_RING_ACK_OK);
            } else {
                DOCA_DPA_DEV_LOG_INFO("Ring add failed: too many rings=%u\n", thread_arg->num_rings);
                send_ring_ack(thread_arg, DPA_MSG_RING_ADD_ACK,
                              add_msg->ring.pod_id, add_msg->generation,
                              DPA_RING_ACK_FULL);
            }
            break;
        }
        case DPA_MSG_RING_DEL: {
            struct comch_add_ring_msg *del_msg = (struct comch_add_ring_msg *)msg;
            /* Only ring.pod_id is meaningful (struct reused so the wire ABI + its
             * _Static_asserts stay frozen). Swap-with-last keeps rings[0,num_rings)
             * dense; consumer_head must travel with its ring. */
            for (uint32_t r = 0; r < thread_arg->num_rings; r++) {
                if (thread_arg->rings[r].pod_id != del_msg->ring.pod_id)
                    continue;
                uint32_t last = thread_arg->num_rings - 1;
                if (r != last) {
                    thread_arg->rings[r] = thread_arg->rings[last];
                    thread_arg->consumer_head[r] =
                        thread_arg->consumer_head[last];
                    thread_arg->ring_generation[r] =
                        thread_arg->ring_generation[last];
                }
                thread_arg->consumer_head[last] = 0;
                thread_arg->ring_generation[last] = 0;
                /* Plain store is enough: this EU is the only mutator (handle_msgs runs
                 * on it), and num_rings is volatile so the drain loop re-reads it. */
                thread_arg->num_rings = last;
                DOCA_DPA_DEV_LOG_INFO("RING_DEL: dropped pod_id=%d at r=%u, num_rings=%u\n",
                                      del_msg->ring.pod_id, r, thread_arg->num_rings);
                break;
            }
            /* This FLUSHED send is on the same ordered producer as every forward
             * DMA. Once ARM receives it, every older DMA WQE that could name the
             * removed ring's mmap/buf_arr has completed. DEL is idempotent: a
             * retry for an already absent pod still gets an ACK. */
            send_ring_ack(thread_arg, DPA_MSG_RING_DEL_ACK,
                          del_msg->ring.pod_id, del_msg->generation,
                          DPA_RING_ACK_OK);
            break;
        }
        case DPA_MSG_WAKE:
            break;
        default:
            DOCA_DPA_DEV_LOG_INFO("Unknown msg type received from host: %d\n", msg->type);
            break;
    }
}

/* Returns the number of consumer-completion messages drained this call (WAKE,
 * RING_ADD, RING_DEL). The pre-park re-scan in run_dma_manager uses this:
 * a nonzero return after arming means a signal landed in the arm→reschedule
 * race window, so the EU must NOT park (it would do so with a consumed
 * one-shot notification → lost wakeup). */
static uint32_t handle_msgs(struct dpa_thread_arg *thread_arg)
{
    doca_dpa_dev_comch_consumer_completion_element_t completion;
    struct comch_msg *msg;
    uint32_t msg_size;
    doca_dpa_dev_comch_consumer_t consumer = thread_arg->dpa_consumer;
    doca_dpa_dev_comch_consumer_completion_t consumer_comp = thread_arg->dpa_consumer_comp;
    uint32_t num_msgs = 0;

    while (doca_dpa_dev_comch_consumer_get_completion(consumer_comp, &completion) != 0) {
        msg = (struct comch_msg *)doca_dpa_dev_comch_consumer_get_completion_imm(completion, &msg_size);
        if (msg == NULL)
            continue;
        handle_dpu_msg(thread_arg, msg);
        num_msgs++;
    }

    if (num_msgs != 0) {
        doca_dpa_dev_comch_consumer_completion_ack(consumer_comp, num_msgs);
        doca_dpa_dev_comch_consumer_ack(consumer, num_msgs);
    }

    return num_msgs;
}

/*
 * Lazy drain: no per-op inflight tracking. The SDK + producer completion queue
 * absorb in-flight ops; we only ack drained completions periodically so the
 * queue does not fill.
 */
static uint32_t drain_producer_completions(struct dpa_thread_arg *thread_arg)
{
    doca_dpa_dev_completion_t producer_comp = thread_arg->dpa_producer_comp;
    doca_dpa_dev_completion_element_t elem;
    uint32_t count = 0;

    while (doca_dpa_dev_get_completion(producer_comp, &elem) != 0) {
        count++;
    }

    if (count > 0)
        doca_dpa_dev_completion_ack(producer_comp, count);
    return count;
}

static struct dma_ring_ctrl *fwd_ring_ctrl(const struct dpa_ring_info *ring)
{
    doca_dpa_dev_buf_t buf =
        doca_dpa_dev_buf_array_get_buf(ring->buf_arr,
                                      DMA_RING_CTRL_SLOT(ring->buf_arr_size));
    return (struct dma_ring_ctrl *)doca_dpa_dev_buf_get_external_ptr(buf);
}

/* Drain one fair-share quantum from ring r. A descriptor is visible only when
 * its generation matches the next consumer ticket. */
static int process_fwd_ring(struct dpa_thread_arg *thread_arg, uint32_t r)
{
    doca_dpa_dev_comch_producer_t producer = thread_arg->dpa_producer;
    uint32_t dpu_consumer_id = thread_arg->dpu_consumer_id;
    struct dpa_ring_info *ring = &thread_arg->rings[r];
    struct comch_dma_comp_msg comp;
    int total_chunks = 0;
    uint32_t desc_idx =
        (uint32_t)(thread_arg->consumer_head[r] % ring->buf_arr_size);

    for (int b = 0; b < RING_BATCH_CAP; b++) {
        doca_dpa_dev_buf_t buf =
            doca_dpa_dev_buf_array_get_buf(ring->buf_arr, desc_idx);
        volatile struct dma_desc *desc =
            (volatile struct dma_desc *)doca_dpa_dev_buf_get_external_ptr(buf);

        if (desc->publish_seq != thread_arg->consumer_head[r] + 1)
            break;

        /* Host enforces desc->size <= slot_size (= DPA_DMA_COPY_MAX = 8KB) in
         * dpumesh_enqueue. Anything larger is a caller bug; drop. */
        if (desc->size > DPA_DMA_COPY_MAX) {
            DOCA_DPA_DEV_LOG_INFO("FWD: desc size %u > %u (slot cap); dropping ring=%u slot=%u\n",
                                  desc->size, DPA_DMA_COPY_MAX, r, desc_idx);
            thread_arg->consumer_head[r]++;
            if (++desc_idx == ring->buf_arr_size)
                desc_idx = 0;
            total_chunks += 1;
            continue;
        }

        /* Leave the descriptor in place when the DPU consumer is unavailable. */
        int stalled = 0;
        {
            uint32_t wait = 0;
            while (doca_dpa_dev_comch_producer_is_consumer_empty(producer, dpu_consumer_id) == 1) {
                if (++wait >= DMA_CONSUMER_EMPTY_WAIT_LOOPS) { stalled = 1; break; }
            }
        }
        if (stalled) {
            DOCA_DPA_DEV_LOG_INFO("FWD: consumer timeout (ring=%u slot=%u). Retrying.\n",
                                  r, desc_idx);
            break;
        }

        /* dma_copy requires 128B-aligned size. A FIN carries desc->size==0
         * (comp.length stays 0 → receiver reads EOF); still issue one min 128B
         * transfer so the DMA engine never sees a zero-length descriptor. */
        uint32_t chunk = ALIGN_UP_128(desc->size);
        if (chunk == 0) chunk = DMA_COPY_SIZE_ALIGN;

        /* Mirror each connection's host TX offset into contiguous DPU staging.
         * The host ring bounds occupancy, and staging tail slack covers DMA size
         * alignment. */
        uint32_t moff = (uint32_t)(desc->addr - ring->host_addr);
        uint64_t staging_base = ring->dpu_addr - (uint64_t)ring->region_off;

        comp.type = DPA_MSG_FWD_DONE;
        comp.pos = moff;                         /* staging offset == host TX offset */
        comp.length = (uint16_t)desc->size;
        /* Endpoint tuple — opaque passthrough from the host-posted desc. src_service
         * is NOT carried; the DPU derives it from src_pod. */
        comp.seq = desc->seq;
        comp.src_port = desc->src_port;
        comp.dst_port = desc->dst_port;
        comp.dst_service = desc->dst_service;
        comp.src_pod_id = ring->pod_id;          /* forward: sender = this ring's pod */
        comp.dst_pod_id = desc->dst_pod_id;      /* may be DMESH_POD_BLANK → DPU resolves */
        comp.generation = thread_arg->ring_generation[r];

        doca_dpa_dev_comch_producer_dma_copy(producer,
                                    dpu_consumer_id,
                                    ring->dpu_mmap,
                                    staging_base + moff,
                                    ring->host_mmap,
                                    desc->addr,
                                    chunk,
                                    (uint8_t *)&comp,
                                    sizeof(struct comch_dma_comp_msg),
                                    DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH |
                                    DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);
        /* no pos[r] advance — the staging offset is host-driven (mirror) */

        thread_arg->consumer_head[r]++;
        if (++desc_idx == ring->buf_arr_size)
            desc_idx = 0;
        total_chunks += 1;
    }

    return total_chunks;
}

/* Drain each ring in round-robin quanta. */
#define HANDLE_MSGS_EVERY 32
#define DRAIN_COMPLETIONS_EVERY 8

/* poll_msgs is clear for the pre-park rescan: its caller drains and counts
 * consumer completions explicitly so a racing one-shot wake is not lost. */
static int drain_all_rings(struct dpa_thread_arg *thread_arg, int poll_msgs)
{
    int total_dma_calls = 0;
    uint32_t iter = 0;

    for (;;) {
        int found = 0;

        if (poll_msgs && (iter & (HANDLE_MSGS_EVERY - 1)) == 0)
            handle_msgs(thread_arg);
        if ((iter & (DRAIN_COMPLETIONS_EVERY - 1)) == 0)
            drain_producer_completions(thread_arg);
        iter++;
        __dpa_thread_window_read_inv();

        uint32_t nr = thread_arg->num_rings;
        for (uint32_t r = 0; r < nr; r++) {
            int chunks = process_fwd_ring(thread_arg, r);
            if (chunks > 0) {
                struct dma_ring_ctrl *ctrl =
                    fwd_ring_ctrl(&thread_arg->rings[r]);
                ctrl->consumer_head = thread_arg->consumer_head[r];
                total_dma_calls += chunks;
                found++;
            }
        }
        if (found == 0)
            break;
        __dpa_thread_window_writeback();
    }

    return total_dma_calls;
}

/* Consecutive empty drains before the EU parks for the FlexIO watchdog. */
#define IDLE_SPINS_BEFORE_PARK  262144u

__dpa_global__ void run_dma_manager(uint64_t arg)
{
    struct dpa_thread_arg *thread_arg = (struct dpa_thread_arg *)arg;
    uint32_t idle_spins = 0;

    /* rings[0] was installed synchronously by ARM h2d_memcpy before thread_run.
     * ACK from inside the EU proves that the thread began executing that exact
     * incarnation; ARM does not publish POD_INIT_READY before receiving it. */
    if (thread_arg->num_rings > 0 && thread_arg->initial_ack_sent == 0) {
        /* Record success in persistent device memory. If no ARM receive is
         * available, leave it clear so a later reschedule retries; once posted,
         * repeated reschedules must not flood the ARM consumer. */
        if (send_ring_ack(thread_arg, DPA_MSG_RING_ADD_ACK,
                          thread_arg->rings[0].pod_id,
                          thread_arg->initial_generation, DPA_RING_ACK_OK))
            thread_arg->initial_ack_sent = 1;
    }

    /* Poll through the idle grace window. Before parking, arm both completion
     * contexts and rescan the rings and consumer completions. Detected work
     * restarts the loop. */
    while (1) {
        handle_msgs(thread_arg);
        int chunks = drain_all_rings(thread_arg, 1);
        drain_producer_completions(thread_arg);
        if (chunks > 0) {
            idle_spins = 0;                       /* work found → keep polling HOT */
        } else if (++idle_spins >= IDLE_SPINS_BEFORE_PARK) {
            doca_dpa_dev_comch_consumer_completion_request_notification(thread_arg->dpa_consumer_comp);
            doca_dpa_dev_completion_request_notification(thread_arg->dpa_producer_comp);
            /* Re-scan after arming. handle_msgs() drains and counts the consumer
             * queue so a control message in the arm window prevents parking. */
            uint32_t msgs = handle_msgs(thread_arg);
            uint32_t producer_comps = drain_producer_completions(thread_arg);
            int rescan = drain_all_rings(thread_arg, 0);
            if (msgs == 0 && producer_comps == 0 && rescan == 0)
                doca_dpa_dev_thread_reschedule();   /* park; woken by WAKE completion */
            idle_spins = 0;                       /* reset after wake */
        }
        /* else: empty but within grace window → loop again (keep polling) */
    }
}
