#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dpu_worker.h"

#include "comch_server.h"
#include "comch_common.h"
#include "dpa.h"
#include "dpa_common.h"
#include "comch_msgq.h"
#include "buffer.h"
#include "ring.h"
#include "dpu_proxy.h"
#include "peer_channel.h"
#include "peer_transport.h"
#include "peer_wire.h"
#include <dpumesh/dmesh_common.h>
#include <dpumesh/dmesh_topology.h>
#include <dmesh_l7.h>

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

DOCA_LOG_REGISTER(DPU_WORKER);

/* ARM worker selected by the DPA receive callback. */
extern __thread int dpu_worker_id;

static int g_arm_affinity_ready = 0;
static int g_arm_allowed_n = 0;
static cpu_set_t g_arm_allowed;
/* Permitted CPUs, quietest interrupt load first. */
static int g_arm_order[CPU_SETSIZE];
static int g_arm_order_n = 0;

/* Count the interrupts each CPU has taken from device lines. A worker sharing a
 * CPU with an active interrupt line competes with hard-IRQ and softirq work and
 * saturates ahead of its peers, so those CPUs are used last. */
static void
dpu_arm_irq_load(unsigned long long *per_cpu, int ncpu)
{
    FILE *f = fopen("/proc/interrupts", "r");
    if (!f)
        return;
    char line[4096];
    if (!fgets(line, sizeof(line), f)) {          /* header names the CPUs */
        fclose(f);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *name = line;
        while (*name == ' ') name++;
        if (*name < '0' || *name > '9')            /* IPI/err rows, not devices */
            continue;
        char *p = colon + 1;
        for (int cpu = 0; cpu < ncpu; cpu++) {
            char *end;
            unsigned long long v = strtoull(p, &end, 10);
            if (end == p)
                break;
            per_cpu[cpu] += v;
            p = end;
        }
    }
    fclose(f);
}

static void
dpu_arm_affinity_init(void)
{
    CPU_ZERO(&g_arm_allowed);
    if (sched_getaffinity(0, sizeof(g_arm_allowed), &g_arm_allowed) != 0) {
        return;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &g_arm_allowed))
            g_arm_allowed_n++;
    g_arm_affinity_ready = g_arm_allowed_n > 0;
    if (!g_arm_affinity_ready)
        return;

    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu <= 0 || ncpu > CPU_SETSIZE)
        return;
    unsigned long long *load = calloc((size_t)ncpu, sizeof(*load));
    if (!load)
        return;
    dpu_arm_irq_load(load, ncpu);

    /* Order the permitted CPUs by interrupt load, quietest first. Pinning walks
     * this order, so the loaded CPUs are only reached once the quiet ones run
     * out and the assignment stays a permutation of what was permitted. */
    g_arm_order_n = 0;
    for (int cpu = 0; cpu < ncpu && g_arm_order_n < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &g_arm_allowed))
            g_arm_order[g_arm_order_n++] = cpu;
    for (int i = 1; i < g_arm_order_n; i++) {
        int cpu = g_arm_order[i], j = i - 1;
        while (j >= 0 && load[g_arm_order[j]] > load[cpu]) {
            g_arm_order[j + 1] = g_arm_order[j];
            j--;
        }
        g_arm_order[j + 1] = cpu;
    }
    free(load);
}

static void
dpu_arm_pin_current(const char *role, int logical_id)
{
    if (!g_arm_affinity_ready)
        return;
    int ordinal = logical_id % g_arm_allowed_n;
    int selected = -1;
    if (g_arm_order_n == g_arm_allowed_n) {
        selected = g_arm_order[ordinal];
    } else {
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &g_arm_allowed))
                continue;
            if (ordinal-- == 0) {
                selected = cpu;
                break;
            }
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

/* L4 routing selects a live, ready backend from pods[]. Connection affinity and
 * L7 host overrides are applied by the caller.
 *
 * The local half of a Service's backend set stays registration-derived rather
 * than generation-derived, and that is deliberate: a Pod that registered
 * between a generation's snapshot and its publication is absent from that one
 * generation without having stopped serving, and a node's own registrations
 * are the node's own attested truth. What the generation adds is the half a
 * node cannot see for itself — the replicas somewhere else. */
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

/* Whether the held generation places a replica of this Service somewhere else.
 * Derived from its endpoint= lines joined with pod= for placement, with this
 * node's own excluded — those are `collect_live_hosts`'s. One endpoint answers
 * the question, so only one is asked for. */
