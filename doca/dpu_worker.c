#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dpu_worker.h"

#include "comch_server.h"
#include "comch_consumer.h"
#include "comch_common.h"
#include "dpa.h"
#include "dpa_common.h"
#include "comch_msgq.h"
#include "buffer.h"
#include "ring.h"
#include "dpu_proxy.h"
#include <dpumesh/dmesh_common.h>
#include <dpumesh/dmesh_topology.h>

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

DOCA_LOG_REGISTER(DPU_WORKER);

/* Set on dedicated ARM data workers. */
__thread int dpu_t_is_worker = 0;

/* ARM worker selected by the DPA receive callback. */
extern __thread int dpu_worker_id;

/* DPUMESH_DIAG emits batch state and idle-flush counters once per second. */
static int g_dpu_diag = 0;
static uint64_t g_lull_hits = 0, g_drain_iters = 0;
static int g_arm_pin_enabled = 0;
static int g_arm_allowed_n = 0;
static cpu_set_t g_arm_allowed;
/* TX_ACK batch counters. */
static uint64_t g_fl_total = 0, g_fl_n15 = 0, g_fl_n16 = 0, g_single_acks = 0;

static void
dpu_arm_affinity_init(void)
{
    const char *env = getenv("DPUMESH_ARM_PIN");
    g_arm_pin_enabled = !env || atoi(env) != 0; /* on by default; 0 opts out */
    CPU_ZERO(&g_arm_allowed);
    if (!g_arm_pin_enabled ||
        sched_getaffinity(0, sizeof(g_arm_allowed), &g_arm_allowed) != 0) {
        g_arm_pin_enabled = 0;
        return;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &g_arm_allowed))
            g_arm_allowed_n++;
    if (g_arm_allowed_n == 0)
        g_arm_pin_enabled = 0;
}

static void
dpu_arm_pin_current(const char *role, int logical_id)
{
    if (!g_arm_pin_enabled)
        return;
    int ordinal = logical_id % g_arm_allowed_n;
    int selected = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &g_arm_allowed))
            continue;
        if (ordinal-- == 0) {
            selected = cpu;
            break;
        }
    }
    if (selected < 0)
        return;
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(selected, &one);
    int err = pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
    if (err == 0)
        DOCA_LOG_INFO("ARM affinity: %s %d -> CPU %d", role, logical_id,
                      selected);
    else
        DOCA_LOG_WARN("ARM affinity failed: %s %d -> CPU %d: %s",
                      role, logical_id, selected, strerror(err));
}

static void
dpu_arm_name_current(const char *role, int logical_id)
{
    char name[16];
    if (strcmp(role, "worker") == 0)
        snprintf(name, sizeof(name), "dmesh-w%d", logical_id);
    else
        snprintf(name, sizeof(name), "dmesh-%s", role);
    int err = pthread_setname_np(pthread_self(), name);
    if (err != 0)
        DOCA_LOG_WARN("ARM thread naming failed for %s %d: %s",
                      role, logical_id, strerror(err));
}

/* ====== TX_ACK send helper ====== */

/* Try to send TX_ACK; on EAGAIN park to the deferred queue. No-op when src_pod
 * is gone (its host died with it). */
static void
send_or_defer_tx_ack(struct objects *objs, struct pod_state *src_pod,
                     uint16_t port, uint16_t seq)
{
    if (!src_pod || !src_pod->connection)
        return;

    struct dmesh_tx_ack_entry e = { .port = port, .seq = seq };
    doca_error_t r = server_send_batch_tx_ack_to(objs, src_pod->connection, &e, 1);
    g_single_acks++;
    if (r == DOCA_SUCCESS)
        return;

    if (r == DOCA_ERROR_AGAIN) {
        /* ACKs are exact, not cumulative: L7 egress can complete out of order when
         * adjacent messages choose different backend pods/engines. Keep every seq. */
        if (objs->num_deferred_tx_acks < MAX_DEFERRED_TX_ACK) {
            int n = objs->num_deferred_tx_acks++;
            objs->deferred_tx_acks[n].conn = src_pod->connection;
            objs->deferred_tx_acks[n].port = port;
            objs->deferred_tx_acks[n].seq  = seq;
        } else {
            DOCA_LOG_ERR("deferred TX_ACK queue full — dropping port=%u seq=%u (pod %d). "
                         "Host TX byte-ring bytes stall until close.",
                         port, seq, src_pod->pod_id);
        }
        return;
    }

    DOCA_LOG_WARN("TX_ACK failed for port=%u seq=%u to pod %d: %s",
                  port, seq, src_pod->pod_id, doca_error_get_descr(r));
}

/* ====== Batched TX_ACK ====== */

/* Flush a pod's accumulated TX_ACK batch as one message. On AGAIN the batch is
 * retained (retried by the next flush, including the idle proc==0 flush). */
static void
flush_txack_batch(struct objects *objs, struct pod_state *pod)
{
    if (!pod || pod->txack_batch_n == 0)
        return;
    if (!pod->connection) { pod->txack_batch_n = 0; return; }
    doca_error_t r = server_send_batch_tx_ack_to(objs, pod->connection,
                                                 pod->txack_batch, pod->txack_batch_n);
    if (r != DOCA_ERROR_AGAIN) {
        g_fl_total++;
        if (pod->txack_batch_n == 15) g_fl_n15++;
        else if (pod->txack_batch_n >= 16) g_fl_n16++;
        pod->txack_batch_n = 0;   /* sent (a hard error drops the batch) */
    }
}

/* Add a TX_ACK to the source pod's batch. A full pending batch uses the
 * single-send path. The proxy also calls this function to release custody. */
