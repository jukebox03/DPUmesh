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

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>         /* sched_yield in the ingest reaper thread */
#include <pthread.h>       /* ingest reaper thread */
#include <sys/epoll.h>     /* event-driven DPU main loop (epoll on PE handles) */
#include <sys/eventfd.h>   /* event-driven reaper -> main wake (no busy-spin) */

DOCA_LOG_REGISTER(DPU_WORKER);

/* Per-thread flag: 1 on the ingest reaper/shard thread. batch_or_send_tx_ack checks it
 * to DEFER its (rare, FIN/error/drop) TX_ACKs to main instead of touching the comch
 * send path (objs->pe is single-threaded on main). Main + egress workers leave it 0. */
__thread int dpu_t_is_ingest = 0;

/* Defined in dpa.c: the recv-cb reads it to route completions to the reaping
 * shard's queue (Design A, M>=2). Each shard thread sets it to its id. */
extern __thread int dpu_reap_shard;

/* Diagnostic (DPUMESH_DIAG=1): once/sec dump of the emit-batch state to root-cause a
 * TX_ACK/REV_DONE starvation hang. g_lull_hits/g_drain_iters count how often main's
 * idle-flush (lull) actually fires vs total drain iterations. */
static int g_dpu_diag = 0;
static uint64_t g_lull_hits = 0, g_drain_iters = 0;
/* batch-16 hang debug: TX_ACK flush-size histogram + single-send fallbacks.
 * Cross-checked against the host's ACKB rx counters to localize a loss. */
static uint64_t g_fl_total = 0, g_fl_n15 = 0, g_fl_n16 = 0, g_single_acks = 0;

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
        /* COALESCE, don't append: the host's reclaim is cumulative (tx_reclaim_ack pops
         * every unit at-or-before `seq`), so a newer seq for the same (conn,port) fully
         * subsumes a queued older one. That bounds this queue by the number of live PORTS
         * instead of the message rate — which is what stops it from ever filling and
         * dropping an ack. Dropping the LAST ack of a conn is unrecoverable: the host's
         * tx_f never reaches tx_w, so try_return_blocks never fires and the port stays
         * FREE-but-draining — its blocks AND its port number leak for the process's life.
         * The scan is O(live ports), and only on this already-stalled path. */
        for (int i = 0; i < objs->num_deferred_tx_acks; i++) {
            deferred_tx_ack_t *d = &objs->deferred_tx_acks[i];
            if (d->conn != src_pod->connection || d->port != port)
                continue;
            if ((uint16_t)(seq - d->seq) < 0x8000u)   /* keep the newer of the two */
                d->seq = seq;
            return;
        }
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

/* Accumulate one TX_ACK into the src pod's batch; flush when full. Falls back to
 * the single-send path when the pod is gone or the batch is already full and a
 * prior flush is still pending (AGAIN).
 * Non-static: the proxy engine (dpu_proxy.c) releases custody through this. */
void
batch_or_send_tx_ack(struct objects *objs, struct pod_state *src_pod,
                     uint16_t port, uint16_t seq)
{
    /* Ingest thread (reaper): must not touch the comch send path (objs->pe is
     * single-threaded on main). Enqueue (pod_id,port,seq); main drains + sends. */
    if (dpu_t_is_ingest) {
        if (!src_pod) return;   /* target gone → nothing to ack */
        pthread_mutex_lock(&objs->pending_txack_lock);
        int cap = (int)(sizeof(objs->pending_txack) / sizeof(objs->pending_txack[0]));
        if (objs->pending_txack_n < cap) {
            int n = objs->pending_txack_n++;
            objs->pending_txack[n].pod_id = src_pod->pod_id;
            objs->pending_txack[n].port   = port;
            objs->pending_txack[n].seq    = seq;
            pthread_mutex_unlock(&objs->pending_txack_lock);
        } else {
            pthread_mutex_unlock(&objs->pending_txack_lock);
            static uint64_t pend_full;
            if ((pend_full++ & 0xFFFFu) == 0)
                DOCA_LOG_ERR("pending_txack full (total %llu) — main not draining; port=%u",
                             (unsigned long long)pend_full, port);
        }
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

/* ====== Batched REV_DONE (mirror of batched TX_ACK) ======
 * The single host PE thread reaping one REV_DONE comch msg per response is the
 * 2-pod cap; coalescing K responses into one msg cuts the PE reap rate K-fold. */

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

/* ====== L4 routing ======
 *
 * dpu_route_l4() is the single point where the DPU picks the destination pod for a
 * forward segment: LOAD-BALANCE over the service's live backend set. It holds NO
 * affinity state — affinity belongs to the caller (px_resolve_backend: conn-sticky
 * pin, or an L7 host override), which is where the conn's lifetime is known. Called
 * by the SG-DMA egress engine (dpu_proxy.c) for DEFERred request segs; the L7 hook
 * rides the same LB via dmesh_l7_ctx.hosts.
 *
 * The backend SET is DERIVED from pods[] on demand (registered + service_id + dma_ready)
 * — pods[] is the single source of truth, so a disconnected backend leaves the set
 * with no bookkeeping (no stale-entry blackhole). Envoy parity: a service == a
 * cluster; pods[] filtered by service_id == the cluster's healthy endpoints.
 */
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

/* ROUND_ROBIN load balancer over the service's live backend set (Envoy default
 * policy). Single ARM thread advances svc_rr → no lock. -1 = no healthy backend. */
static inline int32_t
lb_pick(struct objects *objs, int16_t svc)
{
    int32_t hosts[MAX_PODS];
    int n = collect_live_hosts(objs, svc, hosts);
    if (n <= 0)
        return -1;
    /* Per-service RR cursor. Sharded ingest advances it from M threads — a relaxed
     * atomic keeps it race-free; a rare double-pick only nudges LB balance (benign). */
    uint32_t i = __atomic_fetch_add(&objs->svc_rr[svc], 1, __ATOMIC_RELAXED);
    return hosts[i % (uint32_t)n];
}

/* L4 default route: lb_pick (RR over the derived live backend set). There is no
 * service->backend table — see collect_live_hosts. The single point that resolves a
 * DEFERred request seg's backend for the SG-DMA egress engine (dpu_proxy.c →
 * dpu_route_l4).
 *
 * This is the LB pick ALONE — it carries no affinity of its own. Affinity is the
 * CALLER's (px_resolve_backend): a conn is sticky to the backend its first message
 * picked here, and an L7 hook may override the host outright.
 *
 * Unknown/empty service → -1 (caller drops + TX_ACKs the sender). */
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

/* Drain the ingest thread's deferred TX_ACKs (pending_txack) and send them on the
 * main thread (the only thread allowed to touch the comch send path). LIFO pop is
 * fine — TX_ACKs are independent slot frees. Bounded per call so a flood cannot
 * starve the rest of the main loop. Returns the number sent. */
static int
drain_pending_txack(struct objects *objs)
{
    int sent = 0;
    while (sent < 8192) {
        pthread_mutex_lock(&objs->pending_txack_lock);
        if (objs->pending_txack_n == 0) { pthread_mutex_unlock(&objs->pending_txack_lock); break; }
        int idx = --objs->pending_txack_n;
        int32_t  pod_id = objs->pending_txack[idx].pod_id;
        uint16_t port   = objs->pending_txack[idx].port;
        uint16_t seq    = objs->pending_txack[idx].seq;
        pthread_mutex_unlock(&objs->pending_txack_lock);
        batch_or_send_tx_ack(objs, find_pod_by_id(objs, pod_id), port, seq);
        sent++;
    }
    return sent;
}

/*
 * Process up to max_batch entries from the deferred completion queue.
 * Called from the main loop to avoid blocking inside consumer callbacks.
 * Every forward completion (request AND reply) feeds the per-conn input window
 * → proxy_route (mock) → per-dst SG-DMA egress engine (dpu_proxy.c). All comch
 * sends happen here — never inside consumer callbacks, which would risk
 * re-entrant doca_pe_progress. Returns number of entries processed.
 */
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

        /* Returns 1 consumed, 0 retry (alloc/pool pressure — retain entry),
         * -1 dropped (sender already TX_ACKed). Shard 0: the single-reaper /
         * inline path (its private structures ARE the shared ones for M<=1). */
        int result = px_ingest_forward(objs, 0, entry);
        if (result == 0)
            break;  /* engine backpressure, retry next iteration */

        comp_queue_dequeue(&objs->comp_queue);
        processed++;
    }

    /* Resume conns the egress backpressured (px_ship_seg found a pool empty and left
     * their bytes in the window). Runs even when no completion arrived — a stalled conn
     * is waiting on an egress unit, not on new input. Counts as processed only if it
     * really advanced, so an unrelieved pool still lets the loop park. */
    processed += px_drain_stalled(objs, 0);

    return processed;
}

