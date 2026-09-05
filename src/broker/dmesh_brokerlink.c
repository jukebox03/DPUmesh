#define _GNU_SOURCE
#include "dmesh_brokerlink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int ipc_error(char *buffer, size_t length, const char *what)
{
    if (buffer != NULL && length != 0)
        snprintf(buffer, length, "%s: %s", what, strerror(errno));
    return -1;
}

int dmesh_broker_connect(const char *path, const char *service,
                         char *error, size_t error_len)
{
    struct stat st;
    struct sockaddr_un address;
    if (path == NULL || strlen(path) >= sizeof(address.sun_path) ||
        service == NULL || strlen(service) >= DMESH_IPC_SERVICE_LEN) {
        errno = EINVAL;
        return ipc_error(error, error_len, "invalid broker endpoint");
    }
    if (lstat(path, &st) != 0)
        return ipc_error(error, error_len, "lstat broker socket");
    if (!S_ISSOCK(st.st_mode) || st.st_uid != 0) {
        errno = EPERM;
        return ipc_error(error, error_len, "broker socket is not root-owned");
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return ipc_error(error, error_len, "create broker socket");
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ipc_error(error, error_len, "connect broker socket");
        close(fd);
        return -1;
    }
    struct dmesh_ipc_hello hello = {0};
    memcpy(hello.magic, DMESH_IPC_MAGIC, sizeof(hello.magic));
    hello.type = DMESH_IPC_HELLO;
    hello.version = DMESH_IPC_VERSION;
    snprintf(hello.service, sizeof(hello.service), "%s", service);
    if (send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello)) {
        ipc_error(error, error_len, "send broker HELLO");
        close(fd);
        return -1;
    }
    return fd;
}

int dmesh_broker_recv_ready(int socket_fd, struct dmesh_ipc_ready *ready,
                            int *fds, size_t fds_cap,
                            char *error, size_t error_len)
{
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * DMESH_BROKER_MAX_FDS)];
    } control;
    struct iovec iov = { .iov_base = ready, .iov_len = sizeof(*ready) };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    memset(ready, 0, sizeof(*ready));
    ssize_t received = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC | MSG_TRUNC);
    if (received < 0)
        return ipc_error(error, error_len, "receive broker READY");
    if ((size_t)received == sizeof(struct dmesh_ipc_error) &&
        ready->type == DMESH_IPC_ERROR) {
        errno = EIO;
        if (error != NULL && error_len != 0)
            snprintf(error, error_len, "broker rejected initialization");
        return -1;
    }
    if (received != sizeof(*ready) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        ready->type != DMESH_IPC_READY ||
        ready->version != DMESH_IPC_VERSION ||
        ready->reserved != 0 ||
        ready->fd_count > fds_cap) {
        errno = EBADMSG;
        return ipc_error(error, error_len, "invalid broker READY");
    }
    size_t count = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        size_t n = bytes / sizeof(int);
        if (count + n > fds_cap) {
            errno = EBADMSG;
            goto fail;
        }
        memcpy(&fds[count], CMSG_DATA(cmsg), n * sizeof(int));
        count += n;
    }
    if (count != ready->fd_count) {
        errno = EBADMSG;
        goto fail;
    }
    /* Only the leading K + L + TX + RX descriptors are memfds. The trailing
     * doorbell descriptor is an eventfd and deliberately has no seals. */
    size_t sealed_count = (size_t)ready->k_rings +
                          (size_t)ready->landing_stripes + 2u;
    if (sealed_count > count) {
        errno = EBADMSG;
        goto fail;
    }
    for (size_t i = 0; i < sealed_count; i++) {
        int seals = fcntl(fds[i], F_GET_SEALS);
        int required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
        if (seals < 0 || (seals & required) != required ||
            (seals & F_SEAL_WRITE) != 0) {
            errno = EPERM;
            goto fail;
        }
    }
    return 0;
fail:
    for (size_t i = 0; i < count; i++)
        close(fds[i]);
    return ipc_error(error, error_len, "invalid broker fd set");
}

int dmesh_broker_send_ready(int socket_fd,
                            const struct dmesh_ipc_ready *ready,
                            const int *fds, size_t fd_count)
{
    if (ready == NULL || fds == NULL || fd_count == 0 ||
        fd_count > DMESH_BROKER_MAX_FDS) {
        errno = EINVAL;
        return -1;
    }
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * DMESH_BROKER_MAX_FDS)];
    } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = { .iov_base = (void *)ready, .iov_len = sizeof(*ready) };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = CMSG_SPACE(fd_count * sizeof(int)),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, fd_count * sizeof(int));
    ssize_t sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
    if (sent != sizeof(*ready)) {
        fprintf(stderr, "dmesh_broker: READY send failed: sent=%zd expected=%zu "
                "fds=%zu error=%s\n", sent, sizeof(*ready), fd_count,
                strerror(errno));
        return -1;
    }
    fprintf(stderr, "dmesh_broker: READY sent pod_id=%d fds=%zu\n",
            ready->pod_id, fd_count);
    return 0;
}

int dmesh_broker_recv_hello(int socket_fd, struct dmesh_ipc_hello *hello)
{
    ssize_t n = recv(socket_fd, hello, sizeof(*hello), MSG_TRUNC);
    if (n != sizeof(*hello) ||
        memcmp(hello->magic, DMESH_IPC_MAGIC, sizeof(hello->magic)) != 0 ||
        hello->type != DMESH_IPC_HELLO ||
        hello->version != DMESH_IPC_VERSION || hello->pad[0] || hello->pad[1] ||
        memchr(hello->service, '\0', sizeof(hello->service)) == NULL) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int dmesh_broker_send_error(int socket_fd, int code, const char *message)
{
    struct dmesh_ipc_error reply = {
        .type = DMESH_IPC_ERROR,
        .version = DMESH_IPC_VERSION,
        .code = (uint32_t)code,
    };
    snprintf(reply.text, sizeof(reply.text), "%s",
             message != NULL ? message : "broker error");
    fprintf(stderr, "dmesh_broker: initialization failed: %s (code=%d)\n",
            reply.text, code);
    return send(socket_fd, &reply, sizeof(reply), MSG_NOSIGNAL) == sizeof(reply)
               ? 0 : -1;
}
