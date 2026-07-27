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

    struct dmesh_port_slot *psl = &ctx->ports[17];
    psl->nblk_owned = 2;
    psl->pblk[0] = 0;
    psl->pblk[1] = 1;
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
