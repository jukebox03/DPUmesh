#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This focused white-box test includes the production cursor implementation so it
 * can seed the otherwise-private per-QP TX state without constructing DOCA hardware. */
#include "../src/dmesh_core.c"

static void
seed(struct dpumesh_ctx *ctx, struct dmesh_port_slot *ports, uint8_t *dma)
{
    memset(ctx, 0, sizeof(*ctx));
    memset(ports, 0, 18 * sizeof(*ports));
    ctx->slot_size = 8192;
    ctx->block_size = 65536;
    ctx->blocks_per_conn = 2;
    ctx->dma_buffer = dma;
    ctx->ports = ports;
    atomic_init(&ctx->ports[17].tx_c, 0);
    atomic_init(&ctx->ports[17].tx_s, 0);
    atomic_init(&ctx->ports[17].tx_batch_state, TX_BATCH_IDLE);
    atomic_init(&ctx->ports[17].tx_batch_owner_active, 0);
    atomic_init(&ctx->ports[17].tx_generation, 0);
    atomic_init(&ctx->ports[17].tx_batch_epoch, 0);
    atomic_init(&ctx->ports[17].tx_tail_deadline_ns, 0);
    atomic_init(&ctx->ports[17].tx_fallback_deadline_ns, 0);
    for (int b = 0; b < TX_BLOCKS_PER_CONN; b++)
        atomic_init(&ctx->ports[17].blk_used[b], 0);

    struct dmesh_port_slot *psl = &ctx->ports[17];
    psl->nblk_owned = 2;
    psl->pblk[0] = 0;
    psl->pblk[1] = 1;
}

