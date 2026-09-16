#ifndef DMESH_PEER_SECURITY_WIRE_H
#define DMESH_PEER_SECURITY_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include "peer_tls.h"
#include "workload_limits.h"
#include "peer_identity.h"

#define PEER_SEC_VERSION 2u
#define PEER_SEC_HEADER_LEN 64u
#define PEER_SEC_BODY_MAX 16384u
#define PEER_SEC_FRAME_MAX (PEER_SEC_HEADER_LEN + PEER_SEC_BODY_MAX)
#define PEER_SEC_NAME_MAX 254u
#define PEER_SEC_UID_MAX 128u
#define PEER_SEC_CLUSTER_MAX 128u
#define PEER_SEC_AES128_GCM 1u
#define PEER_SEC_LANES_MAX 16u
#define PEER_SEC_SERVICE_MAX (DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX)

enum peer_sec_type {
    PEER_SEC_HELLO = 1, PEER_SEC_PROPOSE, PEER_SEC_ACCEPT,
    PEER_SEC_LANE_PREPARE, PEER_SEC_LANE_PARAMS,
    PEER_SEC_RX_READY, PEER_SEC_TX_READY, PEER_SEC_LANE_READY,
    PEER_SEC_ACTIVATE, PEER_SEC_ACTIVE_ACK, PEER_SEC_REKEY,
    PEER_SEC_QUIESCE, PEER_SEC_DRAINED, PEER_SEC_COMMIT,
    PEER_SEC_RETIRE, PEER_SEC_REVOKE, PEER_SEC_ERROR,
    PEER_SEC_PING, PEER_SEC_PONG
};

/* Host representations. None of these structures is copied to the wire. */
struct peer_sec_header {
    uint16_t type;
    uint32_t body_len;
    uint8_t session[16];
    uint64_t operation;
    uint8_t association[16];
    uint64_t epoch;
};
struct peer_sec_hello {
    char cluster[PEER_SEC_CLUSTER_MAX];
    char node[PEER_SEC_NAME_MAX];
    uint8_t public_key[32];
    uint8_t boot[16];
    uint8_t nonce[16];
    uint16_t workers;
};
struct peer_sec_endpoint {
    char node[PEER_SEC_NAME_MAX];
    uint8_t public_key[32];
    uint8_t boot[16];
    char pod[PEER_SEC_UID_MAX];
    uint8_t daemon[16];
    uint32_t channel_slot;
    uint64_t channel_generation;
    uint8_t registration_nonce[DMESH_REG_NONCE_SIZE];
};
struct peer_sec_binding {
    char cluster[PEER_SEC_CLUSTER_MAX];
    struct peer_sec_endpoint endpoint[2]; /* canonical node order */
    uint8_t session[16];
    uint8_t association[16];
    uint64_t epoch;
    uint32_t spi[2]; /* direction 0->1, then 1->0; reserved by receivers */
};
struct peer_sec_lane {
    uint64_t id;
    uint64_t generation;
    uint16_t worker[2];
    uint32_t qpn[2];
    uint32_t psn[2];
    uint8_t gid[2][16];
    uint16_t mtu; /* bytes: 256/512/1024/2048/4096 */
};
#define PEER_SEC_LANE_LEN 66u

/* Pod-pair negotiation bodies. PROPOSE is the proposer's view before any QP
 * exists. ACCEPT answers with the acceptor's verified registration, its receive
 * SPI and its half of every lane; LANE_PARAMS returns the proposer's halves.
 * REKEY carries the initiator's new receive SPI, COMMIT both. No body carries
 * key material. Lane ids are strictly ascending within one association. */
