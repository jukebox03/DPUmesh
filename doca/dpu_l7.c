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
 * The default parses the SAME length-prefixed framing as the byte-stream
 * validator (bench/validators/stream_dpumesh.c):
 *     [u32 LE total_len (incl. this 5-byte header)][u8 svc][payload ...]
 * and routes each message to the service named by its `svc` byte (content
 * routing → decision.cluster), letting the engine LOAD-BALANCE across that
 * service's live backends (decision.host = DEFER). Because it matches the
 * validator, `./bench.sh stream` can drive this L7 path directly.
 * Replace the body with your own protocol.
 * ===================================================================
 */

#include "dpu_l7.h"
#include <string.h>

#define L7_HDR      5u                     /* [u32 total_len][u8 svc] */
#define L7_MSG_MAX  (16u * 1024u * 1024u)  /* sanity cap on one message */

int dmesh_l7_route(const uint8_t *buf, uint32_t len,
                   const struct dmesh_l7_ctx *ctx, struct dmesh_l7_decision *out)
{
    if (len < L7_HDR)
        return 0;                          /* 5-byte head not fully in the window yet */

    uint32_t total;
    memcpy(&total, buf, 4);                /* [u32 LE total_len], includes this 5-byte header */
    if (total < L7_HDR || total > L7_MSG_MAX)
        return -1;                         /* implausible → drop the connection */

    out->total_len = total;                /* we know the whole length from the head */

    /* Content routing by the svc byte. 0xFF or out-of-range → keep the client's
     * addressed service (out->cluster is pre-set to ctx->service). The engine then
     * load-balances the chosen service across its live backends (out->host stays
     * DEFER); to pin a specific pod instead, set out->host = one of ctx->hosts. */
    uint8_t svc = buf[4];
    if (svc != 0xFF && svc < 128)
        out->cluster = svc;

    return 1;                              /* decided */
}
