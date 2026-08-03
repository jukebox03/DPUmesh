#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "src/dmesh_core.h"

static uint8_t reservation[64];
static int reserve_calls;
static int reserve_error;
static int commit_calls;
static int after_commit_calls;
static int tx_call_depth;
static int pressure_calls;
static int rx_free_calls;
static int last_rx_slot = -1;
static dmesh_qp_t *tx_ready_qp;
static dmesh_qp_t *tx_error_qp;
static int accept_calls;
static int accept_errno = EAGAIN;

uint8_t *
dpumesh_tx_reserve(dpumesh_ctx_t *ctx, uint16_t port, uint32_t len)
{
    (void)ctx;
    assert(port == 17);
    assert(len == 32);
    reserve_calls++;
    if (reserve_error) { errno = reserve_error; return NULL; }
    return reservation;
}

int
dpumesh_tx_commit(dpumesh_ctx_t *ctx, uint16_t port,
                  const void *buf, uint32_t len)
{
    (void)ctx;
    commit_calls++;
    if (port == 17 && buf == reservation && len == 32) return 0;
    errno = EINVAL;
    return -1;
}

int dmesh_tx_call_begin(dmesh_qp_t *qp) { assert(qp != NULL); tx_call_depth++; return 0; }
void dmesh_tx_call_end(dmesh_qp_t *qp) { assert(qp != NULL); assert(tx_call_depth == 1); tx_call_depth--; }
int dmesh_tx_after_commit(dmesh_qp_t *qp) { assert(qp != NULL); after_commit_calls++; return 0; }
void dmesh_tx_pressure(dmesh_qp_t *qp) { assert(qp != NULL); pressure_calls++; }

dmesh_qp_t *dmesh_accept(dmesh_eq_t *eq)
{
    (void)eq;
    accept_calls++;
    errno = accept_errno;
    return NULL;
}

void *dpumesh_next_tx_ready(struct dmesh_eq *eq)
{
    (void)eq;
    dmesh_qp_t *qp = tx_ready_qp;
    tx_ready_qp = NULL;
    return qp;
}

void *dpumesh_next_tx_error(struct dmesh_eq *eq)
{
    (void)eq;
    dmesh_qp_t *qp = tx_error_qp;
    tx_error_qp = NULL;
    return qp;
}

dmesh_qp_t *dmesh_next_ready(dmesh_eq_t *eq)
{
    (void)eq;
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
    rx_free_calls++;
    last_rx_slot = slot;
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
    assert(tx_call_depth == 1 && pressure_calls == 0);
    assert(dmesh_post_send(&qp, reservation, 32) == 0);
    assert(commit_calls == 1);
    assert(after_commit_calls == 1);
    assert(tx_call_depth == 0);

    reserve_error = EAGAIN;
    assert(dmesh_alloc(&qp, 32) == NULL && errno == EAGAIN);
    assert(pressure_calls == 1 && tx_call_depth == 0);
    reserve_error = 0;

    /* post_send commits and asks the core to publish complete transport batches;
     * it does not force the trailing partial through the public dmesh_flush. */
    assert(dmesh_alloc(&qp, 32) == reservation);
    assert(tx_call_depth == 1);
    assert(dmesh_post_send(&qp, reservation, 32) == 0);
    assert(commit_calls == 2);
    assert(after_commit_calls == 2);
    assert(tx_call_depth == 0);

    errno = 0;
    assert(dmesh_alloc(&qp, 32) == reservation);
    assert(dmesh_post_send(&qp, reservation + 1, 32) == -1);
    assert(errno == EINVAL);
    assert(commit_calls == 3);
    assert(after_commit_calls == 2);
    assert(tx_call_depth == 0);

    /* poll_eq exposes the core one-shot as a payload-free API event. */
    struct dmesh_eq eq = {0};
    dmesh_event_t event = {0};
    eq.ch = &channel;
    accept_errno = ENOMEM;
    accept_calls = 0;
    assert(dmesh_poll_eq(&eq, &event, 1) == 0);
    assert(accept_calls == 1);
    accept_errno = EAGAIN;
    tx_error_qp = &qp;
    assert(dmesh_poll_eq(&eq, &event, 1) == 1);
    assert(event.qp == &qp);
    assert(event.type == DMESH_EVENT_TX_ERROR);
    assert(event.buf == NULL && event.len == 0 && event._rx_token == -1);
    tx_ready_qp = &qp;
    assert(dmesh_poll_eq(&eq, &event, 1) == 1);
    assert(event.qp == &qp);
    assert(event.type == DMESH_EVENT_TX_READY);
    assert(event.buf == NULL && event.len == 0 && event._rx_token == -1);
    assert(dmesh_poll_eq(&eq, &event, 1) == 0);

    uint8_t payload = 0x5a;
    event = (dmesh_event_t){
        .qp = &qp,
        .type = DMESH_EVENT_RECV,
        .buf = &payload,
        .len = 1,
        ._rx_token = 23,
    };
    dmesh_release_rx_buffer(&channel, &event);
    assert(rx_free_calls == 1 && last_rx_slot == 23);
    assert(event.buf == NULL && event._rx_token == -1);
    assert(event.type == DMESH_EVENT_RECV && event.len == 1);
    dmesh_release_rx_buffer(&channel, &event);
    assert(rx_free_calls == 1);

    puts("native_api_contract_test: PASS");
    return 0;
}