/* ====== Ingest sharding — Design A (M consumer PEs, one per shard) ======
 *
 * Each shard OWNS one consumer PE (consumer_pes[id]); DPA channel k is bound to
 * consumer_pes[k % M] (comch_msgq.c), so a shard SLEEPS directly on its own PE's
 * DOCA notification fd — no reaper, no per-message eventfd on the forward path. It
 * reaps its PE (recv-cb fills its queue), then processes: a completion is handled
 * by the shard whose EU it landed on, EXCEPT a ② backend reply, whose session is
 * owned by the CLIENT's shard (encoded in the up_port) — that one is handed to the
 * owner via its cross-shard inbox (the diagram's lock-free ring, MPSC). ① (shared
 * conntrack under routing_lock) never crosses. Each shard sends the DPA WAKE to ITS
 * OWN channels only (k%M==id) — the send producer is bound to that shard's PE, so
 * the WAKE-race fix holds per-shard. Comch sends stay on main (TX_ACK →
 * pending_txack, REV_DONE → egress lanes' done-queue main drains). */

/* The shard that must PROCESS this completion (own its conn window + conntrack).
 * ② reply (up_port leg) → the up_port's owner shard; else the shard it landed on. */
static inline int
owner_shard(struct objects *objs, int here, const dpu_comp_entry_t *e)
{
    if (!objs->shard_shared_routing &&
        e->dst_pod_id != DMESH_POD_BLANK && e->src_port >= DMESH_UPORT_BASE)
        return px_uport_owner(e->src_port, objs->n_ingest_shards);
    return here;                         /* ① / forward request → handle where it landed */
}

/* Wake a parked shard (edge via its eventfd; a missed edge is caught by its 1 ms
 * epoll backstop). Used only for the ② cross-shard hand-off at low load. */
static void
dpu_wake_shard(struct dpu_ingest_shard *sh)
{
    if (sh->wake_fd >= 0 &&
        atomic_load_explicit(&sh->parked, memory_order_acquire)) {
        uint64_t one = 1;
        ssize_t n = write(sh->wake_fd, &one, sizeof(one));
        (void)n;
    }
}

/* Hand a ② reply completion to its owner shard's cross-shard inbox. Producers
 * serialize on xshard_lock (MPSC); the owner shard consumes lock-free (single
 * consumer). Returns 0, or -1 if the inbox is full (caller retries). */
static int
xshard_handoff(struct objects *objs, int owner, const dpu_comp_entry_t *e)
{
    struct dpu_ingest_shard *dst = &objs->ingest_shards[owner];
    pthread_mutex_lock(&dst->xshard_lock);
    int rc = comp_queue_enqueue(&dst->xshard, e);
    pthread_mutex_unlock(&dst->xshard_lock);
    return rc;
}

/* ====== DPU Worker ====== */

/* Consumer-side REAP: progress consumer_pe (DPA forward completions → comp_queue),
 * release recv backpressure (resubmit deferred recv tasks once comp_queue drains
 * below BP_LOW), and drain the consumer_retry stash. Every step here touches
 * consumer_pe / its recv-task pool, so it MUST run on exactly one thread — the
 * ingest reaper when active, else inline on the main loop. Returns whether the
 * consumer PE advanced. */