static void
test_automatic_tail_batcher(void)
{
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(18, sizeof(*ports));
    struct dma_desc *descs = calloc(8, sizeof(*descs));
    struct dma_ring_ctrl *ctrl = calloc(1, sizeof(*ctrl));
    uint8_t *dma = calloc(1, 65536);
    uint16_t *su_seq = calloc(16, sizeof(*su_seq));
    uint64_t *su_end = calloc(16, sizeof(*su_end));
    uint8_t *su_done = calloc(16, sizeof(*su_done));
    assert(ctx && ports && descs && ctrl && dma && su_seq && su_end && su_done);

    struct dma_ring ring = {
        .size = 8,
        .descs = descs,
        .ctrl = ctrl,
    };
    ctx->ports = ports;
    ctx->slot_size = 8192;
    ctx->block_size = 65536;
    ctx->blocks_per_conn = 1;
    ctx->num_slots = 8;
    ctx->k_rings = 1;
    ctx->su_depth = 16;
    ctx->dma_buffer = dma;
    ctx->dma_rings[0] = &ring;

    dmesh_channel_t channel = { .ctx = ctx };
    struct dmesh_eq eq = { .ch = &channel, .notify_efd = -1 };
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++) {
        atomic_init(&eq.tx_error[i], 0);
        atomic_init(&eq.tx_ready[i], 0);
    }
    atomic_init(&eq.tx_error_count, 0);
    atomic_init(&eq.tx_ready_count, 0);
    atomic_init(&eq.wants_notify, 0);
    atomic_init(&eq.suppress_notify, 0);

    dmesh_qp_t qp = {
        .ep = &channel,
        .eq = &eq,
        .role = DMESH_ROLE_CLIENT,
        .local_port = 17,
        .dst_service = 3,
    };
    struct dmesh_port_slot *psl = &ports[17];
    atomic_init(&psl->tx_c, 0);
    atomic_init(&psl->tx_s, 0);
    atomic_init(&psl->tx_batch_state, TX_BATCH_IDLE);
    atomic_init(&psl->tx_batch_owner_active, 0);
    atomic_init(&psl->tx_generation, 1);
    atomic_init(&psl->tx_batch_epoch, 0);
    atomic_init(&psl->tx_tail_deadline_ns, 0);
    atomic_init(&psl->tx_fallback_deadline_ns, 0);
    for (int b = 0; b < TX_BLOCKS_PER_CONN; b++)
        atomic_init(&psl->blk_used[b], 0);
    atomic_init(&psl->tx_error, 0);
    atomic_init(&psl->su_head, 0);
    atomic_init(&psl->su_tail, 0);
    atomic_init(&psl->tx_f, 0);
    psl->user = &qp;
    psl->eq = &eq;
    psl->nblk_owned = 1;
    psl->head_blk_next = 1;
    psl->pblk[0] = 0;
    psl->su_seq = su_seq;
    psl->su_end = su_end;
    psl->su_done = su_done;
    __atomic_store_n(&psl->role, DMESH_ROLE_CLIENT, __ATOMIC_RELEASE);
    assert(tx_batcher_start(ctx) == 0);

    /* An idle stream does not pay the deadline: its first partial publishes now. */
    psl->tx_w = 64;
    atomic_store_explicit(&psl->blk_used[0], 64, memory_order_release);
    atomic_store_explicit(&psl->tx_c, 64, memory_order_release);
    assert(dmesh_tx_call_begin(&qp) == 0);
    assert(dmesh_tx_after_commit(&qp) == 0);
    dmesh_tx_call_end(&qp);
    assert(__atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(descs[0].size == 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == 128);
    assert(psl->tx_w == 128);
    assert(atomic_load_explicit(&psl->tx_batch_state,
                                memory_order_acquire) == TX_BATCH_IDLE);

    /* With an earlier unit in flight, a new partial is retained and the channel
     * worker publishes it within the bounded deadline without facade help. */
    psl->tx_w = 192;
    atomic_store_explicit(&psl->blk_used[0], 192, memory_order_release);
    atomic_store_explicit(&psl->tx_c, 192, memory_order_release);
    uint64_t tail_committed_at = monotonic_ns();
    assert(dmesh_tx_call_begin(&qp) == 0);
    assert(dmesh_tx_after_commit(&qp) == 0);
    assert(atomic_load_explicit(&psl->tx_batch_state,
                                memory_order_acquire) == TX_BATCH_ARMED);
    assert(__atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE) == 1);
    assert(__atomic_load_n(&descs[1].publish_seq, __ATOMIC_ACQUIRE) == 0);
    dmesh_tx_call_end(&qp);
    for (int i = 0; i < 200 &&
         __atomic_load_n(&descs[1].publish_seq, __ATOMIC_ACQUIRE) != 2; i++)
        usleep(1000);
    uint64_t tail_published_at = monotonic_ns();
    assert(__atomic_load_n(&descs[1].publish_seq, __ATOMIC_ACQUIRE) == 2);
    assert(tail_published_at - tail_committed_at >= TX_TAIL_FALLBACK_NS);
    assert(tail_published_at - tail_committed_at < 200000000ull);
    tx_batch_wait_worker(psl);
    assert(__atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE) == 2);
    assert(descs[1].size == 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == 256);
    assert(psl->tx_w == 256);
    assert(atomic_load_explicit(&psl->tx_batch_state,
                                memory_order_acquire) == TX_BATCH_IDLE);

    /* A deadline pass publishes exactly the committed prefix it claimed. */
    psl->tx_w = 384;
    atomic_store_explicit(&psl->blk_used[0], 384, memory_order_release);
    atomic_store_explicit(&psl->tx_c, 384, memory_order_release);
    assert(dmesh_drain_tx_upto_locked(&qp, 1, 320) == 0);
    assert(__atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE) == 3);
    assert(descs[2].size == 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == 320);
    assert(dmesh_flush(&qp) == 0);
    assert(__atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE) == 4);
    assert(descs[3].size == 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == 384);

    /* ACK retirement does not shorten the batching deadline. */
    tx_reclaim_ack(ctx, 17, 1);
    tx_reclaim_ack(ctx, 17, 2);
    tx_reclaim_ack(ctx, 17, 3);
    tx_reclaim_ack(ctx, 17, 4);
    psl->tx_w = 384 + 8192 + 64;
    atomic_store_explicit(&psl->blk_used[0], (uint32_t)psl->tx_w,
                          memory_order_release);
    atomic_store_explicit(&psl->tx_c, psl->tx_w, memory_order_release);
    assert(dmesh_tx_after_commit(&qp) == 0);
    assert(__atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE) == 5);
    assert(descs[4].size == 8192);
    assert(atomic_load_explicit(&psl->tx_batch_state,
                                memory_order_acquire) == TX_BATCH_ARMED);
    uint64_t old_fallback = atomic_load_explicit(
        &psl->tx_fallback_deadline_ns, memory_order_acquire);
    tx_reclaim_ack(ctx, 17, 5);
    assert(atomic_load_explicit(&psl->tx_fallback_deadline_ns,
                                memory_order_acquire) == old_fallback);
    for (int i = 0; i < 200 &&
         __atomic_load_n(&descs[5].publish_seq, __ATOMIC_ACQUIRE) != 6; i++)
        usleep(1000);
    assert(__atomic_load_n(&descs[5].publish_seq, __ATOMIC_ACQUIRE) == 6);
    assert(descs[5].size == 64);

    /* Async failures are one-shot EQ notifications but remain sticky on the QP. */
    tx_error_publish(ctx, psl, 17, EIO);
    assert(dpumesh_next_tx_error(&eq) == &qp);
    assert(dpumesh_next_tx_error(&eq) == NULL);
    errno = 0;
    assert(dmesh_tx_call_begin(&qp) == -1 && errno == EIO);

    tx_batcher_stop(ctx);
    free(ctx->tx_batch_heap);
    pthread_cond_destroy(&ctx->tx_batch_cv);
    pthread_mutex_destroy(&ctx->tx_batch_lock);
    free(su_done);
    free(su_end);
    free(su_seq);
    free(dma);
    free(ctrl);
    free(descs);
    free(ports);
    free(ctx);
}