void
batch_or_send_tx_ack(struct objects *objs, struct pod_state *src_pod,
                     uint16_t port, uint16_t seq)
{
    /* Dedicated workers publish TX_ACKs to main through SPSC rings. */
    if (dpu_t_is_worker) {
        if (!src_pod) return;   /* target gone → nothing to ack */
        int worker = dpu_worker_id;
        if (worker < 0 || worker >= objs->n_data_workers)
            worker = 0;
        struct dpu_data_worker *worker_state = &objs->data_workers[worker];
        uint32_t tail = atomic_load_explicit(&worker_state->pending_txack_tail,
                                             memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&worker_state->pending_txack_head,
                                             memory_order_acquire);
        uint32_t next = (tail + 1u) & (DPU_PENDING_TXACK_SIZE - 1u);
        if (next == head) {
            static uint64_t pend_full;
            if ((pend_full++ & 0xFFFFu) == 0)
                DOCA_LOG_ERR("worker %d pending_txack full (total %llu) — "
                             "main not draining; port=%u",
                             worker, (unsigned long long)pend_full, port);
            return;
        }
        worker_state->pending_txack[tail] = (struct dpu_pending_txack_entry) {
            .pod_id = src_pod->pod_id,
            .port = port,
            .seq = seq,
        };
        atomic_store_explicit(&worker_state->pending_txack_tail, next,
                              memory_order_release);
        dpu_wake_main(objs);
        return;
    }

    if (!src_pod || !src_pod->connection) {
        send_or_defer_tx_ack(objs, src_pod, port, seq);
        return;
    }
    if (src_pod->txack_batch_n >= BATCH_TXACK_MAX) {
        /* Batch full and not yet drained (send pool busy) — single-send this one
         * so no ack is lost; the full batch is retried by the idle proc==0 flush. */
        send_or_defer_tx_ack(objs, src_pod, port, seq);
        return;
    }
    src_pod->txack_batch[src_pod->txack_batch_n].port = port;
    src_pod->txack_batch[src_pod->txack_batch_n].seq  = seq;
    src_pod->txack_batch_n++;
    if (src_pod->txack_batch_n >= BATCH_TXACK_MAX)
        flush_txack_batch(objs, src_pod);
}

/* Batched REV_DONE mirrors batched TX_ACK. */

/* Flush a pod's accumulated REV_DONE batch as one message. On AGAIN the batch is
 * retained (retried by the next flush, including the idle proc==0 flush).
 * Non-static: the proxy engine (dpu_proxy.c) shares this accumulator. */
void
flush_rev_done_batch(struct objects *objs, struct pod_state *pod)
{
    if (!pod || pod->rev_done_batch_n == 0)
        return;
    if (!pod->connection) { pod->rev_done_batch_n = 0; return; }
    doca_error_t r = server_send_batch_rev_done_to(objs, pod->connection,
                                                   pod->rev_done_batch, pod->rev_done_batch_n);
    if (r == DOCA_ERROR_AGAIN)
        return;  /* retain; retried */
    pod->rev_done_batch_n = 0;   /* sent (a hard error drops the batch) */
}

/* L4 routing selects a live, ready backend from pods[]. Connection affinity and
 * L7 host overrides are applied by the caller. */
int
collect_live_hosts(struct objects *objs, int16_t svc, int32_t *out)
{
    int n = 0;
    if (svc < 0 || svc >= POD_ID_SPACE)
        return 0;
    int np = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < np && n < MAX_PODS; i++) {
        struct pod_state *p = &objs->pods[i];
        /* registered (ACQUIRE) publishes service_id; dma_ready gates the data plane
         * — an un-ready backend would only drop, so exclude it from the LB set. */
        if (!__atomic_load_n(&p->registered, __ATOMIC_ACQUIRE))
            continue;
        if (p->service_id != svc)
            continue;
        if (!pod_data_ready(p))
            continue;
        out[n++] = p->pod_id;
    }
    return n;
}

/* Per-service round robin with a relaxed atomic cursor. */
static inline int32_t
lb_pick(struct objects *objs, int16_t svc)
{
    int32_t hosts[MAX_PODS];
    int n = collect_live_hosts(objs, svc, hosts);
    if (n <= 0)
        return -1;
    /* ARM workers share the per-service round-robin cursor. */
    uint32_t i = __atomic_fetch_add(&objs->svc_rr[svc], 1, __ATOMIC_RELAXED);
    return hosts[i % (uint32_t)n];
}

/* Pick a live backend with round-robin balancing. Affinity is caller-owned;
 * unknown or empty services return -1. */
int32_t
dpu_route_l4(struct objects *objs, int16_t svc)
{
    return lb_pick(objs, svc);
}

/* ====== Deferred Completion Queue Drain ====== */

/*
 * Drain the deferred TX_ACK queue. Called every main-loop iteration after
 * doca_pe_progress(pe), which releases comch send-pool slots as send
 * completions fire. Returns the number of ACKs successfully sent; the rest
 * stay in the queue for the next iteration.
 */
static int
drain_deferred_tx_acks(struct objects *objs)
{
    if (objs->num_deferred_tx_acks == 0)
        return 0;

    int sent = 0;
    int kept = 0;
    int total = objs->num_deferred_tx_acks;
    for (int i = 0; i < total; i++) {
        deferred_tx_ack_t *d = &objs->deferred_tx_acks[i];
        struct dmesh_tx_ack_entry e = { .port = d->port, .seq = d->seq };
        doca_error_t rc = server_send_batch_tx_ack_to(objs, d->conn, &e, 1);
        if (rc == DOCA_SUCCESS) {
            sent++;
            continue;
        }
        if (rc == DOCA_ERROR_AGAIN) {
            /* Pool still full — keep entry for next iteration. Compact in
             * place so retained entries stay contiguous + FIFO ordered. */
            if (kept != i)
                objs->deferred_tx_acks[kept] = *d;
            kept++;
            continue;
        }
        /* Hard error — log and drop (the host's TX byte-ring bytes stay
         * unreclaimed until that conn closes; only these once-in-a-blue-moon
         * hard failures ever leak one). */
        DOCA_LOG_WARN("deferred TX_ACK fatal for port=%u seq=%u: %s",
                      d->port, d->seq, doca_error_get_descr(rc));
        sent++;  /* count as "removed from queue" */
    }
    objs->num_deferred_tx_acks = kept;
    return sent;
}

/* Drain each worker's SPSC TX_ACK ring on main, the sole Comch send owner. */
static int
drain_pending_txack(struct objects *objs)
{
    int sent = 0;
    for (int s = 0; s < objs->n_data_workers && sent < 8192; s++) {
        struct dpu_data_worker *worker_state = &objs->data_workers[s];
        uint32_t head = atomic_load_explicit(&worker_state->pending_txack_head,
                                             memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&worker_state->pending_txack_tail,
                                             memory_order_acquire);
        while (head != tail && sent < 8192) {
            struct dpu_pending_txack_entry e = worker_state->pending_txack[head];
            head = (head + 1u) & (DPU_PENDING_TXACK_SIZE - 1u);
            atomic_store_explicit(&worker_state->pending_txack_head, head,
                                  memory_order_release);
            batch_or_send_tx_ack(objs, find_pod_by_id(objs, e.pod_id),
                                 e.port, e.seq);
            sent++;
            tail = atomic_load_explicit(&worker_state->pending_txack_tail,
                                        memory_order_acquire);
        }
    }
    return sent;
}

