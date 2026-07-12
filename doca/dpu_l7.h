#ifndef DPU_L7_H
#define DPU_L7_H

/* ===================================================================
 * dpu_l7.h — the L7 proxy hook (write your parser in dpu_l7.c)
 * ===================================================================
 *
 * This is the ONLY interface an L7 author needs. You implement ONE function,
 * `dmesh_l7_route`, in dpu_l7.c. Everything else (byte-stream reassembly,
 * scatter-gather DMA, custody, credits, load balancing, connection pooling, the
 * DPU/DOCA machinery) is handled for you by the L4 engine — you never see it.
 *
 * Mental model (Envoy's router filter): a connection is a byte stream. You are
 * shown only the HEAD of the message at the FRONT (up to a small bounded window)
 * and asked: "how long is this whole message, and where does it go?" You read the
 * head (length prefix / routing key) and fill in a DECISION. The engine then
 * forwards the whole message (head + body, even the parts you never saw) from
 * staging via scatter-gather DMA — the BODY is never copied — and calls you again
 * for the next message. This is why a large message costs no per-slot memcpy.
 *
 * Envoy parity — you decide, the engine executes:
 *   - cluster : which SERVICE the message routes to (default = the addressed one).
 *   - host    : leave DEFER and the engine's LOAD BALANCER picks a backend of the
 *               cluster (round-robin over its live endpoints — ctx->hosts); OR set
 *               a specific pod to OVERRIDE (Envoy setUpstreamOverrideHost — session
 *               persistence). The engine also does connection-scoped stickiness for
 *               you (see DPUMESH_LB_PER_REQUEST_SVC) so you need not remember pods.
 *
 * You are called only for REQUEST streams. Replies are forwarded back to the
 * client automatically — you never handle them.
 * ===================================================================
 */

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

/* decision.host sentinel: let the engine's load balancer pick (round-robin over
 * ctx->hosts, with connection stickiness unless the service is per-request). */
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
    /* phase 2 (append-only, non-breaking): uint64_t hash;  void *session; */
};

/*
 * dmesh_l7_route — YOU IMPLEMENT THIS (in dpu_l7.c).
 *
 * buf[0 .. len) is the HEAD of the front message — NOT the whole message. `len`
 * is a small bounded window (the body usually is NOT here). Read the head only.
 *
 *   return  > 0 : DECIDED. Fill `out` (at least out->total_len). The engine ships
 *                 out->total_len bytes from staging (even the body you never saw)
 *                 to the resolved backend, then calls you again at the next message.
 *
 *   return  == 0: the HEAD is not fully in buf yet. The engine gives you a few more
 *                 contiguous head bytes and calls again. (Used when a head straddles
 *                 a slot boundary.) Do not rely on `out`.
 *
 *   return  <  0: malformed / protocol violation → the engine drops (poisons) this
 *                 connection.
 *
 * CONTRACT (keep these and you cannot corrupt the engine):
 *   1. Determine total_len from the HEAD (e.g. a length prefix). Do NOT wait for the
 *      body — you will not see it. Return 0 ONLY when the head itself is incomplete;
 *      return >0 as soon as you know the total length.
 *   2. Read only within buf[0 .. len). Never read past `len`.
 *   3. Be STATELESS. On a 0-return you are re-invoked from the same buf[0] with more
 *      head bytes. Re-parse.
 *   4. The HEAD must fit the head window (<= 4 KB, PX_HEAD_MAX). A head that needs
 *      more than that poisons the conn (cf. Envoy max_request_headers_kb). The BODY
 *      has no such limit — it streams.
 *   5. out->cluster must be a SERVICE id (0..127). out->host is either DMESH_LB_DEFER
 *      or one of ctx->hosts (a pod). An unknown service / dead host drops the message.
 *   6. Never block, never malloc/free, never log — this is the hot path.
 *   7. Runs on a single thread — no locking needed.
 */
int dmesh_l7_route(const uint8_t *buf, uint32_t len,
                   const struct dmesh_l7_ctx *ctx, struct dmesh_l7_decision *out);

#endif /* DPU_L7_H */
