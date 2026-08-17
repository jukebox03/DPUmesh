#include "dmesh_attest.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "doca/workload_grant.h"

int
dmesh_attest_request_grant(const char *socket_path,
                           int32_t requested_service_id,
                           const uint8_t nonce[DMESH_REG_NONCE_SIZE],
                           struct dmesh_workload_grant_msg *grant,
                           char *error, size_t error_len)
{
    int fd = -1;
    int rc = -1;
    struct stat st;
    struct sockaddr_un address;
    struct dmesh_attest_request request;

#define ATTEST_ERROR(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); } while (0)
    if (socket_path == NULL || *socket_path == '\0' || grant == NULL ||
        nonce == NULL) {
        ATTEST_ERROR("invalid node-agent request arguments");
        return -1;
    }
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        ATTEST_ERROR("node-agent socket path is too long");
        return -1;
    }
    if (lstat(socket_path, &st) != 0) {
        ATTEST_ERROR("lstat(%s): %s", socket_path, strerror(errno));
        return -1;
    }
    if (!S_ISSOCK(st.st_mode) || st.st_uid != 0) {
        ATTEST_ERROR("%s must be a root-owned Unix socket", socket_path);
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ATTEST_ERROR("socket: %s", strerror(errno));
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ATTEST_ERROR("connect(%s): %s", socket_path, strerror(errno));
        goto out;
    }

    memset(&request, 0, sizeof(request));
    memcpy(request.magic, DMESH_ATTEST_MAGIC, sizeof(request.magic));
    request.version = DMESH_GRANT_VERSION;
    dmesh_grant_put_i32_le(request.service_id_le, requested_service_id);
    memcpy(request.nonce, nonce, sizeof(request.nonce));
    if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request)) {
        ATTEST_ERROR("send node-agent request: %s", strerror(errno));
        goto out;
    }

    memset(grant, 0, sizeof(*grant));
    ssize_t received = recv(fd, grant, sizeof(*grant), MSG_TRUNC);
    if (received != sizeof(*grant)) {
        ATTEST_ERROR("node agent returned %zd bytes; expected %zu",
                     received, sizeof(*grant));
        goto out;
    }
    if (grant->type != DMESH_MSG_WORKLOAD_GRANT ||
        grant->version != DMESH_GRANT_VERSION ||
        dmesh_grant_get_i32_le(grant->service_id_le) !=
            requested_service_id ||
        memcmp(grant->nonce, nonce, DMESH_REG_NONCE_SIZE) != 0) {
        ATTEST_ERROR("node agent returned a grant for another request");
        goto out;
    }
    rc = 0;
out:
    close(fd);
    return rc;
#undef ATTEST_ERROR
}