int
dmesh_service_has_remote(struct objects *objs, int16_t svc)
{
    if (svc < 0 || svc >= POD_ID_SPACE)
        return 0;
    struct dmesh_endpoint_ref one;
    return dmesh_topology_remote_endpoints(objs, svc, objs->node_name,
                                           &one, 1) > 0;
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

/* Select the connection owner.
 *
 * An L7 request goes to the worker that owns the L7 layer; everything else,
 * including every reply, is owned by the worker its port names. */
static inline int
owner_worker(struct objects *objs, const dpu_comp_entry_t *e)
{
    /* An abort retains the original service so it reaches the same Linkerd
     * owner as the request whose source QP it terminates. */
    int32_t route_pod = e->dst_pod_id == DMESH_POD_ABORT
                      ? DMESH_POD_BLANK : e->dst_pod_id;
    int l7 = px_l7_request_owner(objs, route_pod, e->dst_service);
    if (l7 >= 0)
        return l7;
    return dmesh_worker_for_port(e->src_port, objs->n_data_workers);
}

/* Release a worker parked on its notification handles. */
static void
dpu_wake_worker(struct dpu_data_worker *worker_state)
{
    dpu_wake_eventfd(&worker_state->parked, worker_state->wake_fd);
}

/* Hand a reply to its owner worker. Returns -1 when the inbox is full. */
static int
cross_worker_handoff(struct objects *objs, int owner, const dpu_comp_entry_t *e)
{
    struct dpu_data_worker *dst = &objs->data_workers[owner];
    int rc = mpsc_comp_queue_enqueue(&dst->cross_worker, e);
    if (rc == 0)
        dpu_wake_worker(dst);
    return rc;
}

/* ====== DPU Worker ====== */

/* Wake the main loop once per park interval. */
void
dpu_wake_main(struct objects *objs)
{
    dpu_wake_eventfd(&objs->main_parked, objs->main_wake_fd);
}

void
dpu_request_host_doorbell(struct objects *objs, struct pod_state *pod,
                          uint64_t epoch)
{
    if (!pod || epoch == 0)
        return;
    uint64_t current = __atomic_load_n(&pod->rev_doorbell_pending_epoch,
                                        __ATOMIC_ACQUIRE);
    while (current < epoch &&
           !__atomic_compare_exchange_n(&pod->rev_doorbell_pending_epoch,
                                        &current, epoch, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
    }
    if (current < epoch)
        dpu_wake_main(objs);
}

static int
dpu_flush_host_doorbells(struct objects *objs)
{
    int sent = 0;
    int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct pod_state *pod = &objs->pods[i];
        uint64_t pending = __atomic_load_n(&pod->rev_doorbell_pending_epoch,
                                            __ATOMIC_ACQUIRE);
        uint64_t completed = __atomic_load_n(&pod->rev_doorbell_sent_epoch,
                                              __ATOMIC_ACQUIRE);
        if (pending <= completed || !pod->connection)
            continue;
        struct dmesh_rev_doorbell_msg msg = {
            .type = DMESH_MSG_REV_DOORBELL,
            ._pad = { 0, 0, 0 },
        };
        doca_error_t result = server_send_msg_to_conn(
            objs, pod->connection, (const char *)&msg, sizeof(msg));
        if (result == DOCA_ERROR_AGAIN)
            continue;
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_WARN("pod %d reverse doorbell failed: %s",
                          pod->pod_id, doca_error_get_descr(result));
            continue;
        }
        __atomic_store_n(&pod->rev_doorbell_sent_epoch, pending,
                         __ATOMIC_RELEASE);
        sent++;
    }
    return sent;
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
            (void)server_publish_pod_init_result(objs, pod,
                                                 DMESH_POD_INIT_READY);
        }
        finalized++;
    }
    return finalized;
}

/* Progress main-owned control and doorbells. */
static int
dpu_drain_iteration(struct objects *objs)
{
    uint8_t did_ctrl     = doca_pe_progress(objs->pe);  /* new conns, REGISTER, TX_DATA */
    int finalized_init   = dpu_finalize_pending_pod_inits(objs);
    int sent_init_result = server_flush_pod_init_results(objs);
    int sent_doorbell    = dpu_flush_host_doorbells(objs);
    int cleaned_pods = server_progress_pod_cleanup(objs);
    int revoked = server_progress_membership(objs);
    int admission = server_progress_admission(objs);
    int topology = dmesh_topology_progress(objs);
    /* A new generation can re-intern Services, so the L7 mode table follows. */
    if (topology > 0 && px_l7_resolve_modes(objs) != 0)
        DOCA_LOG_WARN("L7 mode lists conflict against the adopted generation; "
                      "previous mode table kept");
    return (did_ctrl || cleaned_pods > 0 || finalized_init > 0 ||
            sent_init_result > 0 || sent_doorbell > 0 || revoked > 0 ||
            admission > 0 || topology > 0);
}

