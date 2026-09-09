#include <assert.h>
#include <stdio.h>
#include "doca/comch_client.h"
#include "doca/object.h"
#include "../doca/comch_client.c"

static enum doca_ctx_states state;
static struct objects *active;
static int allocations, progresses, freed;
struct doca_task *doca_comch_task_send_as_task(struct doca_comch_task_send *task)
{ return (struct doca_task *)task; }
doca_error_t doca_task_get_status(const struct doca_task *task)
{ (void)task; return DOCA_ERROR_CONNECTION_ABORTED; }
void doca_task_free(struct doca_task *task)
{ (void)task; freed++; }
doca_error_t doca_ctx_stop(struct doca_ctx *ctx)
{ (void)ctx; assert(!"context stop inside error callback"); return DOCA_SUCCESS; }

struct doca_ctx *doca_comch_client_as_ctx(struct doca_comch_client *client)
{ return (struct doca_ctx *)client; }
doca_error_t doca_ctx_get_state(const struct doca_ctx *ctx, enum doca_ctx_states *out)
{ (void)ctx; *out = state; return DOCA_SUCCESS; }
uint8_t doca_pe_progress(struct doca_pe *pe)
{
    (void)pe; progresses++;
    state = DOCA_CTX_STATE_IDLE;
    atomic_store(&active->send_tasks_in_flight, 0);
    return 1;
}
doca_error_t doca_comch_client_task_send_alloc_init(
    struct doca_comch_client *client, struct doca_comch_connection *connection,
    const void *msg, uint32_t len, struct doca_comch_task_send **task)
{
    (void)client; (void)connection; (void)msg; (void)len; (void)task;
    allocations++;
    return DOCA_ERROR_NO_MEMORY;
}
int main(void)
{
    static struct objects objs; active = &objs;
    objs.cc_client = (void *)1; objs.connection = (void *)2; objs.pe = (void *)3;
    objs.send_tasks_max = TASK_POOL_MARGIN + 1;
    state = DOCA_CTX_STATE_IDLE;
    assert(client_send_msg(&objs, "x", 1) == DOCA_ERROR_CONNECTION_ABORTED);
    assert(allocations == 0 && atomic_load(&objs.send_tasks_in_flight) == 0);
    state = DOCA_CTX_STATE_STOPPING;
    assert(client_send_msg(&objs, "x", 1) == DOCA_ERROR_CONNECTION_ABORTED);
    assert(allocations == 0);
    /* A peer can disappear while the bounded send-pool wait progresses PE. */
    state = DOCA_CTX_STATE_RUNNING;
    atomic_store(&objs.send_tasks_in_flight, 1);
    assert(client_send_msg(&objs, "x", 1) == DOCA_ERROR_CONNECTION_ABORTED);
    assert(progresses == 1 && allocations == 0);
    assert(atomic_load(&objs.send_tasks_in_flight) == 0);
    state = DOCA_CTX_STATE_RUNNING;
    assert(client_send_msg(&objs, "x", 1) == DOCA_ERROR_NO_MEMORY);
    assert(allocations == 1 && atomic_load(&objs.send_tasks_in_flight) == 0);
    /* Completion callbacks must not stop/free the active SDK connection while
     * PE progress still has it on its stack. The owning loop sees this flag. */
    union doca_data task_data = {.ptr = malloc(4)}, context_data = {.ptr = &objs};
    atomic_store(&objs.send_tasks_in_flight, 1);
    client_send_task_completion_err_callback((void *)42, task_data, context_data);
    assert(objs.client_send_failed && freed == 1);
    assert(atomic_load(&objs.send_tasks_in_flight) == 0);
    assert(client_send_msg(&objs, "x", 1) == DOCA_ERROR_CONNECTION_ABORTED);
    assert(allocations == 1);
    puts("comch_send_state_test: PASS");
}
