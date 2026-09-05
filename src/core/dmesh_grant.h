#ifndef DMESH_GRANT_H
#define DMESH_GRANT_H

#include <stddef.h>
#include <stdint.h>

#include "doca/comch_common.h"

/* The Device Plugin mounts this allocation socket into the selected container. */
#define DMESH_DEFAULT_CHANNEL_SOCKET "/run/dpumesh/channel.sock"

/* Ask the root-owned node runtime to authorize the calling process. The runtime
 * identifies this process with SO_PEERCRED and derives claims from cgroup/K8s
 * metadata; request bytes are never treated as workload identity. */
int dmesh_request_workload_grant(
    const char *socket_path,
    const char *requested_service_name,
    const uint8_t nonce[DMESH_REG_NONCE_SIZE],
    struct dmesh_workload_assert_msg *assertion,
    char *error,
    size_t error_len);

#endif /* DMESH_GRANT_H */
