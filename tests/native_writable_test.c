#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White-box coverage for the production EAGAIN -> TX_READY state machine. Keeping
 * the test at core level makes ACK and shared-pool transitions deterministic without
 * requiring a DPU or weakening the public API with test-only controls. */
#include "../src/dmesh_core.c"

#define TEST_PORT 17

struct fixture {
    struct dpumesh_ctx *ctx;
    struct dmesh_port_slot *ports;
    struct dmesh_eq *eq;
    dmesh_channel_t channel;
    dmesh_qp_t qp;
    uint8_t dma[128];
    uint32_t block_next[2];
};

static void fixture_init(struct fixture *f, int pool_empty)
{
    memset(f, 0, sizeof(*f));
    f->ctx = calloc(1, sizeof(*f->ctx));
    f->ports = calloc(TEST_PORT + 1, sizeof(*f->ports));
    f->eq = calloc(1, sizeof(*f->eq));
    assert(f->ctx != NULL && f->ports != NULL && f->eq != NULL);

    f->ctx->slot_size = 16;
    f->ctx->block_size = 64;
    f->ctx->maxb = 2;
    f->ctx->su_depth = 8;       /* 2 blocks × 4 transport units/block */
    f->ctx->cushion_h = 1;
    f->ctx->n_blocks = 2;
    f->ctx->dma_buffer = f->dma;
    f->ctx->ports = f->ports;
    f->ctx->block_next = f->block_next;
    f->block_next[0] = 1;
    f->block_next[1] = 2;
    atomic_init(&f->ctx->block_free,
                (uint_fast64_t)(pool_empty ? f->ctx->n_blocks : 0));
    atomic_init(&f->ctx->pool_epoch, (uint_fast64_t)0);
    atomic_init(&f->ctx->pool_waiter_count, (uint_fast32_t)0);
    atomic_init(&f->ctx->pool_wait_cursor, (uint_fast32_t)0);
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&f->ctx->pool_waiters[i], (uint_fast64_t)0);

    f->channel.ctx = f->ctx;
    f->eq->ch = &f->channel;
    f->eq->notify_efd = -1;
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&f->eq->tx_ready[i], (uint_fast64_t)0);
    atomic_init(&f->eq->tx_ready_count, (uint_fast32_t)0);

    f->qp.ep = &f->channel;
    f->qp.eq = f->eq;
    f->qp.local_port = TEST_PORT;
    struct dmesh_port_slot *psl = &f->ports[TEST_PORT];
    psl->role = DMESH_ROLE_CLIENT;
    psl->user = &f->qp;
    psl->eq = f->eq;
    for (int i = 0; i < DMESH_TX_MAXB_CAP; i++) psl->pblk[i] = -1;
    atomic_init(&psl->tx_f, (uint_fast64_t)0);
    atomic_init(&psl->su_head, (uint_fast16_t)0);
    atomic_init(&psl->su_tail, (uint_fast16_t)0);
    atomic_init(&psl->tx_wait_state, (uint_fast32_t)DMESH_TX_WAIT_IDLE);
    atomic_init(&psl->tx_wait_reason, (uint_fast32_t)DMESH_TX_WAIT_NONE);
    atomic_init(&psl->tx_wait_tail_blk, (uint_fast64_t)0);
    atomic_init(&psl->tx_wait_tx_w, (uint_fast64_t)0);
    atomic_init(&psl->tx_wait_pool_epoch, (uint_fast64_t)0);
}

static void fixture_destroy(struct fixture *f)
{
    free(f->ports[TEST_PORT].su_seq);
    free(f->ports[TEST_PORT].su_end);
    free(f->ports[TEST_PORT].su_done);
    free(f->eq);
    free(f->ports);
    free(f->ctx);
}

static void fill_qp_window(struct fixture *f)
{
    struct dmesh_port_slot *psl = &f->ports[TEST_PORT];
    psl->tx_w = psl->tx_c = psl->tx_s = 128;
    psl->tail_blk = 0;
    psl->head_blk_next = 2;
    psl->nblk_owned = 2;
    psl->pblk[0] = 0;
    psl->pblk[1] = 1;
}