/* ====== ARM data workers ====== */

/* Progress one worker's consumer PE and deferred receive tasks. */
static enum px_progress_state
dpu_progress_worker_pe(struct dpu_data_worker *worker_state)
{
    enum px_progress_state state = doca_pe_progress(worker_state->pe) ?
        PX_PROGRESS_PROGRESSED : PX_PROGRESS_IDLE;
    if (worker_state->num_deferred_recv > 0 &&
        comp_queue_usage(&worker_state->queue) < COMP_QUEUE_BP_LOW) {
        int remaining = 0, original = worker_state->num_deferred_recv;
        for (int i = 0; i < original; i++) {
            struct doca_task *t = worker_state->deferred_recv[i];
            if (doca_task_submit(t) != DOCA_SUCCESS) {
                worker_state->deferred_recv[remaining++] = t;
            } else {
                struct dmesh_doca_dpa_msgq *mq = doca_task_get_user_data(t).ptr;
                if (mq != NULL)
                    __atomic_fetch_add(&mq->recv_posted, 1,
                                       __ATOMIC_RELAXED);
                state = PX_PROGRESS_PROGRESSED;
            }
        }
        worker_state->num_deferred_recv = remaining;
    }
    return state;
}

/* Drain local and cross-worker completions. */
static enum px_progress_state
dpu_worker_run_budget(struct objects *objs, struct dpu_data_worker *worker_state,
                      int budget)
{
    int did = 0;

    /* Completions received through this worker's consumer PE. */
    for (int n = 0; n < budget; n++) {
        dpu_comp_entry_t *e = comp_queue_peek(&worker_state->queue);
        if (!e)
            break;
        int owner = owner_worker(objs, e);
        if (owner == worker_state->id) {
            if (px_process_forward(objs, worker_state->id, e) == 0) {
                break;                   /* engine backpressure — retain, retry */
            }
            atomic_fetch_add_explicit(&worker_state->stat_local_completions, 1,
                                      memory_order_relaxed);
        } else {
            if (cross_worker_handoff(objs, owner, e) != 0) {
                break;                   /* owner inbox full — retain, retry */
            }
            atomic_fetch_add_explicit(&worker_state->stat_cross_worker_out, 1,
                                      memory_order_relaxed);
        }
        comp_queue_dequeue(&worker_state->queue);
        did++;
    }

    /* Cross-worker reply inbox. */
    for (int n = 0; n < budget; n++) {
        dpu_comp_entry_t *xe = mpsc_comp_queue_peek(&worker_state->cross_worker);
        if (!xe)
            break;
        if (px_process_forward(objs, worker_state->id, xe) == 0) {
            break;                       /* backpressure — leave the front, retry */
        }
        mpsc_comp_queue_dequeue(&worker_state->cross_worker);
        atomic_fetch_add_explicit(&worker_state->stat_cross_worker_in, 1,
                                  memory_order_relaxed);
        did++;
    }

    /* Resume connections stalled by egress backpressure. */
    did += px_drain_stalled(objs, worker_state->id);
    /* Submit DMA, progress completions, and retire owned lanes. */
    enum px_progress_state proxy_state =
        px_worker_drain(objs, worker_state->id);

    if (did)
        return PX_PROGRESS_PROGRESSED;
    return proxy_state;
}