static void
test_commit_races_deadline_claim(void)
{
    enum { messages = 200, ring_size = 512 };
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(18, sizeof(*ports));
    struct dma_desc *descs = calloc(ring_size, sizeof(*descs));
    struct dma_ring_ctrl *ctrl = calloc(1, sizeof(*ctrl));
    uint8_t *dma = calloc(1, 65536);
    uint16_t *su_seq = calloc(ring_size, sizeof(*su_seq));
    uint64_t *su_end = calloc(ring_size, sizeof(*su_end));
    uint8_t *su_done = calloc(ring_size, sizeof(*su_done));
    assert(ctx && ports && descs && ctrl && dma && su_seq && su_end && su_done);

    struct dma_ring ring = {
        .size = ring_size,
        .descs = descs,
        .ctrl = ctrl,
    };
    ctx->ports = ports;
    ctx->slot_size = 8192;
    ctx->block_size = 65536;
    ctx->blocks_per_conn = 1;
    ctx->num_slots = 8;
    ctx->k_rings = 1;
    ctx->su_depth = ring_size;
    ctx->dma_buffer = dma;
    ctx->dma_rings[0] = &ring;

    dmesh_channel_t channel = { .ctx = ctx };
    struct dmesh_eq eq = { .ch = &channel, .notify_efd = -1 };
    atomic_init(&eq.wants_notify, 0);
    atomic_init(&eq.suppress_notify, 0);
    dmesh_qp_t qp = {
        .ep = &channel,
        .eq = &eq,
        .role = DMESH_ROLE_CLIENT,
        .local_port = 17,
        .dst_service = 3,
    };
    struct dmesh_port_slot *psl = &ports[17];
    atomic_init(&psl->tx_c, 0);
    atomic_init(&psl->tx_s, 0);
    atomic_init(&psl->tx_batch_state, TX_BATCH_IDLE);
    atomic_init(&psl->tx_batch_owner_active, 0);
    atomic_init(&psl->tx_generation, 1);
    atomic_init(&psl->tx_batch_epoch, 0);
    atomic_init(&psl->tx_tail_deadline_ns, 0);
    atomic_init(&psl->tx_fallback_deadline_ns, 0);
    atomic_init(&psl->tx_error, 0);
    atomic_init(&psl->su_head, 0);
    atomic_init(&psl->su_tail, 0);
    atomic_init(&psl->tx_f, 0);
    for (int b = 0; b < TX_BLOCKS_PER_CONN; b++)
        atomic_init(&psl->blk_used[b], 0);
    psl->user = &qp;
    psl->eq = &eq;
    psl->nblk_owned = 1;
    psl->head_blk_next = 1;
    psl->pblk[0] = 0;
    psl->su_seq = su_seq;
    psl->su_end = su_end;
    psl->su_done = su_done;
    __atomic_store_n(&psl->role, DMESH_ROLE_CLIENT, __ATOMIC_RELEASE);
    assert(tx_batcher_start(ctx) == 0);

    /* Commits continue across several 1 ms deadlines. The owner either keeps an
     * incomplete ARMED tail or claims a full unit; the worker may claim only the
     * atomically published prefix. TSan covers the actual overlap. */
    for (int i = 0; i < messages; i++) {
        psl->tx_w += 64;
        atomic_store_explicit(&psl->blk_used[0], (uint32_t)psl->tx_w,
                              memory_order_release);
        atomic_store_explicit(&psl->tx_c, psl->tx_w, memory_order_release);
        assert(dmesh_tx_call_begin(&qp) == 0);
        assert(dmesh_tx_after_commit(&qp) == 0);
        dmesh_tx_call_end(&qp);
        usleep(50);
    }
    assert(dmesh_flush(&qp) == 0);
    tx_batcher_stop(ctx);

    uint64_t tickets = __atomic_load_n(&ring.enq_pos, __ATOMIC_ACQUIRE);
    uint64_t bytes = 0;
    for (uint64_t i = 0; i < tickets; i++) {
        assert(__atomic_load_n(&descs[i].publish_seq, __ATOMIC_ACQUIRE) == i + 1);
        bytes += descs[i].size;
    }
    assert(bytes == (uint64_t)messages * 64);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_acquire) == psl->tx_w);
    assert((psl->tx_w & (DPA_DMA_COPY_ALIGN - 1u)) == 0);
    assert(atomic_load_explicit(&psl->tx_batch_state,
                                memory_order_acquire) == TX_BATCH_IDLE);

    free(ctx->tx_batch_heap);
    pthread_cond_destroy(&ctx->tx_batch_cv);
    pthread_mutex_destroy(&ctx->tx_batch_lock);
    free(su_done);
    free(su_end);
    free(su_seq);
    free(dma);
    free(ctrl);
    free(descs);
    free(ports);
    free(ctx);
}

