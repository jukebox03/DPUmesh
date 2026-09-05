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
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
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

static int setup_steady_namespaces(const char *private_root)
{
    /* The host supervisor creates the PID namespace and private procfs. The
     * broker enters its bounded worker cgroup before consuming the Pod HELLO,
     * then isolates the remaining namespaces here. */
    if (unshare(CLONE_NEWNS) != 0 ||
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
        unshare(CLONE_NEWCGROUP | CLONE_NEWNET) != 0)
        return -1;
    if (private_root == NULL || private_root[0] != '/' ||
        mount("tmpfs", private_root, "tmpfs", MS_NOSUID | MS_NODEV,
              "mode=0700,size=4m") != 0)
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

static int connect_launch_socket(const char *path, int *socket_fd)
{
    if (path == NULL ||
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
        send(fd, "B", 1, MSG_NOSIGNAL) != 1) {
        close(fd);
        return -1;
    }
    *socket_fd = fd;
    return 0;
}

/* dpumeshd starts the broker inside a fresh PID namespace with private procfs.
 * The launch socket carries the Pod connection and worker-cgroup fd; both
 * barriers keep dpumeshd in control until isolation is complete. */
static int run_launch(const char *launch_socket, const char *manager_socket,
                      const char *private_root)
{
    int launch_fd = -1;
    int socket_fd = -1;
    int cgroup_dir_fd = -1;
    if (connect_launch_socket(launch_socket, &launch_fd) != 0) {
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
        setup_steady_namespaces(private_root) != 0) {
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
    return dmesh_broker_run(socket_fd, manager_socket, private_root,
                            &stop_requested) == 0 ? 0 : 1;
}

/* The short-lived supervisor is a direct child of dpumeshd. Both parent-death
 * edges are armed before the worker may touch a device, and the socketpair
 * closes the fork/prctl race. The root-only launch endpoint and verified
 * SO_PEERCRED/parent PID need no command-line bootstrap secret. */
static int run_supervised(const char *launch_socket, const char *manager_socket,
                          const char *private_root, pid_t expected_parent)
{
    if (expected_parent <= 1 || getppid() != expected_parent ||
        prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != expected_parent) {
        fprintf(stderr, "dmesh_broker: supervisor parent identity changed\n");
        return 1;
    }
    if (unshare(CLONE_NEWPID | CLONE_NEWNS) != 0 ||
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        fprintf(stderr, "dmesh_broker: supervisor namespace setup failed: %s\n",
                strerror(errno));
        return 1;
    }
    int barrier[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, barrier) != 0) {
        fprintf(stderr, "dmesh_broker: supervisor barrier failed: %s\n",
                strerror(errno));
        return 1;
    }
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "dmesh_broker: supervisor fork failed: %s\n",
                strerror(errno));
        close(barrier[0]);
        close(barrier[1]);
        return 1;
    }
    if (child == 0) {
        close(barrier[0]);
        char go = 0;
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
            send(barrier[1], "R", 1, MSG_NOSIGNAL) != 1 ||
            recv(barrier[1], &go, 1, 0) != 1 || go != 'G')
            _exit(126);
        close(barrier[1]);
        /* This process is PID 1 in the new namespace. Replace inherited host
         * procfs so the steady broker cannot enumerate host processes. */
        if ((umount2("/proc", MNT_DETACH) != 0 && errno != EINVAL) ||
            mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
                  NULL) != 0) {
            fprintf(stderr, "dmesh_broker: private procfs failed: %s\n",
                    strerror(errno));
            _exit(126);
        }
        _exit(run_launch(launch_socket, manager_socket, private_root));
    }

    close(barrier[1]);
    char ready = 0;
    if (recv(barrier[0], &ready, 1, 0) != 1 || ready != 'R' ||
        send(barrier[0], "G", 1, MSG_NOSIGNAL) != 1) {
        kill(child, SIGKILL);
        close(barrier[0]);
        (void)waitpid(child, NULL, 0);
        return 1;
    }
    close(barrier[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int main(int argc, char **argv)
{
    const char *launch_socket = NULL;
    const char *manager_socket = "/run/dpumesh/manager.sock";
    const char *private_root = NULL;
    pid_t expected_parent = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--manager-sock") == 0 && i + 1 < argc)
            manager_socket = argv[++i];
        else if (strcmp(argv[i], "--launch-sock") == 0 && i + 1 < argc)
            launch_socket = argv[++i];
        else if (strcmp(argv[i], "--private-root") == 0 && i + 1 < argc)
            private_root = argv[++i];
        else if (strcmp(argv[i], "--expected-parent") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value <= 1) {
                fprintf(stderr, "dmesh_broker: invalid expected parent\n");
                return 2;
            }
            expected_parent = (pid_t)value;
        }
        else {
            fprintf(stderr, "dmesh_broker: invalid argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (launch_socket == NULL) {
        fprintf(stderr, "dmesh_broker: launch socket required\n");
        return 2;
    }
    if (private_root == NULL || private_root[0] != '/') {
        fprintf(stderr, "dmesh_broker: private root required\n");
        return 2;
    }
    if (expected_parent <= 1) {
        fprintf(stderr, "dmesh_broker: expected parent required\n");
        return 2;
    }
    return run_supervised(launch_socket, manager_socket, private_root,
                          expected_parent);
}