static uint32_t
pending_txack_usage(const struct dpu_data_worker *worker_state)
{
    uint32_t tail = atomic_load_explicit(&worker_state->pending_txack_tail,
                                         memory_order_acquire);
    uint32_t head = atomic_load_explicit(&worker_state->pending_txack_head,
                                         memory_order_acquire);
    return (tail - head) & (DPU_PENDING_TXACK_SIZE - 1u);
}

/* Process up to max_batch deferred completions on the main loop. Routing, egress,
 * and Comch sends run outside consumer callbacks. */
static int
process_completion_queue(struct objects *objs, int max_batch)
{
    int processed = 0;

    /* PE progress is per-batch, not per-entry: the send pool (CC_SEND_TASK_NUM)
     * and consumer recv pool (CC_DATA_PATH_TASK_NUM) are both ≫ max_batch, so a
     * batch (≤128 entries × 2 sends) cannot exhaust either pool mid-batch. */
    while (processed < max_batch) {
        dpu_comp_entry_t *entry = comp_queue_peek(&objs->comp_queue);
        if (!entry)
            break;

        /* Worker 0 owns the A=1 connection and routing state. */
        int result = px_process_forward(objs, 0, entry);
        if (result == 0)
            break;  /* engine backpressure, retry next iteration */

        comp_queue_dequeue(&objs->comp_queue);
        processed++;
    }

    /* Resume conns the egress backpressured (egress allocation left
     * their bytes in the window). Runs even when no completion arrived — a stalled conn
     * is waiting on an egress unit, not on new input. Counts as processed only if it
     * really advanced, so an unrelieved pool still lets the loop park. */
    processed += px_drain_stalled(objs, 0);

    return processed;
}

/* Select the connection owner. */
static inline int
owner_worker(struct objects *objs, int here, const dpu_comp_entry_t *e)
{
    if (objs->worker_shared_routing || objs->n_data_workers <= 1)
        return here;
    return dmesh_worker_for_port(e->src_port, objs->n_data_workers);
}

/* Wake a parked worker after a cross-worker handoff. */
static void
dpu_wake_worker(struct dpu_data_worker *worker_state)
{
    if (worker_state->wake_fd < 0)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &worker_state->parked, &expected, 0,
            memory_order_acq_rel, memory_order_acquire))
        return;
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(worker_state->wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    (void)n;
}

/* Hand a reply to its owner worker. Returns -1 when the inbox is full. */
static int
cross_worker_handoff(struct objects *objs, int owner, const dpu_comp_entry_t *e)
{
    struct dpu_data_worker *dst = &objs->data_workers[owner];
    return mpsc_comp_queue_enqueue(&dst->cross_worker, e);
}

/* ====== DPU Worker ====== */

/* Progress the A=1 consumer PE and its deferred receive tasks. */
static int
dpu_progress_data_pe(struct objects *objs)
{
    int did_consumer = doca_pe_progress(objs->consumer_pe);

    /* Resubmit receives below the completion-queue low watermark. */
    if (objs->num_deferred_recv > 0 &&
        completion_usage(objs) < COMP_QUEUE_BP_LOW) {
        int remaining = 0, resubmitted = 0, original = objs->num_deferred_recv;
        for (int i = 0; i < original; i++) {
            struct doca_task *t = objs->deferred_recv[i];
            doca_error_t rs = doca_task_submit(t);
            if (rs == DOCA_SUCCESS) {
                resubmitted++;
            } else {
                objs->deferred_recv[remaining++] = t;
                DOCA_LOG_WARN("Deferred recv resubmit failed: %s; retaining",
                              doca_error_get_descr(rs));
            }
        }
        objs->num_deferred_recv = remaining;
        if (resubmitted > 0)
            DOCA_LOG_INFO("Backpressure release: resubmitted %d/%d deferred recv tasks (retained %d)",
                          resubmitted, original, remaining);
    }

    /* Drain consumer_retry tasks stashed by the consumer completion callback. */
    objects_drain_consumer_retry(objs);

    return did_consumer;
}

/* Wake the main loop once per park interval. */
void
dpu_wake_main(struct objects *objs)
{
    if (objs->main_wake_fd < 0)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &objs->main_parked, &expected, 0,
            memory_order_acq_rel, memory_order_acquire))
        return;
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(objs->main_wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    (void)n;
}

/* Publish pod readiness after all EU setup acknowledgements arrive. */
static int
dpu_finalize_pending_pod_inits(struct objects *objs)
{
    int finalized = 0;
    int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct pod_state *pod = &objs->pods[i];
        if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE) ||
            pod->init_result != DMESH_POD_INIT_PENDING || pod_data_ready(pod) ||
            !__atomic_load_n(&pod->dpa_setup_complete, __ATOMIC_ACQUIRE))
            continue;
        uint32_t expected = __atomic_load_n(&pod->dpa_add_expected_mask,
                                             __ATOMIC_ACQUIRE);
        uint32_t received = __atomic_load_n(&pod->dpa_add_ack_mask,
                                             __ATOMIC_ACQUIRE);
        int setup_progress = progress_setup_pod_dma(objs, pod);
        if (setup_progress < 0) {
            DOCA_LOG_ERR("DPA ring setup retry failed for pod %d", pod->pod_id);
            (void)server_publish_pod_init_result(objs, pod,
                                                 DMESH_POD_INIT_DPA_FAILED);
            finalized++;
            continue;
        }
        received = __atomic_load_n(&pod->dpa_add_ack_mask,
                                    __ATOMIC_ACQUIRE);
        if ((received & expected) != expected)
            continue;
        if (__atomic_load_n(&pod->dpa_add_ack_failed, __ATOMIC_ACQUIRE)) {
            DOCA_LOG_ERR("DPA rejected ring setup for pod %d (ack mask 0x%x)",
                         pod->pod_id, received);
            (void)server_publish_pod_init_result(objs, pod,
                                                 DMESH_POD_INIT_DPA_FAILED);
        } else {
            /* RELEASE publication: setup_complete ACQUIRE above observed all
             * data-plane fields, and dma_ready is the reader-side gate. */
            __atomic_store_n(&pod->dma_ready, 1, __ATOMIC_RELEASE);
            DOCA_LOG_INFO("DPU pod is data-ready: pod_id=%d generation=%u EU mask=0x%x",
                          pod->pod_id,
                          __atomic_load_n(&pod->dma_generation, __ATOMIC_RELAXED),
                          expected);
            (void)server_publish_pod_init_result(objs, pod,
                                                 DMESH_POD_INIT_READY);
        }
        finalized++;
    }
    return finalized;
}

