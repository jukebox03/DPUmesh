/*
 * dpu_main.c - DPUmesh DPU binary entry point
 *
 * Runs on BlueField DPU ARM cores.
 * Usage: dpumesh_dpu -p <pci-addr> -r <rep-pci-addr>
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    /* A write to a socket whose peer has gone must return EPIPE, not end the
     * process. The embedded Rust proxy is a static library, so the runtime
     * start-up that would ignore SIGPIPE never runs; do it here, before
     * anything opens a socket. */
    signal(SIGPIPE, SIG_IGN);

    /* Heap-allocated (struct objects is large); never freed — the process runs
     * until killed and run_dpu_worker() below blocks forever. */
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

    /* A grant's signed node_name is checked against this DPU's own node, so a
     * DPU that does not know its node cannot verify any registration. */
    const char *node_name = getenv("DPUMESH_NODE_NAME");
    if (node_name == NULL || *node_name == '\0' ||
        strlen(node_name) >= sizeof(objs->node_name)) {
        DOCA_LOG_ERR("Trusted registration configuration failed: "
                     "DPUMESH_NODE_NAME must name this Kubernetes node");
        result = DOCA_ERROR_INVALID_VALUE;
        goto exit;
    }
    snprintf(objs->node_name, sizeof(objs->node_name), "%s", node_name);

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
     * — the node agent reports it and the controller publishes it in this
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

    /* Run DPU worker (blocking) */
    run_dpu_worker(objs);

argp_cleanup:
    clean_argp();
exit:
    return result == DOCA_SUCCESS ? 0 : 1;
}
