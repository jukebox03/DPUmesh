#ifndef DMESH_TOPOLOGY_GEN_H
#define DMESH_TOPOLOGY_GEN_H

#include <stddef.h>
#include <stdint.h>

#include "comch_common.h"

struct objects;

/* Bounds a generation may not exceed; a larger one is refused rather than
 * truncated, because a truncated table would read as withdrawal. The byte
 * bound is the topology's own — the 256 KiB membership bound is one node's. */
#define DMESH_GEN_POD_MAX 65536u
#define DMESH_GEN_NODE_MAX 1024u
#define DMESH_GEN_SERVICE_MAX 4096u
#define DMESH_GEN_ENDPOINT_MAX 65536u
#define DMESH_TOPOLOGY_MAX_BYTES (16u * 1024u * 1024u)
/* Controller public keys held for rotation overlap, enforced at load like the
 * registration keyring's cap. */
#define DMESH_CONTROLLER_KEYS_MAX 4u
/* Node-local compact Service ids the DPU may intern ([0, 127]). */
#define DMESH_TOPOLOGY_INTERN_MAX 128

struct dmesh_gen_node {
    char name[DMESH_K8S_NAME_MAX];
    uint32_t rdma_ip_be;
    uint16_t rdma_port;
    char agent_key_id[DMESH_GRANT_KEY_ID_MAX];
    uint8_t agent_public_key[32];
    uint8_t dpu_static_public_key[32];
};

struct dmesh_gen_service {
    char key[DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX]; /* "namespace/name" */
    uint32_t cluster_ip_be;
    uint16_t port;
    uint8_t is_protected;
    /* Node-local interned id, stable across adoptions while the Service
     * persists; -1 once the 128-id space is exhausted. */
    int16_t interned;
    uint32_t endpoint_first;      /* into tables->endpoints, grouped per service */
    uint32_t endpoint_count;
};

struct dmesh_gen_pod {
    char uid[DMESH_POD_UID_MAX];  /* RFC 4122 text, 36 used */
    char node_name[DMESH_K8S_NAME_MAX];
    char namespace_name[DMESH_K8S_NAMESPACE_MAX];
    char service_account[DMESH_K8S_NAME_MAX];
    uint32_t ip_be;
};

struct dmesh_gen_endpoint {
    uint32_t service;             /* index into tables->services */
    uint32_t pod;                 /* index into tables->pods */
};

/* One adopted generation, heap-allocated whole and swapped on success, exactly
 * as the membership consumer stages. pods are sorted by uid and services by
 * key, so lookups are bsearch. */
struct dmesh_topology_tables {
    uint64_t version;
    struct dmesh_gen_node *nodes;
    size_t node_count;
    struct dmesh_gen_service *services;
    size_t service_count;
    struct dmesh_gen_pod *pods;
    size_t pod_count;
    struct dmesh_gen_endpoint *endpoints;
    size_t endpoint_count;
};

struct dmesh_topology_intern {
    char key[DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX];
    uint8_t in_use;
};

/* Consumer state, owned by the Comch control thread like membership. Worker
 * threads read `tables` through one acquire load (the interning FFI); the
 * displaced generation is therefore parked in `retired` and freed only on the
 * next adoption, so a reader's brief use of the old pointer stays valid across
 * the swap (adoptions are at least a poll interval apart). */
struct dmesh_topology {
    char path[4096];
    char key_dir[4096];           /* DPUMESH_CONTROLLER_KEY_DIR: public keys only */
    int enabled;
    struct dmesh_topology_tables *tables;   /* NULL until the first adoption */
    struct dmesh_topology_tables *retired;  /* previous generation, grace-held */
    struct dmesh_topology_intern interned[DMESH_TOPOLOGY_INTERN_MAX];
    uint64_t stamp_ino;
    int64_t stamp_sec;
    int64_t stamp_nsec;
    uint64_t stamp_size;
    uint64_t rejected;
    uint64_t next_check_ns;
};

enum dmesh_topology_result {
    DMESH_TOPOLOGY_UNCHANGED = 0,
    /* Read, verified and parsed, and the version equals the held one — the
     * countable "unchanged" adoption outcome, distinct from a file the stamp
     * says was never touched. */
    DMESH_TOPOLOGY_SAME_VERSION,
    DMESH_TOPOLOGY_ADOPTED,
    DMESH_TOPOLOGY_UNREADABLE,
    DMESH_TOPOLOGY_MALFORMED,
    DMESH_TOPOLOGY_ROLLBACK,
    DMESH_TOPOLOGY_OVERFLOW,
    DMESH_TOPOLOGY_UNSIGNED,
    DMESH_TOPOLOGY_BAD_KEY_ID,
    DMESH_TOPOLOGY_BAD_SIG,
};

const char *dmesh_topology_result_name(enum dmesh_topology_result result);

