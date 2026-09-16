#ifndef DMESH_PEER_ASSOCIATION_H
#define DMESH_PEER_ASSOCIATION_H
#include "peer_security_wire.h"
#include "peer_identity.h"

#define PEER_ASSOC_LANES_MAX 16u
/* The control owner never performs verbs/flow operations on a worker's behalf.
 * It emits commands, and accepts completions only for the exact token issued.
 * Adapters must copy requests before returning to their asynchronous queues. */
enum peer_crypto_action {
    PEER_CRYPTO_INSTALL_RX = 1, /* SA + auth/replay drop + allowed local QPNs */
    PEER_CRYPTO_INSTALL_TX,     /* encrypted TX + miss drop, never plain forwarding */
    PEER_CRYPTO_ENABLE_QPS,     /* owner workers: INIT -> RTR/RTS, no app SEND */
    PEER_CRYPTO_QUIESCE,        /* stop new DATA/OPEN; drain CQ, DMA, STREAM_ACK */
    PEER_CRYPTO_RESUME,         /* apply epoch to all workers; admission still gated */
    PEER_CRYPTO_REMOVE_OLD,     /* detach and delete retiring SA/rules, completion required */
    PEER_CRYPTO_BLOCK,          /* HW TX/RX fence, including default/miss paths */
    PEER_CRYPTO_DESTROY_QPS,    /* workers close QPs AND fence DMA before completion */
    PEER_CRYPTO_REMOVE_ALL      /* flow removal complete before deleting all owned SAs */
};
enum peer_assoc_phase {
    PEER_ASSOC_RX, PEER_ASSOC_WAIT_RX, PEER_ASSOC_TX, PEER_ASSOC_WAIT_TX,
    PEER_ASSOC_QP, PEER_ASSOC_WAIT_QP, PEER_ASSOC_DRAIN, PEER_ASSOC_WAIT_DRAIN,
    PEER_ASSOC_RESUME, PEER_ASSOC_ACTIVE, PEER_ASSOC_BLOCK,
    PEER_ASSOC_DESTROY, PEER_ASSOC_REMOVE, PEER_ASSOC_QUARANTINED, PEER_ASSOC_DEAD
};
struct peer_assoc_token { uint64_t manager, generation; uint32_t slot; };
struct peer_crypto_request {
    struct peer_assoc_token token;
    uint64_t operation;
    enum peer_crypto_action action;
    struct peer_sec_binding binding;
    struct peer_sec_lane lanes[PEER_ASSOC_LANES_MAX];
    unsigned lane_count, local_endpoint;
    uint64_t old_epoch;
    uint8_t material[20]; /* only INSTALL_RX/TX; MUST cleanse after submission */
};
struct peer_assoc_event {
    struct peer_assoc_token token;
    uint16_t type;
    uint8_t association[16], digest[32];
    uint64_t epoch;
};
struct peer_assoc_limits {
    unsigned pairs, lanes, sas; /* pending and quarantined resources count */
    uint64_t setup_timeout_ns, overlap_ns;
};
struct peer_assoc_stats { unsigned pairs, lanes, sas, quarantined; };
/* Secret-free copy handed from the control owner to the owning worker.
 * A worker must check token/revision, QP identity and these independent lease
 * deadlines before exposing pair_ready. This is not permission to invoke the
 * manager from a data worker or bypass the hardware completion callbacks. */
struct peer_assoc_grant {
    struct peer_assoc_token token;
    uint64_t revision, epoch;
    struct dmesh_peer_pair pair;
    struct peer_sec_lane lane;
    uint64_t deadline[3], lease_generation[3];
};
struct peer_associations;
int peer_associations_new(const struct peer_assoc_limits *, struct peer_associations **);
/* Refuses to free while QPs/flows/SA operations can still reference slots. */
int peer_associations_free(struct peer_associations *);
/* Binding/lane parameters were authorized over the pinned control session.
 * Deadlines are independent authority/policy/control leases, in monotonic ns.
 * TLS remains owned by the caller until this association is DEAD. */
int peer_association_open(struct peer_associations *, struct peer_tls_conn *,
                          const struct peer_sec_binding *, unsigned local_endpoint,
                          const struct peer_sec_lane *, unsigned lane_count,
                          const uint64_t deadlines[3], uint64_t now_ns,
                          struct peer_assoc_token *);
/* Resolve authenticated wire IDs to an owner token; never route by slot alone.
 * 1 found, 0 unknown/retired, -1 invalid arguments. */
int peer_associations_lookup(struct peer_associations *, const uint8_t session[16],
                             const uint8_t association[16], struct peer_assoc_token *);
/* Issue at most one outstanding operation per pair. 1 request, 0 none, -1 bad
 * arguments. Tick expires leases, and keeps resources until completion/fences. */
void peer_associations_tick(struct peer_associations *, uint64_t now_ns);
int peer_associations_next(struct peer_associations *, uint64_t now_ns,
                           struct peer_crypto_request *);
/* status: 0 complete, -1 failed with known final outcome, -2 unknown outcome.
 * Unknown outcome quarantines resources; never guess completion from timeout.
 * 1 stale/duplicate token, 0 accepted, -1 invalid. */
int peer_association_complete(struct peer_associations *, const struct peer_crypto_request *,
                              int status, uint64_t now_ns);
/* Notification remains owned until acked after bounded control queue admission. */
int peer_associations_event(struct peer_associations *, struct peer_assoc_event *);
int peer_association_event_sent(struct peer_associations *, const struct peer_assoc_event *);
int peer_association_signal(struct peer_associations *, struct peer_assoc_token,
                            uint16_t type, uint64_t epoch, const uint8_t digest[32], uint64_t now_ns);
int peer_association_rekey(struct peer_associations *, struct peer_assoc_token,
                           const uint32_t spi[2], uint64_t now_ns);
int peer_association_revoke(struct peer_associations *, struct peer_assoc_token);
int peer_association_release(struct peer_associations *, struct peer_assoc_token);
int peer_association_admit(struct peer_associations *, struct peer_assoc_token, uint64_t now_ns);
/* 1 snapshot, 0 not active/unknown worker, -1 invalid; zero output on failure.
 * Only the control owner calls this and copies the result into its worker queue. */
int peer_association_grant(struct peer_associations *, struct peer_assoc_token,
                           unsigned worker, uint64_t now_ns, struct peer_assoc_grant *);
/* Lease renewals require newer per-source generations; cannot revive expiry. */
int peer_association_renew(struct peer_associations *, struct peer_assoc_token, unsigned source,
                           uint64_t generation, uint64_t deadline, uint64_t now_ns);
enum peer_assoc_phase peer_association_phase(struct peer_associations *, struct peer_assoc_token);
void peer_associations_stats(const struct peer_associations *, struct peer_assoc_stats *);
void peer_crypto_request_cleanse(struct peer_crypto_request *);
#endif
