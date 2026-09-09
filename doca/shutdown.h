#ifndef DMESH_SHUTDOWN_H
#define DMESH_SHUTDOWN_H
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_log.h>
#include <time.h>

/* Called after joining all ARM owners. Fail closed before destroying resources
 * still in use. The process supervisor handles an abnormal exit. */
#define DMESH_STOP_CHECK(call) do { \
    doca_error_t er_ = (call); \
    if (er_ != DOCA_SUCCESS) { \
        DOCA_LOG_ERR("shutdown %s: %s", #call, doca_error_get_name(er_)); \
        return er_; \
    } \
} while (0)

static inline doca_error_t
dmesh_stop_context(struct doca_ctx *ctx, struct doca_pe *pe)
{
    enum doca_ctx_states state;
    DMESH_STOP_CHECK(doca_ctx_get_state(ctx, &state));
    if (state == DOCA_CTX_STATE_IDLE) return DOCA_SUCCESS;
    doca_error_t er = doca_ctx_stop(ctx);
    if (er != DOCA_SUCCESS && er != DOCA_ERROR_IN_PROGRESS) return er;
    struct timespec start, now, pause = { .tv_nsec = 100000 };
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        if (pe) doca_pe_progress(pe);
        DMESH_STOP_CHECK(doca_ctx_get_state(ctx, &state));
        if (state == DOCA_CTX_STATE_IDLE) return DOCA_SUCCESS;
        nanosleep(&pause, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < 3);
    return DOCA_ERROR_TIME_OUT;
}
#endif
