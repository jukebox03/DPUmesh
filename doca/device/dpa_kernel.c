#include "doca_dpa_dev.h"
#include "doca_dpa_dev_comch_msgq.h"
#include "doca_dpa_dev_buf.h"
#include "dpaintrin.h"
#include "dpa_common.h"

/* Max DMA size for doca_dpa_dev_comch_producer_dma_copy.
 * HW supports up to 8KB per single call with 128B-aligned addresses. */
/* The DPU staging slot is DPUMESH_SLOT_SIZE bytes; each FORWARD dma_copy (host →
 * DPU staging) is capped at DPA_DMA_COPY_MAX. If the cap exceeded the canonical slot
 * size a copy could overflow the staging slot, so couple them at compile time.
 * (The DPA is forward-only — DPU→host egress is the ARM SG-DMA engine, dpu_proxy.c —
 * so no reverse DMA is involved here.) */
_Static_assert(DPA_DMA_COPY_MAX <= DPUMESH_SLOT_SIZE,
               "DPA_DMA_COPY_MAX must not exceed DPUMESH_SLOT_SIZE");

/* Max consecutive descriptors drained from one ring per process_fwd_ring call.
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
    doca_dpa_dev_comch_consumer_ack(consumer, num_msg);

    return 0;
}

/* A ring ACK is a control barrier, not a data completion, and it needs a posted
 * ARM receive to land. The first attempt for a message waits briefly; the retry
 * from the park path probes once, so a starved DPU channel never holds the EU.
 * A dropped ACK is re-driven by the ARM control thread, which resends
 * RING_ADD/RING_DEL to unacked EUs every 10 ms. */
#define RING_ACK_CONSUMER_WAIT_LOOPS 100000u
#define RING_ACK_PROBE_ONLY          1u
static int send_ring_ack(struct dpa_thread_arg *thread_arg, uint8_t type,
                         int32_t pod_id, uint32_t generation, uint8_t status,
                         uint32_t wait_loops)
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
           ++wait < wait_loops) {
    }
    if (wait >= wait_loops)
        return 0;
    doca_dpa_dev_comch_producer_post_send_imm_only(
        thread_arg->dpa_producer, thread_arg->dpu_consumer_id,
        (const uint8_t *)&ack, sizeof(ack), DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
    thread_arg->producer_deferred = 0;
    return 1;
}

/* Record a DEL ACK the DPU channel had no receive for. One entry per ring this
 * EU can hold, so a burst of teardowns cannot displace an earlier fence. */
static void pending_del_push(struct dpa_thread_arg *thread_arg, int32_t pod_id,
                             uint32_t generation)
{
    for (uint32_t i = 0; i < thread_arg->pending_del_n; i++) {
        if (thread_arg->pending_del_pod[i] == pod_id) {
            thread_arg->pending_del_generation[i] = generation;
            return;
        }
    }
    if (thread_arg->pending_del_n >= MAX_DPA_RINGS)
        return;                  /* ARM re-drives this RING_DEL in 10 ms */
    uint32_t slot = thread_arg->pending_del_n;
    thread_arg->pending_del_pod[slot] = pod_id;
    thread_arg->pending_del_generation[slot] = generation;
    thread_arg->pending_del_n = slot + 1;
}

/* Forget a fence that has landed, so a later probe cannot resend it. */
static void pending_del_drop(struct dpa_thread_arg *thread_arg, int32_t pod_id)
{
    for (uint32_t i = 0; i < thread_arg->pending_del_n; i++) {
        if (thread_arg->pending_del_pod[i] != pod_id)
            continue;
        uint32_t last = thread_arg->pending_del_n - 1;
        thread_arg->pending_del_pod[i] = thread_arg->pending_del_pod[last];
        thread_arg->pending_del_generation[i] =
            thread_arg->pending_del_generation[last];
        thread_arg->pending_del_n = last;
        return;
    }
}

/* One nonblocking probe per outstanding fence, compacting the ones that land.
 * Called only where the EU is about to be released anyway, so a starved channel
 * costs no forwarding. */
static void pending_del_retry(struct dpa_thread_arg *thread_arg)
{
    uint32_t kept = 0;

    for (uint32_t i = 0; i < thread_arg->pending_del_n; i++) {
        if (send_ring_ack(thread_arg, DPA_MSG_RING_DEL_ACK,
                          thread_arg->pending_del_pod[i],
                          thread_arg->pending_del_generation[i],
                          DPA_RING_ACK_OK, RING_ACK_PROBE_ONLY))
            continue;
        thread_arg->pending_del_pod[kept] = thread_arg->pending_del_pod[i];
        thread_arg->pending_del_generation[kept] =
            thread_arg->pending_del_generation[i];
        kept++;
    }
    thread_arg->pending_del_n = kept;
}