/* Progress the main-owned control, emission, and A=1 data paths. */
static int
dpu_drain_iteration(struct objects *objs)
{
    /* A=1 progresses data inline; A>=2 leaves data progress to ARM workers. */
    uint8_t did_consumer = objs->dedicated_workers ? 0 : (uint8_t)dpu_progress_data_pe(objs);
    uint8_t did_ctrl     = doca_pe_progress(objs->pe);  /* new conns, REGISTER, TX_DATA */
    int finalized_init   = dpu_finalize_pending_pod_inits(objs);
    int sent_init_result = server_flush_pod_init_results(objs);

    /* Retry deferred TX_ACKs right after pe_progress so just-released send-pool
     * slots are available. */
    drain_deferred_tx_acks(objs);

    /* Emit worker-produced TX_ACKs from the main Comch PE. */
    int sent_pend = objs->dedicated_workers ? drain_pending_txack(objs) : 0;

    /* A=1 processes its completion queue on main. */
    int proc = objs->dedicated_workers ? 0 : process_completion_queue(objs, 128);

    /* SG-DMA egress: submit queued per-dst batches + emit completed batches'
     * REV_DONE entries and custody TX_ACKs (the unified DPU→host reverse path). */
    int px_progressed = px_drain(objs);

    /* POD_UNREGISTER cleanup is deliberately after px_drain: this pass first
     * retires/drops dead destination lanes, then the cleanup state machine may
     * combine ARM quiescence with DPA DEL_ACKs and destroy imported mappings. */
    int cleaned_pods = server_progress_pod_cleanup(objs);

    /* Flush partial batches when the active progress path is idle. */
    int lull = objs->dedicated_workers ? (!px_progressed && sent_pend == 0) : (proc == 0);
    if (lull)
        for (int i = 0; i < objs->num_pods; i++) {
            flush_txack_batch(objs, &objs->pods[i]);
            flush_rev_done_batch(objs, &objs->pods[i]);
        }

    if (g_dpu_diag) { g_drain_iters++; if (lull) g_lull_hits++; }
    return (did_consumer || did_ctrl || proc > 0 || px_progressed ||
            cleaned_pods > 0 || sent_pend > 0 || finalized_init > 0 ||
            sent_init_result > 0);
}

/* DIAG dump (once/sec): per-pod TX_ACK/REV_DONE batch fill + pending_txack + how often
 * the idle-flush fired. A pod stuck with tx>0/rev>0 while lull=0 == starvation (batch
 * below threshold + no idle-flush). Worker queue depths show forward backlog.
 * QUIET AT IDLE: skipped while nothing moved since the last dump (no queued/batched
 * work and all flush counters frozen) — the line before silence carries the final
 * counters, so a fully-idle hang still leaves its state in the log without the
 * 1 Hz heartbeat flooding it. Any activity resumes the once/sec cadence. */
static void
dpu_diag_dump(struct objects *objs)
{
    static uint64_t prev_fl, prev_sgl, prev_stalls;
    static int prev_pend = -1;   /* -1 → always print the first dump */
    uint32_t pending = 0;
    int busy = 0;
    for (int s = 0; s < objs->n_data_workers; s++) {
        pending += pending_txack_usage(&objs->data_workers[s]);
        busy |= (comp_queue_usage(&objs->data_workers[s].queue) != 0);
    }
    busy |= pending != 0;
    {
        int np0 = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
        for (int i = 0; i < np0 && !busy; i++)
            busy |= (objs->pods[i].txack_batch_n != 0 ||
                     objs->pods[i].rev_done_batch_n != 0);
    }
    /* A conn backpressured by egress allocation is parked on a pool and moves nothing, so it
     * looks idle to every counter above. A climbing stall count keeps the dump alive. */
    uint64_t stalls = px_stall_total(objs);
    if (!busy && g_fl_total == prev_fl && g_single_acks == prev_sgl &&
        stalls == prev_stalls && prev_pend == 0) {
        g_lull_hits = 0;
        g_drain_iters = 0;
        return;                      /* idle + unchanged → stay silent */
    }
    prev_fl = g_fl_total;
    prev_sgl = g_single_acks;
    prev_stalls = stalls;
    prev_pend = busy ? 1 : 0;

    char buf[700];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "DIAG iters=%llu lull=%llu pend_txack=%d fl=%llu/15:%llu/16:%llu sgl=%llu q[",
                  (unsigned long long)g_drain_iters, (unsigned long long)g_lull_hits,
                  (int)pending,
                  (unsigned long long)g_fl_total, (unsigned long long)g_fl_n15,
                  (unsigned long long)g_fl_n16, (unsigned long long)g_single_acks);
    for (int s = 0; s < objs->n_data_workers && n < (int)sizeof(buf) - 90; s++) {
        struct dpu_data_worker *worker_state = &objs->data_workers[s];
        n += snprintf(buf + n, sizeof(buf) - n, "%u/l%llu/x%llu ",
                      comp_queue_usage(&worker_state->queue),
                      (unsigned long long)atomic_load_explicit(
                          &worker_state->stat_local_completions, memory_order_relaxed),
                      (unsigned long long)atomic_load_explicit(
                          &worker_state->stat_cross_worker_out, memory_order_relaxed));
    }
    n += snprintf(buf + n, sizeof(buf) - n, "] pods:");
    int np = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < np && n < (int)sizeof(buf) - 40; i++) {
        struct pod_state *p = &objs->pods[i];
        if (!__atomic_load_n(&p->registered, __ATOMIC_ACQUIRE))
            continue;
        n += snprintf(buf + n, sizeof(buf) - n, " p%d[tx=%d rev=%d]",
                      p->pod_id, p->txack_batch_n, p->rev_done_batch_n);
    }
    /* Proxy delivery counters: drop= bytes lost (the sender was ACKed for them anyway),
     * stall= backpressure parks. drop>0 is a bug; a climbing stall= is a dry pool. */
    if (n < (int)sizeof(buf) - 80)
        n += px_diag_str(objs, buf + n, (int)sizeof(buf) - n);
    DOCA_LOG_WARN("%s", buf);
    g_lull_hits = 0;
    g_drain_iters = 0;
}