static void
test_fallback_waits_for_active_owner(void)
{
    struct dmesh_port_slot psl = {0};
    atomic_init(&psl.tx_batch_state, TX_BATCH_ARMED);
    atomic_init(&psl.tx_batch_owner_active, 1);

    int expected = TX_BATCH_ARMED;
    assert(atomic_compare_exchange_strong_explicit(
        &psl.tx_batch_state, &expected, TX_BATCH_WORKER,
        memory_order_acq_rel, memory_order_acquire));
    assert(atomic_load_explicit(&psl.tx_batch_owner_active,
                                memory_order_acquire) == 1);
    atomic_store_explicit(&psl.tx_batch_owner_active, 0,
                          memory_order_release);
}

static void
test_commit_handoffs_to_deadline_worker(void)
{
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(18, sizeof(*ports));
    assert(ctx && ports);
    dmesh_channel_t channel = { .ctx = ctx };
    dmesh_qp_t qp = { .ep = &channel, .local_port = 17 };
    ctx->ports = ports;
    atomic_init(&ports[17].tx_batch_state, TX_BATCH_WORKER);
    atomic_init(&ports[17].tx_batch_owner_active, 0);

    assert(dmesh_tx_after_commit(&qp) == 0);
    assert(atomic_load_explicit(&ports[17].tx_batch_state,
                                memory_order_acquire) ==
           TX_BATCH_WORKER_DIRTY);

    int expected = TX_BATCH_WORKER_DIRTY;
    assert(atomic_compare_exchange_strong_explicit(
        &ports[17].tx_batch_state, &expected, TX_BATCH_WORKER,
        memory_order_acq_rel, memory_order_acquire));
    expected = TX_BATCH_WORKER;
    assert(atomic_compare_exchange_strong_explicit(
        &ports[17].tx_batch_state, &expected, TX_BATCH_IDLE,
        memory_order_acq_rel, memory_order_acquire));
    free(ports);
    free(ctx);
}