#if defined(__aarch64__)
static inline uint64_t dpu_wake_clock_now(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static inline uint64_t dpu_wake_clock_hz(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
#else
static inline uint64_t dpu_wake_clock_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static inline uint64_t dpu_wake_clock_hz(void)
{
    return 1000000000ull;
}
#endif

/* Optional periodic WAKE to the DPA threads that own rings.
 *
 * The message arrives as a consumer completion, re-triggering a thread that has
 * released its execution unit. `DPUMESH_DPA_WAKE_US` is the period in
 * microseconds; 0, the default, sends none. A ringless execution unit has
 * nothing to poll and is never nudged. */
static uint64_t dpu_dpa_nudge_period(void)
{
    static uint64_t period;
    static int resolved;
    if (!resolved) {
        const char *value = getenv("DPUMESH_DPA_WAKE_US");
        long us = (value && *value) ? strtol(value, NULL, 10) : 0;
        period = us > 0 ? (dpu_wake_clock_hz() * (uint64_t)us) / 1000000ull : 0;
        if (us > 0 && period == 0)
            period = 1;
        __atomic_store_n(&resolved, 1, __ATOMIC_RELEASE);
    }
    return period;
}

/* Execution units holding a forward ring of some data-ready pod. */
static uint32_t dpu_dpa_ring_eu_mask(const struct objects *objs)
{
    uint32_t mask = 0;
    for (int i = 0; i < MAX_PODS; i++) {
        const struct pod_state *pod = &objs->pods[i];
        if (!__atomic_load_n(&pod->dma_ready, __ATOMIC_ACQUIRE))
            continue;
        for (int j = 0; j < objs->k_rings; j++) {
            int eu = dmesh_dpa_eu_for_ring(pod->pod_id, objs->k_rings, j,
                                           objs->num_dpa_threads,
                                           objs->n_data_workers);
            if (eu >= 0 && eu < MAX_DPA_EU)
                mask |= 1u << eu;
        }
    }
    return mask;
}

static void dpu_send_wake_worker(struct objects *objs, int id)
{
    uint32_t mask = dpu_dpa_ring_eu_mask(objs);
    if (mask == 0)
        return;
    struct comch_msg trigger;
    memset(&trigger, 0, sizeof(trigger));
    trigger.type = DPA_MSG_WAKE;
    for (int k = id; k < objs->num_dpa_threads; k += objs->n_data_workers)
        if (objs->dpa_thread_running[k] && (mask & (1u << k)))
            (void)dmesh_doca_dpa_msgq_send_try(&objs->dpa_comches[k]->send,
                                               &trigger, sizeof(trigger));
}

/* Send the nudge if its period has elapsed. */
static void dpu_dpa_nudge_due(struct dpu_data_worker *worker_state)
{
    uint64_t period = dpu_dpa_nudge_period();
    if (period == 0)
        return;
    uint64_t now = dpu_wake_clock_now();
    if (worker_state->dpa_nudge_deadline == 0) {
        worker_state->dpa_nudge_deadline = now + period;
        return;
    }
    if ((int64_t)(now - worker_state->dpa_nudge_deadline) < 0)
        return;
    dpu_send_wake_worker(worker_state->objs, worker_state->id);
    worker_state->dpa_nudge_deadline = now + period;
}

/* ARM data-worker thread. The embedded Linkerd runtime owns the loop. */
static void *
dpu_data_worker_main(void *arg)
{
    struct dpu_data_worker *worker_state = (struct dpu_data_worker *)arg;
    dpu_worker_id = worker_state->id;
    dpu_arm_name_current("worker", worker_state->id);
    dpu_arm_pin_current("worker", worker_state->id);

    if (l7_worker_run(worker_state->id, worker_state) < 0) {
        DOCA_LOG_ERR("ARM worker %d runtime failed", worker_state->id);
        atomic_store_explicit(&worker_state->init_state, -1, memory_order_release);
    }
    return NULL;
}

int
dmesh_l7_driver_notification_fds(void *driver, int *completion_fd,
                                 int *dma_fd, int *wake_fd)
{
    struct dpu_data_worker *worker_state = driver;
    doca_notification_handle_t completion = 0;
    if (!worker_state || !completion_fd || !dma_fd || !wake_fd ||
        doca_pe_get_notification_handle(worker_state->pe, &completion) != DOCA_SUCCESS)
        return -1;
    px_bind_worker(worker_state->objs, worker_state->id);
    *completion_fd = (int)completion;
    *dma_fd = px_worker_notification_fd(worker_state->objs, worker_state->id);
    *wake_fd = worker_state->wake_epfd >= 0 ? worker_state->wake_epfd
                                            : worker_state->wake_fd;
    return *wake_fd >= 0 ? 0 : -1;
}

int
dmesh_l7_driver_arm(void *driver)
{
    struct dpu_data_worker *worker_state = driver;
    if (!worker_state ||
        doca_pe_request_notification(worker_state->pe) != DOCA_SUCCESS)
        return -1;
    (void)px_worker_arm_notification(worker_state->objs, worker_state->id);
    atomic_store_explicit(&worker_state->parked, 1, memory_order_release);
    return 0;
}

int
dmesh_l7_driver_drain(void *driver, int budget)
{
    struct dpu_data_worker *worker_state = driver;
    if (!worker_state || budget <= 0)
        return -1;
    px_bind_worker(worker_state->objs, worker_state->id);
    enum px_progress_state pe = dpu_progress_worker_pe(worker_state);
    enum px_progress_state run = dpu_worker_run_budget(worker_state->objs,
                                                       worker_state, budget);
    if (pe == PX_PROGRESS_PROGRESSED || run == PX_PROGRESS_PROGRESSED)
        return PX_PROGRESS_PROGRESSED;
    if (run == PX_PROGRESS_PENDING ||
        !mpsc_comp_queue_empty(&worker_state->cross_worker))
        return PX_PROGRESS_PENDING;
    return PX_PROGRESS_IDLE;
}

int
dmesh_l7_driver_clear_notifications(void *driver)
{
    struct dpu_data_worker *worker_state = driver;
    doca_notification_handle_t completion = 0;
    if (!worker_state ||
        doca_pe_get_notification_handle(worker_state->pe, &completion) != DOCA_SUCCESS)
        return -1;
    atomic_store_explicit(&worker_state->parked, 0, memory_order_release);
    uint64_t value;
    while (read(worker_state->wake_fd, &value, sizeof(value)) == sizeof(value)) {}
    (void)doca_pe_clear_notification(worker_state->pe, completion);
    int dma_fd = px_worker_notification_fd(worker_state->objs, worker_state->id);
    if (dma_fd >= 0)
        px_worker_clear_notification(worker_state->objs, worker_state->id, dma_fd);
    return 0;
}

int
dmesh_l7_driver_maintenance(void *driver)
{
    struct dpu_data_worker *worker_state = driver;
    if (!worker_state)
        return -1;
    dpu_dpa_nudge_due(worker_state);
    /* Idle peer channels are swept on their own cadence: a channel is idle for
     * a minute before it is worth closing, and maintenance runs every
     * millisecond. */
    if (worker_state->peer_rt) {
        uint64_t now = dpu_wake_clock_now();
        if ((int64_t)(now - worker_state->peer_evict_deadline) >= 0) {
            px_peer_evict_idle(worker_state->objs, worker_state->id);
            worker_state->peer_evict_deadline = now + dpu_wake_clock_hz();
        }
    }
    px_l7_stats_report(worker_state->objs, worker_state->id);
    px_peer_stats_report(worker_state->objs, worker_state->id);
    return 0;
}

int
dmesh_l7_driver_stopped(void *driver)
{
    struct dpu_data_worker *worker_state = driver;
    return !worker_state || worker_state->stop;
}

void
dmesh_l7_driver_ready(void *driver)
{
    struct dpu_data_worker *worker_state = driver;
    if (worker_state)
        atomic_store_explicit(&worker_state->init_state, 1, memory_order_release);
}

void
dmesh_l7_driver_failed(void *driver)
{
    struct dpu_data_worker *worker_state = driver;
    if (worker_state)
        atomic_store_explicit(&worker_state->init_state, -1, memory_order_release);
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
        if (worker_state->running) {
            pthread_join(worker_state->thread, NULL);
            worker_state->running = 0;
        }
        if (worker_state->wake_epfd >= 0) {
            close(worker_state->wake_epfd);
            worker_state->wake_epfd = -1;
        }
        if (worker_state->wake_fd >= 0) {
            close(worker_state->wake_fd);
            worker_state->wake_fd = -1;
        }
        /* Released whether or not the thread ever ran: bring-up takes the
         * listening port before the threads are created. The runtime owns its
         * carrier, so this closes that listener and every connection under it. */
        dmesh_peer_transport_free(worker_state->peer_rt);
        worker_state->peer_rt = NULL;
    }
}

/* Publish DPU readiness after all selected execution paths are initialized. */
static void
dpu_publish_ready_and_setup_pods(struct objects *objs)
{
    objs->dpu_ready = 1;
    int kmax = objs->k_rings > 0 ? objs->k_rings : 1;
    int lmax = objs->n_data_workers > 0 ? objs->n_data_workers : 1;
    int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct pod_state *pod = &objs->pods[i];
        if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE) ||
            pod->init_result != DMESH_POD_INIT_PENDING)
            continue;
        if (pod->ring_mmap_count != kmax || pod->remote_mmap == NULL ||
            pod->host_rx_mmap == NULL ||
            pod->rev_ring_mmap_count != lmax ||
            pod->dma_ready)
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

