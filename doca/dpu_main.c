/*
 * dpu_main.c - DPUmesh DPU binary entry point
 *
 * Runs on BlueField DPU ARM cores.
 * Usage: dpumesh_dpu -p <pci-addr> -r <rep-pci-addr>
 */

#include <execinfo.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "local_control.h"
#include "comch_server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <doca_dev.h>
#include <doca_log.h>
#include <doca_build_config.h>

#include "config.h"
#include "common.h"
#include "object.h"
#include "dpu_worker.h"
#include "workload_grant.h"
#include "pod_membership.h"
#include "topology.h"
#include "peer_channel.h"
#include "control_scope.h"

DOCA_LOG_REGISTER(DPU_MAIN);

/* A termination request leaves the worker loop, which then drains and releases
 * hardware; it does not end the process from the handler. */
volatile sig_atomic_t dmesh_dpu_stop;
static void request_stop(int sig) { (void)sig; dmesh_dpu_stop = 1; }

/* The process log is the only record of how this process ended: a fatal
 * signal leaves its frames here before the default action runs, and an exit
 * call leaves a line. */
static void trace_fatal_signal(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    char line[64];
    int len = snprintf(line, sizeof(line), "dpumesh_dpu: fatal signal %d\n", sig);
    if (len > 0)
        (void)!write(STDERR_FILENO, line, (size_t)len);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void trace_exit(void)
{
    static const char line[] = "dpumesh_dpu: exit() called\n";
    (void)!write(STDERR_FILENO, line, sizeof(line) - 1);
}

static void install_exit_traces(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = trace_fatal_signal;
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &sa, NULL);
    sa.sa_handler = request_stop; sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    atexit(trace_exit);
}

int main(int argc, char **argv)
{
    install_exit_traces();
    /* One runtime owns the device. The lock is held for the life of the
     * process, so a service and a Pod can never drive the same BlueField. */
    const char *lock_path = getenv("DPUMESH_RUNTIME_LOCK");
    if (!lock_path) lock_path = "/run/dpumesh-runtime/runtime.lock";
    (void)mkdir("/run/dpumesh-runtime", 0700);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "dpumesh_dpu: another runtime owns hardware or lock unavailable\n");
        return 1;
    }
    const char *ready_file = getenv("DPUMESH_READY_FILE");
    if (ready_file) unlink(ready_file);

    /* A write to a socket whose peer has gone must return EPIPE, not end the
     * process. The embedded Rust proxy is a static library, so the runtime
     * start-up that would ignore SIGPIPE never runs; do it here, before
     * anything opens a socket. */
    signal(SIGPIPE, SIG_IGN);

    /* Heap-allocated: the large runtime state lives until the worker drains. */
    struct objects *objs = calloc(1, sizeof(*objs));
    struct global_config gcfg = {0};
    doca_error_t result;
    struct doca_log_backend *sdk_log;

    if (!objs) {
        fprintf(stderr, "Failed to allocate objects struct\n");
        return 1;
    }

    /* Logging setup */
    result = doca_log_backend_create_standard();
    if (result != DOCA_SUCCESS)
        goto exit;

    result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    if (result != DOCA_SUCCESS)
        goto exit;

    result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
    if (result != DOCA_SUCCESS)
        goto exit;

    /* A grant is scoped to one cluster and node. A DPU that does not know both
     * refuses every registration. */
    const char *cluster_id = getenv("DPUMESH_CLUSTER_ID");
    const char *node_name = getenv("DPUMESH_NODE_NAME");
    if (cluster_id == NULL || *cluster_id == '\0' ||
        strlen(cluster_id) >= sizeof(objs->cluster_id) ||
        node_name == NULL || *node_name == '\0' ||
        strlen(node_name) >= sizeof(objs->node_name)) {
        DOCA_LOG_ERR("Trusted registration configuration failed: "
                     "DPUMESH_CLUSTER_ID and DPUMESH_NODE_NAME are required");
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }
    snprintf(objs->cluster_id, sizeof(objs->cluster_id), "%s", cluster_id);
    snprintf(objs->node_name, sizeof(objs->node_name), "%s", node_name);

    const char *mode = getenv("DPUMESH_REGISTRATION_MODE");
    if (mode && strcmp(mode, "grant") && strcmp(mode, "direct")) {
        result = DOCA_ERROR_INVALID_VALUE; goto exit;
    }
    objs->local_registration = mode && !strcmp(mode, "direct");
    char registration_error[256] = {0};
    if (dmesh_registration_configure(objs, registration_error,
                                     sizeof(registration_error)) != 0) {
        DOCA_LOG_ERR("Trusted registration configuration failed: %s",
                     registration_error);
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }
    char membership_error[256] = {0};
    if (dmesh_membership_configure(objs, membership_error,
                                   sizeof(membership_error)) != 0) {
        DOCA_LOG_ERR("Membership configuration failed: %s", membership_error);
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }
    if (dmesh_admission_configure(objs, membership_error,
                                  sizeof(membership_error)) != 0) {
        DOCA_LOG_ERR("Admission configuration failed: %s", membership_error);
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }
    if (dmesh_topology_configure(objs, membership_error,
                                 sizeof(membership_error)) != 0) {
        DOCA_LOG_ERR("Topology configuration failed: %s", membership_error);
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }
    if (dmesh_scope_configure(objs, membership_error,
                              sizeof(membership_error)) != 0) {
        DOCA_LOG_ERR("Control-plane scope configuration failed: %s", membership_error);
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }

    /* The node credential: one static keypair per DPU, generated here at first
     * boot into a 0400 file that never leaves it. Only the public half travels
     * — the host runtime reports it and the controller publishes it in this
     * node's `node=` line, so a peer authenticates this DPU with a key it took
     * from the generation rather than from this DPU. */
    const char *credential = getenv("DPUMESH_NODE_KEY_FILE");
    if (credential && *credential) {
        char credential_error[256] = {0};
        if (dmesh_peer_node_key_load(credential, objs->node_public_key, NULL,
                                     credential_error, sizeof(credential_error)) != 0) {
            DOCA_LOG_ERR("Node credential failed: %s", credential_error);
            result = DOCA_ERROR_INVALID_VALUE;
            goto exit;
        }
        objs->node_key_ready = 1;
        const char *published = getenv("DPUMESH_NODE_KEY_PUBLIC_FILE");
        if (published && *published &&
            dmesh_peer_node_key_publish(published, objs->node_public_key) != 0)
            DOCA_LOG_WARN("Node credential public half could not be published at %s; "
                          "this node stays unreachable as a peer until it is",
                          published);
    }

    /* Detect mode */
