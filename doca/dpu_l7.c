/* Length-prefixed RPC codec used by the DPUmesh L7 validator. */

#include "dpu_l7.h"
#include <string.h>

#define L7_HDR       16u                    /* [u32 magic][u32 seq][u32 payload_len][u32 aux] */
#define L7_REQ_MAGIC 0x62526571u            /* "bReq" */
#define L7_REP_MAGIC 0x62526570u            /* "bRep" */
#define L7_MSG_MAX   (128u * 1024u)

int dmesh_l7_decode(enum dmesh_l7_direction direction,
                    const uint8_t *buf, uint32_t len,
                    const struct dmesh_l7_ctx *ctx,
                    struct dmesh_l7_decision *out)
{
    (void)direction;
    (void)ctx;
    if (len < L7_HDR)
        return 0;                          /* head not fully in the window yet */

    uint32_t magic, payload_len;
    memcpy(&magic,       buf + 0, 4);
    memcpy(&payload_len, buf + 8, 4);
    if ((magic != L7_REQ_MAGIC && magic != L7_REP_MAGIC) ||
        payload_len > L7_MSG_MAX - L7_HDR)
        return -1;                         /* not our protocol → drop the connection */

    out->total_len = L7_HDR + payload_len;

    /* Request routing defaults remain unchanged. */
    return 1;                              /* decided */
}
