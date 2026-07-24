#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White-box stress coverage for the production shard->egress publication queue.
 * Function-section GC discards the hardware paths from the included source. */
#include "../doca/dpu_proxy.c"

#define PRODUCERS 2
#define PER_PRODUCER 20000

struct producer_arg {
    struct dmesh_proxy *px;
    struct px_unit *units;
    int id;
    atomic_int *done;
};

struct done_producer_arg {
    struct px_engine *eng;
    struct px_unit *units;
    int count;
    atomic_int *done;
};

static void *producer_main(void *opaque)
{
    struct producer_arg *a = (struct producer_arg *)opaque;
    px_cur_shard = &a->px->shards[a->id];
    for (int i = 0; i < PER_PRODUCER; i++) {
        struct px_unit *u = &a->units[i];
        memset(u, 0, sizeof(*u));
        u->src_pod_id = (int8_t)a->id;
        u->seq = (uint16_t)(i + 1);
        px_lane_enqueue(a->px, 0, 0, u);
        if ((i & 127) == 0)
            sched_yield();
    }
    atomic_fetch_add_explicit(a->done, 1, memory_order_release);
    return NULL;
}

static void *done_producer_main(void *opaque)
{
    struct done_producer_arg *a = (struct done_producer_arg *)opaque;
    for (int i = 0; i < a->count; i++) {
        struct px_unit *u = &a->units[i];
        memset(u, 0, sizeof(*u));
        u->dst_pod_idx = 0;
        u->seq = (uint16_t)(i + 1);
        while (!px_done_publish(a->eng, u, u, 0, 1))
            sched_yield();
    }
    atomic_store_explicit(a->done, 1, memory_order_release);
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
    atomic_init(&px->pool_waiters, 0);

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
    assert(extra.pieces == NULL && px->unit_free == &extra);
    assert(px->stat_egress_merges == 1);

    struct px_unit incompatible;
    memset(&incompatible, 0, sizeof(incompatible));
    incompatible = merged;
    incompatible.dst_port++;
    incompatible.pieces = incompatible.pieces_tail = NULL;
    incompatible.npieces = 0;
    assert(!px_l7_unit_absorb(px, &merged, &incompatible));

    px->n_eng = 2;
    px->n_shards = PRODUCERS;
    for (int s = 0; s < PRODUCERS; s++)
        px->shards[s].id = s;

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

    struct px_lane *ln = &px->lanes[0][0];
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
    assert(__atomic_load_n(&px->stat_units, __ATOMIC_RELAXED) ==
           PRODUCERS * PER_PRODUCER);

    /* Same-owner FIFO and cross-owner inbox. */
    px->run_to_completion = 1;
    px_cur_shard = &px->shards[0];
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
    px->run_to_completion = 0;

    /* Worker-to-main SPSC completion ring. */
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    objs->proxy = px;
    struct px_engine *eng = &px->engines[0];
    eng->objs = objs;
    atomic_init(&eng->done_prod, 0);
    atomic_init(&eng->done_cons, 0);
    enum { DONE_UNITS = 20000 };
    struct px_unit *done_units = calloc(DONE_UNITS, sizeof(*done_units));
    assert(done_units != NULL);
    atomic_int done_published;
    atomic_init(&done_published, 0);
    struct done_producer_arg done_arg = {
        .eng = eng,
        .units = done_units,
        .count = DONE_UNITS,
        .done = &done_published,
    };
    pthread_t done_tid;
    assert(pthread_create(&done_tid, NULL, done_producer_main, &done_arg) == 0);

    int done_consumed = 0;
    while (done_consumed < DONE_UNITS) {
        (void)px_done_pull(eng);
        while (eng->emit_head) {
            struct px_unit *u = eng->emit_head;
            eng->emit_head = u->next;
            if (!eng->emit_head)
                eng->emit_tail = NULL;
            assert(u->seq == (uint16_t)(done_consumed + 1));
            __atomic_fetch_sub(&objs->pods[0].egress_pending_emit, 1,
                               __ATOMIC_ACQ_REL);
            done_consumed++;
        }
        if (!atomic_load_explicit(&done_published, memory_order_acquire))
            sched_yield();
    }
    pthread_join(done_tid, NULL);
    assert(atomic_load_explicit(&eng->done_prod, memory_order_acquire) ==
           atomic_load_explicit(&eng->done_cons, memory_order_acquire));
    assert(__atomic_load_n(&objs->pods[0].egress_pending_emit,
                           __ATOMIC_ACQUIRE) == 0);
    free(done_units);

    /* Coalesced worker wake. */
    eng->threaded = 1;
    eng->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(eng->wake_fd >= 0);
    atomic_init(&eng->parked, 1);
    atomic_init(&eng->stat_wakes, 0);
    atomic_init(&eng->stat_wake_errors, 0);
    px_engine_wake(eng);
    px_engine_wake(eng);
    uint64_t wakes = 0;
    assert(read(eng->wake_fd, &wakes, sizeof(wakes)) == (ssize_t)sizeof(wakes));
    assert(wakes == 1);
    assert(atomic_load_explicit(&eng->parked, memory_order_acquire) == 0);
    assert(atomic_load_explicit(&eng->stat_wakes, memory_order_relaxed) == 1);
    assert(atomic_load_explicit(&eng->stat_wake_errors,
                                memory_order_relaxed) == 0);
    close(eng->wake_fd);
    eng->wake_fd = -1;

    /* Owner-selective wake and shared-pool waiter mask. */
    struct px_engine *eng0 = &px->engines[0];
    struct px_engine *eng1 = &px->engines[1];
    eng0->threaded = eng1->threaded = 1;
    eng0->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    eng1->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(eng0->wake_fd >= 0 && eng1->wake_fd >= 0);
    atomic_store_explicit(&eng0->parked, 1, memory_order_release);
    atomic_init(&eng1->parked, 1);
    atomic_init(&eng1->stat_wakes, 0);
    atomic_init(&eng1->stat_wake_errors, 0);
    px_wake_workers(px, 1u << 1);
    errno = 0;
    assert(read(eng0->wake_fd, &wakes, sizeof(wakes)) == -1 && errno == EAGAIN);
    assert(read(eng1->wake_fd, &wakes, sizeof(wakes)) == (ssize_t)sizeof(wakes));
    assert(wakes == 1);

    atomic_store_explicit(&px->pool_waiters, 0, memory_order_relaxed);
    px_cur_shard = &px->shards[0];
    px_pool_mark_waiter(px);
    assert(atomic_exchange_explicit(&px->pool_waiters, 0,
                                    memory_order_acq_rel) == 1u);
    close(eng0->wake_fd);
    close(eng1->wake_fd);
    eng0->wake_fd = eng1->wake_fd = -1;

    pthread_mutex_destroy(&px->pool_lock);
    free(objs);
    free(px);
    puts("proxy_lane_queue_test: PASS");
    return 0;
}