/* The port a node's first ARM worker listens on for peers. The generation
 * binds one port per node and worker w takes that port plus w, so the two ends
 * of a channel are always the same worker index on both nodes. */
#define DPU_PEER_PORT_DEFAULT 47900

static int
dpu_peer_wire_new(const char *kind, uint32_t bind_ip_be, uint16_t port,
                  const struct peer_wire_ops **ops, void **wctx,
                  char *error, size_t error_len)
{
    if (strcmp(kind, "rdma") == 0)
        return peer_wire_rdma_new(bind_ip_be, port, ops, wctx, error, error_len);
    if (strcmp(kind, "tcp") == 0)
        return peer_wire_tcp_new(bind_ip_be, port, ops, wctx, error, error_len);
    snprintf(error, error_len, "DPUMESH_PEER_TRANSPORT='%s' names no carrier "
                               "(rdma, tcp)", kind);
    return -1;
}

/* Give every ARM worker its own carrier and authenticated-session runtime, so
 * a peer connection is driven by the thread that already owns the streams it
 * carries and no peer state crosses workers.
 *
 * Nothing here is required: a node with DPUMESH_PEER_TRANSPORT unset runs as
 * it does today, with remote destinations refused. A node that asked for a
 * carrier and could not get one keeps starting for the same reason — a fabric
 * that is not up must not take the node's local traffic down with it. */
