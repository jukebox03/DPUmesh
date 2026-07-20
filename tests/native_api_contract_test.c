#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "src/dmesh_core.h"

static uint8_t reservation[64];
static int reserve_calls;
static int commit_calls;
static int full_drain_calls;
static dmesh_qp_t *tx_ready_qp;

uint8_t *
dpumesh_tx_reserve(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len)
{
    (void)ctx;
    assert(port == 17);
    assert(len == 32);
    reserve_calls++;
    return reservation;
}

int
dpumesh_tx_commit(dpumesh_ctx_t *ctx, uint16_t port,
                  const void *buf, uint32_t len)
{
    (void)ctx;
    commit_calls++;
    return port == 17 && buf == reservation && len == 32 ? 0 : -1;
}

int
dmesh_flush_full(dmesh_qp_t *qp)
{
    assert(qp != NULL);
    full_drain_calls++;
    return 0;
}

dmesh_qp_t *dmesh_accept(dmesh_cq_t *cq)
{
    (void)cq;
    errno = EAGAIN;
    return NULL;
}

void *dpumesh_next_tx_ready(struct dmesh_cq *cq)
{
    (void)cq;
    dmesh_qp_t *qp = tx_ready_qp;
    tx_ready_qp = NULL;
    return qp;
}

dmesh_qp_t *dmesh_next_ready(dmesh_cq_t *cq)
{
    (void)cq;
    return NULL;
}

int dpumesh_conn_recv(dpumesh_ctx_t *ctx, uint16_t port, sw_descriptor_t *out)
{
    (void)ctx;
    (void)port;
    (void)out;
    return 0;
}

uint8_t *dpumesh_rx_buf(dpumesh_ctx_t *ctx, int slot)
{
    (void)ctx;
    (void)slot;
    return NULL;
}

void dpumesh_rx_free(dpumesh_ctx_t *ctx, int slot)
{
    (void)ctx;
    (void)slot;
}

int
main(void)
{
    dmesh_channel_t channel = {0};
    dmesh_qp_t qp = {0};
    qp.ep = &channel;
    qp.local_port = 17;

    assert(dmesh_alloc(&qp, 32) == reservation);
    assert(reserve_calls == 1);

    /* post_send commits and asks the core to publish complete transport batches;
     * it does not force the trailing partial through the public dmesh_flush. */
    assert(dmesh_post_send(&qp, reservation, 32, 0, 0) == 0);
    assert(commit_calls == 1);
    assert(full_drain_calls == 1);

    errno = 0;
    assert(dmesh_post_send(&qp, reservation, 32, 0, 1) == -1);
    assert(errno == EINVAL);
    assert(commit_calls == 1);
    assert(full_drain_calls == 1);

    errno = 0;
    assert(dmesh_post_send(&qp, reservation + 1, 32, 0, 0) == -1);
    assert(errno == EINVAL);
    assert(commit_calls == 2);
    assert(full_drain_calls == 1);

    /* poll_cq exposes the core one-shot as a complete, payload-free API WC. */
    struct dmesh_cq cq = {0};
    dmesh_wc_t wc = {0};
    cq.ch = &channel;
    tx_ready_qp = &qp;
    assert(dmesh_poll_cq(&cq, &wc, 1) == 1);
    assert(wc.qp == &qp);
    assert(wc.opcode == DMESH_WC_TX_READY);
    assert(wc.buf == NULL && wc.len == 0 && wc.stream == 0 && wc.rx_slot == -1);
    assert(dmesh_poll_cq(&cq, &wc, 1) == 0);

    puts("native_api_contract_test: PASS");
    return 0;
}