/* Send the periodic wake message to every running DPA EU. */
static void
dpu_send_wake(struct objects *objs)
{
    if (!objs->dpa_thread_running_any)
        return;
    struct comch_msg trigger;
    memset(&trigger, 0, sizeof(trigger));
    trigger.type = DPA_MSG_WAKE;
    for (int k = 0; k < objs->num_dpa_threads; k++)
        if (objs->dpa_thread_running[k])
            (void)dmesh_doca_dpa_msgq_send_try(&objs->dpa_comches[k]->send,
                                                &trigger, sizeof(trigger));
}

/* ====== ARM data workers ====== */

/* Send the periodic wake message to this worker's DPA channels. */
static void
dpu_send_wake_worker(struct objects *objs, int id)
{
    if (!objs->dpa_thread_running_any)
        return;
    struct comch_msg trigger;
    memset(&trigger, 0, sizeof(trigger));
    trigger.type = DPA_MSG_WAKE;
    int worker_count = objs->n_data_workers;
    for (int k = id; k < objs->num_dpa_threads; k += worker_count)
        if (objs->dpa_thread_running[k])
            (void)dmesh_doca_dpa_msgq_send_try(&objs->dpa_comches[k]->send,
                                                &trigger, sizeof(trigger));
}

/* Progress one worker's consumer PE and deferred receive tasks. */
static int
dpu_progress_worker_pe(struct objects *objs, struct dpu_data_worker *worker_state)
{
    int did = doca_pe_progress(worker_state->pe);
    if (worker_state->num_deferred_recv > 0 &&
        comp_queue_usage(&worker_state->queue) < COMP_QUEUE_BP_LOW) {
        int remaining = 0, original = worker_state->num_deferred_recv;
        for (int i = 0; i < original; i++) {
            struct doca_task *t = worker_state->deferred_recv[i];
            if (doca_task_submit(t) != DOCA_SUCCESS)
                worker_state->deferred_recv[remaining++] = t;
        }
        worker_state->num_deferred_recv = remaining;
    }
    if (worker_state->id == 0)
        objects_drain_consumer_retry(objs);
    return did;
}

/* Maximum DPA completions processed before DMA progress. */
#define DPU_WORKER_COMPLETION_BUDGET 64

/* Drain local and cross-worker completions while preserving each queue's front on
 * backpressure. Returns the number of completions advanced. */
static int
dpu_worker_run(struct objects *objs, struct dpu_data_worker *worker_state)
{
    int did = 0, woke[MAX_ARM_WORKERS] = { 0 };

    /* Completions received through this worker's consumer PE. */
    for (int n = 0; n < DPU_WORKER_COMPLETION_BUDGET; n++) {
        dpu_comp_entry_t *e = comp_queue_peek(&worker_state->queue);
        if (!e)
            break;
        int owner = owner_worker(objs, worker_state->id, e);
        if (owner == worker_state->id) {
            if (px_process_forward(objs, worker_state->id, e) == 0)
                break;                   /* engine backpressure — retain, retry */
            atomic_fetch_add_explicit(&worker_state->stat_local_completions, 1,
                                      memory_order_relaxed);
        } else {
            if (cross_worker_handoff(objs, owner, e) != 0)
                break;                   /* owner inbox full — retain, retry */
            woke[owner] = 1;
            atomic_fetch_add_explicit(&worker_state->stat_cross_worker_out, 1,
                                      memory_order_relaxed);
        }
        comp_queue_dequeue(&worker_state->queue);
        did++;
    }

    /* Cross-worker reply inbox. */
    for (int n = 0; n < DPU_WORKER_COMPLETION_BUDGET; n++) {
        dpu_comp_entry_t *xe = mpsc_comp_queue_peek(&worker_state->cross_worker);
        if (!xe)
            break;
        if (px_process_forward(objs, worker_state->id, xe) == 0)
            break;                       /* backpressure — leave the front, retry */
        mpsc_comp_queue_dequeue(&worker_state->cross_worker);
        atomic_fetch_add_explicit(&worker_state->stat_cross_worker_in, 1,
                                  memory_order_relaxed);
        did++;
    }

    /* Resume connections stalled by egress backpressure. */
    did += px_drain_stalled(objs, worker_state->id);
    /* Submit DMA, progress completions, and retire owned lanes. */
    did += px_worker_drain(objs, worker_state->id);

    for (int s = 0; s < objs->n_data_workers; s++)
        if (woke[s])
            dpu_wake_worker(&objs->data_workers[s]);
    return did;
}

/* ARM data worker event loop. */
static void *
dpu_data_worker_main(void *arg)
{
    struct dpu_data_worker *worker_state = (struct dpu_data_worker *)arg;
    struct objects *objs = worker_state->objs;
    dpu_t_is_worker = 1;
    dpu_worker_id = worker_state->id;
    dpu_arm_name_current("worker", worker_state->id);
    dpu_arm_pin_current("worker", worker_state->id);

    doca_notification_handle_t cfd = 0;
    int dfd = px_worker_notification_fd(objs, worker_state->id);
    int ep = -1;
    if (doca_pe_get_notification_handle(worker_state->pe, &cfd) == DOCA_SUCCESS)
        ep = epoll_create1(0);
    if (ep >= 0) {
        struct epoll_event ec = { .events = EPOLLIN, .data = { .u32 = 0 } };  /* own PE fd */
        struct epoll_event ew = { .events = EPOLLIN, .data = { .u32 = 1 } };  /* cross-worker wake */
        if (epoll_ctl(ep, EPOLL_CTL_ADD, (int)cfd, &ec) != 0 ||
            epoll_ctl(ep, EPOLL_CTL_ADD, worker_state->wake_fd, &ew) != 0) { close(ep); ep = -1; }
        if (ep >= 0 && dfd >= 0) {
            struct epoll_event ed = { .events = EPOLLIN, .data = { .u32 = 2 } };
            if (epoll_ctl(ep, EPOLL_CTL_ADD, dfd, &ed) != 0) {
                DOCA_LOG_WARN("worker %d: DMA PE notification unavailable; "
                              "1 ms event-loop backstop remains active", worker_state->id);
                dfd = -1;
            }
        }
    }
    if (ep < 0) {
        DOCA_LOG_ERR("ARM worker %d: consumer PE event-loop setup failed",
                     worker_state->id);
        atomic_store_explicit(&worker_state->init_state, -1, memory_order_release);
        return NULL;
    }

    atomic_store_explicit(&worker_state->init_state, 1, memory_order_release);

    struct timespec last, now;
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (!worker_state->stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9 >= 0.001) {
            dpu_send_wake_worker(objs, worker_state->id);
            last = now;
        }
        int did = dpu_progress_worker_pe(objs, worker_state);
        int run = dpu_worker_run(objs, worker_state);
        if (did || run) {
            dpu_wake_main(objs);
            continue;                    /* stay hot while there is work */
        }
        /* Arm notifications, recheck queues, then wait up to 1 ms. */
        (void)doca_pe_request_notification(worker_state->pe);
        if (dfd >= 0)
            (void)px_worker_arm_notification(objs, worker_state->id);
        atomic_store_explicit(&worker_state->parked, 1, memory_order_release);
        if (dpu_progress_worker_pe(objs, worker_state) || dpu_worker_run(objs, worker_state) ||
            !mpsc_comp_queue_empty(&worker_state->cross_worker)) {
            atomic_store_explicit(&worker_state->parked, 0, memory_order_release);
            (void)doca_pe_clear_notification(worker_state->pe, cfd);
            px_worker_clear_notification(objs, worker_state->id, dfd);
            dpu_wake_main(objs);
            continue;
        }
        struct epoll_event evs[3];
        (void)epoll_wait(ep, evs, 3, 1);
        atomic_store_explicit(&worker_state->parked, 0, memory_order_release);
        uint64_t drain; ssize_t rn = read(worker_state->wake_fd, &drain, sizeof(drain)); (void)rn;
        (void)doca_pe_clear_notification(worker_state->pe, cfd);
        px_worker_clear_notification(objs, worker_state->id, dfd);
    }
    close(ep);
    return NULL;
}

