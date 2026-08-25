#ifndef DMESH_PEER_TLS_H
#define DMESH_PEER_TLS_H

#include <stddef.h>
#include <stdint.h>

/* The node-to-node handshake: mutual TLS 1.3, with no wire underneath it.
 *
 * This module moves plaintext one way and ciphertext the other and knows
 * nothing about what carries the ciphertext — a socket, a queue pair, or a
 * test harness passing buffers between two stacks in one process. That is what
 * lets the RDMA and TCP wires share one authenticated, encrypted layer.
 *
 * Authentication is mutual and certificate-based, but a certificate chain is
 * not why a peer is believed: every node self-signs over its own static key,
 * so a chain proves only that the presenter holds the key it presented. The
 * caller pins the key this module reads back against the one the held topology
 * generation binds to the node name the peer claims, and that pin — not the
 * chain — is the decision. A man in the middle completes a handshake with its
 * own key and is refused at the pin.
 *
 * Session resumption is off: every connection performs a fresh key agreement,
 * so a pairwise key is never reused across connections and never outlives one.
 */

struct peer_tls_ctx;
struct peer_tls_conn;

/* One context per worker, holding the node credential and the certificate that
 * carries it. `seed` is the 32-byte private half `dmesh_peer_node_key_load`
 * keeps; `node_name` names the subject and is descriptive only — nothing
 * downstream decides on it, because the peer's claimed name arrives in the
 * prologue and is settled against the key. */
int  peer_tls_ctx_new(const uint8_t seed[32], const char *node_name,
                      struct peer_tls_ctx **out, char *error, size_t error_len);
void peer_tls_ctx_free(struct peer_tls_ctx *ctx);

/* The public half this context presents, for the caller to compare against the
 * one the node published. */
void peer_tls_ctx_public_key(const struct peer_tls_ctx *ctx, uint8_t key[32]);

/* `initiator` picks the side: the end that dialled speaks first. */
int  peer_tls_conn_new(struct peer_tls_ctx *ctx, int initiator,
                       struct peer_tls_conn **out);
void peer_tls_conn_free(struct peer_tls_conn *conn);

/* Advance the handshake as far as the ciphertext on hand allows. 1 when it
 * completed, 0 while it needs more bytes in either direction, -1 on a fault.
 * A fault is permanent: every later call on this connection returns one. */
int  peer_tls_handshake(struct peer_tls_conn *conn);
int  peer_tls_established(const struct peer_tls_conn *conn);
int  peer_tls_faulted(const struct peer_tls_conn *conn);

/* The peer's authenticated raw Ed25519 public key, captured when the handshake
 * completed. Negative before then. */
int  peer_tls_peer_key(const struct peer_tls_conn *conn, uint8_t key[32]);

/* Plaintext in. All-or-nothing by construction: the record layer writes into a
 * memory buffer that grows, so a write that is refused was refused by TLS, not
 * by backpressure. 0 accepted the whole buffer, -1 fault. */
int  peer_tls_write(struct peer_tls_conn *conn, const void *buf, size_t len);
/* Plaintext out. Bytes copied, 0 when none are ready, -1 fault. */
long peer_tls_read(struct peer_tls_conn *conn, void *buf, size_t cap);

/* Ciphertext out, for the wire to carry. Bytes copied, 0 when none, -1 fault. */
long peer_tls_out(struct peer_tls_conn *conn, void *buf, size_t cap);
/* How much ciphertext is waiting, so a caller can tell pending work from idle
 * without attempting a read. */
size_t peer_tls_out_pending(struct peer_tls_conn *conn);
/* Ciphertext in, as the wire delivers it. Order must be preserved; the split
 * into calls need not be. 0 accepted, -1 fault. */
int  peer_tls_in(struct peer_tls_conn *conn, const void *buf, size_t len);

#endif /* DMESH_PEER_TLS_H */
