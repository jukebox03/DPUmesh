#ifndef DMESH_PEER_WIRE_VERBS_H
#define DMESH_PEER_WIRE_VERBS_H
#include "peer_wire.h"
struct ibv_context;
struct peer_verbs_endpoint {
    uint32_t qpn, psn;
    uint8_t gid[16];
    uint16_t mtu;
};
/* Manual RC setup for a TLS-negotiated lane. Shares the existing RDMA ring,
 * lease, completion and stale-WR fences. Device context is BORROWED; one owner
 * worker gets its own PD/CQ. No CM listen/connect or network discovery occurs.
 * The supplied GID must currently be IPv4 RoCEv2 on an active Ethernet port. */
int peer_wire_verbs_new(struct ibv_context *, uint8_t port, uint32_t gid_index,
                        uint16_t mtu, const struct peer_wire_ops **, void **wctx,
                        char *error, size_t error_len);
/* QP remains INIT with receives posted; no remote access keys are exchanged. */
int peer_wire_verbs_prepare(void *wctx, void **wc, struct peer_verbs_endpoint *local);
/* Call ONLY after both RX and TX hardware paths have completed installation.
 * protection_ready is the control owner's barrier result, never a wire claim.
 * RTR may emit automatic responses, so this guard precedes both RTR and RTS. */
int peer_wire_verbs_activate(void *wc, const struct peer_verbs_endpoint *remote,
                             int protection_ready);
/* Does not include application STREAM_ACK or host DMA; the worker must fence
 * those separately. 1 means every SEND completion and TX lease has retired. */
int peer_wire_verbs_send_drained(void *wc);
/* On failure retains QP/MR/memory for reconciliation; never frees live DMA
 * memory because a verbs destroy call failed. Caller keeps the context alive. */
int peer_wire_verbs_close(void *wc);
#endif
