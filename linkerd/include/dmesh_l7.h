#ifndef DMESH_L7_H
#define DMESH_L7_H

/* DPUmesh and embedded L7 adapter ABI. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMESH_L7_MODE_OPAQUE   1
#define DMESH_L7_MODE_FULL     2

#define DMESH_L7_DECLINE_ERROR          (-1)
#define DMESH_L7_DECLINE_NOT_ATTACHED   (-2)
#define DMESH_L7_DECLINE_MODE           (-3)
#define DMESH_L7_DECLINE_SESSION_LIMIT  (-4)
#define DMESH_L7_DECLINE_UNKNOWN_REPLY  (-5)

/* Addresses are in host byte order. `workload` and `source_identity` are
 * NUL-terminated. */
struct dmesh_l7_flow {
    /* The source Pod's real cluster IP, from its signed assertion within a
     * node or from the generation across one. It is what an authorization
     * policy's `networks` clause is matched against, so nothing synthetic may
     * appear here: a clause written against the cluster CIDR has to evaluate
     * against an address the cluster actually assigned. */
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    int32_t  src_pod;
    int32_t  dst_service;
    int32_t  peer_pod;
    uint8_t  mode;
    uint8_t  is_reply;
    /* Max namespace (63) + max Pod DNS subdomain (253) plus JSON framing. */
    char     workload[384];
    /* The verified source identity, as Linkerd names one:
     * `<service-account>.<namespace>.serviceaccount.identity.<trust-domain>`.
     * Built from the retained registration within a node and from the
     * generation across one; empty when the source is not attested. No Pod
     * supplies any part of it. */
    char     source_identity[254];
};

static inline uint64_t dmesh_l7_conn_handle(int32_t pod, uint16_t port)
{
    return ((uint64_t)(uint8_t)pod << 16) | (uint64_t)port;
}

/* The low 24 bits remain the routable (pod,port) key; the high 32 bits are a
 * worker-local incarnation. C uses the generation-bearing form in production
 * so a callback queued by a closed Linkerd task cannot land on a newly
 * allocated connection that reused the same port. */
static inline uint64_t dmesh_l7_conn_handle_generation(int32_t pod,
                                                       uint16_t port,
                                                       uint32_t generation)
{
    return ((uint64_t)generation << 24) | dmesh_l7_conn_handle(pod, port);
}

static inline int32_t dmesh_l7_handle_pod(uint64_t conn)
{
    return (int32_t)(int8_t)(conn >> 16);
}

static inline uint16_t dmesh_l7_handle_port(uint64_t conn)
{
    return (uint16_t)conn;
}

static inline uint32_t dmesh_l7_handle_generation(uint64_t conn)
{
    return (uint32_t)(conn >> 24);
}

#define DMESH_L7_BACKEND_ANY (-1)
#define DMESH_L7_ORIGIN      (-2)

/* L7 adapter entry points. */
int l7_worker_run(int worker_id, void *driver);

int  l7_conn_open(int worker_id, uint64_t conn,
                  const struct dmesh_l7_flow *flow);
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);
/* EOF closes one input half; close drops its complete paired session. */
void l7_conn_eof(int worker_id, uint64_t conn);
void l7_conn_close(int worker_id, uint64_t conn);

/* The destination-side admission verdict for one inbound stream, asked before
 * the stream is admitted to a registered Pod. `flow.workload` names the
 * destination Pod, which is the policy subject; `flow.dst_ip:dst_port` is the
 * port the policy is watched on; `flow.src_ip:src_port` and
 * `flow.source_identity` are the verified client, and no part of either comes
 * from a Pod. One watch is held per destination Pod and port and shared by
 * every stream arriving at it, so a stream costs an evaluation rather than a
 * session build. 1 admits, 0 refuses, negative means no verdict is available
 * and the destination Service's protection class decides. */
#define DMESH_L7_VERDICT_ADMIT         1
#define DMESH_L7_VERDICT_REFUSE        0
#define DMESH_L7_VERDICT_NO_POLICY    (-1)
#define DMESH_L7_VERDICT_NOT_ATTACHED (-2)
int  l7_inbound_verdict(int worker_id, const struct dmesh_l7_flow *flow);
/* Drop the policy watches held for a destination Pod whose registration ended.
 * The watch lifetime is the cached store's lifetime, so this is what ends it.
 * Registration ends on the control thread and each worker's stores are its
 * own, so this reaches only the caller's; the workers prune their own against
 * `dmesh_l7_workloads`. */
