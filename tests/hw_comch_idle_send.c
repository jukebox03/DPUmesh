/* Manual BlueField test only. "unsafe" calls the SDK after IDLE on purpose,
 * to be observed under gdb; "guarded" must return CONNECTION_ABORTED. Uses one
 * unauthenticated Comch connection, without workload registration or exported
 * memory. Usage: hw_comch_idle_send PCI unsafe|guarded */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "doca/common.h"
#include "doca/comch_client.h"
#include "doca/object.h"

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "unsafe") && strcmp(argv[2], "guarded"))) return 2;
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs);
    assert(open_doca_device_with_pci(argv[1], NULL, &objs->dev) == DOCA_SUCCESS);
    assert(init_comch_ctrl_path_client("DPUMesh", objs) == DOCA_SUCCESS);
    struct doca_ctx *ctx = doca_comch_client_as_ctx(objs->cc_client);
    doca_error_t result = doca_ctx_stop(ctx);
    assert(result == DOCA_SUCCESS || result == DOCA_ERROR_IN_PROGRESS);
    enum doca_ctx_states state;
    struct timespec start, now, pause = {.tv_nsec = 100000};
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        doca_pe_progress(objs->pe);
        assert(doca_ctx_get_state(ctx, &state) == DOCA_SUCCESS);
        if (state == DOCA_CTX_STATE_IDLE) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - start.tv_sec < 5);
        nanosleep(&pause, NULL);
    } while (1);
    puts("Comch reached IDLE; attempting the selected send path"); fflush(stdout);
    if (!strcmp(argv[2], "unsafe")) {
        struct doca_comch_task_send *task = NULL;
        result = doca_comch_client_task_send_alloc_init(objs->cc_client,
                     objs->connection, "x", 1, &task);
        printf("unsafe allocation returned %d\n", result);
        if (result == DOCA_SUCCESS) doca_task_free(doca_comch_task_send_as_task(task));
    } else {
        result = client_send_msg(objs, "x", 1);
        assert(result == DOCA_ERROR_CONNECTION_ABORTED);
        printf("guarded send returned CONNECTION_ABORTED; no task allocation\n");
    }
    cleanup_objects(objs);
    assert(!objs->cleanup_failed);
    free(objs);
    return 0;
}
