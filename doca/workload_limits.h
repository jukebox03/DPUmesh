#ifndef DMESH_WORKLOAD_LIMITS_H
#define DMESH_WORKLOAD_LIMITS_H

/* Shared identity field sizes: paired-host registration and peer security
 * must bind the same complete values. Independent of the DOCA SDK. */
#define DMESH_REG_NONCE_SIZE 32u
#define DMESH_POD_UID_MAX 64u
#define DMESH_K8S_NAMESPACE_MAX 64u
#define DMESH_K8S_NAME_MAX 254u
#define DMESH_SVC_NAME_MAX 64u
#define DMESH_DAEMON_INCARNATION_SIZE 16u

#endif
