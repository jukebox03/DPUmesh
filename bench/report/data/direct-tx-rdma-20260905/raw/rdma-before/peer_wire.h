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
};

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