/* Parse DPUMESH_TOPOLOGY_FILE / DPUMESH_CONTROLLER_KEY_DIR. An unset file
 * leaves the consumer disabled; a set file requires the key directory. */
int dmesh_topology_configure(struct objects *objs, char *error, size_t error_len);

/* Adopt the current generation if the publisher installed a newer one. Only a
 * strictly newer, signature-verified, completely parsed generation replaces
 * the live tables. */
enum dmesh_topology_result dmesh_topology_refresh(struct objects *objs);

/* Poll DPUMESH_TOPOLOGY_FILE at the membership cadence and count each outcome
 * as dmesh_control_events_total{kind="topology",reason=...}. Runs on the Comch
 * control thread. Returns nonzero when a new generation was adopted. */
int dmesh_topology_progress(struct objects *objs);

/* Lookups against the held generation. NULL/-1 when nothing is held or the
 * name is absent. */
const struct dmesh_gen_pod *dmesh_topology_pod(const struct objects *objs,
                                               const char *pod_uid);
const struct dmesh_gen_service *
dmesh_topology_service(const struct objects *objs, const char *key);
int dmesh_topology_interned_id(const struct objects *objs, const char *key);

/* One endpoint of a Service, as the routing layer needs it: which Pod, where
 * it is, and what to say to reach it. */
struct dmesh_endpoint_ref {
    const char *pod_uid;
    const char *node_name;
    uint32_t ip_be;
};

/* The endpoints the held generation names for the Service interned as `svc`,
 * excluding those placed on `node_name` — the local half is the node's own
 * live registrations, which is what keeps a Pod that registered before the
 * next generation routable. Returns how many were written. */
int dmesh_topology_remote_endpoints(const struct objects *objs, int16_t svc,
                                    const char *node_name,
                                    struct dmesh_endpoint_ref *out, int max);
/* Select one remote endpoint without truncating the Service's endpoint set.
 * When pod_uid is non-NULL it must match exactly; otherwise ordinal is reduced
 * across the complete remote set. */
int dmesh_topology_remote_endpoint(const struct objects *objs, int16_t svc,
                                   const char *node_name,
                                   const char *pod_uid, uint64_t ordinal,
                                   struct dmesh_endpoint_ref *out);

/* The protection class of the Service interned as `svc`: 1 when the held
 * generation's `protected=` set names it, 0 when the generation names the
 * Service and does not protect it, -1 when no generation decides. What the
 * feed grades is the interaction rules, not whether a Pod is attested — every
 * registration is assertion-verified either way — and no Pod input reaches
 * this, so a Pod cannot opt its Service out. */
int dmesh_topology_service_protection(const struct objects *objs, int16_t svc);

/* The port the Service interned as `svc` is reached on, or 0 when no held
 * generation names it. It is what an inbound policy is watched on: a policy is
 * per Pod and port, and the client's own port names nothing. */
uint16_t dmesh_topology_service_port(const struct objects *objs, int16_t svc);

/* 1 when the held generation places `pod_uid` on `node_name`. This is the
 * identity check a destination applies to a peer's claim: a lookup in a signed
 * table, and the reason the generation is signed by a key no DPU holds. */
int dmesh_topology_pod_on_node(const struct objects *objs, const char *pod_uid,
                               const char *node_name);
/* 1 only when the generation places this exact Pod UID in the qualified
 * Service. Used to verify a peer's source-Service claim. */
int dmesh_topology_pod_in_service(const struct objects *objs,
                                  const char *pod_uid,
                                  const char *service_key);

/* The node record the peer channel authenticates against: its static handshake
 * key and the transport address it listens on. 1 when the generation binds the
 * name. A node whose DPU has not reported a key yet carries the all-zero
 * placeholder and is refused here rather than at the handshake. */
int dmesh_topology_node_peer(const struct objects *objs, const char *node_name,
                             const uint8_t **static_key, uint32_t *ip_be,
                             uint16_t *port);

/* Resolve the agent public key for an assertion. Returns 1 when the held
 * generation names `node_name` — then *key is its agent key when key_id
 * matches, NULL otherwise (refuse as bad-key-id). Returns 0 when no held
 * generation decides, and the caller falls back to the installed keyring. */
int dmesh_topology_node_key(const struct objects *objs, const char *node_name,
                            const char *key_id, const uint8_t **key);

/* Parse one verified document body into freshly allocated tables. Exposed for
 * unit tests; `interned` carries the stable-id state across adoptions. */
enum dmesh_topology_result
dmesh_topology_parse(const char *document, size_t length,
                     struct dmesh_topology_intern *interned,
                     struct dmesh_topology_tables **out);

void dmesh_topology_tables_free(struct dmesh_topology_tables *tables);

#endif /* DMESH_TOPOLOGY_GEN_H */