void l7_inbound_forget(int worker_id, const char *workload);
/* Control-plane admission accounting. `kind` is the decision surface
 * (`assert`, `registration`, `registration-timeout`, `membership`,
 * `revocation`, `admission`, `topology`, `peer`, `inbound`) and `reason` a
 * stable lowercase slug, `ok` for the accepting outcome. Registration
 * decisions are
 * taken on the Comch control thread and peer refusals on a data worker, so the
 * counters are process-global and this is called from several threads. */
void l7_control_event(const char *kind, const char *reason);

/* DPUmesh data-path entry points. */
uint8_t *dmesh_l7_tx_reserve(int worker_id, uint64_t conn, uint32_t *cap);
int dmesh_l7_tx_commit(int worker_id, uint64_t conn, int32_t backend_pod,
                       uint32_t len);
int dmesh_l7_tx_commit_remote(int worker_id, uint64_t conn,
                              const char *pod_uid, uint32_t len);
/* Publish one ordered output FIN after Linkerd drained that direction. Origin
 * ignores pod_uid. A remote backend supplies its exact Pod UID; local/Any use
 * backend_pod and pass NULL. Returns 1 accepted, 0 backpressured, -1 terminal. */
int dmesh_l7_tx_fin(int worker_id, uint64_t conn, int32_t backend_pod,
                    const char *pod_uid);
/* Linkerd ended the paired session without an orderly two-FIN completion.
 * The datapath aborts the remaining transport halves on its next worker pass. */
void dmesh_l7_session_failed(int worker_id, uint64_t conn);
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);
/* Length of the signed prefix of an authoritative feed document, or -1 when it
 * is unsigned or its signature does not verify against the feed keyring. Only
 * that prefix may be parsed. */
long dmesh_l7_verify_feed(const uint8_t *document, size_t length);
/* The node-local compact id the held topology generation interns for the
 * `namespace/name` Service key, or -1 while no generation defines it. The id
 * is a transport identifier, never workload identity. */
int32_t dmesh_l7_svc_for_name(int worker_id, const char *key);

/* The live destination Pod behind one Kubernetes Pod UID.
 *
 * DPUmesh chooses the backend Pod itself on the data path, so an endpoint the
 * Linkerd balancer selected is resolved through this rather than dialled. Each
 * negative outcome is a distinct decline — never a round robin and never a TCP
 * fallback, both of which would carry a protected stream somewhere its policy
 * never named. */
#define DMESH_L7_ENDPOINT_UNRESOLVED (-1)  /* no live registration serves it */
#define DMESH_L7_ENDPOINT_REMOTE     (-2)  /* the generation places it on another node */
#define DMESH_L7_ENDPOINT_STALE      (-3)  /* the mapping predates the held generation */
int32_t dmesh_l7_pod_for_uid(int worker_id, const char *pod_uid);

/* One destination this DPU serves, as an inbound policy subject. */
struct dmesh_l7_workload {
    char     workload[384];   /* same bound as dmesh_l7_flow.workload */
    uint32_t ip;              /* the Pod's cluster address, host byte order */
    uint16_t port;            /* the port its Service publishes */
};

/* Every registered, data-ready Pod inside the controller's scope whose Service
 * publishes a port, written into `out` up to `max`; returns how many were
 * written, or -1 when the caller does not own this worker.
 *
 * A worker reconciles the watches it holds against this list on its maintenance
 * pass: starting one when a Pod registers, so the answer is in place before its
 * first caller, and dropping one whose Pod is gone, which bounds the held set. */
int dmesh_l7_workloads(int worker_id, struct dmesh_l7_workload *out, int max);

/* Persistent runtime backend implemented by DPUmesh. */
int dmesh_l7_driver_notification_fds(void *driver, int *completion_fd,
                                     int *dma_fd, int *wake_fd);
int dmesh_l7_driver_arm(void *driver);
int dmesh_l7_driver_drain(void *driver, int budget);
int dmesh_l7_driver_clear_notifications(void *driver);
int dmesh_l7_driver_maintenance(void *driver);
int dmesh_l7_driver_stopped(void *driver);
void dmesh_l7_driver_ready(void *driver);
void dmesh_l7_driver_failed(void *driver);

#ifdef __cplusplus
}
#endif

#endif /* DMESH_L7_H */