static void
stop_data_workers(struct objects *objs)
{
    for (int s = 0; s < objs->n_data_workers; s++) {
        struct dpu_data_worker *worker_state = &objs->data_workers[s];
        if (!worker_state->running)
            continue;
        worker_state->stop = 1;
        dpu_wake_worker(worker_state);
    }
    for (int s = 0; s < objs->n_data_workers; s++) {
        struct dpu_data_worker *worker_state = &objs->data_workers[s];
        if (!worker_state->running)
            continue;
        pthread_join(worker_state->thread, NULL);
        worker_state->running = 0;
        if (worker_state->wake_fd >= 0) {
            close(worker_state->wake_fd);
            worker_state->wake_fd = -1;
        }
    }
}

/* Publish DPU readiness after all selected execution paths are initialized. */
static void
dpu_publish_ready_and_setup_pods(struct objects *objs)
{
    objs->dpu_ready = 1;
    int kmax = objs->k_rings > 0 ? objs->k_rings : 1;
    int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct pod_state *pod = &objs->pods[i];
        if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE) ||
            pod->init_result != DMESH_POD_INIT_PENDING)
            continue;
        if (pod->ring_mmap_count != kmax || pod->remote_mmap == NULL ||
            pod->host_rx_mmap == NULL || pod->dma_ready)
            continue;

        doca_error_t result = setup_pod_dma(objs, pod);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("deferred setup_pod_dma failed for pod %d: %s",
                         pod->pod_id, doca_error_get_descr(result));
            (void)server_publish_pod_init_result(objs, pod,
                                                 DMESH_POD_INIT_DPA_FAILED);
            continue;
        }
        /* setup_pod_dma only arms the ACK barrier. READY is published by
         * dpu_finalize_pending_pod_inits after every target EU responds. */
    }
}

