#include "object.h"

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_comch.h>

#include <time.h>

#include "comch_server.h"  /* CC_SEND_TASK_NUM */

DOCA_LOG_REGISTER(OBJECT);

/* Initialize the task-pool counters; call at startup before any submit. */
void
objects_init_task_pools(struct objects *objs)
{
    atomic_store(&objs->send_tasks_in_flight, 0);
    objs->send_tasks_max = CC_SEND_TASK_NUM;
}

static int
elapsed_at_least(const struct timespec *start, const struct timespec *now,
                 time_t seconds)
{
    time_t sec = now->tv_sec - start->tv_sec;
    if (now->tv_nsec < start->tv_nsec)
        sec--;
    return sec >= seconds;
}

void
cleanup_comch_object(struct objects *objs)
{
    doca_error_t result;

    /* cc_server/cc_client share a union, but they are not interchangeable DOCA
     * objects. The DPU build owns a server and the host build owns a client.
     * Stop the correct context and drive its PE to IDLE before destroying it;
     * otherwise the wrong destroy call leaves the PE and device permanently
     * IN_USE on every dmesh_destroy_channel(). */
#ifdef DOCA_ARCH_DPU
    struct doca_ctx *comch_ctx = objs->cc_server == NULL
                                     ? NULL
                                     : doca_comch_server_as_ctx(objs->cc_server);
#else
    struct doca_ctx *comch_ctx = objs->cc_client == NULL
                                     ? NULL
                                     : doca_comch_client_as_ctx(objs->cc_client);
#endif
    if (comch_ctx != NULL) {
        enum doca_ctx_states state;
        result = doca_ctx_get_state(comch_ctx, &state);
        if (result == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE) {
            result = doca_ctx_stop(comch_ctx);
            if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
                DOCA_LOG_ERR("Failed to stop cc context with error = %s",
                             doca_error_get_name(result));
            } else {
                const struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000 };
                struct timespec start, now;
                clock_gettime(CLOCK_MONOTONIC, &start);
                do {
                    if (objs->pe != NULL)
                        (void)doca_pe_progress(objs->pe);
                    nanosleep(&pause, NULL);
                    result = doca_ctx_get_state(comch_ctx, &state);
                    if (result != DOCA_SUCCESS || state == DOCA_CTX_STATE_IDLE)
                        break;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                } while (!elapsed_at_least(&start, &now, 5));
                if (result != DOCA_SUCCESS || state != DOCA_CTX_STATE_IDLE) {
                    DOCA_LOG_ERR("CC context did not reach IDLE before cleanup");
                }
            }
        } else if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to query cc context state with error = %s",
                         doca_error_get_name(result));
        }

#ifdef DOCA_ARCH_DPU
        result = doca_comch_server_destroy(objs->cc_server);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to destroy cc server properly with error = %s",
                         doca_error_get_name(result));
        }
        objs->cc_server = NULL;
#else
        result = doca_comch_client_destroy(objs->cc_client);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to destroy cc client properly with error = %s",
                         doca_error_get_name(result));
        }
        objs->cc_client = NULL;
#endif
    }
}

void
cleanup_objects(struct objects *objs)
{
    doca_error_t result;

    cleanup_comch_object(objs);

    if (objs->pe) {
        result = doca_pe_destroy(objs->pe);
        if(result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to destroy pe properly with error = %s", doca_error_get_name(result));
        }
        objs->pe = NULL;
    }

    if (objs->rep_dev) {
        result = doca_dev_rep_close(objs->rep_dev);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to close rep device properly with error = %s", doca_error_get_name(result));
        }
        objs->rep_dev = NULL;
    }

    if (objs->dev) {
        result = doca_dev_close(objs->dev);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to close device properly with error = %s", doca_error_get_name(result));
        }
        objs->dev = NULL;
    }

}
