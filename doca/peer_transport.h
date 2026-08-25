#ifndef DMESH_PEER_TRANSPORT_H
#define DMESH_PEER_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "peer_channel.h"
#include "peer_wire.h"

/* The seam's implementation: it puts the authenticated session (peer_tls) on
 * top of a byte carrier (peer_wire) and hands the pair up as the five
 * callbacks `dmesh_peer_channel` already calls.
 *
 * The split is what makes each half testable. The carrier knows nothing about
 * keys; the session knows nothing about sockets or queue pairs; and this file
 * is the only place that knows both — which is why it is also the only place
 * that knows the prologue, the one thing that has to cross before either half
 * can say who it is talking to.
 *
 * Nothing here touches the DPU's own state. It takes a peer table and a
 * carrier and drives them, so the whole stack runs in one process in CI. */

struct peer_transport_rt;

struct peer_transport_config {
    const char *node_name;              /* this node, as the generation names it */
    const uint8_t *seed;                /* 32 bytes: the node's private half */
    const struct peer_wire_ops *wire;
    void *wire_ctx;                     /* the runtime takes ownership */
    /* How long an inbound connection may stay unauthenticated before it is
     * dropped. Zero takes the default. */
    uint64_t handshake_timeout_ns;
    /* The clock, so a test can hold it still. NULL takes CLOCK_MONOTONIC. */
    uint64_t (*now_ns)(void *ctx);
    void *now_ctx;
};

#define DMESH_PEER_HANDSHAKE_TIMEOUT_NS (5ull * 1000000000ull)

int  dmesh_peer_transport_new(const struct peer_transport_config *config,
                              struct peer_transport_rt **out,
                              char *error, size_t error_len);
void dmesh_peer_transport_free(struct peer_transport_rt *rt);

/* The vtable to hand `dmesh_peer_table_init`, with the runtime as its context. */
const struct dmesh_peer_transport *dmesh_peer_transport_ops(void);
/* Give the runtime the table it accepts inbound connections into. Until this
 * is called an arriving connection has nowhere to land and is dropped. */
void dmesh_peer_transport_attach(struct peer_transport_rt *rt,
                                 struct dmesh_peer_table *table);

/* Carry every connection that is not yet a channel forward one step: finish
 * connects, run handshakes, read prologues, and adopt what authenticated.
 * Returns whether anything actually moved. */
int dmesh_peer_transport_progress(struct peer_transport_rt *rt);
/* Whether a connection is mid-handshake or holding ciphertext a full send
 * queue refused. This is what keeps a worker from sleeping through its own
 * unfinished business; it is not a reason to spin. */
int dmesh_peer_transport_pending(struct peer_transport_rt *rt);
/* The descriptor a worker adds to its own event loop. */
int dmesh_peer_transport_epfd(struct peer_transport_rt *rt);

/* This node's public half, as derived from the seed. The value a peer pins. */
void dmesh_peer_transport_public_key(const struct peer_transport_rt *rt,
                                     uint8_t key[32]);

struct peer_transport_stats {
    uint64_t accepted;      /* inbound connections adopted into a channel */
    uint64_t refused;       /* inbound connections a channel would not take */
    uint64_t faults;        /* connections lost before or after authenticating */
    uint32_t live;
};
void dmesh_peer_transport_stats(const struct peer_transport_rt *rt,
                                struct peer_transport_stats *out);

#endif /* DMESH_PEER_TRANSPORT_H */
