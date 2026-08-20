#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White-box stress coverage for worker lane publication. */
#include "../doca/dpu_proxy.c"

static int l7_close_calls;

/* This white-box test links only dpu_proxy.c. Keep the production lookup and
 * routing dependencies deterministic for the reserve/commit cases below. */
struct pod_state *find_pod_by_id(struct objects *objs, int32_t pod_id)
{
    int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (objs->pods[i].registered && objs->pods[i].pod_id == pod_id)
            return &objs->pods[i];
    return NULL;
}

int32_t dpu_route_l4(struct objects *objs, int16_t svc)
{
    (void)objs;
    (void)svc;
    return -1;
}

/* No generation is held in this build, so no Service has a replica elsewhere
 * and every unroutable stream stays unroutable. */
int dmesh_service_has_remote(struct objects *objs, int16_t svc)
{
    (void)objs;
    (void)svc;
    return 0;
}

void l7_conn_eof(int worker_id, uint64_t conn)
{
    (void)worker_id;
    (void)conn;
}

/* Poison and peer refusals are counted through the adapter's control-event
 * family; this build has no adapter, so the count lands in the worker stat
 * line alone. */
void l7_control_event(const char *kind, const char *reason)
{
    (void)kind;
    (void)reason;
}

/* No policy source in this build, so the destination Service's protection
 * class decides — which is what a deployment without a control plane has. */
int l7_inbound_verdict(int worker_id, const struct dmesh_l7_flow *flow)
{
    (void)worker_id;
    (void)flow;
    return DMESH_L7_VERDICT_NO_POLICY;
}

void l7_conn_close(int worker_id, uint64_t conn)
{
    (void)worker_id;
    (void)conn;
    l7_close_calls++;
}

void l7_report(int worker_id, uint64_t conn, uint64_t bytes_in,
               uint64_t bytes_out, uint64_t duration_ns, int reason)
{
    (void)worker_id;
    (void)conn;
    (void)bytes_in;
    (void)bytes_out;
    (void)duration_ns;
    (void)reason;
}

#define PRODUCERS 2
#define PER_PRODUCER 20000
#define TEST_REGION 2

struct producer_arg {
    struct dmesh_proxy *px;
    struct px_unit *units;
    int id;
    atomic_int *done;
};

static void *producer_main(void *opaque)
{
    struct producer_arg *a = (struct producer_arg *)opaque;
    px_cur_worker = &a->px->workers[a->id];
    for (int i = 0; i < PER_PRODUCER; i++) {
        struct px_unit *u = &a->units[i];
        memset(u, 0, sizeof(*u));
        u->src_pod_id = (int8_t)a->id;
        u->seq = (uint16_t)(i + 1);
        px_lane_enqueue(a->px, 0, TEST_REGION, u);
        if ((i & 127) == 0)
            sched_yield();
    }
    atomic_fetch_add_explicit(a->done, 1, memory_order_release);
    return NULL;
}

