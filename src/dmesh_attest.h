#ifndef DMESH_ATTEST_H
#define DMESH_ATTEST_H

#include <stddef.h>
#include <stdint.h>

#include "doca/comch_common.h"

/* Where the node-agent DaemonSet publishes its request socket.
 * DPUMESH_ATTEST_SOCKET names another one. */
#define DMESH_DEFAULT_ATTEST_SOCKET "/run/dpumesh/attest.sock"

/* Ask the root-owned node agent to authorize the calling process. The agent
 * identifies this process with SO_PEERCRED and derives claims from cgroup/K8s
 * metadata; request bytes are never treated as workload identity. */
int dmesh_attest_request_grant(
    const char *socket_path,
    int32_t requested_service_id,
    const uint8_t nonce[DMESH_REG_NONCE_SIZE],
    struct dmesh_workload_grant_msg *grant,
    char *error,
    size_t error_len);

#endif /* DMESH_ATTEST_H */
