#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "src/dmesh_core.h"

static uint8_t reservation[64];
static int reserve_calls;
static int commit_calls;
static int full_drain_calls;

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

    puts("native_api_contract_test: PASS");
    return 0;
}
