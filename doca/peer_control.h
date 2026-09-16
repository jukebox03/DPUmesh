#ifndef DMESH_PEER_CONTROL_H
#define DMESH_PEER_CONTROL_H
#include "peer_security_wire.h"
#include "peer_wire.h"

struct peer_control;
struct peer_control_config {
    struct peer_sec_hello local; /* nonce is replaced with fresh RNG bytes per connection */
    char remote_node[PEER_SEC_NAME_MAX];
    uint8_t remote_key[32]; /* from a held, verified authority snapshot */
    uint64_t authority_deadline_ns;
    uint64_t setup_timeout_ns; /* zero -> 5 seconds */
    uint64_t control_lease_ns; /* zero -> 5 seconds */
    /* 0 accepted, 1 retry without side effects, -1 protocol/authorization fault.
     * Body is borrowed only during this call. Always on the control owner. */
    int (*message)(void *, const struct peer_sec_header *, const uint8_t *);
    void *message_ctx;
    /* Inbound connections do not know their peer until TLS authenticates its
     * key. With remote_node empty, this is consulted once after the handshake:
     * 1 names the node the held authority snapshot binds to `key` (that name
     * and key then become the pin), 0 refuses the connection. Never consulted
     * when remote_node is set. */
    int (*resolve)(void *, const uint8_t key[32], char node[PEER_SEC_NAME_MAX]);
    void *resolve_ctx;
};
/* One owner thread; TLS context is borrowed, connection is owned on success.
 * Caller progresses the carrier context separately (it can serve many peers).
 * No data frames are sent through this connection. */
int peer_control_new(const struct peer_control_config *, struct peer_tls_ctx *,
                     int initiator, const struct peer_wire_ops *, void *wire_conn,
                     uint64_t now_ns, struct peer_control **out);
/* Associations borrow this connection's TLS object. Fence/release those before
 * freeing the control, even after it faults. A fault does not free the object. */
void peer_control_free(struct peer_control *);
int peer_control_progress(struct peer_control *, uint64_t now_ns);
int peer_control_ready(const struct peer_control *, uint64_t now_ns);
int peer_control_faulted(const struct peer_control *);
const uint8_t *peer_control_session(const struct peer_control *);
const struct peer_sec_hello *peer_control_remote(const struct peer_control *);
struct peer_tls_conn *peer_control_tls(struct peer_control *, uint64_t now_ns);
int peer_control_binding_valid(const struct peer_control *, const struct peer_sec_binding *,
                               unsigned local_endpoint, uint64_t now_ns);
/* 1 queued, 0 bounded backpressure, -1 fault/invalid. The module assigns wire
 * session/operation ID. Queue admission copies the entire body, never keys. */
int peer_control_send(struct peer_control *, uint16_t type, const uint8_t association[16],
                      uint64_t epoch, const void *body, size_t len, uint64_t now_ns);
/* Only a newer verified authority snapshot may call this. Stale refreshes do
 * not extend the lease. Call progress first; expiration cannot be revived. */
int peer_control_authority(struct peer_control *, uint64_t generation,
                           uint64_t deadline_ns, uint64_t now_ns);
/* The control lease as last renewed by a challenge answer; 0 before ready. */
uint64_t peer_control_lease_deadline(const struct peer_control *);
#endif
