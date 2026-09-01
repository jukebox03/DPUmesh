#define _GNU_SOURCE
#include "dmesh_broker_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void request_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int move_to_cgroup(int cgroup_dir_fd)
{
    int cgroup_procs_fd = openat(cgroup_dir_fd, "cgroup.procs",
                                 O_WRONLY | O_CLOEXEC);
    close(cgroup_dir_fd);
    if (cgroup_procs_fd < 0)
        return -1;
    char pid_text[32];
    int pid_len = snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid());
    int rc = 0;
    if (pid_len <= 0 ||
        write(cgroup_procs_fd, pid_text, (size_t)pid_len) != pid_len)
        rc = -1;
    close(cgroup_procs_fd);
    return rc;
}

static int setup_steady_namespaces(void)
{
    /* The PID namespace and its private procfs are created by the host
     * supervisor.  Everything else is made private here, after the broker has
     * entered the attested Pod cgroup but before it consumes the Pod HELLO. */
    if (unshare(CLONE_NEWNS) != 0 ||
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
        unshare(CLONE_NEWCGROUP | CLONE_NEWNET) != 0)
        return -1;
    char private_root[] = "/run/dpumesh/.broker-root.XXXXXX";
    if (mkdtemp(private_root) == NULL || chmod(private_root, 0700) != 0 ||
        mount("tmpfs", private_root, "tmpfs", MS_NOSUID | MS_NODEV,
              "mode=0700,size=4m") != 0 ||
        setenv("DPUMESH_BROKER_PRIVATE_ROOT", private_root, 1) != 0)
        return -1;
    return 0;
}

static int recv_launch_fds(int launch_fd, int *socket_fd, int *cgroup_dir_fd)
{
    char byte = 0;
    struct iovec iov = { .iov_base = &byte, .iov_len = sizeof(byte) };
    char control[CMSG_SPACE(sizeof(int) * 2)] = {0};
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t n;
    do {
        n = recvmsg(launch_fd, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n != 1 || byte != 'F' || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        errno = EPROTO;
        return -1;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int) * 2) ||
        CMSG_NXTHDR(&msg, cmsg) != NULL) {
        errno = EPROTO;
        return -1;
    }
    int fds[2];
    memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
    *socket_fd = fds[0];
    *cgroup_dir_fd = fds[1];
    return 0;
}

static int connect_launch_socket(const char *path, const char *token,
                                 int *socket_fd)
{
    if (path == NULL || token == NULL || strlen(token) != 64 ||
        strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        send(fd, token, 64, MSG_NOSIGNAL) != 64) {
        close(fd);
        return -1;
    }
    *socket_fd = fd;
    return 0;
}

/* The node agent starts the broker under the host service manager, inside a
 * fresh PID namespace with a private procfs. The launch socket carries the
 * Pod connection and the broker child-cgroup fd; both barriers keep the agent
 * in control until isolation is complete. */
static int run_launch(const char *launch_socket, const char *launch_token,
                      const char *agent_socket)
{
    int launch_fd = -1;
    int socket_fd = -1;
    int cgroup_dir_fd = -1;
    if (connect_launch_socket(launch_socket, launch_token, &launch_fd) != 0) {
        fprintf(stderr, "dmesh_broker: launch socket failed: %s\n", strerror(errno));
        return 1;
    }
    if (recv_launch_fds(launch_fd, &socket_fd, &cgroup_dir_fd) != 0 ||
        move_to_cgroup(cgroup_dir_fd) != 0) {
        fprintf(stderr, "dmesh_broker: launch fd/cgroup setup failed: %s\n",
                strerror(errno));
        close(launch_fd);
        return 1;
    }
    if (send(launch_fd, "M", 1, MSG_NOSIGNAL) != 1 ||
        setup_steady_namespaces() != 0) {
        fprintf(stderr, "dmesh_broker: namespace isolation failed: %s\n",
                strerror(errno));
        close(launch_fd);
        close(socket_fd);
        return 1;
    }
    char final_go = 0;
    ssize_t n;
    do {
        n = recv(launch_fd, &final_go, 1, 0);
    } while (n < 0 && errno == EINTR);
    close(launch_fd);
    if (n != 1 || final_go != 'G') {
        fprintf(stderr, "dmesh_broker: invalid final barrier\n");
        close(socket_fd);
        return 1;
    }
    struct sigaction action = { .sa_handler = request_stop };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "dmesh_broker: signal setup failed: %s\n", strerror(errno));
        close(socket_fd);
        return 1;
    }
    return dmesh_broker_run(socket_fd, agent_socket, &stop_requested) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *launch_socket = NULL;
    const char *launch_token = NULL;
    const char *agent_socket = "/run/dpumesh/attest.sock";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent-sock") == 0 && i + 1 < argc)
            agent_socket = argv[++i];
        else if (strcmp(argv[i], "--launch-sock") == 0 && i + 1 < argc)
            launch_socket = argv[++i];
        else if (strcmp(argv[i], "--launch-token") == 0 && i + 1 < argc)
            launch_token = argv[++i];
        else {
            fprintf(stderr, "dmesh_broker: invalid argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (launch_socket == NULL || launch_token == NULL) {
        fprintf(stderr, "dmesh_broker: launch socket/token required\n");
        return 2;
    }
    return run_launch(launch_socket, launch_token, agent_socket);
}
