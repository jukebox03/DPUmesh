#ifndef DPU_L7_H
#define DPU_L7_H

/* L7 request-routing hook implemented in dpu_l7.c.
 *
 * The hook reads a bounded message head and returns total message length,
 * destination service, and an optional backend override. The L4 engine streams
 * the complete body through SG-DMA and handles replies. */

#include <stdint.h>

/* Read-only context for one routing decision (no DPU/DOCA types). */
struct dmesh_l7_ctx {
    int32_t        service;      /* the service the client addressed = default cluster */
    int32_t        client_pod;   /* who sent this stream (informational) */
    uint16_t       client_port;  /* the client's port    (informational) */
    /* The cluster's live backend endpoints (pods of `service`, healthy + ready),
     * so you can pick a host YOURSELF (e.g. consistent hashing / session table)
     * and return it in decision.host. Leave decision.host = DEFER to let the engine
     * load-balance instead — then you can ignore these. Valid only during the call. */
    const int32_t *hosts;
    int32_t        n_hosts;
};

/* decision.host sentinel: let the engine's load balancer pick this message's backend
 * (round-robin over ctx->hosts). Per message — there is no connection pin here. */
#define DMESH_LB_DEFER (-1)

/* What YOU fill in per message (the engine zero-inits it before the call, then
 * pre-sets cluster = ctx->service and host = DMESH_LB_DEFER, so leaving them
 * untouched routes to the addressed service and load-balances). */
struct dmesh_l7_decision {
    uint32_t total_len;   /* REQUIRED: the front message is this many bytes TOTAL
                           * (head + body). MAY be much larger than the head window;
                           * the engine ships all of it from staging via SG. */
    int32_t  cluster;     /* route to this SERVICE id (0..127); default = ctx->service.
                           * Content routing: overwrite to send to a different service. */
    int32_t  host;        /* DMESH_LB_DEFER = the engine load-balances the cluster;
                           * >=0 = OVERRIDE — route to this exact pod (must be a live
                           * backend, else the message is dropped). */
};

/* Inspect buf[0,len) without blocking or allocation. Return >0 with a complete
 * decision, 0 for an incomplete head, or <0 for a protocol error. The hook is
 * stateless across retries; total_len may exceed the head window. */
int dmesh_l7_route(const uint8_t *buf, uint32_t len,
                   const struct dmesh_l7_ctx *ctx, struct dmesh_l7_decision *out);

#endif /* DPU_L7_H */