static uint8_t
stream_pattern(uint64_t offset)
{
    uint64_t x = offset * 0x9e3779b97f4a7c15ull + 0xd1b54a32d192ed03ull;
    x ^= x >> 29;
    return (uint8_t)x;
}

static void
test_large_commits_preserve_stream_bytes(void)
{
    enum {
        port = 17,
        block_size = 512 * 1024,
        blocks = 8,
        slot_size = 8192,
        ring_size = 4096,
        commits = 2048,
        commit_size = 32 * 1024 + 513,
        ack_window = 256,
    };

    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(port + 1, sizeof(*ports));
    struct dma_desc *descs = calloc(ring_size, sizeof(*descs));
    struct dma_ring_ctrl *ctrl = calloc(1, sizeof(*ctrl));
    uint8_t *dma = calloc(blocks, block_size);
    uint32_t *block_next = calloc(blocks, sizeof(*block_next));
    uint16_t *su_seq = calloc(512, sizeof(*su_seq));
    uint64_t *su_end = calloc(512, sizeof(*su_end));
    uint8_t *su_done = calloc(512, sizeof(*su_done));
    uint16_t pending[512] = {0};
    assert(ctx && ports && descs && ctrl && dma && block_next &&
           su_seq && su_end && su_done);

    struct dma_ring ring = {
        .size = ring_size,
        .descs = descs,
        .ctrl = ctrl,
    };
    ctx->ports = ports;
    ctx->slot_size = slot_size;
    ctx->block_size = block_size;
    ctx->blocks_per_conn = blocks;
    ctx->n_blocks = blocks;
    ctx->num_slots = blocks * block_size / slot_size;
    ctx->k_rings = 1;
    ctx->su_depth = 512;
    ctx->recycle_reserve = 1;
    ctx->dma_buffer = dma;
    ctx->block_next = block_next;
    ctx->dma_rings[0] = &ring;
    for (int i = 0; i < blocks; i++)
        block_next[i] = (uint32_t)(i + 1);
    atomic_init(&ctx->block_free, 0);
    atomic_init(&ctx->pool_epoch, 0);
    atomic_init(&ctx->pool_waiter_count, 0);
    atomic_init(&ctx->pool_wait_cursor, 0);
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&ctx->pool_waiters[i], 0);

    dmesh_channel_t channel = { .ctx = ctx };
    struct dmesh_eq eq = { .ch = &channel, .notify_efd = -1 };
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++) {
        atomic_init(&eq.tx_error[i], 0);
        atomic_init(&eq.tx_ready[i], 0);
    }
    atomic_init(&eq.tx_error_count, 0);
    atomic_init(&eq.tx_ready_count, 0);
    atomic_init(&eq.wants_notify, 0);
    atomic_init(&eq.suppress_notify, 0);

    dmesh_qp_t qp = {
        .ep = &channel,
        .eq = &eq,
        .role = DMESH_ROLE_CLIENT,
        .local_port = port,
        .dst_service = 3,
    };
    struct dmesh_port_slot *psl = &ports[port];
    atomic_init(&psl->tx_c, 0);
    atomic_init(&psl->tx_s, 0);
    atomic_init(&psl->tx_f, 0);
    atomic_init(&psl->tx_batch_state, TX_BATCH_IDLE);
    atomic_init(&psl->tx_batch_owner_active, 0);
    atomic_init(&psl->tx_generation, 1);
    atomic_init(&psl->tx_batch_epoch, 0);
    atomic_init(&psl->tx_tail_deadline_ns, 0);
    atomic_init(&psl->tx_fallback_deadline_ns, 0);
    atomic_init(&psl->tx_error, 0);
    atomic_init(&psl->tx_wait_state, DMESH_TX_WAIT_IDLE);
    atomic_init(&psl->tx_wait_reason, DMESH_TX_WAIT_NONE);
    atomic_init(&psl->tx_wait_tail_blk, 0);
    atomic_init(&psl->tx_wait_tx_w, 0);
    atomic_init(&psl->tx_wait_pool_epoch, 0);
    atomic_init(&psl->su_head, 0);
    atomic_init(&psl->su_tail, 0);
    for (int i = 0; i < TX_BLOCKS_PER_CONN; i++) {
        atomic_init(&psl->blk_used[i], 0);
        psl->pblk[i] = -1;
    }
    psl->user = &qp;
    psl->eq = &eq;
    psl->su_seq = su_seq;
    psl->su_end = su_end;
    psl->su_done = su_done;
    __atomic_store_n(&psl->role, DMESH_ROLE_CLIENT, __ATOMIC_RELEASE);
    assert(tx_batcher_start(ctx) == 0);

    uint64_t source_offset = 0;
    uint64_t copied_offset = 0;
    uint64_t ticket = 0;
    uint32_t ack_head = 0, ack_tail = 0;