struct peer_sec_lane_map { uint64_t id, generation; uint16_t worker[2]; };
struct peer_sec_propose {
    char cluster[PEER_SEC_CLUSTER_MAX];
    char source_pod[DMESH_POD_UID_MAX], target_pod[DMESH_POD_UID_MAX];
    struct dmesh_peer_registration source;
    uint8_t association[16];
    uint32_t rx_spi, incarnation;
    char service[PEER_SEC_SERVICE_MAX];
    uint16_t port, mtu, lane_count;
    struct peer_sec_lane_map lane[PEER_SEC_LANES_MAX];
};
struct peer_sec_half_lane { uint64_t id, generation; uint32_t qpn, psn; uint8_t gid[16]; };
struct peer_sec_lane_params {
    uint8_t association[16];
    uint32_t rx_spi;                              /* ACCEPT only */
    struct dmesh_peer_registration registration;  /* ACCEPT only */
    uint16_t mtu, lane_count;
    struct peer_sec_half_lane lane[PEER_SEC_LANES_MAX];
};
enum peer_sec_error {
    PEER_SEC_ERR_REFUSED = 1, PEER_SEC_ERR_BUSY, PEER_SEC_ERR_CONFLICT,
    PEER_SEC_ERR_MALFORMED, PEER_SEC_ERR_REVOKED, PEER_SEC_ERR_MISMATCH
};
int peer_sec_propose_encode(const struct peer_sec_propose *, void *, size_t, size_t *);
int peer_sec_propose_decode(const void *, size_t, struct peer_sec_propose *);
/* accept selects the ACCEPT layout (registration + rx_spi present). */
int peer_sec_lane_params_encode(const struct peer_sec_lane_params *, int accept,
                                void *, size_t, size_t *);
int peer_sec_lane_params_decode(const void *, size_t, int accept, struct peer_sec_lane_params *);
#define PEER_SEC_REKEY_LEN 10u
int peer_sec_rekey_encode(const uint32_t spi[2], uint8_t out[PEER_SEC_REKEY_LEN]);
int peer_sec_rekey_decode(const void *, size_t, uint32_t spi[2]);
#define PEER_SEC_ERROR_LEN 4u
int peer_sec_error_encode(uint16_t code, uint8_t out[PEER_SEC_ERROR_LEN]);
int peer_sec_error_decode(const void *, size_t, uint16_t *code);

enum peer_sec_data_type {
    PEER_SEC_STREAM_OPEN = 1, PEER_SEC_STREAM_OPEN_ACK,
    PEER_SEC_DATA, PEER_SEC_STREAM_ACK, PEER_SEC_STREAM_FIN
};
#define PEER_SEC_DATA_HEADER_LEN 64u
#define PEER_SEC_DATA_MAX 65536u
struct peer_sec_data_header {
    uint16_t type;
    uint32_t body_len;
    uint8_t association[16];
    uint64_t lane_id, lane_generation;
    uint32_t handle;
    uint64_t stream_generation, sequence;
};
int peer_sec_data_encode(const struct peer_sec_data_header *, uint8_t out[64]);
/* One RDMA message, exact length required. This validates the envelope, not
 * application policy. Caller checks stream handle/generation against its live
 * stream table before acting, including for ACK/FIN. HW SA->QP enforcement is
 * independently required before packets can reach the RNIC. */
int peer_sec_data_decode(const void *, size_t, const uint8_t association[16],
                         uint64_t lane_id, uint64_t lane_generation,
                         struct peer_sec_data_header *);

int peer_sec_nonzero(const void *bytes, size_t len);
int peer_sec_header_encode(const struct peer_sec_header *, uint8_t out[64]);
/* 0 = valid header, 1 = need 64 bytes, -1 = malformed. Checks lengths before
 * a caller allocates or reads a body. Trailing bytes belong to the body. */
int peer_sec_header_decode(const void *, size_t, struct peer_sec_header *);
int peer_sec_hello_encode(const struct peer_sec_hello *, void *, size_t, size_t *);
int peer_sec_hello_decode(const void *, size_t, struct peer_sec_hello *);
int peer_sec_binding_valid(const struct peer_sec_binding *);
int peer_sec_binding_encode(const struct peer_sec_binding *, void *, size_t, size_t *);
int peer_sec_binding_decode(const void *, size_t, struct peer_sec_binding *);
int peer_sec_lane_encode(const struct peer_sec_lane *, uint8_t out[PEER_SEC_LANE_LEN]);
int peer_sec_lane_decode(const void *, size_t, struct peer_sec_lane *);
/* QPNs are bound to the lane transcript, not to the shared directional SA key.
 * Derivation verifies both endpoint keys against this pinned TLS connection. */
int peer_sec_derive(struct peer_tls_conn *, const struct peer_sec_binding *,
                    unsigned local_endpoint, unsigned direction, uint8_t out[20]);
int peer_sec_session_id(const struct peer_sec_hello *, const struct peer_sec_hello *,
                        uint8_t out[16]);

#endif