int main(void)
{
    /* Adjacent, unclaimed DPA completions share one custody object and extend its
     * exact-ACK range. Claimed or oversized tails must remain separate. */
    struct px_arrival arr;
    memset(&arr, 0, sizeof(arr));
    arr.pod_idx = 3;
    arr.staging_off = 4096;
    arr.len = 8192;
    arr.ack_pod = 7;
    arr.ack_port = 42;
    arr.ack_seq = 10;
    arr.ack_first_seq = 10;
    atomic_init(&arr.unfreed, arr.len + 1u);
    struct px_conn conn;
    memset(&conn, 0, sizeof(conn));
    conn.whead = conn.wtail = &arr;
    conn.stream_end = arr.len;
    dpu_comp_entry_t comp;
    memset(&comp, 0, sizeof(comp));
    comp.pod_idx = 3;
    comp.buf_offset = 4096 + 8192;
    comp.length = 8192;
    comp.src_pod_id = 7;
    comp.src_port = 42;
    comp.seq = 11;
    assert(px_arrival_try_extend(&conn, &comp, 1));
    assert(arr.len == 16384 && arr.ack_seq == 11 && conn.stream_end == 16384);
    assert(atomic_load(&arr.unfreed) == 16385);
    arr.claimed_round = 1;
    comp.buf_offset += comp.length;
    comp.seq++;
    assert(!px_arrival_try_extend(&conn, &comp, 1));

    struct dmesh_proxy *px = calloc(1, sizeof(*px));
    assert(px != NULL);
    assert(pthread_mutex_init(&px->pool_lock, NULL) == 0);

    /* Three workers with two producer threads: TEST_REGION's owner (2 % 3) is
     * neither producer, so every publication takes the cross-owner inbox. */
    px->n_workers = 3;
    for (int s = 0; s < PRODUCERS; s++)
        px->workers[s].id = s;

    struct px_unit *units[PRODUCERS];
    struct producer_arg args[PRODUCERS];
    pthread_t tids[PRODUCERS];
    atomic_int done;
    atomic_init(&done, 0);
    for (int s = 0; s < PRODUCERS; s++) {
        units[s] = calloc(PER_PRODUCER, sizeof(*units[s]));
        assert(units[s] != NULL);
        args[s] = (struct producer_arg){ .px = px, .units = units[s], .id = s, .done = &done };
        assert(pthread_create(&tids[s], NULL, producer_main, &args[s]) == 0);
    }

    struct px_lane *ln = &px->lanes[0][TEST_REGION];
    uint32_t last[PRODUCERS] = {0};
    int consumed = 0;
    while (consumed < PRODUCERS * PER_PRODUCER) {
        (void)px_lane_splice_inbox(px, ln);
        while (ln->qhead) {
            struct px_unit *u = ln->qhead;
            ln->qhead = u->next;
            if (!ln->qhead)
                ln->qtail = NULL;
            int s = (int)u->src_pod_id;
            assert(s >= 0 && s < PRODUCERS);
            assert(u->seq == (uint16_t)(last[s] + 1));
            last[s]++;
            consumed++;
        }
        if (atomic_load_explicit(&done, memory_order_acquire) < PRODUCERS)
            sched_yield();
    }
    for (int s = 0; s < PRODUCERS; s++) {
        pthread_join(tids[s], NULL);
        assert(last[s] == PER_PRODUCER);
        free(units[s]);
    }
    assert(!px_lane_inbox_nonempty(px, ln));

    assert(px_lane_wrap_action(24577, 8192, 32768, 3) ==
           PX_LANE_WRAP_WAIT);
    assert(px_lane_wrap_action(24577, 8192, 32768, 0) ==
           PX_LANE_WRAP_RESET);
    assert(px_lane_wrap_action(16384, 8192, 32768, 3) ==
           PX_LANE_WRAP_NONE);

    /* Delivery sequences are scoped to destination host QPs. */
    px->workers[0].buckets =
        calloc(PX_CONN_HASH, sizeof(*px->workers[0].buckets));
    px->workers[0].ct = calloc(1, sizeof(*px->workers[0].ct));
    assert(px->workers[0].buckets != NULL && px->workers[0].ct != NULL);
    px->workers[0].ct->next_uport = DMESH_UPORT_BASE;
    px_cur_worker = &px->workers[0];
    struct px_conn *downstream = px_conn_get(px, 5, 1234, 0, 1);
    assert(downstream != NULL);
    assert(px_delivery_seq_counter(px, 5, 1234) ==
           &downstream->return_seq);
    uint16_t upstream = dpu_upstream_create(px->workers[0].ct,
                                            5, 1234, 7, 0, 0, 1);
    assert(upstream >= DMESH_UPORT_BASE);
    assert(px_delivery_seq_counter(px, 7, upstream) ==
           &px->workers[0].ct->upstream[upstream].delivery_seq);
    assert(px_delivery_seq_counter(px, 8, upstream) == NULL);

    /* Same-owner FIFO and cross-owner inbox. */
    px->n_workers = 2;
    px_cur_worker = &px->workers[0];
    struct px_unit local_unit, remote_unit;
    memset(&local_unit, 0, sizeof(local_unit));
    memset(&remote_unit, 0, sizeof(remote_unit));
    struct px_lane *local_lane = &px->lanes[1][0];   /* region 0 -> worker 0 */
    struct px_lane *remote_lane = &px->lanes[1][1];  /* region 1 -> worker 1 */
    px_lane_enqueue(px, 1, 0, &local_unit);
    assert(local_lane->qhead == &local_unit && local_lane->qtail == &local_unit);
    assert(!px_lane_inbox_nonempty(px, local_lane));
    local_lane->qhead = local_lane->qtail = NULL;

    px_lane_enqueue(px, 1, 1, &remote_unit);
    assert(remote_lane->qhead == NULL);
    assert(px_lane_inbox_nonempty(px, remote_lane));
    assert(px_lane_splice_inbox(px, remote_lane));
    assert(remote_lane->qhead == &remote_unit && remote_lane->qtail == &remote_unit);
    remote_lane->qhead = remote_lane->qtail = NULL;
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    objs->proxy = px;

    /* Selected L7 requests bypass the normal port hash and are handed to the
     * configured Linkerd owner. Ordinary L4 and already-resolved flows do not. */
    px->l7_attached = 1;
    px->l7_worker = 1;
    px->svc_mode[11] = PX_L7_OPAQUE;
    px->svc_mode[12] = PX_L7_FULL;
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, 11) == 1);
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, 12) == 1);
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, 13) == -1);
    assert(px_l7_request_owner(objs, 7, 11) == -1);
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, -1) == -1);
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, POD_ID_SPACE) == -1);
    px->l7_worker = PX_L7_WORKER_ALL;
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, 11) == -1);
    assert(px_l7_request_owner(objs, DMESH_POD_BLANK, 12) == -1);

    struct px_engine *eng = &px->engines[0];
    eng->objs = objs;

    /* A disconnected client is purged by its connection-owning worker before
     * that worker reports pod quiescence. This closes the long-lived L7
     * session, drops the reply/conntrack edge, and releases the last source
     * staging reference without trying to ACK a vanished pod. */
    px->n_workers = 1;
    px->workers[0].id = 0;
    px->workers[0].objs = objs;
    struct px_conn *reply = px_conn_get(px, 7, upstream, 1, 1);
    assert(reply != NULL);
    downstream->l7_open = 1;
    struct px_arrival *held = calloc(1, sizeof(*held));
    assert(held != NULL);
    held->pod_idx = 0;
    held->ack_pod = 5;
    atomic_init(&held->unfreed, 1);
    downstream->whead = downstream->wtail = held;
    objs->pods[0].pod_id = 5;
    objs->pods[0].registered = 0;
    objs->pods[0].proxy_source_refs = 1;
    __atomic_store_n(&objs->num_pods, 1, __ATOMIC_RELEASE);
    assert(px_worker_quiesce_pod_connections(objs, 5) >= 2);
    assert(px_conn_find(px, 5, 1234) == NULL);
    assert(px_conn_find(px, 7, upstream) == NULL);
    assert(!px->workers[0].ct->upstream[upstream].in_use);
    assert(objs->pods[0].proxy_source_refs == 0);
    assert(l7_close_calls == 1);
    tls_arr_mag = NULL;
    tls_arr_mag_n = 0;
    free(held);
    downstream = px_conn_get(px, 5, 1234, 0, 1);
    assert(downstream != NULL);

    /* Current batches retry once; stale or repeated failures are terminal. */
    struct pod_state *retry_pod = &objs->pods[0];
    atomic_store_explicit(&retry_pod->dma_ready, 1, memory_order_release);
    atomic_store_explicit(&retry_pod->dma_generation, 7,
                          memory_order_release);
    retry_pod->host_rx_mmap = (struct doca_mmap *)(uintptr_t)1;
    retry_pod->host_rx_addr = (void *)(uintptr_t)1;
    struct px_batch retry_batch;
    memset(&retry_batch, 0, sizeof(retry_batch));
    retry_batch.pod_idx = 0;
    retry_batch.pod_generation = 7;
    retry_batch.state = PX_BATCH_INFLIGHT;
    px_batch_record_error(eng, &retry_batch, DOCA_ERROR_IO_FAILED);
    assert(retry_batch.state == PX_BATCH_RETRY_PENDING);
    assert(retry_batch.retry_count == 1 && eng->retry_batches == 1);
    eng->retry_probe = &retry_batch;
    retry_batch.state = PX_BATCH_INFLIGHT;
    px_batch_record_error(eng, &retry_batch, DOCA_ERROR_IO_FAILED);
    assert(retry_batch.state == PX_BATCH_ERROR);
    assert(eng->retry_batches == 0 && eng->retry_probe == NULL);

    struct px_batch stale_batch;
    memset(&stale_batch, 0, sizeof(stale_batch));
    stale_batch.pod_idx = 0;
    stale_batch.pod_generation = 6;
    stale_batch.state = PX_BATCH_INFLIGHT;
    px_batch_record_error(eng, &stale_batch, DOCA_ERROR_IO_FAILED);
    assert(stale_batch.state == PX_BATCH_ERROR);
    assert(eng->retry_batches == 0);

    /* Reverse publication state clears after its DMA task exits. */
    struct px_rev_pub dead_pub;
    memset(&dead_pub, 0, sizeof(dead_pub));
    dead_pub.count = 3;
    dead_pub.publish_count = 2;
    dead_pub.producer_tail = 9;
    dead_pub.state = PX_REV_META_INFLIGHT;
    assert(!px_rev_drop_dead(&dead_pub));
    assert(dead_pub.count == 3);
    dead_pub.state = PX_REV_IDLE;
    assert(px_rev_drop_dead(&dead_pub));
    assert(dead_pub.count == 0 && dead_pub.publish_count == 0);
    assert(dead_pub.producer_tail == 0 && dead_pub.state == PX_REV_IDLE);

    /* Batch retirement follows fqueue order. */
    struct px_lane ordered_lane;
    struct px_batch ordered_batches[3];
    struct px_unit ordered_units[3];
    memset(&ordered_lane, 0, sizeof(ordered_lane));
    memset(ordered_batches, 0, sizeof(ordered_batches));
    memset(ordered_units, 0, sizeof(ordered_units));
    for (int i = 0; i < 3; i++) {
        ordered_batches[i].units = &ordered_units[i];
        ordered_batches[i].entries = 1;
        ordered_units[i].dst_pod_idx = 0;
        ordered_units[i].seq = (uint16_t)(i + 1);
        if (i < 2)
            ordered_batches[i].next = &ordered_batches[i + 1];
    }
    ordered_batches[0].state = PX_BATCH_DONE;
    ordered_batches[1].state = PX_BATCH_RETRY_PENDING;
    ordered_batches[1].retry_count = 1;
    ordered_batches[2].state = PX_BATCH_DONE;
    ordered_lane.fhead = &ordered_batches[0];
    ordered_lane.ftail = &ordered_batches[2];
    ordered_lane.sent_entries = 3;
    eng->retry_batches = 1;
    assert(px_lane_retire(eng, &ordered_lane));
    assert(ordered_lane.fhead == &ordered_batches[1]);
    assert(eng->emit_head == &ordered_units[0]);
    assert(ordered_units[0].next == NULL);
    px_batch_leave_retry(eng, &ordered_batches[1]);
    ordered_batches[1].state = PX_BATCH_DONE;
    assert(px_lane_retire(eng, &ordered_lane));
    assert(ordered_lane.fhead == NULL && ordered_lane.ftail == NULL);
    assert(eng->emit_head == &ordered_units[0]);
    assert(ordered_units[0].next == &ordered_units[1]);
    assert(ordered_units[1].next == &ordered_units[2]);
    assert(ordered_units[2].next == NULL);
    assert(eng->retry_batches == 0 && ordered_lane.sent_entries == 3);
    eng->emit_head = eng->emit_tail = NULL;
    eng->batch_free = NULL;
    atomic_store_explicit(&retry_pod->egress_pending_emit, 0,
                          memory_order_relaxed);

    retry_pod->k_rings = 8;
    retry_pod->landing_stripes = 2;
    assert(px_landing_stripes(retry_pod) == 2);
    __atomic_store_n(&objs->num_pods, 1, __ATOMIC_RELEASE);
    struct px_unit pending_unit;
    memset(&pending_unit, 0, sizeof(pending_unit));
    px->lanes[0][0].qhead = px->lanes[0][0].qtail = &pending_unit;
    assert(!px_worker_has_pending(eng));
    eng->dma_tasks_inflight = 1;
    assert(px_worker_has_pending(eng));
    eng->dma_tasks_inflight = 0;
    px->lanes[0][0].qhead = px->lanes[0][0].qtail = NULL;
    __atomic_store_n(&objs->num_pods, 0, __ATOMIC_RELEASE);

    /* The L7 reserve/commit ABI owns exactly one arena chunk per connection.
     * Exercise the real C implementation, including cancellation, invalid
     * length, successful publication, and close-time reclamation. */
    tls_chunk_mag = NULL;
    tls_chunk_mag_n = 0;
    tls_piece_mag = NULL;
    tls_piece_mag_n = 0;
    tls_unit_mag = NULL;
    tls_unit_mag_n = 0;

    struct px_chunk *tx_chunks = calloc(2, sizeof(*tx_chunks));
    struct px_piece *tx_pieces = calloc(2, sizeof(*tx_pieces));
    struct px_unit *tx_units = calloc(2, sizeof(*tx_units));
    px->arena = calloc(2, PX_ARENA_CHUNK);
    assert(tx_chunks && tx_pieces && tx_units && px->arena);
    tx_chunks[0].off = 0;
    tx_chunks[0].next = &tx_chunks[1];
    tx_chunks[1].off = PX_ARENA_CHUNK;
    tx_pieces[0].next = &tx_pieces[1];
    tx_units[0].next = &tx_units[1];
    px->chunk_free = tx_chunks;
    px->piece_free = tx_pieces;
    px->unit_free = tx_units;

    struct pod_state *tx_pod = &objs->pods[0];
    memset(tx_pod, 0, sizeof(*tx_pod));
    tx_pod->pod_id = 5;
    tx_pod->service_id = 9;
    tx_pod->registered = 1;
    tx_pod->dma_ready = 1;
    tx_pod->host_rx_mmap = (struct doca_mmap *)(uintptr_t)1;
    tx_pod->host_rx_addr = (void *)(uintptr_t)1;
    tx_pod->k_rings = 1;
    tx_pod->landing_stripes = 1;
    __atomic_store_n(&objs->num_pods, 1, __ATOMIC_RELEASE);
    px->n_workers = 1;
    px->workers[0].id = 0;
    px->workers[0].objs = objs;
    px_cur_worker = &px->workers[0];

    uint64_t tx_handle = dmesh_l7_conn_handle(5, 1234);
    uint32_t cap = 123;
    assert(dmesh_l7_tx_reserve(1, tx_handle, &cap) == NULL);
    assert(dmesh_l7_tx_reserve(0, UINT64_C(0x001f0001), &cap) == NULL);

    uint8_t *reserved = dmesh_l7_tx_reserve(0, tx_handle, &cap);
    assert(reserved != NULL && cap == PX_ARENA_CHUNK);
    assert(downstream->l7_tx_chunk != NULL);
    assert(dmesh_l7_tx_reserve(0, tx_handle, &cap) == NULL);
    assert(dmesh_l7_tx_commit(0, tx_handle, DMESH_L7_ORIGIN, 0) == 0);
    assert(downstream->l7_tx_chunk == NULL);

    reserved = dmesh_l7_tx_reserve(0, tx_handle, &cap);
    assert(reserved != NULL);
    assert(dmesh_l7_tx_commit(0, tx_handle, DMESH_L7_ORIGIN,
                              PX_ARENA_CHUNK + 1u) == -1);
    assert(downstream->l7_tx_chunk == NULL);

    static const uint8_t payload[] = "reserved-output";
    reserved = dmesh_l7_tx_reserve(0, tx_handle, &cap);
    assert(reserved != NULL && cap >= sizeof(payload));
    memcpy(reserved, payload, sizeof(payload));
    assert(dmesh_l7_tx_commit(0, tx_handle, DMESH_L7_ORIGIN,
                              sizeof(payload)) == (int)sizeof(payload));
    assert(downstream->l7_tx_chunk == NULL);
    struct px_lane *tx_lane = &px->lanes[0][0];
    assert(tx_lane->qhead != NULL && tx_lane->qhead == tx_lane->qtail);
    struct px_unit *tx_unit = tx_lane->qhead;
    assert(tx_unit->total_len == sizeof(payload));
    assert(tx_unit->pieces && tx_unit->pieces->chunk);
    assert(tx_unit->pieces->len == sizeof(payload));
    assert(memcmp(px->arena + tx_unit->pieces->staging_off,
                  payload, sizeof(payload)) == 0);
    tx_lane->qhead = tx_lane->qtail = NULL;
    px_unit_free_node(px, tx_unit);
    assert(dmesh_l7_tx_commit(0, tx_handle, DMESH_L7_ORIGIN, 1) == -1);

    reserved = dmesh_l7_tx_reserve(0, tx_handle, &cap);
    assert(reserved != NULL);
    px_l7_close(objs, downstream, 0);
    assert(downstream->l7_tx_chunk == NULL);
    reserved = dmesh_l7_tx_reserve(0, tx_handle, &cap);
    assert(reserved != NULL);
    assert(dmesh_l7_tx_commit(0, tx_handle, DMESH_L7_ORIGIN, 0) == 0);

    /* One extent is acknowledged by one reverse entry naming its whole run,
     * whatever the run's length. */
    px->rev_scratch = calloc(1, PX_REV_STAGE_STRIDE);
    assert(px->rev_scratch != NULL);
    px_ack_queue_init(&eng->ack_releases);
    eng->ack_retry_head = eng->ack_retry_tail = NULL;
    px->n_workers = 1;
    px_cur_worker = &px->workers[0];

    struct pod_state *ack_pod = &objs->pods[0];
    memset(ack_pod, 0, sizeof(*ack_pod));
    ack_pod->pod_id = 5;
    ack_pod->registered = 1;
    ack_pod->dma_ready = 1;
    ack_pod->k_rings = 1;
    ack_pod->landing_stripes = 1;
    __atomic_store_n(&objs->num_pods, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&ack_pod->proxy_source_refs, 1, __ATOMIC_RELEASE);

    struct px_rev_pub *ack_pub = &px->lanes[0][0].rev;
    const struct dmesh_rev_ring_entry *ack_entry =
        (const struct dmesh_rev_ring_entry *)px->rev_scratch;

    struct px_arrival run;
    memset(&run, 0, sizeof(run));
    run.pod_idx = 0;
    run.ack_pod = 5;
    run.ack_port = 42;
    run.ack_first_seq = 60000;         /* the run wraps the 16-bit space */
    run.ack_seq = 4;
    assert(px_ack_queue_push(&eng->ack_releases, &run));
    assert(px_drain_ack_releases(eng, 256) == 1);
    assert(ack_pub->count == 1);
    assert(ack_entry->kind == DMESH_REV_ENTRY_TX_ACK);
    assert(ack_entry->payload.ack.port == 42);
    assert(ack_entry->payload.ack.seq == 60000);
    assert(ack_entry->payload.ack.seq_count == (uint16_t)(4 - 60000 + 1));
    assert(__atomic_load_n(&ack_pod->proxy_source_refs, __ATOMIC_ACQUIRE) == 0);
    assert(px_ack_queue_front(&eng->ack_releases) == NULL);

    /* A single-sequence extent, and the FIN path, both name a run of one. */
    ack_pub->count = 0;
    struct px_arrival one;
    memset(&one, 0, sizeof(one));
    one.pod_idx = 0;
    one.ack_pod = 5;
    one.ack_port = 42;
    one.ack_first_seq = 9;
    one.ack_seq = 9;
    __atomic_store_n(&ack_pod->proxy_source_refs, 1, __ATOMIC_RELEASE);
    assert(px_ack_queue_push(&eng->ack_releases, &one));
    assert(px_drain_ack_releases(eng, 256) == 1);
    assert(ack_pub->count == 1);
    assert(ack_entry->payload.ack.seq == 9);
    assert(ack_entry->payload.ack.seq_count == 1);
    ack_pub->count = 0;
    assert(px_emit_tx_ack(objs, 5, 42, 77) == 1);
    assert(ack_pub->count == 1);
    assert(ack_entry->payload.ack.seq == 77);
    assert(ack_entry->payload.ack.seq_count == 1);
    ack_pub->count = 0;

    /* Coalescing stops before a run outgrows the count that publishes it. */
    struct px_arrival wide;
    memset(&wide, 0, sizeof(wide));
    wide.pod_idx = 3;
    wide.len = 8192;
    wide.ack_pod = 7;
    wide.ack_port = 42;
    wide.ack_first_seq = 100;
    wide.ack_seq = (uint16_t)(100u + PX_ACK_RUN_MAX - 2u);
    atomic_init(&wide.unfreed, wide.len + 1u);
    struct px_conn wide_conn;
    memset(&wide_conn, 0, sizeof(wide_conn));
    wide_conn.whead = wide_conn.wtail = &wide;
    dpu_comp_entry_t wide_comp;
    memset(&wide_comp, 0, sizeof(wide_comp));
    wide_comp.pod_idx = 3;
    wide_comp.buf_offset = 8192;
    wide_comp.length = 1;
    wide_comp.src_pod_id = 7;
    wide_comp.src_port = 42;
    wide_comp.seq = (uint16_t)(wide.ack_seq + 1u);
    assert(!px_arrival_try_extend(&wide_conn, &wide_comp, 1));
    wide.ack_seq = (uint16_t)(100u + PX_ACK_RUN_MAX - 3u);
    wide_comp.seq = (uint16_t)(wide.ack_seq + 1u);
    assert(px_arrival_try_extend(&wide_conn, &wide_comp, 1));
    assert((uint16_t)(wide.ack_seq - wide.ack_first_seq + 1u) ==
           (uint16_t)(PX_ACK_RUN_MAX - 1u));

    free(px->rev_scratch);
    px->rev_scratch = NULL;
    tls_arr_mag = NULL;
    tls_arr_mag_n = 0;

    tls_chunk_mag = NULL;
    tls_chunk_mag_n = 0;
    tls_piece_mag = NULL;
    tls_piece_mag_n = 0;
    tls_unit_mag = NULL;
    tls_unit_mag_n = 0;
    px->chunk_free = NULL;
    px->piece_free = NULL;
    px->unit_free = NULL;
    free(px->arena);
    px->arena = NULL;
    free(tx_chunks);
    free(tx_pieces);
    free(tx_units);

    pthread_mutex_destroy(&px->pool_lock);
    free(downstream);
    free(px->workers[0].ct);
    free(px->workers[0].buckets);
    free(objs);
    free(px);
    puts("proxy_lane_queue_test: PASS");
    return 0;
}
