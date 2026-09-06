#ifndef DMESH_PEER_WIRE_H
#define DMESH_PEER_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* The byte carrier underneath the peer channel's authenticated session.
 *
 * It moves whole messages and knows nothing else: not that a message holds
 * ciphertext, not what the channel's own frames look like, not that a
 * handshake precedes them. That is the whole point — TCP and an RDMA queue
 * pair both fit behind this, so CI exercises the entire stack above it on a
 * machine with no NIC, and bring-up can fall back to sockets without changing
 * a line of the layers that matter.
 *
 * A message is a chunk of TLS ciphertext. Its bound is the largest peer frame
 * plus the per-record overhead TLS adds when it splits that frame across
 * records, rounded up: 64 KiB of extent, its header, five records' worth of
 * tags, and slack. */
#define PEER_WIRE_MSG_MAX 73728u

/* Worker-confined synchronous leases. Never access a pointer or call an op
 * after wire close. Each direction permits one outstanding lease. */
enum { PEER_WIRE_TX = 1, PEER_WIRE_RX = 2 };
struct peer_wire_token {
    uint64_t conn_epoch;
    uint64_t lease_generation;
    uint32_t slot;
    uint32_t kind;
};
struct peer_wire_tx_lease {
    uint8_t *data;
    size_t cap;
    struct peer_wire_token token;
};
struct peer_wire_rx_lease {
    const uint8_t *data;
    size_t len;
    struct peer_wire_token token;
};

struct peer_wire_ops {
    /* Start a connection. Returns 0 with *wc set even when the connect has not
     * completed, which is the common case: `established` reports when it has. */
    int  (*connect)(void *wctx, uint32_t ip_be, uint16_t port, void **wc);
    /* Absorb readiness: finish outbound connects, flush what a full send queue
     * held back, and hand out inbound connections that finished arriving.
     * Returns whether anything actually moved, never whether work is pending —
     * a caller that spins on the second one never sleeps. */
    int  (*progress)(void *wctx, void **accepted, int max, int *n_accepted);
    /* All of `len` is taken (1), none of it is (0), or the connection is dead
     * (-1). A message is never partially accepted: the layer above encrypts in
     * sequence, so a half-sent message has no recoverable state. */
    int  (*send_msg)(void *wc, const void *buf, size_t len);
    /* One whole message, 0 when none has arrived yet, negative on fault. */
    long (*recv_msg)(void *wc, void *buf, size_t cap);
    int  (*established)(void *wc);
    int  (*faulted)(void *wc);
    void (*close)(void *wc);
    /* The one descriptor a worker's event loop waits on for every connection
     * this carrier holds, or -1 when it has none. */
    int  (*epfd)(void *wctx);
    void (*ctx_free)(void *wctx);
    /* Optional complete groups: TX three ops, RX two ops. Reserve/acquire:
     * 1 grants ownership, 0 unavailable, -1 fault; outputs cleared on 0/-1.
     * Commit: 1 posts the whole nonempty prefix, -1 terminal, never 0.
     * Valid commit/cancel/release consume and clear the lease, even on fault.
     * Invalid tokens return -1 without touching another owner's slot.
     * Cancel/release: 0 success, -1 fault. SEND CQ returns posted TX memory;
     * RX is reposted only on release, never while borrowed. */
    int (*tx_reserve)(void *wc, struct peer_wire_tx_lease *out);
    int (*tx_commit)(void *wc, struct peer_wire_tx_lease *lease, size_t len);
    int (*tx_cancel)(void *wc, struct peer_wire_tx_lease *lease);
    int (*rx_acquire)(void *wc, struct peer_wire_rx_lease *out);
    int (*rx_release)(void *wc, struct peer_wire_rx_lease *lease);
};

static inline int peer_wire_ops_valid(const struct peer_wire_ops *ops)
{
    if (!ops) return 0;
    int tx = !!ops->tx_reserve + !!ops->tx_commit + !!ops->tx_cancel;
    int rx = !!ops->rx_acquire + !!ops->rx_release;
    return (tx == 0 || tx == 3) && (rx == 0 || rx == 2);
}

/* The TCP carrier: what CI runs and what bring-up falls back to. It binds and
 * listens before returning, so a port that cannot be taken is reported here
 * rather than surfacing later as connections that never arrive. */
int peer_wire_tcp_new(uint32_t bind_ip_be, uint16_t port,
                      const struct peer_wire_ops **ops, void **wctx,
                      char *error, size_t error_len);
/* The port actually bound, which is what the caller asked for unless it asked
 * for 0. */
uint16_t peer_wire_tcp_port(void *wctx);

/* The RDMA carrier: what the mesh runs on between nodes. The bind address must
 * be the local address of an rdma device, because the protection domain and
 * completion queue every connection shares are taken from the device that
 * address resolves to. Like the TCP carrier it binds and listens before
 * returning, so a fabric that cannot carry this is reported here. */
int peer_wire_rdma_new(uint32_t bind_ip_be, uint16_t port,
                       const struct peer_wire_ops **ops, void **wctx,
                       char *error, size_t error_len);
uint16_t peer_wire_rdma_port(void *wctx);

#endif /* DMESH_PEER_WIRE_H */