static int
dpu_reap_iteration(struct objects *objs)
{
    int did_consumer = doca_pe_progress(objs->consumer_pe);

    /* Backpressure release: resubmit deferred recv tasks once the ingest
     * hand-off (comp_queue) drains below BP_LOW. */
    if (objs->num_deferred_recv > 0 &&
        ingest_usage(objs) < COMP_QUEUE_BP_LOW) {
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

/* Wake the main loop if it is parked in epoll (edge signal via the eventfd). Guarded
 * by main_parked so there is NO write() per message while main is spinning under load
 * (main only parks at idle). A missed edge is harmless: main's epoll has a 1 ms
 * keepalive timeout that re-polls, so the eventfd is a latency optimization, not a
 * correctness dependency. Non-static: egress workers (dpu_proxy.c) also wake main when
 * an SG-DMA batch completes so emit is prompt at low load. */
void
dpu_wake_main(struct objects *objs)
{
    if (objs->reaper_wake_fd >= 0 &&
        atomic_load_explicit(&objs->main_parked, memory_order_acquire)) {
        uint64_t one = 1;
        ssize_t n = write(objs->reaper_wake_fd, &one, sizeof(one));
        (void)n;
    }
}

static void dpu_send_wake(struct objects *objs);   /* fwd decl: the reaper sends the DPA keepalive */

/* Ingest reaper/shard thread (DPUMESH_INGEST_REAP=1). Owns consumer_pe and runs the
 * ingest half of the pipeline — REAP (consumer_pe → comp_queue) AND PROCESS (comp_queue
 * → px_ingest_forward: parse/route) — off the single main funnel; main is left with
 * emit + ctrl + sends (diagram ①). EVENT-DRIVEN, exactly like the original loop handled
 * consumer_pe: SLEEP on its notification fd (arm → re-check → epoll_wait, 1 ms backstop),
 * wake on a real DPA completion — NO busy-spin. Wakes main (if parked) when it produced
 * egress work / deferred TX_ACKs. */
static void *
dpu_reaper_main(void *arg)
{
    struct objects *objs = (struct objects *)arg;
    dpu_t_is_ingest = 1;   /* our TX_ACKs defer to main (comch send is main-only) */

    doca_notification_handle_t cfd = 0;
    int ep = -1;
    if (doca_pe_get_notification_handle(objs->consumer_pe, &cfd) == DOCA_SUCCESS)
        ep = epoll_create1(0);
    if (ep >= 0) {
        struct epoll_event ec = { .events = EPOLLIN, .data = { .u32 = 0 } };  /* consumer_pe */
        if (epoll_ctl(ep, EPOLL_CTL_ADD, (int)cfd, &ec) != 0) { close(ep); ep = -1; }
    }
    if (ep < 0) {
        /* Without a notification fd the reaper cannot sleep; busy-spin is forbidden,
         * so stop and let the caller fall back to the inline (reaper-off) driver. */
        DOCA_LOG_ERR("reaper: consumer_pe epoll setup failed — reaper disabled");
        objs->reaper_active = 0;
        objs->reaper_stop = 1;
        return NULL;
    }

    /* The DPA WAKE keepalive (poke a parked EU ~1 kHz) is a comch producer send on
     * the dpa_comch channels — which are bound to consumer_pe. Only the consumer_pe
     * OWNER may submit on it, so the reaper (not main) sends it while the reaper is
     * active; otherwise main's submit races the reaper's doca_pe_progress and corrupts
     * the PE (the DPA then parks forever → forward ingest freezes). */
    struct timespec r_last, r_now;
    clock_gettime(CLOCK_MONOTONIC, &r_last);
    while (!objs->reaper_stop) {
        clock_gettime(CLOCK_MONOTONIC, &r_now);
        if ((r_now.tv_sec - r_last.tv_sec) + (r_now.tv_nsec - r_last.tv_nsec) / 1e9 >= 0.001) {
            dpu_send_wake(objs);
            r_last = r_now;
        }
        int did  = dpu_reap_iteration(objs);            /* reap DPA completions → comp_queue */
        int proc = process_completion_queue(objs, 128); /* parse/route (ingest) → egress lanes */
        if (did || proc) {
            dpu_wake_main(objs);      /* egress work / deferred TX_ACKs → let main emit+send */
            continue;                 /* stay hot while there is work */
        }
        /* Idle: arm consumer_pe, RE-CHECK once (close the drain→arm race), then block
         * on epoll (1 ms backstop to re-poll even if a notification is missed). */
        (void)doca_pe_request_notification(objs->consumer_pe);
        did  = dpu_reap_iteration(objs);
        proc = process_completion_queue(objs, 128);
        if (did || proc) {
            (void)doca_pe_clear_notification(objs->consumer_pe, cfd);
            dpu_wake_main(objs);
            continue;
        }
        struct epoll_event evs[1];
        (void)epoll_wait(ep, evs, 1, 1);
        (void)doca_pe_clear_notification(objs->consumer_pe, cfd);
    }
    close(ep);
    return NULL;
}

/* One pass of the DPU worker's drain work: reap the consumer PE (unless the reaper
 * thread owns it), progress the ctrl PE, retry deferred TX_ACKs, drain the
 * completion queue, run egress. Returns non-zero if any progress was made. The
 * event-driven driver uses this to detect the idle point; the busy-poll driver
 * calls it once per iteration. Shared by both drivers so they cannot drift. */
static int
dpu_drain_iteration(struct objects *objs)
{
    /* Reap + process inline unless the reaper thread owns ingest. When the reaper is
     * active it does BOTH reap (consumer_pe → comp_queue) AND process (comp_queue →
     * px_ingest_forward: parse/route). consumer_pe/deferred_recv/consumer_retry and the
     * proxy conn-table/conntrack are then its exclusive domain; main is left with
     * ctrl + emit + all comch sends. */
    uint8_t did_consumer = objs->reaper_active ? 0 : (uint8_t)dpu_reap_iteration(objs);
    uint8_t did_ctrl     = doca_pe_progress(objs->pe);  /* new conns, REGISTER, TX_DATA */

    /* Retry deferred TX_ACKs right after pe_progress so just-released send-pool
     * slots are available. */
    drain_deferred_tx_acks(objs);

    /* Send the TX_ACKs the ingest thread deferred to us (comch send is main-only). */
    int sent_pend = objs->reaper_active ? drain_pending_txack(objs) : 0;

    /* Drain deferred completion queue (proxy ingest) — only when reaper OFF; the
     * reaper does process_completion_queue itself when active. */
    int proc = objs->reaper_active ? 0 : process_completion_queue(objs, 128);

    /* SG-DMA egress: submit queued per-dst batches + emit completed batches'
     * REV_DONE entries and custody TX_ACKs (the unified DPU→host reverse path). */
    int px_progressed = px_drain(objs);

    /* Lull → flush partial TX_ACK + REV_DONE batches so low-load latency is not held
     * by coalescing. reaper-OFF: unchanged (proc==0). reaper-ON: main never processes,
     * so a lull = egress + deferred-txack idle. */
    int lull = objs->reaper_active ? (!px_progressed && sent_pend == 0) : (proc == 0);
    if (lull)
        for (int i = 0; i < objs->num_pods; i++) {
            flush_txack_batch(objs, &objs->pods[i]);
            flush_rev_done_batch(objs, &objs->pods[i]);
        }

    if (g_dpu_diag) { g_drain_iters++; if (lull) g_lull_hits++; }
    return (did_consumer || did_ctrl || proc > 0 || px_progressed || sent_pend > 0);
}

/* DIAG dump (once/sec): per-pod TX_ACK/REV_DONE batch fill + pending_txack + how often
 * the idle-flush fired. A pod stuck with tx>0/rev>0 while lull=0 == starvation (batch
 * below threshold + no idle-flush). Shard queue depths show ingest backlog.
 * QUIET AT IDLE: skipped while nothing moved since the last dump (no queued/batched
 * work and all flush counters frozen) — the line before silence carries the final
 * counters, so a fully-idle hang still leaves its state in the log without the
 * 1 Hz heartbeat flooding it. Any activity resumes the once/sec cadence. */
static void
dpu_diag_dump(struct objects *objs)
{
    static uint64_t prev_fl, prev_sgl, prev_stalls;
    static int prev_pend = -1;   /* -1 → always print the first dump */
    int busy = (objs->pending_txack_n != 0);
    for (int s = 0; s < objs->n_ingest_shards && !busy; s++)
        busy |= (comp_queue_usage(&objs->ingest_shards[s].queue) != 0);
    {
        int np0 = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
        for (int i = 0; i < np0 && !busy; i++)
            busy |= (objs->pods[i].txack_batch_n != 0 ||
                     objs->pods[i].rev_done_batch_n != 0);
    }
    /* A conn backpressured by px_ship_seg is parked on a pool and moves nothing, so it
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
                  objs->pending_txack_n,
                  (unsigned long long)g_fl_total, (unsigned long long)g_fl_n15,
                  (unsigned long long)g_fl_n16, (unsigned long long)g_single_acks);
    for (int s = 0; s < objs->n_ingest_shards && n < (int)sizeof(buf) - 60; s++)
        n += snprintf(buf + n, sizeof(buf) - n, "%u ",
                      comp_queue_usage(&objs->ingest_shards[s].queue));
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

/* Send DPA_MSG_WAKE to every running EU (the ~1 ms keepalive). A parked EU is not
 * woken by a silent forward-ring desc->valid=1 store (no completion), so the ARM
 * pokes it on cadence; it re-scans its rings on the WAKE. No-op under load. */
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

/* ====== Design-A shard thread body (M consumer PEs) ====== */

/* Send the DPA WAKE keepalive to THIS shard's channels only (k % M == id). The
 * send producer for channel k is bound to consumer_pes[k%M] = this shard's PE, so
 * only this shard's thread may submit on it (PE thread-safety / WAKE-race fix). */
static void
dpu_send_wake_shard(struct objects *objs, int id)
{
    if (!objs->dpa_thread_running_any)
        return;
    struct comch_msg trigger;
    memset(&trigger, 0, sizeof(trigger));
    trigger.type = DPA_MSG_WAKE;
    int M = objs->n_ingest_shards;
    for (int k = id; k < objs->num_dpa_threads; k += M)
        if (objs->dpa_thread_running[k])
            (void)dmesh_doca_dpa_msgq_send_try(&objs->dpa_comches[k]->send,
                                                &trigger, sizeof(trigger));
}

/* Per-shard REAP: progress THIS shard's PE (recv-cb fills sh->queue), release its
 * recv backpressure, and — shard 0 only, which owns the data-path consumer's PE —
 * drain the consumer_retry stash. Returns whether the PE advanced. */
static int
dpu_reap_shard_pe(struct objects *objs, struct dpu_ingest_shard *sh)
{
    int did = doca_pe_progress(sh->pe);
    if (sh->num_deferred_recv > 0 &&
        comp_queue_usage(&sh->queue) < COMP_QUEUE_BP_LOW) {
        int remaining = 0, original = sh->num_deferred_recv;
        for (int i = 0; i < original; i++) {
            struct doca_task *t = sh->deferred_recv[i];
            if (doca_task_submit(t) != DOCA_SUCCESS)
                sh->deferred_recv[remaining++] = t;
        }
        sh->num_deferred_recv = remaining;
    }
    if (sh->id == 0)
        objects_drain_consumer_retry(objs);
    return did;
}

/* Process this shard's reaped completions (sh->queue) + its cross-shard inbox.
 * A completion handled locally (owner == self) → px_ingest_forward; a ② reply for
 * another shard → xshard_handoff to its owner. Both queues drain FRONT-stable so an
 * engine-backpressure (r==0) just leaves the front entry for the next pass (order
 * preserved). Returns the number of completions made progress on. */
static int
dpu_shard_run(struct objects *objs, struct dpu_ingest_shard *sh)
{
    int did = 0, woke[MAX_INGEST_SHARDS] = { 0 };

    /* own reaped completions (this shard's EU) */
    for (int n = 0; n < 512; n++) {
        dpu_comp_entry_t *e = comp_queue_peek(&sh->queue);
        if (!e)
            break;
        int owner = owner_shard(objs, sh->id, e);
        if (owner == sh->id) {
            if (px_ingest_forward(objs, sh->id, e) == 0)
                break;                   /* engine backpressure — retain, retry */
        } else {
            if (xshard_handoff(objs, owner, e) != 0)
                break;                   /* owner inbox full — retain, retry */
            woke[owner] = 1;
        }
        comp_queue_dequeue(&sh->queue);
        did++;
    }

    /* ② cross-shard inbox: replies other shards handed to us. Single consumer (this
     * shard) → lock-free peek/dequeue; producers serialize on xshard_lock. */
    for (int n = 0; n < 512; n++) {
        dpu_comp_entry_t *xe = comp_queue_peek(&sh->xshard);
        if (!xe)
            break;
        if (px_ingest_forward(objs, sh->id, xe) == 0)
            break;                       /* backpressure — leave the front, retry */
        comp_queue_dequeue(&sh->xshard);
        did++;
    }

    /* Resume this shard's egress-backpressured conns (see process_completion_queue).
     * Shard-local list, shard-local conn table → no lock. */
    did += px_drain_stalled(objs, sh->id);

    for (int s = 0; s < objs->n_ingest_shards; s++)
        if (woke[s])
            dpu_wake_shard(&objs->ingest_shards[s]);
    return did;
}

/* Ingest shard thread (Design A, DPUMESH_INGEST_SHARDS>=2). OWNS consumer_pes[id]
 * and SLEEPS directly on its DOCA notification fd (+ its cross-shard wake eventfd),
 * exactly like the single reaper sleeps on consumer_pe — event-driven, NO busy-spin.
 * Reaps its PE, processes (with ② cross-shard hand-off), sends the DPA WAKE to its
 * own channels, and wakes main to emit. */
static void *
dpu_shard_main(void *arg)
{
    struct dpu_ingest_shard *sh = (struct dpu_ingest_shard *)arg;
    struct objects *objs = sh->objs;
    dpu_t_is_ingest = 1;                 /* our TX_ACKs defer to main (comch send is main-only) */
    dpu_reap_shard = sh->id;             /* recv-cb routes completions to sh->queue */

    doca_notification_handle_t cfd = 0;
    int ep = -1;
    if (doca_pe_get_notification_handle(sh->pe, &cfd) == DOCA_SUCCESS)
        ep = epoll_create1(0);
    if (ep >= 0) {
        struct epoll_event ec = { .events = EPOLLIN, .data = { .u32 = 0 } };  /* own PE fd */
        struct epoll_event ew = { .events = EPOLLIN, .data = { .u32 = 1 } };  /* cross-shard wake */
        if (epoll_ctl(ep, EPOLL_CTL_ADD, (int)cfd, &ec) != 0 ||
            epoll_ctl(ep, EPOLL_CTL_ADD, sh->wake_fd, &ew) != 0) { close(ep); ep = -1; }
    }
    if (ep < 0) {
        DOCA_LOG_ERR("ingest shard %d: consumer_pe epoll setup failed — shard disabled", sh->id);
        return NULL;
    }

    struct timespec last, now;
    clock_gettime(CLOCK_MONOTONIC, &last);
    while (!sh->stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9 >= 0.001) {
            dpu_send_wake_shard(objs, sh->id);
            last = now;
        }
        int did = dpu_reap_shard_pe(objs, sh);
        int run = dpu_shard_run(objs, sh);
        if (did || run) {
            dpu_wake_main(objs);
            continue;                    /* stay hot while there is work */
        }
        /* Idle: arm own PE, PARK, RE-CHECK (close the reap→park + xshard→park races),
         * then sleep on {own PE fd, cross-shard wake}, 1 ms backstop. */
        (void)doca_pe_request_notification(sh->pe);
        atomic_store_explicit(&sh->parked, 1, memory_order_release);
        if (dpu_reap_shard_pe(objs, sh) || dpu_shard_run(objs, sh) ||
            !comp_queue_empty(&sh->xshard)) {
            atomic_store_explicit(&sh->parked, 0, memory_order_release);
            (void)doca_pe_clear_notification(sh->pe, cfd);
            dpu_wake_main(objs);
            continue;
        }
        struct epoll_event evs[2];
        (void)epoll_wait(ep, evs, 2, 1);
        atomic_store_explicit(&sh->parked, 0, memory_order_release);
        uint64_t drain; ssize_t rn = read(sh->wake_fd, &drain, sizeof(drain)); (void)rn;
        (void)doca_pe_clear_notification(sh->pe, cfd);
    }
    close(ep);
    return NULL;
}

void
run_dpu_worker(struct objects *objs)
{
    doca_error_t result;

    /* Event-driven main loop: the ARM SLEEPS on epoll over the two PE notification
     * handles, waking on a real DPA→DPU completion (FWD_DONE/REV_DONE), a host
     * control message, OR a ~1 ms epoll timeout. On each tick it sends the 1 ms
     * DPU→DPA WAKE keepalive (the DPA EU parks when idle and a silent desc->valid=1
     * store can't wake it, so the ARM pokes it ~1 kHz). This is NOT busy-poll — the
     * ARM sleeps between ticks, so idle CPU is a few % (≈1 kHz wakeups), vs a full
     * core for busy-poll. If the epoll setup fails, it falls back to busy-poll. */
    const double keepalive_sec = 0.001;   /* 1 ms DPU→DPA WAKE cadence */
    struct timespec now, last_kick;
    double kick_elapsed;

    DOCA_LOG_INFO("Starting DPU worker");

    /* Init pods table. See object.h pods[] concurrency model — lock-free
     * with atomic publication on `registered`; no mutex needed. */
    memset(objs->pods, 0, sizeof(objs->pods));
    objs->num_pods = 0;

    /* find_pod_by_id's O(1) pod_id->slot map starts empty (-1 = no live pod). The
     * per-service LB cursor starts at 0. Done before any PE/thread starts, so plain
     * stores are race-free here. (The service->backend set is derived from pods[]
     * live, so there is no service table to initialize.) */
    for (int i = 0; i < POD_ID_SPACE; i++) {
        objs->pod_id_to_slot[i] = -1;
        objs->svc_rr[i]         = 0;
    }

    /* Init DPU connection tracking (model B). Heap-allocated — too large to embed
     * on the stack. calloc zeroes every in_use flag; next_uport starts at BASE. */
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

    /* Not ready until DPA + msgq init completes (below). Gates process_mmap_msg's
     * setup_pod_dma so a fast host's early mmaps don't set up before the msgq. */
    objs->dpu_ready = 0;

    /* N (DPA EU threads) + K (forward rings/pod). Defaults are the measured config
     * (N=4 EUs, K=2 rings, so a 2-pod pair spreads across 4 distinct EUs; K=2 is the
     * sweet spot, K=4 is flat — the DPA op-rate caps ~810K dma_copy/s), each overridable
     * by env (DPUMESH_DPA_THREADS / DPUMESH_RINGS_PER_POD). Set BEFORE the comch server
     * accepts pods so process_mmap_msg sees the right K (else it rejects a pod's 2nd
     * forward ring and that pod never reaches dma_ready). init_dpa_objects re-checks
     * `<= 0`, so it reuses these. N is clamped to MAX_DPA_EU (the dpa_threads[] cap). */
    objs->num_dpa_threads = DPA_THREADS_DEFAULT;   /* tentative (for the K clamp below); AUTO-DETECTED in init_dpa_objects */
    objs->dpa_threads_auto = 1;
    { const char *ne = getenv("DPUMESH_DPA_THREADS");
      if (ne && *ne) { int v = atoi(ne);
          if (v >= 1) { if (v > MAX_DPA_EU) v = MAX_DPA_EU; objs->num_dpa_threads = v; objs->dpa_threads_auto = 0; } } }
    /* K = forward rings per pod (EU-sharding). Deploy option DPUMESH_RINGS_PER_POD
     * (default 2); clamped to [1, min(N, MAX_EU_PER_POD)]. The HOST reads the SAME
     * env (dmesh_core.c) and MUST land on the same K — forward rings pair 1:1, a
     * mismatch makes a pod never reach dma_ready. A conn pins to ONE ring
     * (src_port % K); different conns spread across the K rings. K≤N (=4). */
    objs->k_rings = DPUMESH_RINGS_PER_POD_DEFAULT;   /* = 2 */
    { const char *ke = getenv("DPUMESH_RINGS_PER_POD");
      if (ke && *ke) {
          int v = atoi(ke);
          if (v >= 1) {
              if (v > objs->num_dpa_threads) v = objs->num_dpa_threads;
              if (v > MAX_EU_PER_POD) v = MAX_EU_PER_POD;
              objs->k_rings = v;
          }
      } }
    DOCA_LOG_WARN("K forward rings/pod = %d (DPUMESH_RINGS_PER_POD; N=%d)",
                  objs->k_rings, objs->num_dpa_threads);

    /* Ingest reaper (DPUMESH_INGEST_REAP=1): run the DPA forward-completion REAP
     * on a dedicated thread so it no longer shares the single main funnel with
     * parse/route/egress/emit/sends. Default off = reap inline on the main loop
     * (original behaviour). Spawned just before the main loop (below). */
    objs->reaper_active = 0;
    objs->reaper_stop = 0;
    objs->reaper_wake_fd = -1;                       /* -1 → dpu_wake_main is a no-op */
    atomic_store_explicit(&objs->main_parked, 0, memory_order_relaxed);
    objs->pending_txack_n = 0;
    pthread_mutex_init(&objs->pending_txack_lock, NULL);
    { const char *re = getenv("DPUMESH_INGEST_REAP");
      if (re && atoi(re) >= 1) objs->reaper_active = 1; }

    /* Ingest sharding (diagram ①②③, Design A). DPUMESH_INGEST_SHARDS=M (>=2) gives
     * each shard its OWN consumer PE (DPA channel k → consumer_pes[k%M]); shard m
     * reaps consumer_pes[m] via its own DOCA fd and runs parse/route in parallel.
     * Main is left with emit + ctrl (reaper_active drives the emit-only loop; no
     * separate reaper thread for M>=2). DPUMESH_SHARD_SHARED=1 selects ① (shared
     * conntrack/route under routing_lock; a reply is handled where it lands) instead
     * of the default ② (share-nothing: per-shard conntrack, a reply is handed to its
     * owner shard's cross-shard inbox). DPUMESH_SHARD_HOST_EMIT=1 = ③ scaffold. M=1
     * = the single reaper (DPUMESH_INGEST_REAP) or inline-on-main (default). */
    objs->n_ingest_shards = 1;
    objs->shard_shared_routing = 0;
    objs->shard_host_emit = 0;
    pthread_mutex_init(&objs->routing_lock, NULL);
    for (int s = 0; s < MAX_INGEST_SHARDS; s++) {
        struct dpu_ingest_shard *sh = &objs->ingest_shards[s];
        sh->objs = objs;
        sh->id = s;
        sh->pe = NULL;                   /* = consumer_pes[s], set once PEs exist */
        sh->queue.head = sh->queue.tail = 0;
        sh->xshard.head = sh->xshard.tail = 0;
        pthread_mutex_init(&sh->xshard_lock, NULL);
        sh->num_deferred_recv = 0;
        sh->wake_fd = -1;
        atomic_store_explicit(&sh->parked, 0, memory_order_relaxed);
        sh->stop = 0;
        sh->running = 0;
    }
    for (int s = 0; s < MAX_INGEST_SHARDS; s++)
        objs->consumer_pes[s] = NULL;
    { const char *me = getenv("DPUMESH_INGEST_SHARDS");
      if (me && *me) { int v = atoi(me);
          if (v > MAX_INGEST_SHARDS) v = MAX_INGEST_SHARDS;
          if (v >= 1) objs->n_ingest_shards = v; } }
    { const char *se = getenv("DPUMESH_SHARD_SHARED");
      if (se && atoi(se) >= 1) objs->shard_shared_routing = 1; }
    { const char *he = getenv("DPUMESH_SHARD_HOST_EMIT");
      if (he && atoi(he) >= 1) objs->shard_host_emit = 1; }
    { const char *de = getenv("DPUMESH_DIAG");
      if (de && atoi(de) >= 1) g_dpu_diag = 1; }
    if (objs->n_ingest_shards >= 2)
        objs->reaper_active = 1;          /* M>=2: main runs the emit-only loop (shards reap their own PEs) */

    DOCA_LOG_WARN("Ingest reaper = %s (DPUMESH_INGEST_REAP)",
                  objs->reaper_active ? "ON (dedicated thread)" : "off (inline on main)");
    if (objs->n_ingest_shards >= 2)
        DOCA_LOG_WARN("INGEST SHARDS = %d (mode=%s%s) — parse/route parallelized",
                      objs->n_ingest_shards,
                      objs->shard_shared_routing ? "① shared-routing+lock" : "② share-nothing",
                      objs->shard_host_emit ? " + ③ host-emit" : "");

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

    /* 2b. Per-shard consumer PEs (Design A, M>=2). consumer_pes[0] IS the data-path
     *     consumer's PE (created in step 2); create [1..M-1] so DPA channel k binds to
     *     consumer_pes[k%M] and shard m owns its own DOCA notification fd. On failure,
     *     fall back to a single PE (n_ingest_shards=1) — all channels then bind to
     *     consumer_pes[0] and the M==1 reaper/inline path runs (validated). */
    objs->consumer_pes[0] = objs->consumer_pe;
    if (objs->n_ingest_shards >= 2) {
        for (int s = 1; s < objs->n_ingest_shards; s++) {
            result = doca_pe_create(&objs->consumer_pes[s]);
            if (result != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to create consumer PE for shard %d: %s — disabling sharding",
                             s, doca_error_get_descr(result));
                for (int t = 1; t < s; t++) { doca_pe_destroy(objs->consumer_pes[t]); objs->consumer_pes[t] = NULL; }
                objs->n_ingest_shards = 1;
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

    /* 5. comch DPA message queue. Channel k binds to consumer_pes[k % M] (Design A);
     *    M==1 → all bind to consumer_pes[0] (== consumer_pe, unchanged). */
    result = init_comch_dpa_msgq(objs, objs->consumer_pe);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init comch DPA msgq: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    /* 6. Publish the DPU as ready, then run per-pod DMA setup for any pod whose
     *    mmaps ALREADY arrived during init (a fast host can connect + export before
     *    the msgq is up; process_mmap_msg stored them but deferred setup_pod_dma via
     *    the dpu_ready gate). Later pods set up inline via process_mmap_msg. */
    objs->dpu_ready = 1;
    {
        int kmax = objs->k_rings > 0 ? objs->k_rings : 1;
        for (int i = 0; i < objs->num_pods; i++) {
            struct pod_state *pod = &objs->pods[i];
            if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE))
                continue;
            if (pod->ring_mmap_count >= kmax && pod->remote_mmap && !pod->dma_ready) {
                result = setup_pod_dma(objs, pod);
                if (result != DOCA_SUCCESS) {
                    DOCA_LOG_ERR("deferred setup_pod_dma failed for pod %d: %s",
                                 pod->pod_id, doca_error_get_descr(result));
                    continue;
                }
            }
        }
    }

    /* 7. SG-DMA egress engine (dpu_proxy.c) — the UNIFIED DPU→host reverse path,
     *    ALWAYS on. Requires objs->dev (opened in dpu_main before run_dpu_worker)
     *    + objs->pe (step 1) live. DPUMESH_PROXY only selects the request parser
     *    (passthru default = metadata L4 route; frame; L7). */
    result = px_init(objs);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init L7-proxy L4 engine: %s",
                     doca_error_get_descr(result));
        cleanup_objects(objs);
        return;
    }

    DOCA_LOG_INFO("DPU worker initialized (event-based), entering main loop");

    /* ===== Ingest shard threads (Design A, M>=2) =====
     * Each shard OWNS consumer_pes[id] (DPA channels bound in step 5) and sleeps on
     * its own DOCA notification fd + a cross-shard wake eventfd — no reaper, no
     * per-message eventfd on the forward path. Spawn them before the main emit loop.
     * A spawn failure here is an init-time resource error (channels are already
     * bound to M PEs, so there is no clean single-PE fallback) → abort cleanly. */
    if (objs->n_ingest_shards >= 2) {
        for (int s = 0; s < objs->n_ingest_shards; s++) {
            struct dpu_ingest_shard *sh = &objs->ingest_shards[s];
            sh->pe = objs->consumer_pes[s];
            sh->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (sh->wake_fd < 0 ||
                pthread_create(&sh->thread, NULL, dpu_shard_main, sh) != 0) {
                DOCA_LOG_ERR("Ingest shard %d spawn failed (eventfd/thread) — aborting", s);
                cleanup_objects(objs);
                return;
            }
            sh->running = 1;
        }
        DOCA_LOG_WARN("INGEST SHARD THREADS spawned: %d (event-driven, each owns its consumer PE)",
                      objs->n_ingest_shards);
    }

    /* ===== Ingest-reaper driver =====
     * When the reaper is enabled, spawn the thread that OWNS consumer_pe (reaps
     * DPA forward completions into comp_queue). The main loop then BUSY-POLLS —
     * it drains comp_queue + progresses the ctrl PE + runs egress — because it can
     * no longer sleep on a consumer_pe notification it does not own (comp_queue
     * fills asynchronously from the reaper). This spreads the ingest reap onto its
     * own core, off the single main funnel. On spawn failure, fall through to the
     * inline event-driven driver below (reaper_active cleared → reap inline). */
    if (objs->reaper_active) {
        /* Set up the reaper→main wake (eventfd) + main's epoll {ctrl_pe, eventfd}
         * BEFORE spawning, so any setup failure cleanly disables the reaper and
         * falls through to the normal event-driven driver (no half-started state,
         * no busy-spin). */
        atomic_store_explicit(&objs->main_parked, 0, memory_order_release);
        objs->reaper_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        doca_notification_handle_t pfd = 0;
        int rep = -1;
        if (objs->reaper_wake_fd >= 0 &&
            doca_pe_get_notification_handle(objs->pe, &pfd) == DOCA_SUCCESS)
            rep = epoll_create1(0);
        if (rep >= 0) {
            struct epoll_event ept = { .events = EPOLLIN, .data = { .u32 = 1 } };  /* ctrl pe   */
            struct epoll_event ee  = { .events = EPOLLIN, .data = { .u32 = 2 } };  /* reaper wake */
            if (epoll_ctl(rep, EPOLL_CTL_ADD, (int)pfd, &ept) != 0 ||
                epoll_ctl(rep, EPOLL_CTL_ADD, objs->reaper_wake_fd, &ee) != 0) {
                close(rep); rep = -1;
            }
        }
        /* M>=2 (Design A): the shard threads (already spawned) each reap their OWN
         * consumer PE — main must NOT also run the inline driver (it would
         * double-progress consumer_pes[0]). Only spawn the single Design-B reaper for
         * M==1. A main-epoll failure is fatal for the sharded config (no safe
         * fall-through), so abort; for M==1 it falls back to the inline driver. */
        int reaper_ok = (rep >= 0);
        if (reaper_ok && objs->n_ingest_shards < 2)
            reaper_ok = (pthread_create(&objs->reaper_thread, NULL, dpu_reaper_main, objs) == 0);
        if (!reaper_ok) {
            if (objs->n_ingest_shards >= 2) {
                DOCA_LOG_ERR("main emit-loop epoll setup failed with %d ingest shards — aborting",
                             objs->n_ingest_shards);
                if (rep >= 0) close(rep);
                cleanup_objects(objs);
                return;
            }
            DOCA_LOG_ERR("Ingest reaper setup failed (eventfd/epoll/thread) — falling back to inline reap");
            if (rep >= 0) close(rep);
            if (objs->reaper_wake_fd >= 0) { close(objs->reaper_wake_fd); objs->reaper_wake_fd = -1; }
            objs->reaper_active = 0;
            /* fall through to the normal event-driven driver below */
        } else {
            DOCA_LOG_WARN("MAIN EMIT LOOP (event-driven): %s; main sleeps on "
                          "{ctrl_pe, wake_fd} — no busy-spin",
                          objs->n_ingest_shards >= 2
                              ? "M ingest shards each own their consumer PE"
                              : "reaper owns consumer_pe");
            clock_gettime(CLOCK_MONOTONIC, &last_kick);
            struct timespec last_dbg = last_kick;
            (void)last_kick; (void)kick_elapsed;   /* keepalive is the reaper's/shards' job now */
            while (true) {
                int progressed = dpu_drain_iteration(objs);   /* reap skipped (reaper owns it) */
                if (g_dpu_diag) {
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    if ((now.tv_sec - last_dbg.tv_sec) + (now.tv_nsec - last_dbg.tv_nsec) / 1e9 >= 1.0) {
                        dpu_diag_dump(objs);
                        last_dbg = now;
                    }
                }
                /* NOTE: main does NOT send the DPA WAKE here — the reaper owns
                 * consumer_pe and sends it (else main's submit races the reaper). */
                if (progressed)
                    continue;   /* work → re-poll, no sleep (low latency under load) */

                /* Idle: arm ctrl_pe, PARK (so the reaper will signal us), RE-CHECK
                 * once to close the drain→park race, then SLEEP on epoll. The 1 ms
                 * timeout re-sends the keepalive and backstops any missed wake. */
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
                uint64_t drain; ssize_t rn = read(objs->reaper_wake_fd, &drain, sizeof(drain)); (void)rn;
                (void)doca_pe_clear_notification(objs->pe, pfd);
            }
            return;  /* not reached */
        }
    }

    /* ===== Event-driven driver =====
     * epoll over {consumer_pe fd, ctrl pe fd}. Each pass: drain → send the 1 ms
     * keepalive WAKE on cadence → if idle, arm → re-poll → block on epoll with a
     * 1 ms timeout (so we wake to re-send the keepalive even with no completion).
     * The ARM SLEEPS between ticks (epoll), so idle CPU is a few % (~1 kHz wakeups)
     * — not the full core busy-poll burns. Falls back to busy-poll if setup fails. */
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

    DOCA_LOG_INFO("DPU worker: EVENT-DRIVEN main loop armed (consumer_pe fd=%d, ctrl_pe fd=%d)",
                  (int)cfd, (int)pfd);

    clock_gettime(CLOCK_MONOTONIC, &last_kick);
    struct timespec last_dbg = last_kick;
    while (true) {
        int progressed = dpu_drain_iteration(objs);   /* ONE pass */

        /* 1 ms DPU→DPA keepalive WAKE, gated to ~1 kHz (checked every pass, not
         * only when sleeping, so a sustained-load stretch still pokes a briefly-
         * parked reverse EU). At idle the 1 ms epoll timeout below brings us back
         * here to re-send it. */
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

        /* Idle: arm both PEs, then RE-POLL once before blocking (arm→re-check→
         * block) to close the drain→arm race — a completion landing between the
         * last drain and the arm must not be stranded. */
        (void)doca_pe_request_notification(objs->consumer_pe);
        (void)doca_pe_request_notification(objs->pe);

        if (dpu_drain_iteration(objs)) {
            /* Work arrived during/just before arm — handle it, don't sleep. */
            (void)doca_pe_clear_notification(objs->consumer_pe, cfd);
            (void)doca_pe_clear_notification(objs->pe, pfd);
            continue;
        }

        /* SLEEP on epoll with a 1 ms timeout: wake on a real completion (FWD_DONE/
         * REV_DONE/ctrl) OR after ≤1 ms to re-send the keepalive WAKE. The ARM
         * sleeps between ticks → idle CPU a few % (~1 kHz wakeups), NOT a busy-poll
         * full core. The timeout also backstops any missed PE notification. */
        struct epoll_event evs[2];
        (void)epoll_wait(ep, evs, 2, 1);

        /* Clear PE notifications so they can be re-armed (SELECTIVE contract). */
        (void)doca_pe_clear_notification(objs->consumer_pe, cfd);
        (void)doca_pe_clear_notification(objs->pe, pfd);
    }
}
