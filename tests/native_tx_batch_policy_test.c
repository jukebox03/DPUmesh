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
    ctx->maxb = 2;
    ctx->dma_buffer = dma;
    ctx->ports = ports;

    struct dmesh_port_slot *psl = &ctx->ports[17];
    psl->nblk_owned = 2;
    psl->pblk[0] = 0;
    psl->pblk[1] = 1;
}

int
main(void)
{
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
    psl->tx_c = 7000;
    psl->blk_used[0] = 7000;
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 0 && len == 7000);

    /* A full slot is immediately eligible, while its trailing partial remains. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    psl->tx_c = 9000;
    psl->blk_used[0] = 9000;
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 0 && len == 8192);
    psl->tx_s += len;
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 8192 && len == 808);

    /* A short physical-block tail is sealed once later-block bytes commit. It must
     * ship before those later bytes even in full-only mode; only the newest partial
     * remains buffered. */
    seed(ctx, ports, dma);
    psl = &ctx->ports[17];
    psl->tx_s = 7 * 8192;
    psl->tx_c = 65536 + 1000;
    psl->blk_used[0] = 60000;
    psl->blk_used[1] = 1000;
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 1);
    assert(moff == 7 * 8192 && len == 60000 - 7 * 8192);
    psl->tx_s += len;
    assert(dpumesh_tx_next_send(ctx, 17, 0, &moff, &len) == 0);
    assert(psl->tx_s == 65536); /* the production selector skipped the logical pad */
    assert(dpumesh_tx_next_send(ctx, 17, 1, &moff, &len) == 1);
    assert(moff == 65536 && len == 1000);

    free(ports);
    free(ctx);
    puts("native_tx_batch_policy_test: PASS");
    return 0;
}