static void handle_dpu_msg(struct dpa_thread_arg *thread_arg, const struct comch_msg *msg)
{
    switch(msg->type) {
        case DPA_MSG_RING_ADD: {
            struct comch_add_ring_msg *add_msg = (struct comch_add_ring_msg *)msg;
            /* A pod holds at most one ring per EU, so a same-pod_id entry is a
             * stale duplicate and is overwritten in place. */
            uint32_t slot = thread_arg->num_rings;
            for (uint32_t r = 0; r < thread_arg->num_rings; r++) {
                if (thread_arg->rings[r].pod_id == add_msg->ring.pod_id) {
                    slot = r;
                    break;
                }
            }
            if (slot < MAX_DPA_RINGS) {
                thread_arg->rings[slot] = add_msg->ring;
                thread_arg->ring_generation[slot] = add_msg->generation;
                thread_arg->consumer_head[slot] = 0;
                if (slot == thread_arg->num_rings)
                    thread_arg->num_rings++;
                send_ring_ack(thread_arg, DPA_MSG_RING_ADD_ACK,
                              add_msg->ring.pod_id, add_msg->generation,
                              DPA_RING_ACK_OK, RING_ACK_CONSUMER_WAIT_LOOPS);
            } else {
                send_ring_ack(thread_arg, DPA_MSG_RING_ADD_ACK,
                              add_msg->ring.pod_id, add_msg->generation,
                              DPA_RING_ACK_FULL, RING_ACK_CONSUMER_WAIT_LOOPS);
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
                break;
            }
            /* This FLUSHED send is on the same ordered producer as every forward
             * DMA. Once ARM receives it, every older DMA WQE that could name the
             * removed ring's mmap/buf_arr has completed. DEL is idempotent: a
             * retry for an already absent pod still gets an ACK. A consumer with
             * no posted receive is retained as a fence and retried before this
             * EU parks, because ARM holds the slot and its mappings until the
             * ACK lands. */
            if (send_ring_ack(thread_arg, DPA_MSG_RING_DEL_ACK,
                              del_msg->ring.pod_id, del_msg->generation,
                              DPA_RING_ACK_OK, RING_ACK_CONSUMER_WAIT_LOOPS))
                pending_del_drop(thread_arg, del_msg->ring.pod_id);
            else
                pending_del_push(thread_arg, del_msg->ring.pod_id,
                                 del_msg->generation);
            break;
        }
        default:
            break;
    }
}

/* Returns the number of consumer-completion messages drained this call (WAKE,
 * RING_ADD, RING_DEL). The pre-park re-scan in park_or_yield uses this:
 * a nonzero return after arming means a signal landed in the arm→reschedule
 * race window, so the EU must NOT park (it would do so with a consumed
 * one-shot notification → lost wakeup). */
