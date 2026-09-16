#ifndef DMESH_PEER_CRYPTO_H
#define DMESH_PEER_CRYPTO_H
#include "peer_association.h"

/* The hardware side of one node crypto manager: SA, flow and fence operations
 * the association registry emits as `peer_crypto_request`. The manager never
 * performs verbs or DOCA Flow work itself; an adapter owns the device and
 * reports each request's final outcome exactly once.
 *
 * Only completion status 0 may mean every hardware entry is in place. Status
 * -1 is a final, known failure. Status -2 is an unknown outcome and quarantines
 * the pair's resources; an adapter that loses track of a request must report
 * -2 rather than guess. A completion whose identity does not match the exact
 * request issued is stale and ignored. */
struct peer_crypto_completion {
    struct peer_assoc_token token;
    uint64_t operation;
    enum peer_crypto_action action;
    uint8_t session[16], association[16];
    uint64_t epoch;
    int status;
};
struct peer_crypto_adapter {
    void *ctx;
    /* 1 request copied and owned by the adapter (the caller cleanses its copy),
     * 0 bounded backpressure (retry later, nothing was taken), -1 refused. */
    int (*submit)(void *ctx, const struct peer_crypto_request *);
    /* 1 completion copied, 0 none pending. */
    int (*poll)(void *ctx, struct peer_crypto_completion *);
    /* A descriptor readable when poll may return something, or -1. */
    int (*fd)(void *ctx);
    /* Optional. 0 when the hardware owner is unreachable: every association it
     * protected has lost its enforcement and the manager retires them all. */
    int (*healthy)(void *ctx);
};
static inline int peer_crypto_adapter_valid(const struct peer_crypto_adapter *a)
{ return a && a->submit && a->poll && a->fd; }
#endif
