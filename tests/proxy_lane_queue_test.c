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

    /* Complete L7 frames with identical delivery metadata collapse into one
     * unit, keeping piece order/custody and the first delivery sequence. */
    struct px_piece p1, p2;
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    p1.len = 1000;
    p2.len = 1200;
    struct px_unit merged, extra;
    memset(&merged, 0, sizeof(merged));
    memset(&extra, 0, sizeof(extra));
    merged.src_pod_id = extra.src_pod_id = 1;
    merged.src_service = extra.src_service = 11;
    merged.dst_service = extra.dst_service = 16;
    merged.src_port = extra.src_port = 30000;
    merged.dst_port = extra.dst_port = 30000;
    merged.org_port = extra.org_port = 1234;
    merged.dst_pod_idx = extra.dst_pod_idx = 2;
    merged.dma_isolated = extra.dma_isolated = 1;
    merged.seq = 7;
    extra.seq = 8;
    merged.total_len = p1.len;
    extra.total_len = p2.len;
    merged.pieces = merged.pieces_tail = &p1;
    extra.pieces = extra.pieces_tail = &p2;
    merged.npieces = extra.npieces = 1;
    px->sg_pieces_max = 8;
    assert(px_l7_unit_absorb(px, &merged, &extra));
    assert(merged.total_len == 2200 && merged.npieces == 2 && merged.seq == 7);
    assert(merged.pieces == &p1 && merged.pieces_tail == &p2 && p1.next == &p2);
    assert(extra.pieces == NULL && tls_unit_mag == &extra);

    struct px_unit incompatible;
    memset(&incompatible, 0, sizeof(incompatible));
    incompatible = merged;
    incompatible.dst_port++;
    incompatible.pieces = incompatible.pieces_tail = NULL;
    incompatible.npieces = 0;
    assert(!px_l7_unit_absorb(px, &merged, &incompatible));

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
    struct px_engine *eng = &px->engines[0];
    eng->objs = objs;

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

    pthread_mutex_destroy(&px->pool_lock);
    free(downstream);
    free(px->workers[0].ct);
    free(px->workers[0].buckets);
    free(objs);
    free(px);
    puts("proxy_lane_queue_test: PASS");
    return 0;
}