static void test_qp_window_wakes_at_block_reclaim(void)
{
    struct fixture f;
    fixture_init(&f, 1);
    fill_qp_window(&f);

    errno = 0;
    assert(dpumesh_tx_reserve(f.ctx, TEST_PORT, 8) == NULL);
    assert(errno == EAGAIN);
    struct dmesh_port_slot *psl = &f.ports[TEST_PORT];
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_ARMED);
    assert(atomic_load(&psl->tx_wait_reason) == DMESH_TX_WAIT_QP_RECLAIM);

    /* An ACK within the same physical block does not make a new block admissible. */
    psl->su_seq[0] = 1;
    psl->su_end[0] = 32;
    psl->su_done[0] = 0;
    psl->su_seq[1] = 2;
    psl->su_end[1] = 64;
    psl->su_done[1] = 0;
    atomic_store(&psl->su_head, (uint_fast16_t)2);
    tx_reclaim_ack(f.ctx, TEST_PORT, 1);
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_ARMED);
    assert(atomic_load(&f.eq->tx_ready_count) == 0);

    /* Crossing the block boundary produces exactly one EQ event. */
    tx_reclaim_ack(f.ctx, TEST_PORT, 2);
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_READY);
    assert(atomic_load(&f.eq->tx_ready_count) == 1);
    assert(dpumesh_next_tx_ready(f.eq) == &f.qp);
    assert(dpumesh_next_tx_ready(f.eq) == NULL);
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_IDLE);
    fixture_destroy(&f);
}

static void test_out_of_order_ack_waits_for_contiguous_prefix(void)
{
    struct fixture f;
    fixture_init(&f, 0);
    struct dmesh_port_slot *psl = &f.ports[TEST_PORT];
    uint8_t *buf = dpumesh_tx_reserve(f.ctx, TEST_PORT, 32);
    assert(buf != NULL);
    assert(dpumesh_tx_commit(f.ctx, TEST_PORT, buf, 32) == 0);
    size_t moff;
    uint32_t len;
    assert(dpumesh_tx_next_send(f.ctx, TEST_PORT, 0, &moff, &len) == 1 && len == 16);
    dpumesh_tx_sent(f.ctx, TEST_PORT, 100, len);
    assert(dpumesh_tx_next_send(f.ctx, TEST_PORT, 0, &moff, &len) == 1 && len == 16);
    dpumesh_tx_sent(f.ctx, TEST_PORT, 101, len);

    tx_reclaim_ack(f.ctx, TEST_PORT, 101);       /* later engine finishes first */
    assert(atomic_load(&psl->tx_f) == 0);
    assert(atomic_load(&psl->su_tail) == 0);
    tx_reclaim_ack(f.ctx, TEST_PORT, 100);
    assert(atomic_load(&psl->tx_f) == 32);
    assert(atomic_load(&psl->su_tail) == 2);
    tx_reclaim_ack(f.ctx, TEST_PORT, 101);       /* stale duplicate */
    assert(atomic_load(&psl->tx_f) == 32);
    fixture_destroy(&f);
}

static void test_arm_recheck_closes_lost_wakeup(void)
{
    struct fixture f;
    fixture_init(&f, 1);
    fill_qp_window(&f);
    struct dmesh_port_slot *psl = &f.ports[TEST_PORT];

    /* Model an ACK that wins immediately before ARMED is published. The arm-side
     * snapshot recheck must queue readiness without waiting for another ACK. */
    atomic_store(&psl->tx_f, (uint_fast64_t)64);
    tx_wait_arm(f.ctx, psl, TEST_PORT, DMESH_TX_WAIT_QP_RECLAIM);
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_READY);
    assert(dpumesh_next_tx_ready(f.eq) == &f.qp);
    fixture_destroy(&f);
}

static void test_shared_pool_return_and_direct_retry(void)
{
    struct fixture f;
    fixture_init(&f, 1);
    struct dmesh_port_slot *psl = &f.ports[TEST_PORT];

    errno = 0;
    assert(dpumesh_tx_reserve(f.ctx, TEST_PORT, 8) == NULL);
    assert(errno == EAGAIN);
    assert(atomic_load(&psl->tx_wait_reason) == DMESH_TX_WAIT_SHARED_POOL);
    assert(atomic_load(&f.ctx->pool_waiter_count) == 1);

    block_pool_return(f.ctx, 0);
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_READY);
    assert(atomic_load(&f.eq->tx_ready_count) == 1);

    /* Polling applications may retry first. Success consumes the block and cancels
     * the obsolete queued event, preserving one-shot semantics. */
    assert(dpumesh_tx_reserve(f.ctx, TEST_PORT, 8) == f.dma);
    assert(atomic_load(&psl->tx_wait_state) == DMESH_TX_WAIT_IDLE);
    assert(atomic_load(&f.eq->tx_ready_count) == 0);
    assert(dpumesh_next_tx_ready(f.eq) == NULL);
    fixture_destroy(&f);
}