#define CONSUME_PUBLISHED() do {                                                \
        for (;;) {                                                             \
            struct dma_desc *desc = &descs[ticket % ring_size];                \
            if (__atomic_load_n(&desc->publish_seq, __ATOMIC_ACQUIRE) !=       \
                    ticket + 1)                                                \
                break;                                                         \
            assert(desc->size > 0 && desc->size <= slot_size);                 \
            assert(dpa_dma_aligned_copy_len(                                  \
                       desc->addr - (uint64_t)(uintptr_t)                    \
                           ctx->dma_buffer, desc->size) <=                    \
                   DPA_DMA_COPY_MAX);                                         \
            const uint8_t *src = (const uint8_t *)(uintptr_t)desc->addr;        \
            for (uint32_t j = 0; j < desc->size; j++)                          \
                assert(src[j] == stream_pattern(copied_offset + j));            \
            copied_offset += desc->size;                                       \
            pending[ack_head++ & 511u] = desc->seq;                            \
            ticket++;                                                          \
            __atomic_store_n(&ctrl->consumer_head, ticket, __ATOMIC_RELEASE);  \
        }                                                                      \
        while (ack_head - ack_tail > ack_window)                               \
            tx_reclaim_ack(ctx, port, pending[ack_tail++ & 511u]);             \
    } while (0)

    for (int i = 0; i < commits; i++) {
        uint8_t *dst;
        for (;;) {
            assert(dmesh_tx_call_begin(&qp) == 0);
            dst = dpumesh_tx_reserve(ctx, port, commit_size);
            if (dst != NULL) break;
            assert(errno == EAGAIN);
            dmesh_tx_call_end(&qp);
            dmesh_tx_pressure(&qp);
            CONSUME_PUBLISHED();
            while (ack_tail != ack_head)
                tx_reclaim_ack(ctx, port, pending[ack_tail++ & 511u]);
            sched_yield();
        }
        for (uint32_t j = 0; j < commit_size; j++)
            dst[j] = stream_pattern(source_offset + j);
        assert(dpumesh_tx_commit(ctx, port, dst, commit_size) == 0);
        assert(dmesh_tx_after_commit(&qp) == 0);
        dmesh_tx_call_end(&qp);
        source_offset += commit_size;
        CONSUME_PUBLISHED();
    }
    assert(dmesh_flush(&qp) == 0);
    tx_batch_wait_worker(psl);
    for (int wait = 0; copied_offset != source_offset && wait < 1000; wait++) {
        CONSUME_PUBLISHED();
        if (copied_offset != source_offset)
            usleep(1000);
    }
    CONSUME_PUBLISHED();
    assert(copied_offset == source_offset);
    while (ack_tail != ack_head)
        tx_reclaim_ack(ctx, port, pending[ack_tail++ & 511u]);
    assert(atomic_load_explicit(&psl->tx_f, memory_order_acquire) == psl->tx_w);