static void
dpu_peer_bringup(struct objects *objs)
{
    const char *kind = getenv("DPUMESH_PEER_TRANSPORT");
    if (!kind || !*kind)
        return;

    const char *credential = getenv("DPUMESH_NODE_KEY_FILE");
    if (!objs->node_key_ready || !credential || !*credential) {
        DOCA_LOG_WARN("peer carrier '%s' not started: DPUMESH_NODE_KEY_FILE is "
                      "unset, so this node holds no credential to authenticate "
                      "with", kind);
        return;
    }
    if (objs->node_name[0] == '\0') {
        DOCA_LOG_WARN("peer carrier '%s' not started: this node has no name for "
                      "a peer to bind", kind);
        return;
    }

    /* The private half is read here rather than kept from startup: it is the
     * one input the session layer needs and nothing else on the DPU does. */
    uint8_t seed[32];
    uint8_t public_key[32];
    char error[256] = {0};
    if (dmesh_peer_node_key_load(credential, public_key, seed,
                                 error, sizeof(error)) != 0) {
        DOCA_LOG_WARN("peer carrier '%s' not started: %s", kind, error);
        return;
    }
    if (memcmp(public_key, objs->node_public_key, sizeof(public_key)) != 0) {
        explicit_bzero(seed, sizeof(seed));
        DOCA_LOG_WARN("peer carrier '%s' not started: %s no longer holds the "
                      "credential this node published", kind, credential);
        return;
    }

    uint32_t bind_ip_be = htonl(INADDR_ANY);
    const char *bind = getenv("DPUMESH_PEER_BIND");
    if (bind && *bind) {
        struct in_addr address;
        if (inet_pton(AF_INET, bind, &address) != 1) {
            explicit_bzero(seed, sizeof(seed));
            DOCA_LOG_WARN("peer carrier '%s' not started: DPUMESH_PEER_BIND='%s' "
                          "is not an IPv4 address", kind, bind);
            return;
        }
        bind_ip_be = address.s_addr;
    }

    unsigned port_base = DPU_PEER_PORT_DEFAULT;
    const char *port_env = getenv("DPUMESH_PEER_PORT");
    if (port_env && *port_env) {
        unsigned long value = strtoul(port_env, NULL, 10);
        if (value < 1 || value + (unsigned long)objs->n_data_workers > 65535) {
            explicit_bzero(seed, sizeof(seed));
            DOCA_LOG_WARN("peer carrier '%s' not started: DPUMESH_PEER_PORT='%s' "
                          "leaves no room for %d worker ports", kind, port_env,
                          objs->n_data_workers);
            return;
        }
        port_base = (unsigned)value;
    }

    uint64_t handshake_timeout_ns = DMESH_PEER_HANDSHAKE_TIMEOUT_NS;
    const char *timeout_env = getenv("DPUMESH_PEER_HANDSHAKE_TIMEOUT_MS");
    if (timeout_env && *timeout_env) {
        unsigned long ms = strtoul(timeout_env, NULL, 10);
        if (ms >= 1 && ms <= 600000)
            handshake_timeout_ns = (uint64_t)ms * 1000000ull;
    }

    /* Every worker or none: a node whose workers are only partly reachable
     * would carry the streams that landed on one worker and refuse the ones
     * that landed on another, for the same pair of Pods. The runtimes are
     * therefore built first and bound into the proxy only once all of them
     * stand. */
    struct peer_transport_rt *runtimes[MAX_ARM_WORKERS] = {0};
    int built = 0;
    for (; built < objs->n_data_workers; built++) {
        uint16_t port = (uint16_t)(port_base + (unsigned)built);
        const struct peer_wire_ops *ops = NULL;
        void *wire_ctx = NULL;
        if (dpu_peer_wire_new(kind, bind_ip_be, port, &ops, &wire_ctx,
                              error, sizeof(error)) != 0) {
            snprintf(error + strlen(error), sizeof(error) - strlen(error),
                     " (worker %d, port %u)", built, (unsigned)port);
            break;
        }
        struct peer_transport_config config = {
            .node_name = objs->node_name,
            .seed = seed,
            .wire = ops,
            .wire_ctx = wire_ctx,
            .handshake_timeout_ns = handshake_timeout_ns,
        };
        if (dmesh_peer_transport_new(&config, &runtimes[built],
                                     error, sizeof(error)) != 0) {
            ops->ctx_free(wire_ctx);
            break;
        }
    }
    explicit_bzero(seed, sizeof(seed));

    if (built < objs->n_data_workers) {
        for (int s = 0; s < built; s++)
            dmesh_peer_transport_free(runtimes[s]);
        DOCA_LOG_WARN("PEER CARRIER: %s did not come up, so this node refuses "
                      "remote destinations: %s", kind, error);
        return;
    }

    for (int s = 0; s < objs->n_data_workers; s++) {
        if (px_peer_configure(objs, s, dmesh_peer_transport_ops(),
                              runtimes[s]) != 0) {
            /* The proxy refuses only what it refuses for every worker alike,
             * so the first one decides for all of them. */
            for (int t = s; t < objs->n_data_workers; t++)
                dmesh_peer_transport_free(runtimes[t]);
            for (int t = 0; t < s; t++) {
                px_peer_detach(objs, t);
                dmesh_peer_transport_free(objs->data_workers[t].peer_rt);
                objs->data_workers[t].peer_rt = NULL;
            }
            DOCA_LOG_WARN("PEER CARRIER: %s could not be bound to the proxy, so "
                          "this node refuses remote destinations", kind);
            return;
        }
        dmesh_peer_transport_attach(runtimes[s], px_peer_table(objs, s));
        objs->data_workers[s].peer_rt = runtimes[s];
    }

    DOCA_LOG_WARN("PEER CARRIER: %s, %d worker(s) on ports %u-%u",
                  kind, objs->n_data_workers, port_base,
                  port_base + (unsigned)objs->n_data_workers - 1);
}

