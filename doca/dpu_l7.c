/* ===================================================================
 * dpu_l7.c — WRITE YOUR L7 PROXY HERE
 * ===================================================================
 *
 * This is the only file you edit. Implement dmesh_l7_route(): given the HEAD of
 * the front message, fill in a DECISION — how long the WHOLE message is, and
 * (optionally) which service/pod it goes to. The engine ships the whole message
 * (head + body) from staging via scatter-gather — you never see or copy the body.
 * Read dpu_l7.h for the full contract.
 *
 * Running this hook on a service IS the choice to route it PER MESSAGE: the codec
 * below tells the engine where each message ends, so the engine load-balances every
 * one of them across the service's live backends. A service with no codec is an
 * opaque byte stream and stays pinned to one backend.
 *
 * The default parses the benchmark's framing (bench/bench.h), so
 * `DPUMESH_PROXY_L7_SVC=<echo svc>` puts `./bench.sh` traffic on this path:
 *     [u32 magic][u32 seq][u32 payload_len][u32 aux][payload ...]
 * Replace the body with your own protocol.
 * ===================================================================
 */

#include "dpu_l7.h"
#include <string.h>

#define L7_HDR       16u                    /* [u32 magic][u32 seq][u32 payload_len][u32 aux] */
#define L7_REQ_MAGIC 0x62526571u            /* "bReq" — must match bench/bench.h */
#define L7_MSG_MAX   (16u * 1024u * 1024u)  /* sanity cap on one message */

int dmesh_l7_route(const uint8_t *buf, uint32_t len,
                   const struct dmesh_l7_ctx *ctx, struct dmesh_l7_decision *out)
{
    (void)ctx;
    if (len < L7_HDR)
        return 0;                          /* head not fully in the window yet */

    uint32_t magic, payload_len;
    memcpy(&magic,       buf + 0, 4);
    memcpy(&payload_len, buf + 8, 4);
    if (magic != L7_REQ_MAGIC || payload_len > L7_MSG_MAX - L7_HDR)
        return -1;                         /* not our protocol → drop the connection */

    out->total_len = L7_HDR + payload_len;

    /* cluster stays ctx->service (the client addressed it) and host stays DEFER, so the
     * engine load-balances THIS message over the cluster's live backends. To keep a
     * session on one pod instead, set out->host to one of ctx->hosts. */
    return 1;                              /* decided */
}