#undef CONSUME_PUBLISHED
    tx_batcher_stop(ctx);
    free(ctx->tx_batch_heap);
    pthread_cond_destroy(&ctx->tx_batch_cv);
    pthread_mutex_destroy(&ctx->tx_batch_lock);
    free(su_done);
    free(su_end);
    free(su_seq);
    free(block_next);
    free(dma);
    free(ctrl);
    free(descs);
    free(ports);
    free(ctx);
}

int
main(void)
{
    /* Production defaults: a 64 MiB shared pool, 512 KiB contiguous extents,
     * and eight lazily-owned extents (4 MiB/QP). The reclaim FIFO must cover
     * all 512 possible 8 KiB transport units without a second admission point. */
    struct dpumesh_ctx *defaults = calloc(1, sizeof(*defaults));
    assert(defaults != NULL);
    init_config(defaults, NULL, DMESH_SVC_NONE);
    assert(defaults->num_slots == 8192);
    assert(defaults->slot_size == 8192);
    assert(defaults->block_size == 512 * 1024);
    assert(defaults->n_blocks == 128);
    assert(defaults->blocks_per_conn == 8);
    assert(defaults->su_depth == 512);
    free(defaults);

    struct dmesh_port_slot rx = {0};
    sw_descriptor_t d = { .seq = 7, .body_buf_slot = 100, .body_len = 10 };
    assert(rx_seq_accept(&rx, &d));
    d.body_buf_slot = 110;
    d.body_len = 5;
    assert(rx_seq_accept(&rx, &d));
    d.body_buf_slot = 100;
    d.body_len = 10;
    assert(!rx_seq_accept(&rx, &d));
    d.seq = 8;
    d.body_buf_slot = 200;
    d.body_len = 0;
    assert(rx_seq_accept(&rx, &d));
    assert(!rx_seq_accept(&rx, &d));
    d.seq = 7;
    assert(!rx_seq_accept(&rx, &d));

    /* Rejected descriptors return their landing credit. */
    struct dpumesh_ctx *credit_ctx = calloc(1, sizeof(*credit_ctx));
    struct dmesh_port_slot *credit_ports = calloc(18, sizeof(*credit_ports));
    struct dma_desc *credit_descs = calloc(2, sizeof(*credit_descs));
    assert(credit_ctx != NULL && credit_ports != NULL && credit_descs != NULL);
    struct dma_ring credit_ring = {
        .size = 1,
        .descs = credit_descs,
    };
    credit_ctx->ports = credit_ports;
    credit_ctx->k_rings = 1;
    credit_ctx->rx_region_size = 8192;
    credit_ctx->dma_rings[0] = &credit_ring;
    atomic_init(&credit_ports[17].role, DMESH_ROLE_CLIENT);
    credit_ports[17].rx_seq_valid = 1;
    credit_ports[17].rx_seq = 9;
    sw_descriptor_t stale = {
        .dst_port = 17,
        .seq = 8,
        .body_buf_slot = 0,
        .body_len = 1,
    };
    rx_deliver_desc(credit_ctx, &stale, 0);
    volatile uint64_t *returned =
        (volatile uint64_t *)(credit_descs + DMA_RING_CREDIT_SLOT(1));
    assert(*returned == 1);
    free(credit_descs);
    free(credit_ports);
    free(credit_ctx);

    struct dpumesh_ctx *geometry = calloc(1, sizeof(*geometry));
    assert(geometry != NULL);
    geometry->k_rings = 8;
    geometry->rx_dma_buf_size = 8u * DPUMESH_SLOT_SIZE;
    assert(configure_landing_geometry(geometry, 2) == DOCA_SUCCESS);
    assert(geometry->rx_region_size == 4u * DPUMESH_SLOT_SIZE);
    for (int r = 0; r < geometry->k_rings; r++) {
        struct dma_ring *ring = calloc(1, sizeof(*ring));
        assert(ring != NULL);
        ring->size = 1;
        ring->descs = calloc(2, sizeof(*ring->descs));
        assert(ring->descs != NULL);
        geometry->dma_rings[r] = ring;
    }
    for (int stripe = 0; stripe < 2; stripe++) {
        for (int shard = 0; shard < 4; shard++) {
            int pos = (int)((size_t)stripe * geometry->rx_region_size +
                            (size_t)shard * DPUMESH_SLOT_SIZE);
            assert(rx_credit_shard_index(geometry, pos) ==
                   stripe + shard * 2);
            rx_credit_return(geometry, pos);
        }
    }
    for (int r = 0; r < geometry->k_rings; r++) {
        volatile uint64_t *counter =
            (volatile uint64_t *)(geometry->dma_rings[r]->descs +
                                  DMA_RING_CREDIT_SLOT(1));
        assert(*counter == 1);
        free(geometry->dma_rings[r]->descs);
        free(geometry->dma_rings[r]);
    }
    free(geometry);

    static uint8_t dma[2 * 65536];
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(18, sizeof(*ports));
    assert(ctx != NULL);
    assert(ports != NULL);
    size_t moff = 0;
    uint32_t len = 0;
    struct dmesh_port_slot *psl;

    /* post_send mode keeps the newest fillable partial; flush mode forces it. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_c, 7000, memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 7000, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 0 && len == 7000);

    /* A full slot is immediately eligible, while its trailing partial remains. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_c, 9000, memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 9000, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 0 && len == 8192);
    atomic_fetch_add_explicit(&psl->tx_s, len, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 8192 && len == 808);

    /* An unaligned send head is capped so its aligned DPA DMA window remains
     * within one hardware operation. The following descriptor starts aligned. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_s, 64, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_c, 64 + 8192, memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 64 + 8192,
                          memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 64 && len == 8192 - 64);
    assert(dpa_dma_aligned_copy_len(moff, len) == DPA_DMA_COPY_MAX);
    atomic_fetch_add_explicit(&psl->tx_s, len, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 8192 && len == 64);
    assert(dpa_dma_aligned_copy_len(moff, len) == DPA_DMA_COPY_ALIGN);

    /* A short physical-block tail is sealed once later-block bytes commit. It must
     * ship before those later bytes even in full-only mode; only the newest partial
     * remains buffered. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    atomic_store_explicit(&psl->tx_s, 7 * 8192, memory_order_relaxed);
    atomic_store_explicit(&psl->tx_c, 65536 + 1000,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[0], 60000,
                          memory_order_relaxed);
    atomic_store_explicit(&psl->blk_used[1], 1000,
                          memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 7 * 8192 && len == 60000 - 7 * 8192);
    atomic_fetch_add_explicit(&psl->tx_s, len, memory_order_relaxed);
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(atomic_load_explicit(&psl->tx_s, memory_order_relaxed) == 65536);
    /* The production selector skipped the logical pad. */
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 65536 && len == 1000);

    free(ports);
    free(ctx);
    test_automatic_tail_batcher();
    test_commit_races_deadline_claim();
    test_fallback_waits_for_active_owner();
    test_commit_handoffs_to_deadline_worker();
    test_large_commits_preserve_stream_bytes();
    puts("native_tx_batch_policy_test: PASS");
    return 0;
}
