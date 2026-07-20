/* Validation-pod supervisor for preloaded tcp_echo and tcp_client children.
 *
 * CTRL_PORT accepts RUN <count> <size> <connections>, forwards it to the client,
 * and returns its OK or ERR result. Child failure terminates the runner. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>

static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}

static pid_t spawn(const char *bin, char *const argv[],
                   const char *env_kv[][2], int *stdin_w, int *stdout_r) {
    int inp[2] = { -1, -1 }, outp[2] = { -1, -1 };
    if (stdin_w  && pipe(inp)  < 0) return -1;
    if (stdout_r && pipe(outp) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (stdin_w)  { dup2(inp[0], 0);  close(inp[0]);  close(inp[1]); }
        if (stdout_r) { dup2(outp[1], 1); close(outp[0]); close(outp[1]); }
        for (int i = 0; env_kv[i][0]; i++)
            setenv(env_kv[i][0], env_kv[i][1], 1);
        execv(bin, argv);
        fprintf(stderr, "execv(%s): %s\n", bin, strerror(errno));
        _exit(127);
    }
    if (stdin_w)  { close(inp[0]);  *stdin_w  = inp[1]; }
    if (stdout_r) { close(outp[1]); *stdout_r = outp[0]; }
    return pid;
}

/* Read one '\n'-terminated line from fd with a timeout. Returns len or -1. */
static ssize_t read_line(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t got = 0;
    while (got + 1 < cap) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, timeout_ms);
        if (r <= 0) return -1;
        ssize_t n = read(fd, buf + got, 1);
        if (n <= 0) return -1;
        if (buf[got++] == '\n') break;
    }
    buf[got] = 0;
    return (ssize_t)got;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);   /* pod log = pipe → line-buffer it */
    const char *lib       = env_or("PRELOAD_LIB", "/usr/local/lib/libdmesh_preload.so");
    const char *echo_port = env_or("ECHO_PORT", "9095");
    const char *svc       = env_or("PRELOAD_SVC", "15");
    int ctrl_port         = atoi(env_or("CTRL_PORT", "9092"));
    const char *echo_bin  = env_or("ECHO_BIN", "/usr/local/bin/tcp_echo");
    const char *client_bin= env_or("CLIENT_BIN", "/usr/local/bin/tcp_client");

    /* Registry FIRST — before ANY preloaded child starts (NAMING.md §5: the harness
     * plays the dpumesh-controller). ONE row serves both directions: the echo
     * server's identity (name → svc) and the client's connect() (ClusterIP:port →
     * svc). It MUST exist before the echo child's shim ctor runs, or that child could
     * not resolve its own $DPUMESH_SERVICE. Format: "IP:port name svc". */
    const char *svc_name = env_or("PRELOAD_SERVICE", "preload-dpumesh");
    const char *svc_ip   = env_or("SVC_IP", "10.96.0.15");
    const char *reg_path = "/tmp/dpumesh_registry";
    FILE *rf = fopen(reg_path, "w");
    if (!rf) { fprintf(stderr, "runner: registry open failed\n"); return 1; }
    fprintf(rf, "%s:%s %s %s\n", svc_ip, echo_port, svc_name, svc);
    fclose(rf);

    /* echo server (persistent): identity + listen port injected via env, resolved
     * through the registry above (no integer typed). */
    const char *echo_env[][2] = {
        { "LD_PRELOAD", lib },
        { "DPUMESH_CONFIG", reg_path },
        { "DPUMESH_SERVICE", svc_name },
        { "DPUMESH_PORT", echo_port },
        { NULL, NULL },
    };
    char *echo_argv[] = { (char *)echo_bin, (char *)echo_port, NULL };
    pid_t echo_pid = spawn(echo_bin, echo_argv, echo_env, NULL, NULL);
    if (echo_pid < 0) { fprintf(stderr, "runner: echo spawn failed\n"); return 1; }
    sleep(2);                          /* channel registration + listen */

    /* client daemon (persistent, stdin-driven): pure client — no identity, just the
     * registry for connect() resolution. */
    const char *cli_env[][2] = {
        { "LD_PRELOAD", lib },
        { "DPUMESH_CONFIG", reg_path },
        { NULL, NULL },
    };
    char *cli_argv[] = { (char *)client_bin, (char *)svc_ip, (char *)echo_port, NULL };
    int cli_in = -1, cli_out = -1;
    pid_t cli_pid = spawn(client_bin, cli_argv, cli_env, &cli_in, &cli_out);
    if (cli_pid < 0) { fprintf(stderr, "runner: client spawn failed\n"); return 1; }

    /* control listener (plain kernel TCP — this process is NOT preloaded) */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons((uint16_t)ctrl_port);
    if (bind(lfd, (struct sockaddr *)&sin, sizeof sin) < 0 || listen(lfd, 16) < 0) {
        fprintf(stderr, "runner: ctrl bind/listen: %s\n", strerror(errno));
        return 1;
    }
    printf("runner: up (svc=%s echo_port=%s ctrl=%d)\n", svc, echo_port, ctrl_port);
    fflush(stdout);

    char line[256], resp[256];
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }

        /* A dead child is unrecoverable for this pod, but DO NOT exit: a pod
         * restart re-registers 2 dmesh channels while the DPU never reclaims
         * dead ones (MAX_PODS=8 table) — restarting would burn slots and wedge
         * the whole mesh. Stay up and keep reporting the error instead. */
        if (waitpid(echo_pid, NULL, WNOHANG) != 0 || waitpid(cli_pid, NULL, WNOHANG) != 0) {
            dprintf(cfd, "ERR child_exited\n");
            close(cfd);
            continue;
        }

        ssize_t n = read_line(cfd, line, sizeof line, 10000);
        long n_msgs, size, conns;
        if (n <= 0 || sscanf(line, "RUN %ld %ld %ld", &n_msgs, &size, &conns) != 3) {
            dprintf(cfd, "ERR bad_command\n");
            close(cfd);
            continue;
        }
        /* Drain any stale client output (e.g. a RESULT left over from a prior
         * aborted exchange) so the reply below can't desync. */
        while (read_line(cli_out, resp, sizeof resp, 0) > 0) {}

        dprintf(cli_in, "RUN %ld %ld %ld\n", n_msgs, size, conns);

        /* The client's stdout also carries library chatter (DOCA SDK logs its
         * channel init to stdout on the first connect) — skip everything until
         * the RESULT line. Generous overall ceiling for big RUNs. */
        unsigned long long ok = 0, fail = 0;
        unsigned p50 = 0, p99 = 0;
        unsigned long rps = 0;
        int got = 0;
        for (;;) {
            if (read_line(cli_out, resp, sizeof resp, 600000) <= 0) break;
            if (strncmp(resp, "RESULT ", 7) == 0) { got = 1; break; }
            fputs(resp, stdout);       /* surface skipped lines in the pod log */
        }
        if (!got) {
            dprintf(cfd, "ERR client_timeout\n");
            close(cfd);
            continue;                  /* stay up — see the child_exited note */
        }
        if (sscanf(resp, "RESULT %llu %llu %u %u %lu", &ok, &fail, &p50, &p99, &rps) == 5)
            dprintf(cfd, "OK %llu %llu %u %u %lu\n", ok, fail, p50, p99, rps);
        else
            dprintf(cfd, "ERR bad_result\n");
        close(cfd);
    }
    return 0;
}