static uint32_t handle_msgs(struct dpa_thread_arg *thread_arg)
{
    doca_dpa_dev_comch_consumer_completion_element_t completion;
    struct comch_msg *msg;
    uint32_t msg_size;
    doca_dpa_dev_comch_consumer_t consumer = thread_arg->dpa_consumer;
    doca_dpa_dev_comch_consumer_completion_t consumer_comp =
        thread_arg->dpa_consumer_comp;
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

    if (count > 0) {
        doca_dpa_dev_completion_ack(producer_comp, count);
        doca_dpa_dev_completion_request_notification(producer_comp);
    }
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
static int process_fwd_ring(struct dpa_thread_arg *thread_arg, uint32_t r,
                            uint32_t budget)
{
    doca_dpa_dev_comch_producer_t producer = thread_arg->dpa_producer;
    uint32_t dpu_consumer_id = thread_arg->dpu_consumer_id;
    struct dpa_ring_info *ring = &thread_arg->rings[r];
    struct comch_dma_comp_msg comp;
    int total_chunks = 0;
    uint32_t desc_idx =
        (uint32_t)(thread_arg->consumer_head[r] % ring->buf_arr_size);

    for (uint32_t b = 0; b < RING_BATCH_CAP && b < budget; b++) {
        doca_dpa_dev_buf_t buf =
            doca_dpa_dev_buf_array_get_buf(ring->buf_arr, desc_idx);
        volatile struct dma_desc *desc =
            (volatile struct dma_desc *)doca_dpa_dev_buf_get_external_ptr(buf);

        if (desc->publish_seq != thread_arg->consumer_head[r] + 1)
            break;

        /* One read each of the fields the host process publishes, so every
         * bound below is checked against the same value the DMA then uses. */
        uint64_t host_off = desc->addr;
        uint32_t dma_size = desc->size;

        /* Host enforces size <= slot_size (= DPA_DMA_COPY_MAX = 8KB) in
         * dpumesh_enqueue. Anything larger is a caller bug; drop. */
        if (dma_size > DPA_DMA_COPY_MAX) {
            thread_arg->consumer_head[r]++;
            if (++desc_idx == ring->buf_arr_size)
                desc_idx = 0;
            total_chunks += 1;
            continue;
        }

        /* Leave the descriptor in place when the ARM consumer has no receive
         * credit and return to the outer loop, which releases the EU within the
         * watchdog window. The next poll pass retries the descriptor. */
        if (doca_dpa_dev_comch_producer_is_consumer_empty(
                producer, dpu_consumer_id) == 1)
            break;

        /* dma_copy requires 128B-aligned size. A FIN carries size==0
         * (comp.length stays 0 → receiver reads EOF); still issue one min 128B
         * transfer so the DMA engine never sees a zero-length descriptor. */
        /* desc->addr is a byte offset into the host TX buffer, mirrored into
         * contiguous DPU staging. The producer is the workload process, which
         * owns no DOCA object, so the aligned span is bounded against the
         * registered host buffer before it names either endpoint. */
        uint32_t moff = (uint32_t)host_off;
        uint64_t staging_base = ring->dpu_addr - (uint64_t)ring->region_off;
        uint32_t prefix = dpa_dma_copy_prefix(moff);
        uint32_t chunk = dpa_dma_aligned_copy_len(moff, dma_size);
        if (chunk == 0)
            chunk = DPA_DMA_COPY_ALIGN;
        if (chunk > DPA_DMA_COPY_MAX ||
            host_off >= (uint64_t)ring->host_buf_size ||
            (uint64_t)(moff - prefix) + chunk >
                (uint64_t)ring->host_buf_size) {
            thread_arg->consumer_head[r]++;
            if (++desc_idx == ring->buf_arr_size)
                desc_idx = 0;
            total_chunks += 1;
            continue;
        }

        comp.type = DPA_MSG_FWD_DONE;
        comp.pos = moff;                         /* staging offset == host TX offset */
        comp.length = (uint16_t)dma_size;
        /* Endpoint tuple — opaque passthrough from the host-posted desc. src_service
         * is NOT carried; the DPU derives it from src_pod. */
        comp.seq = desc->seq;
        comp.src_port = desc->src_port;
        comp.dst_port = desc->dst_port;
        comp.dst_service = desc->dst_service;
        comp.src_pod_id = ring->pod_id;          /* forward: sender = this ring's pod */
        comp.dst_pod_id = desc->dst_pod_id;      /* may be DMESH_POD_BLANK → DPU resolves */
        comp.generation = thread_arg->ring_generation[r];

        uint32_t submit_flags = DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH;
        if (!dpa_producer_report_due(&thread_arg->producer_deferred))
            submit_flags |= DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS;

        doca_dpa_dev_comch_producer_dma_copy(producer,
                                    dpu_consumer_id,
                                    ring->dpu_mmap,
                                    staging_base + moff - prefix,
                                    ring->host_mmap,
                                    ring->host_addr + (uint64_t)(moff - prefix),
                                    chunk,
                                    (uint8_t *)&comp,
                                    sizeof(struct comch_dma_comp_msg),
                                    submit_flags);

        thread_arg->consumer_head[r]++;
        if (++desc_idx == ring->buf_arr_size)
            desc_idx = 0;
        total_chunks += 1;
    }

    return total_chunks;
}

/* Drain each ring in round-robin quanta. */
#define HANDLE_MSGS_EVERY 32

static int drain_all_rings(struct dpa_thread_arg *thread_arg, uint32_t budget)
{
    int total_dma_calls = 0;
    uint32_t iter = 0;

    for (;;) {
        int found = 0;

        if ((iter & (HANDLE_MSGS_EVERY - 1)) == 0)
            handle_msgs(thread_arg);
        drain_producer_completions(thread_arg);
        iter++;
        __dpa_thread_window_read_inv();

        uint32_t nr = thread_arg->num_rings;
        for (uint32_t r = 0; r < nr; r++) {
            uint32_t remaining = budget - (uint32_t)total_dma_calls;
            if (remaining == 0)
                break;
            int chunks = process_fwd_ring(thread_arg, r, remaining);
            if (chunks > 0) {
                struct dma_ring_ctrl *ctrl =
                    fwd_ring_ctrl(&thread_arg->rings[r]);
                ctrl->consumer_head = thread_arg->consumer_head[r];
                total_dma_calls += chunks;
                found++;
            }
        }
        if ((uint32_t)total_dma_calls >= budget) {
            __dpa_thread_window_writeback();
            break;
        }
        if (found == 0)
            break;
        __dpa_thread_window_writeback();
    }

    return total_dma_calls;
}

/* Consecutive empty drains before the thread releases its EU. */
#define IDLE_SPINS_BEFORE_PARK  262144u

/* This helper and its data thread have the same fixed EU affinity, so the DPA
 * scheduler cannot run it until the data thread has released that EU. Its
 * notification therefore always reaches an already-parked data thread. It runs
 * only when notified: doca_dpa_thread_run makes a thread activatable, it does
 * not execute the kernel. */
__dpa_global__ void run_dma_yield_helper(uint64_t resume_notification)
{
    doca_dpa_dev_thread_notify(resume_notification);
    doca_dpa_dev_thread_reschedule();
}

/* The one place this thread releases its EU.
 *
 * doca_dpa_dev_thread_reschedule only promises that a NEW completion re-triggers
 * the thread, so anything already queued when the notifications are armed has to
 * be found by the rescan below — otherwise the thread parks on top of it and
 * waits for the next message. A thread that still owns rings or an
 * unacknowledged teardown fence never reaches that park: it hands the EU to the
 * same-affinity helper, which wakes it once the scheduler has actually taken
 * the EU away.
 */
static void park_or_yield(struct dpa_thread_arg *thread_arg)
{
    pending_del_retry(thread_arg);

    if (thread_arg->num_rings > 0 || thread_arg->pending_del_n > 0) {
        doca_dpa_dev_thread_notify(thread_arg->yield_notification);
        doca_dpa_dev_thread_reschedule();
        return;
    }

    for (;;) {
        doca_dpa_dev_comch_consumer_completion_request_notification(
            thread_arg->dpa_consumer_comp);
        doca_dpa_dev_completion_request_notification(
            thread_arg->dpa_producer_comp);
        /* Rescan after arming: a control message or completion that landed in
         * the arm window must cancel the park, not be parked on. */
        uint32_t msgs = handle_msgs(thread_arg);
        uint32_t comps = drain_producer_completions(thread_arg);
        if (thread_arg->num_rings > 0 || thread_arg->pending_del_n > 0)
            return;                       /* work arrived → resume polling */
        if (msgs == 0 && comps == 0) {
            doca_dpa_dev_thread_reschedule();
            return;
        }
    }
}

__dpa_global__ void run_dma_manager(uint64_t arg)
{
    struct dpa_thread_arg *thread_arg = (struct dpa_thread_arg *)arg;
    uint32_t idle_spins = 0;
    uint32_t completed_since_reschedule = 0;

    /* rings[0] was installed synchronously by ARM h2d_memcpy before thread_run.
     * ACK from inside the EU proves that the thread began executing that exact
     * incarnation; ARM does not publish POD_INIT_READY before receiving it. */
    if (thread_arg->num_rings > 0 && thread_arg->initial_ack_sent == 0) {
        /* Record success in persistent device memory. If no ARM receive is
         * available, leave it clear so a later reschedule retries; once posted,
         * repeated reschedules must not flood the ARM consumer. */
        if (send_ring_ack(thread_arg, DPA_MSG_RING_ADD_ACK,
                          thread_arg->rings[0].pod_id,
                          thread_arg->initial_generation, DPA_RING_ACK_OK,
                          RING_ACK_CONSUMER_WAIT_LOOPS))
            thread_arg->initial_ack_sent = 1;
    }

    /* Poll through the idle grace window; park_or_yield owns every release of
     * the EU, whether it is the watchdog handoff of a live EU or the real park
     * of a ringless one. */
    while (1) {
        handle_msgs(thread_arg);
        uint32_t budget = dpa_reschedule_budget(
            completed_since_reschedule, thread_arg->producer_deferred);
        int chunks = drain_all_rings(thread_arg, budget);
        if (chunks > 0) {
            idle_spins = 0;                       /* work found → keep polling HOT */
            if (dpa_reschedule_due(&completed_since_reschedule,
                                   (uint32_t)chunks,
                                   thread_arg->producer_deferred))
                park_or_yield(thread_arg);
            drain_producer_completions(thread_arg);
        } else if (++idle_spins >= IDLE_SPINS_BEFORE_PARK) {
            park_or_yield(thread_arg);
            idle_spins = 0;                       /* reset after wake */
        }
        /* else: empty but within grace window → loop again (keep polling) */
    }
}
