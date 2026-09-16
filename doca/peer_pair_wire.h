#ifndef DMESH_PEER_PAIR_WIRE_H
#define DMESH_PEER_PAIR_WIRE_H
#include "peer_channel.h"

/* Worker-owned per-stream generations. No TLS or HW objects, and no keys.
 * One complete v1 host frame is translated into/from the canonical v2 wire.
 * Encode exactly once per accepted frame, retaining bytes on would-block.
 * A codec error is terminal: the channel must close, not retry the mutation. */
struct peer_pair_wire;
struct peer_pair_wire *peer_pair_wire_new(const struct dmesh_peer_pair *);
void peer_pair_wire_free(struct peer_pair_wire *);
long peer_pair_wire_encode(struct peer_pair_wire *, const void *host_frame,
                           size_t host_len, void *wire_frame, size_t capacity);
/* Returns complete wire length, 0 incomplete, -1 malformed/stale. The output
 * header/payload use the existing channel's host representation and custody. */
long peer_pair_wire_decode(struct peer_pair_wire *, const void *wire_frame,
                           size_t wire_len, uint32_t incarnation,
                           struct dmesh_peer_msg_header *, void *payload, size_t capacity);
#endif
