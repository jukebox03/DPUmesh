#include <assert.h>
#include <stdio.h>
#include "doca/object.h"
static enum doca_ctx_states state;
static int ticks, stuck, destroyed, pe_destroyed, closed;
static doca_error_t destroy_result;
int clock_gettime(clockid_t clock, struct timespec *ts)
{ (void)clock; ts->tv_sec = ticks++; ts->tv_nsec = 0; return 0; }
int nanosleep(const struct timespec *req, struct timespec *rem)
{ (void)req; (void)rem; return 0; }
struct doca_ctx *doca_comch_client_as_ctx(struct doca_comch_client *client)
{ return (struct doca_ctx *)client; }
doca_error_t doca_ctx_get_state(const struct doca_ctx *ctx, enum doca_ctx_states *out)
{ (void)ctx; *out = state; return DOCA_SUCCESS; }
doca_error_t doca_ctx_get_num_inflight_tasks(const struct doca_ctx *ctx, size_t *n)
{ (void)ctx; *n = stuck ? 1 : 0; return DOCA_SUCCESS; }
doca_error_t doca_ctx_stop(struct doca_ctx *ctx)
{ (void)ctx; state = DOCA_CTX_STATE_STOPPING; return DOCA_ERROR_IN_PROGRESS; }
uint8_t doca_pe_progress(struct doca_pe *pe)
{ (void)pe; if (!stuck) state = DOCA_CTX_STATE_IDLE; return 1; }
doca_error_t doca_comch_client_destroy(struct doca_comch_client *client)
{ (void)client; destroyed++; return destroy_result; }
doca_error_t doca_pe_destroy(struct doca_pe *pe)
{ (void)pe; pe_destroyed++; return DOCA_SUCCESS; }
doca_error_t doca_dev_close(struct doca_dev *dev)
{ (void)dev; closed++; return DOCA_SUCCESS; }
int main(void)
{
    static struct objects objs;
    objs.cc_client = (void *)1; objs.pe = (void *)2; objs.dev = (void *)3;
    state = DOCA_CTX_STATE_RUNNING; stuck = 1;
    cleanup_objects(&objs);
    assert(objs.cleanup_failed && objs.cc_client && objs.pe && objs.dev);
    assert(!destroyed && !pe_destroyed && !closed);
    state = DOCA_CTX_STATE_IDLE; stuck = 0; destroy_result = DOCA_ERROR_IN_USE;
    cleanup_objects(&objs);
    assert(destroyed == 1 && !pe_destroyed && !closed);
    assert(objs.cc_client && objs.pe && objs.dev);
    destroy_result = DOCA_SUCCESS;
    cleanup_objects(&objs);
    assert(!objs.cc_client && !objs.pe && !objs.dev);
    assert(destroyed == 2 && pe_destroyed == 1 && closed == 1);
    puts("comch_cleanup_test: PASS");
}