#ifdef DOCA_ARCH_DPU
    gcfg.mode = DPU_MODE;
#else
    gcfg.mode = HOST_MODE;
#endif

    /* Parse command-line arguments (-p, -r) */
    result = init_argp(NULL, &gcfg, argc, argv);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to parse arguments: %s", doca_error_get_descr(result));
        goto exit;
    }

    /* Open DOCA device */
    result = open_doca_device_with_pci(gcfg.dev_pci_addr, NULL, &(objs->dev));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to open DOCA device at %s", gcfg.dev_pci_addr);
        goto argp_cleanup;
    }

    /* Open representor device (DPU mode) */
    if (gcfg.mode == DPU_MODE) {
        result = open_doca_device_rep_with_pci(objs->dev,
                                               DOCA_DEVINFO_REP_FILTER_NET,
                                               gcfg.dev_rep_pci_addr,
                                               &(objs->rep_dev));
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to open representor device at %s",
                         gcfg.dev_rep_pci_addr);
            cleanup_objects(objs);
            goto argp_cleanup;
        }
    }

    if (objs->local_registration) {
        char host_uri[320];
        snprintf(host_uri, sizeof(host_uri), "spiffe://dpumesh.io/node/%s", objs->node_name);
        const char *port = getenv("DPUMESH_LOCAL_PORT");
        objs->local_control = dmesh_local_control_create(
            getenv("DPUMESH_LOCAL_BIND"), port ? (unsigned)strtoul(port, NULL, 10) : 4791,
            getenv("DPUMESH_LOCAL_CA"), getenv("DPUMESH_LOCAL_CERT"),
            getenv("DPUMESH_LOCAL_KEY"), host_uri, server_local_dispatch,
            server_local_retire, server_local_available, objs);
        if (!objs->local_control) {
            DOCA_LOG_ERR("paired-host control TLS configuration failed");
            result = DOCA_ERROR_INVALID_VALUE; cleanup_objects(objs); goto argp_cleanup;
        }
    }
    result = run_dpu_worker(objs) == 0 ? DOCA_SUCCESS : DOCA_ERROR_BAD_STATE;
    dmesh_local_control_destroy(objs->local_control);
    objs->local_control = NULL;
    if (ready_file) unlink(ready_file);

argp_cleanup:
    clean_argp();
exit:
    return result == DOCA_SUCCESS ? 0 : 1;
}