void
run_dpu_worker(struct objects *objs)
{
    doca_error_t result;

    /* DPA wake cadence for parked EUs. */
    const double keepalive_sec = 0.001;   /* 1 ms DPU→DPA WAKE cadence */
    struct timespec now, last_kick;
    double kick_elapsed;

    DOCA_LOG_INFO("Starting DPU worker");

    /* Initialize pod state. */
    memset(objs->pods, 0, sizeof(objs->pods));
    objs->num_pods = 0;

    /* Initialize pod lookup and load-balancer cursors. */
    for (int i = 0; i < POD_ID_SPACE; i++) {
        objs->pod_id_to_slot[i] = -1;
        objs->svc_rr[i]         = 0;
    }

    /* Initialize shared connection tracking. */
    objs->conntrack = calloc(1, sizeof(struct dpu_conntrack));
    if (!objs->conntrack) {
        DOCA_LOG_ERR("Failed to allocate DPU connection tracking table");
        cleanup_objects(objs);
        return;
    }
    objs->conntrack->next_uport = DMESH_UPORT_BASE;

    /* Init deferred completion queue + backpressure state */
    objs->comp_queue.head = 0;
    objs->comp_queue.tail = 0;
    objs->num_deferred_recv = 0;

    /* Readiness is published after all data paths are initialized. */
    objs->dpu_ready = 0;

    /* Configure DPA EU threads and forward rings before the server accepts pod
     * mappings. Environment overrides are clamped to the supported limits. */
    objs->num_dpa_threads = DPA_THREADS_DEFAULT;   /* tentative; auto-detected in init_dpa_objects */
    objs->dpa_threads_auto = 1;
    { const char *ne = getenv("DPUMESH_DPA_THREADS");
      if (ne && *ne) { int v = atoi(ne);
          if (v >= 1) { if (v > MAX_DPA_EU) v = MAX_DPA_EU; objs->num_dpa_threads = v; objs->dpa_threads_auto = 0; } } }
    /* K forward rings per pod. Host and DPU use the same configured K. */
    objs->k_rings = DPUMESH_RINGS_PER_POD_DEFAULT;   /* = 2 */
    { const char *ke = getenv("DPUMESH_RINGS_PER_POD");
      if (ke && *ke) {
          int v = atoi(ke);
          if (v >= 1 && v <= MAX_EU_PER_POD)
              objs->k_rings = v;
      } }

    /* A=1 runs on main; A>=2 uses dedicated ARM data workers. */
    objs->dedicated_workers = 0;
    objs->main_wake_fd = -1;
    atomic_store_explicit(&objs->main_parked, 0, memory_order_relaxed);
    /* Configure ARM data workers and routing ownership. */
    objs->n_data_workers = 1;
    objs->worker_shared_routing = 0;
    pthread_mutex_init(&objs->routing_lock, NULL);
    for (int s = 0; s < MAX_ARM_WORKERS; s++) {
        struct dpu_data_worker *worker_state = &objs->data_workers[s];
        worker_state->objs = objs;
        worker_state->id = s;
        worker_state->pe = NULL;
        worker_state->queue.head = worker_state->queue.tail = 0;
        mpsc_comp_queue_init(&worker_state->cross_worker);
        atomic_init(&worker_state->pending_txack_head, 0);
        atomic_init(&worker_state->pending_txack_tail, 0);
        atomic_init(&worker_state->stat_local_completions, 0);
        atomic_init(&worker_state->stat_cross_worker_out, 0);
        atomic_init(&worker_state->stat_cross_worker_in, 0);
        worker_state->num_deferred_recv = 0;
        worker_state->wake_fd = -1;
        atomic_store_explicit(&worker_state->parked, 0, memory_order_relaxed);
        atomic_store_explicit(&worker_state->init_state, 0, memory_order_relaxed);
        worker_state->stop = 0;
        worker_state->running = 0;
    }
    for (int s = 0; s < MAX_ARM_WORKERS; s++)
        objs->consumer_pes[s] = NULL;
    { const char *me = getenv("DPUMESH_ARM_WORKERS");
      if (me && *me) { int v = atoi(me);
          if (v > MAX_ARM_WORKERS)
              v = MAX_ARM_WORKERS;
          if (v >= 1) objs->n_data_workers = v; } }
    {
        int requested = objs->n_data_workers;
        while (objs->n_data_workers > 1 &&
               (objs->n_data_workers > objs->k_rings ||
                objs->k_rings % objs->n_data_workers != 0))
            objs->n_data_workers--;
        if (objs->n_data_workers != requested)
            DOCA_LOG_WARN("ARM worker count adjusted: requested A=%d, active A=%d, K=%d",
                          requested, objs->n_data_workers, objs->k_rings);
    }
    { const char *se = getenv("DPUMESH_WORKER_SHARED_ROUTING");
      if (se && atoi(se) >= 1) objs->worker_shared_routing = 1; }
    { const char *de = getenv("DPUMESH_DIAG");
      if (de && atoi(de) >= 1) g_dpu_diag = 1; }
    if (objs->n_data_workers >= 2)
        objs->dedicated_workers = 1;

    dpu_arm_affinity_init();
    DOCA_LOG_WARN("Requested data topology: K/A=%d/%d (N finalized after DPA query)",
                  objs->k_rings, objs->n_data_workers);
    DOCA_LOG_WARN("ARM data path = %s",
                  objs->n_data_workers >= 2
                      ? "dedicated worker threads"
                      : "single run-to-completion main thread");
    if (objs->n_data_workers >= 2)
        DOCA_LOG_WARN("ARM DATA WORKERS = %d (routing=%s)",
                      objs->n_data_workers,
                      objs->worker_shared_routing ? "shared-routing+lock"
                                                 : "share-nothing");

    /* 1. comch control path server (waits for first connection) */
    result = init_comch_ctrl_path_server("DPUMesh", objs, true);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init comch control path server: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* 2. comch datapath consumer (for DPA → DPU messages) */
    result = init_comch_datapath_consumer(objs);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init comch datapath consumer: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* Create one consumer PE per ARM data worker. */
    objs->consumer_pes[0] = objs->consumer_pe;
    if (objs->n_data_workers >= 2) {
        for (int s = 1; s < objs->n_data_workers; s++) {
            result = doca_pe_create(&objs->consumer_pes[s]);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to create consumer PE for worker %d: %s",
                             s, doca_error_get_descr(result));
                for (int t = 1; t < s; t++) { doca_pe_destroy(objs->consumer_pes[t]); objs->consumer_pes[t] = NULL; }
                objs->n_data_workers = 1;
                objs->dedicated_workers = 0;
                break;
            }
        }
    }

    /* 3. DPA app init (shared) */
    result = init_dpa_objects(objs);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init DPA objects: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* 4. DPA threads create (one per EU on the shared device; not run yet —
     *    each EU is started on its first assigned pod in setup_pod_dma). EU
     *    thread k is pinned to absolute EU k (partition exposes abs_EUs 0-63). */
    for (int k = 0; k < objs->num_dpa_threads; k++) {
        result = dmesh_doca_dpa_thread_create(objs->dpa_threads[k], k);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to create DPA thread EU %d: %s",
                         k, doca_error_get_descr(result));
            cleanup_objects(objs);
            return;
        }
    }

    /* DPA channel k binds to consumer_pes[k % A]. */
    result = init_comch_dpa_msgq(objs, objs->consumer_pe);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init comch DPA msgq: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* Pin workers to [0,A) and main to A when available. */
    dpu_arm_pin_current("main", objs->dedicated_workers
                                ? objs->n_data_workers : 0);

    /* SG-DMA DPU-to-host path and request parser. */
    result = px_init(objs);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init L7-proxy L4 engine: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* Each ARM data worker owns one consumer PE and wake fd. */
    if (objs->n_data_workers >= 2) {
        for (int s = 0; s < objs->n_data_workers; s++) {
            struct dpu_data_worker *worker_state = &objs->data_workers[s];
            worker_state->pe = objs->consumer_pes[s];
            worker_state->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (worker_state->wake_fd < 0) {
                DOCA_LOG_ERR("ARM worker %d eventfd creation failed", s);
                stop_data_workers(objs);
                cleanup_objects(objs);
                return;
            }
            if (pthread_create(&worker_state->thread, NULL, dpu_data_worker_main, worker_state) != 0) {
                DOCA_LOG_ERR("ARM worker %d thread creation failed", s);
                close(worker_state->wake_fd);
                worker_state->wake_fd = -1;
                stop_data_workers(objs);
                cleanup_objects(objs);
                return;
            }
            worker_state->running = 1;
            const struct timespec init_pause = { .tv_sec = 0, .tv_nsec = 100000 };
            int attempts = 0;
            while (atomic_load_explicit(&worker_state->init_state, memory_order_acquire) == 0 &&
                   attempts++ < 20000)
                nanosleep(&init_pause, NULL);
            if (atomic_load_explicit(&worker_state->init_state, memory_order_acquire) != 1) {
                DOCA_LOG_ERR("ARM worker %d event loop did not initialize", s);
                stop_data_workers(objs);
                cleanup_objects(objs);
                return;
            }
        }
        DOCA_LOG_WARN("ARM DATA WORKER THREADS = %d", objs->n_data_workers);
    }

    /* Dedicated data workers feed the main Comch emitter through SPSC queues. */
    if (objs->dedicated_workers) {
        atomic_store_explicit(&objs->main_parked, 0, memory_order_release);
        objs->main_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        doca_notification_handle_t pfd = 0;
        int rep = -1;
        if (objs->main_wake_fd >= 0 &&
            doca_pe_get_notification_handle(objs->pe, &pfd) == DOCA_SUCCESS)
            rep = epoll_create1(0);
        if (rep >= 0) {
            struct epoll_event ept = { .events = EPOLLIN, .data = { .u32 = 1 } };  /* ctrl pe   */
            struct epoll_event ee  = { .events = EPOLLIN, .data = { .u32 = 2 } };  /* worker wake */
            if (epoll_ctl(rep, EPOLL_CTL_ADD, (int)pfd, &ept) != 0 ||
                epoll_ctl(rep, EPOLL_CTL_ADD, objs->main_wake_fd, &ee) != 0) {
                close(rep); rep = -1;
            }
        }
        if (rep < 0) {
            DOCA_LOG_ERR("main emitter event-loop setup failed for %d data workers",
                         objs->n_data_workers);
            stop_data_workers(objs);
            cleanup_objects(objs);
            return;
        }

        dpu_publish_ready_and_setup_pods(objs);
        DOCA_LOG_WARN("MAIN COMCH EMITTER: workers=%d, event-driven",
                      objs->n_data_workers);
        struct timespec last_dbg;
        clock_gettime(CLOCK_MONOTONIC, &last_dbg);
        while (true) {
            int progressed = dpu_drain_iteration(objs);
            if (g_dpu_diag) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                if ((now.tv_sec - last_dbg.tv_sec) +
                        (now.tv_nsec - last_dbg.tv_nsec) / 1e9 >= 1.0) {
                    dpu_diag_dump(objs);
                    last_dbg = now;
                }
            }
            if (progressed)
                continue;

            (void)doca_pe_request_notification(objs->pe);
            atomic_store_explicit(&objs->main_parked, 1, memory_order_release);
            if (dpu_drain_iteration(objs)) {
                atomic_store_explicit(&objs->main_parked, 0, memory_order_release);
                (void)doca_pe_clear_notification(objs->pe, pfd);
                continue;
            }
            struct epoll_event evs[2];
            (void)epoll_wait(rep, evs, 2, 1);
            atomic_store_explicit(&objs->main_parked, 0, memory_order_release);
            uint64_t drain;
            ssize_t rn = read(objs->main_wake_fd, &drain, sizeof(drain));
            (void)rn;
            (void)doca_pe_clear_notification(objs->pe, pfd);
        }
        return;
    }

    /* A=1 event loop over the consumer and control PEs. */
    doca_notification_handle_t cfd = 0, pfd = 0;
    doca_error_t hc = doca_pe_get_notification_handle(objs->consumer_pe, &cfd);
    doca_error_t hp = doca_pe_get_notification_handle(objs->pe, &pfd);
    int ep  = (hc == DOCA_SUCCESS && hp == DOCA_SUCCESS) ? epoll_create1(0) : -1;
    int setup_ok = (ep >= 0);
    if (setup_ok) {
        struct epoll_event ec = { .events = EPOLLIN, .data = { .u32 = 0 } };  /* consumer_pe */
        struct epoll_event ept = { .events = EPOLLIN, .data = { .u32 = 1 } }; /* ctrl pe    */
        if (epoll_ctl(ep, EPOLL_CTL_ADD, (int)cfd, &ec) != 0 ||
            epoll_ctl(ep, EPOLL_CTL_ADD, (int)pfd, &ept) != 0)
            setup_ok = 0;
    }
    if (!setup_ok) {
        DOCA_LOG_WARN("Event-loop setup failed (consumer_pe=%s ctrl_pe=%s ep=%d) → falling back to busy-poll",
                      doca_error_get_name(hc), doca_error_get_name(hp), ep);
        if (ep >= 0) close(ep);
        clock_gettime(CLOCK_MONOTONIC, &last_kick);
        dpu_publish_ready_and_setup_pods(objs);
        DOCA_LOG_INFO("DPU worker initialized (busy-poll fallback), entering main loop");
        while (true) {
            dpu_drain_iteration(objs);
            clock_gettime(CLOCK_MONOTONIC, &now);
            kick_elapsed = (now.tv_sec - last_kick.tv_sec) +
                           (now.tv_nsec - last_kick.tv_nsec) / 1e9;
            if (kick_elapsed >= keepalive_sec) {
                dpu_send_wake(objs);
                last_kick = now;
            }
        }
        return;  /* not reached */
    }

    dpu_publish_ready_and_setup_pods(objs);
    DOCA_LOG_INFO("DPU worker: EVENT-DRIVEN main loop armed (consumer_pe fd=%d, ctrl_pe fd=%d)",
                  (int)cfd, (int)pfd);

    clock_gettime(CLOCK_MONOTONIC, &last_kick);
    struct timespec last_dbg = last_kick;
    while (true) {
        int progressed = dpu_drain_iteration(objs);   /* ONE pass */

        /* Periodic DPA wake. */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (g_dpu_diag &&
            (now.tv_sec - last_dbg.tv_sec) + (now.tv_nsec - last_dbg.tv_nsec) / 1e9 >= 1.0) {
            dpu_diag_dump(objs);
            last_dbg = now;
        }
        kick_elapsed = (now.tv_sec - last_kick.tv_sec) +
                       (now.tv_nsec - last_kick.tv_nsec) / 1e9;
        if (kick_elapsed >= keepalive_sec) {
            dpu_send_wake(objs);
            last_kick = now;
        }

        if (progressed)
            continue;   /* work this pass → poll again (no sleep under load) */

        /* Arm both PEs and recheck before blocking. */
        (void)doca_pe_request_notification(objs->consumer_pe);
        (void)doca_pe_request_notification(objs->pe);

        if (dpu_drain_iteration(objs)) {
            (void)doca_pe_clear_notification(objs->consumer_pe, cfd);
            (void)doca_pe_clear_notification(objs->pe, pfd);
            continue;
        }

        /* Wait for PE activity or the 1 ms DPA wake interval. */
        struct epoll_event evs[2];
        (void)epoll_wait(ep, evs, 2, 1);

        /* Clear PE notifications so they can be re-armed (SELECTIVE contract). */
        (void)doca_pe_clear_notification(objs->consumer_pe, cfd);
        (void)doca_pe_clear_notification(objs->pe, pfd);
    }
}
