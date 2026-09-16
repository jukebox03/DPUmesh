#ifndef DMESH_PEER_CRYPTO_IPC_H
#define DMESH_PEER_CRYPTO_IPC_H
#include "peer_crypto.h"

/* The wire between the runtime's crypto adapter and the crypto-owner process
 * that holds the NIC: a UNIX stream socket carrying framed requests one way
 * and completions the other. The owner is the only process that opens the
 * DOCA Flow device; the runtime never links DPDK.
 *
 * Frames are explicit big-endian encodings, never C structures. A request
 * carries the 20 key bytes of an install; both ends cleanse their copies as
 * soon as the bytes have been handed on. The socket path is created by the
 * owner with mode 0600, so the key material only ever crosses a descriptor
 * the runtime's user owns. */
#define PEER_CRYPTO_IPC_VERSION 1u
#define PEER_CRYPTO_IPC_HEADER_LEN 16u
#define PEER_CRYPTO_IPC_BODY_MAX 2048u
#define PEER_CRYPTO_IPC_FRAME_MAX (PEER_CRYPTO_IPC_HEADER_LEN + PEER_CRYPTO_IPC_BODY_MAX)
enum peer_crypto_ipc_type {
    PEER_CRYPTO_IPC_HELLO = 1,   /* owner -> runtime once per connection */
    PEER_CRYPTO_IPC_REQUEST,     /* runtime -> owner */
    PEER_CRYPTO_IPC_COMPLETION   /* owner -> runtime, exactly once per request */
};
/* 0 = complete header, 1 = need more bytes, -1 = malformed. */
int peer_crypto_ipc_header_decode(const void *buf, size_t len, uint16_t *type, uint32_t *body_len);
int peer_crypto_ipc_hello_encode(unsigned sdk_major, unsigned sdk_minor, void *buf, size_t cap, size_t *len);
int peer_crypto_ipc_hello_decode(const void *body, size_t len, unsigned *sdk_major, unsigned *sdk_minor);
/* The request encoding includes the material; callers cleanse `buf` after it
 * has been written to the socket. */
int peer_crypto_ipc_request_encode(const struct peer_crypto_request *, void *buf, size_t cap, size_t *len);
int peer_crypto_ipc_request_decode(const void *body, size_t len, struct peer_crypto_request *);
int peer_crypto_ipc_completion_encode(const struct peer_crypto_completion *, void *buf, size_t cap, size_t *len);
int peer_crypto_ipc_completion_decode(const void *body, size_t len, struct peer_crypto_completion *);

/* The runtime side: a `peer_crypto_adapter` that connects to the owner's
 * socket, reconnects when it drops, and answers the manager honestly when it
 * cannot reach hardware. Requests in flight when the owner disappears are
 * completed as unknown outcome (-2); an install that cannot be sent is refused
 * (-1); `healthy` reports whether an owner is connected right now. */
struct peer_crypto_ipc;
int peer_crypto_ipc_new(const char *socket_path, struct peer_crypto_ipc **out, char *error, size_t error_len);
void peer_crypto_ipc_free(struct peer_crypto_ipc *);
struct peer_crypto_adapter peer_crypto_ipc_adapter(struct peer_crypto_ipc *);
#endif
