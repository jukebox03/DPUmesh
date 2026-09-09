#include <assert.h>
#include <stdio.h>
#include "../src/core/dmesh_core.c"

static struct objects *active;
static int sends, ticks, complete_sends;
static enum doca_ctx_states state;
int clock_gettime(clockid_t clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = 1 + ticks / 10;
    ts->tv_nsec = (ticks++ % 10) * 100000000;
    return 0;
}
int nanosleep(const struct timespec *req, struct timespec *rem)
{ (void)req; (void)rem; return 0; }
struct doca_ctx *doca_comch_client_as_ctx(struct doca_comch_client *client)
{ return (struct doca_ctx *)client; }
doca_error_t doca_ctx_get_state(const struct doca_ctx *ctx, enum doca_ctx_states *out)
{ (void)ctx; *out = state; return DOCA_SUCCESS; }
doca_error_t client_send_msg(struct objects *objs, const char *msg, size_t len)
{
    (void)msg; (void)len;
    sends++;
    atomic_fetch_add(&objs->send_tasks_in_flight, 1);
    return DOCA_SUCCESS;
}
uint8_t doca_pe_progress(struct doca_pe *pe)
{
    (void)pe;
    if (complete_sends) {
        atomic_store(&active->send_tasks_in_flight, 0);
        if (sends == 2) active->pod_quiesced = 1;
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--hardening")) {
        assert(geteuid() == 0);
        assert(prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == 0);
        assert(broker_harden_process() == 0);
        int parent_signal = 0;
        assert(prctl(PR_GET_PDEATHSIG, &parent_signal, 0, 0, 0) == 0);
        assert(geteuid() == 65532 && parent_signal == SIGKILL);
        puts("broker hardening: uid=65532 parent-death signal=SIGKILL: PASS");
        return 0;
    }
    static dpumesh_ctx_t ctx;
    active = &ctx.doca_objs;
    active->cc_client = (void *)1; active->connection = (void *)2;
    active->pe = (void *)3; active->assigned_pod_id = 0;
    state = DOCA_CTX_STATE_RUNNING;
    /* The peer stays apparently RUNNING without completing any sends. The
     * previous code queued dozens of retries before its quiesce timeout. */
    assert(request_remote_pod_quiesce(&ctx) == -1);
    assert(sends == 1 && atomic_load(&active->send_tasks_in_flight) == 1);
    /* A completed send with a lost application ACK must still be retried. */
    sends = ticks = 0; complete_sends = 1;
    atomic_store(&active->send_tasks_in_flight, 0);
    assert(request_remote_pod_quiesce(&ctx) == 0);
    assert(sends == 2);
    puts("broker_quiesce_test: PASS");
}