static void test_pool_eagain_does_not_commit_padding(void)
{
    struct fixture f;
    fixture_init(&f, 1);
    struct dmesh_port_slot *psl = &f.ports[TEST_PORT];
    psl->tx_w = psl->tx_c = psl->tx_s = 60;
    psl->head_blk_next = 1;
    psl->nblk_owned = 1;
    psl->pblk[0] = 0;
    psl->blk_used[0] = 60;

    /* The 8-byte message needs padding into a new block, but the pool is empty.
     * EAGAIN must leave the logical write head untouched so retry is a no-op. */
    errno = 0;
    assert(dpumesh_tx_reserve(f.ctx, TEST_PORT, 8) == NULL);
    assert(errno == EAGAIN);
    assert(psl->tx_w == 60 && psl->tx_c == 60 && psl->tx_s == 60);
    assert(psl->head_blk_next == 1 && psl->pblk[0] == 0);
    fixture_destroy(&f);
}

static void test_default_four_mib_window_and_dynamic_fifo(void)
{
    struct dpumesh_ctx *ctx = calloc(1, sizeof(*ctx));
    struct dmesh_port_slot *ports = calloc(TEST_PORT + 1, sizeof(*ports));
    uint8_t *dma = calloc(1, 4u * 1024u * 1024u);
    uint32_t *next = calloc(8, sizeof(*next));
    assert(ctx && ports && dma && next);

    ctx->slot_size = 8192;
    ctx->block_size = 512 * 1024;
    ctx->maxb = 8;
    ctx->su_depth = 512;
    ctx->cushion_h = 1;
    ctx->n_blocks = 8;
    ctx->dma_buffer = dma;
    ctx->ports = ports;
    ctx->block_next = next;
    for (int i = 0; i < 8; i++) next[i] = (uint32_t)(i + 1);
    atomic_init(&ctx->block_free, (uint_fast64_t)0);
    atomic_init(&ctx->pool_epoch, (uint_fast64_t)0);
    for (uint32_t i = 0; i < DMESH_TX_READY_WORDS; i++)
        atomic_init(&ctx->pool_waiters[i], (uint_fast64_t)0);

    struct dmesh_port_slot *psl = &ports[TEST_PORT];
    psl->role = DMESH_ROLE_CLIENT;
    for (int i = 0; i < DMESH_TX_MAXB_CAP; i++) psl->pblk[i] = -1;
    atomic_init(&psl->tx_f, (uint_fast64_t)0);
    atomic_init(&psl->su_head, (uint_fast16_t)0);
    atomic_init(&psl->su_tail, (uint_fast16_t)0);
    atomic_init(&psl->tx_wait_state, (uint_fast32_t)DMESH_TX_WAIT_IDLE);
    atomic_init(&psl->tx_wait_reason, (uint_fast32_t)DMESH_TX_WAIT_NONE);

    for (int i = 0; i < 8; i++) {
        uint8_t *buf = dpumesh_tx_reserve(ctx, TEST_PORT, 512 * 1024);
        assert(buf != NULL);
        assert(dpumesh_tx_commit(ctx, TEST_PORT, buf, 512 * 1024) == 0);
    }
    assert(psl->tx_w == 4u * 1024u * 1024u);
    errno = 0;
    assert(dpumesh_tx_reserve(ctx, TEST_PORT, 1) == NULL);
    assert(errno == EAGAIN);

    size_t moff;
    uint32_t len;
    for (uint16_t seq = 1; seq <= 512; seq++) {
        assert(dpumesh_tx_next_send(ctx, TEST_PORT, 0, &moff, &len) == 1);
        assert(len == 8192);
        dpumesh_tx_sent(ctx, TEST_PORT, seq, len);
    }
    assert(dpumesh_tx_next_send(ctx, TEST_PORT, 0, &moff, &len) == 0);
    assert(atomic_load(&psl->su_head) == 512);

    /* The DPU batches these exact ACKs into four control messages. Once the
     * complete 512 KiB prefix is acknowledged, the next reserve recycles that
     * extent while the remaining 3.5 MiB stays in flight. */
    for (uint16_t seq = 1; seq <= 64; seq++)
        tx_reclaim_ack(ctx, TEST_PORT, seq);
    assert(atomic_load(&psl->tx_f) == 512u * 1024u);
    assert(dpumesh_tx_reserve(ctx, TEST_PORT, 1) != NULL);

    free(psl->su_seq);
    free(psl->su_end);
    free(psl->su_done);
    free(next);
    free(dma);
    free(ports);
    free(ctx);
}

int main(void)
{
    test_qp_window_wakes_at_block_reclaim();
    test_out_of_order_ack_waits_for_contiguous_prefix();
    test_arm_recheck_closes_lost_wakeup();
    test_shared_pool_return_and_direct_retry();
    test_pool_eagain_does_not_commit_padding();
    test_default_four_mib_window_and_dynamic_fifo();
    puts("native_writable_test: PASS");
    return 0;
}