void
run_dpu_worker(struct objects *objs)
{
    doca_error_t result;

    /* Initialize pod state. */
    memset(objs->pods, 0, sizeof(objs->pods));
    objs->num_pods = 0;

    /* Initialize pod lookup and load-balancer cursors. */
    for (int i = 0; i < POD_ID_SPACE; i++) {
        objs->pod_id_to_slot[i] = -1;
        objs->svc_rr[i]         = 0;
    }

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
    objs->k_rings = DPUMESH_RINGS_PER_POD_DEFAULT;
    { const char *ke = getenv("DPUMESH_RINGS_PER_POD");
      if (ke && *ke) {
          int v = atoi(ke);
          if (v >= 1 && v <= MAX_EU_PER_POD)
              objs->k_rings = v;
      } }

    objs->main_wake_fd = -1;
    atomic_store_explicit(&objs->main_parked, 0, memory_order_relaxed);
    /* Configure ARM data workers. */
    objs->n_data_workers = 1;
    for (int s = 0; s < MAX_ARM_WORKERS; s++) {
        struct dpu_data_worker *worker_state = &objs->data_workers[s];
        worker_state->objs = objs;
        worker_state->id = s;
        worker_state->pe = NULL;
        worker_state->queue.head = worker_state->queue.tail = 0;
        mpsc_comp_queue_init(&worker_state->cross_worker);
        atomic_init(&worker_state->stat_local_completions, 0);
        atomic_init(&worker_state->stat_cross_worker_out, 0);
        atomic_init(&worker_state->stat_cross_worker_in, 0);
        worker_state->num_deferred_recv = 0;
        worker_state->wake_fd = -1;
        worker_state->wake_epfd = -1;
        worker_state->peer_rt = NULL;
        worker_state->peer_evict_deadline = 0;
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
    dpu_arm_affinity_init();
    DOCA_LOG_WARN("Requested data topology: K/A=%d/%d (N finalized after DPA query)",
                  objs->k_rings, objs->n_data_workers);
    DOCA_LOG_WARN("ARM DATA WORKERS = %d", objs->n_data_workers);

    /* 1. comch control path server (waits for first connection) */
    result = init_comch_ctrl_path_server("DPUMesh", objs);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init comch control path server: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* 2. ARM consumer PEs for DPA completion channels. */
    result = doca_pe_create(&objs->consumer_pe);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create the first DPA completion PE: %s",
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
    dpu_arm_pin_current("main", objs->n_data_workers);

    /* SG-DMA DPU-to-host path and request parser. */
    result = px_init(objs);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init L7-proxy L4 engine: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* The inter-node carrier, once the proxy holds the tables it binds into
     * and before any worker thread can look for one. */
    dpu_peer_bringup(objs);

    /* Each ARM data worker owns one consumer PE and wake fd. */
    {
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
            /* A worker with a peer carrier has two things that wake it from
             * outside its own engine, and its runtime waits on one descriptor.
             * Collect both here rather than widening that contract. */
            if (worker_state->peer_rt) {
                int carrier = dmesh_peer_transport_epfd(worker_state->peer_rt);
                struct epoll_event wake = { .events = EPOLLIN, .data = { .u32 = 1 } };
                struct epoll_event peer = { .events = EPOLLIN, .data = { .u32 = 2 } };
                worker_state->wake_epfd = epoll_create1(EPOLL_CLOEXEC);
                if (worker_state->wake_epfd < 0 || carrier < 0 ||
                    epoll_ctl(worker_state->wake_epfd, EPOLL_CTL_ADD,
                              worker_state->wake_fd, &wake) != 0 ||
                    epoll_ctl(worker_state->wake_epfd, EPOLL_CTL_ADD,
                              carrier, &peer) != 0) {
                    DOCA_LOG_ERR("ARM worker %d peer wake set-up failed", s);
                    stop_data_workers(objs);
                    cleanup_objects(objs);
                    return;
                }
            }
            if (pthread_create(&worker_state->thread, NULL, dpu_data_worker_main, worker_state) != 0) {
                DOCA_LOG_ERR("ARM worker %d thread creation failed", s);
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
                DOCA_LOG_ERR("ARM worker %d polling loop did not initialize", s);
                stop_data_workers(objs);
                cleanup_objects(objs);
                return;
            }
        }
        DOCA_LOG_WARN("ARM DATA WORKER THREADS = %d", objs->n_data_workers);
    }

    /* The main thread owns control messages and reverse-ring doorbells. */
    {
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
            DOCA_LOG_ERR("main control event-loop setup failed for %d data workers",
                         objs->n_data_workers);
            stop_data_workers(objs);
            cleanup_objects(objs);
            return;
        }

        dpu_publish_ready_and_setup_pods(objs);
        /* The 1 ms backstop remains for control/membership progress. DPA live
         * EUs use a same-EU helper for watchdog handoff and need no ARM tick. */
        DOCA_LOG_WARN("MAIN CONTROL/DOORBELL: workers=%d, notification-driven "
                      "(1 ms backstop tick)", objs->n_data_workers);
        uint64_t health_period = dpu_wake_clock_hz();
        if (health_period == 0)
            health_period = 1;
        uint64_t health_deadline = dpu_wake_clock_now() + health_period;
        int dpa_fatal_reported = 0;
        while (true) {
            uint64_t now = dpu_wake_clock_now();
            if (!dpa_fatal_reported &&
                (int64_t)(now - health_deadline) >= 0) {
                doca_error_t health = doca_dpa_peek_at_last_error(objs->dpa);
                if (health != DOCA_SUCCESS) {
                    DOCA_LOG_ERR("DPA fatal state detected: %s",
                                 doca_error_get_descr(health));
                    dpa_fatal_reported = 1;
                }
                health_deadline = now + health_period;
            }
            int progressed = dpu_drain_iteration(objs);
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
    }
}
