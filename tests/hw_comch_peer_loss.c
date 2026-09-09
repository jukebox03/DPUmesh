/* Manual hardware reproducer: connect without registration/exported memory,
 * SIGKILL the DPU runtime, then SIGUSR1 this process to retry one send at a time and
 * drain its error completion. Never part of the automatic test suite. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <doca_log.h>
#include "doca/common.h"
#include "doca/comch_client.h"
#include "doca/object.h"
static volatile sig_atomic_t proceed;
static void ready_to_send(int sig) { (void)sig; proceed = 1; }
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    doca_log_backend_create_standard();
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs);
    signal(SIGUSR1, ready_to_send);
    assert(open_doca_device_with_pci(argv[1], NULL, &objs->dev) == DOCA_SUCCESS);
    assert(init_comch_ctrl_path_client("DPUMesh", objs) == DOCA_SUCCESS);
    doca_notification_handle_t handle;
    assert(doca_pe_get_notification_handle(objs->pe, &handle) == DOCA_SUCCESS);
    assert(doca_pe_set_event_mode(objs->pe, DOCA_PE_EVENT_MODE_PROGRESS_ALL) == DOCA_SUCCESS);
    assert(doca_pe_request_notification(objs->pe) == DOCA_SUCCESS);
    printf("connected pid=%d; kill peer then send SIGUSR1\n", getpid()); fflush(stdout);
    while (!proceed) usleep(10000);
    doca_error_t result = client_send_msg(objs, "x", 1);
    printf("send returned %d\n", result); fflush(stdout);
    enum doca_ctx_states state;
    time_t start = time(NULL);
    do {
        doca_pe_progress(objs->pe);
        assert(doca_ctx_get_state(doca_comch_client_as_ctx(objs->cc_client), &state) == DOCA_SUCCESS);
        if (state != DOCA_CTX_STATE_RUNNING) break;
        if (atomic_load(&objs->send_tasks_in_flight) == 0) {
            usleep(100000);
            result = client_send_msg(objs, "x", 1);
            if (result != DOCA_SUCCESS) break;
        } else usleep(100);
    } while (time(NULL) - start < 15);
    printf("before cleanup state=%d owned=%d\n", state, atomic_load(&objs->send_tasks_in_flight)); fflush(stdout);
    cleanup_objects(objs);
    assert(!objs->cleanup_failed);
    free(objs);
    puts("peer-loss cleanup: PASS");
    return 0;
}
