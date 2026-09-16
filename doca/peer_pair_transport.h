#ifndef DMESH_PEER_PAIR_TRANSPORT_H
#define DMESH_PEER_PAIR_TRANSPORT_H
#include "peer_association.h"
#include "peer_channel.h"
#include "peer_wire_verbs.h"

/* One data-worker owner and one node-control owner. Neither owner calls into
 * the other's state: the two bounded SPSC mailboxes copy secret-free messages.
 * These are process-local messages, never wire structs or authorization from a Pod.
 */
#define PEER_PAIR_MAILBOX_CAP 64u
struct peer_pair_ref { uint64_t runtime, generation; uint32_t slot; };
struct peer_pair_offer {
    char node[DMESH_K8S_NAME_MAX];
    uint8_t key[32];
    uint32_t ip_be, incarnation;
    uint16_t port;
    struct dmesh_peer_pair pair;
    struct dmesh_peer_stream_open intent;
};
enum peer_pair_command_type {
    PEER_PAIR_AUTHORIZE = 1, /* inbound snapshot + policy query, allocates no QP/slot */
    PEER_PAIR_PREPARE,     /* recheck first-port authorization; create INIT QP/RQ */
    PEER_PAIR_ENABLE,      /* both RX/TX HW installed; allow RTR/RTS, no app data */
    PEER_PAIR_GRANT,       /* association ACTIVE + peer barrier; publish readiness */
    PEER_PAIR_QUIESCE,     /* pause new OPEN/DATA, wait SEND + channel + DMA custody */
    PEER_PAIR_RESUME,      /* rekey epoch staged; keep admission paused until GRANT */
    PEER_PAIR_BLOCK,       /* stop data, reset channel; node owner separately fences HW */
    PEER_PAIR_DESTROY,     /* ONLY after HW block completion; QP close + DMA fence */
    PEER_PAIR_FORGET       /* manager retired flows/SAs and acknowledges slot release */
};
struct peer_pair_command {
    enum peer_pair_command_type type;
    uint64_t operation;       /* strictly increasing per connection; no wrap */
    struct peer_pair_ref ref; /* zero ref on a PREPARE that allocates the lane */
    struct peer_assoc_token association;
    unsigned local_endpoint;
    int hardware_ready;      /* ENABLE: RX/TX ready; DESTROY: BLOCK complete */
    /* Zero-ref PREPARE: 0 accepts an inbound lane (the offer's intent names
     * this node as destination), 1 prepares an outbound lane for a worker whose
     * own stream has not opened yet; connect_pair adopts it when one does. */
    int outbound;
    struct peer_pair_offer offer; /* AUTHORIZE/PREPARE: authenticated remote identity */
    struct peer_sec_lane lane;    /* ENABLE only */
    struct peer_assoc_grant grant; /* GRANT only */
    uint64_t deadline[3];     /* PREPARE/ENABLE: independently verified leases */
    uint64_t epoch;           /* ENABLE/RESUME */
};
enum peer_pair_event_type { PEER_PAIR_OPEN = 1, PEER_PAIR_COMPLETED, PEER_PAIR_REVOKE };
struct peer_pair_event {
    enum peer_pair_event_type type;
    struct peer_pair_ref ref;
    uint64_t operation;
    enum peer_pair_command_type command;
    struct peer_assoc_token association;
    int status; /* 0 completed, 1 stale, -1 rejected/final failure, -2 quarantine */
    struct peer_pair_offer offer;
    struct peer_verbs_endpoint endpoint;
};

/* The injectable driver is for deterministic failure tests. The production
 * factory below selects manual verbs, never TCP or TLS for application data.
 * close must retain its handle/MR on failure; ctx_free follows successful close.
 */
struct peer_pair_driver {
    const struct peer_wire_ops *wire;
    void *ctx;
    int (*prepare)(void *, void **, struct peer_verbs_endpoint *);
    int (*activate)(void *, const struct peer_verbs_endpoint *, int);
    int (*send_drained)(void *);
    int (*close)(void *);
};
struct peer_pair_transport_config {
    unsigned worker, connections; /* bounded; 1..256 */
    uint64_t setup_timeout_ns;
    uint64_t (*now_ns)(void *);
    void *now_ctx;
    /* Mandatory, worker-local. Checks retained proxy DMA/arrival references
     * even after reset cleared the channel's application accounting. */
    int (*dma_fenced)(void *, struct dmesh_peer_channel *, uint32_t incarnation);
    void *dma_ctx;
};
struct peer_pair_transport;
int peer_pair_transport_new(const struct peer_pair_transport_config *,
    struct ibv_context *, uint8_t port, uint32_t gid_index, uint16_t mtu,
    struct peer_pair_transport **, char *error, size_t error_len);
/* Driver ownership transfers only on success. Constructor/destructor run while
 * both owners are stopped; data-path functions run on the owning worker. */
int peer_pair_transport_new_driver(const struct peer_pair_transport_config *,
    const struct peer_pair_driver *, struct peer_pair_transport **);
int peer_pair_transport_free(struct peer_pair_transport *); /* -1 if not fully retired */
void peer_pair_transport_attach(struct peer_pair_transport *, struct dmesh_peer_table *);
const struct dmesh_peer_transport *peer_pair_transport_ops(void);
int peer_pair_transport_epfd(struct peer_pair_transport *);
/* Only node-control owner uses these; 1 copied, 0 bounded backpressure, -1 error. */
int peer_pair_manager_submit(struct peer_pair_transport *, const struct peer_pair_command *);
int peer_pair_manager_poll(struct peer_pair_transport *, struct peer_pair_event *);
int peer_pair_manager_fd(struct peer_pair_transport *);
#endif
