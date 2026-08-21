/* The layout and the constants of the L7 adapter contract, as C declares them.
 *
 * `linkerd/rust/src/lib.rs` mirrors `dmesh_l7.h` by hand — the staticlib is
 * built by a different compiler in a different tree, so there is nothing that
 * checks the two agree. This test states the numbers once on the C side; the
 * `abi` and `session_key_matches_c_handle` tests in the wrapper state the same
 * numbers on the Rust side. A field that moves fails one of the two rather than
 * corrupting a flow at run time.
 */

#include <dmesh_l7.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "l7_abi_contract_test: %s:%d: %s\n", __FILE__,     \
                    __LINE__, #cond);                                          \
            return 1;                                                          \
        }                                                                      \
    } while (0)

/* struct dmesh_l7_flow */
_Static_assert(sizeof(struct dmesh_l7_flow) == 664, "flow size");
_Static_assert(_Alignof(struct dmesh_l7_flow) == 4, "flow alignment");
_Static_assert(offsetof(struct dmesh_l7_flow, src_ip) == 0, "src_ip");
_Static_assert(offsetof(struct dmesh_l7_flow, dst_ip) == 4, "dst_ip");
_Static_assert(offsetof(struct dmesh_l7_flow, src_port) == 8, "src_port");
_Static_assert(offsetof(struct dmesh_l7_flow, dst_port) == 10, "dst_port");
_Static_assert(offsetof(struct dmesh_l7_flow, src_pod) == 12, "src_pod");
_Static_assert(offsetof(struct dmesh_l7_flow, dst_service) == 16, "dst_service");
_Static_assert(offsetof(struct dmesh_l7_flow, peer_pod) == 20, "peer_pod");
_Static_assert(offsetof(struct dmesh_l7_flow, mode) == 24, "mode");
_Static_assert(offsetof(struct dmesh_l7_flow, is_reply) == 25, "is_reply");
_Static_assert(offsetof(struct dmesh_l7_flow, workload) == 26, "workload");
_Static_assert(sizeof(((struct dmesh_l7_flow *)0)->workload) == 384, "workload size");
_Static_assert(offsetof(struct dmesh_l7_flow, source_identity) == 410, "source_identity");
_Static_assert(sizeof(((struct dmesh_l7_flow *)0)->source_identity) == 254,
               "source_identity size");

/* The values both sides compare against, rather than translate. */
_Static_assert(DMESH_L7_MODE_OPAQUE == 1, "mode opaque");
_Static_assert(DMESH_L7_MODE_FULL == 2, "mode full");
_Static_assert(DMESH_L7_BACKEND_ANY == -1, "backend any");
_Static_assert(DMESH_L7_ORIGIN == -2, "backend origin");
_Static_assert(DMESH_L7_DECLINE_ERROR == -1, "decline error");
_Static_assert(DMESH_L7_DECLINE_NOT_ATTACHED == -2, "decline not attached");
_Static_assert(DMESH_L7_DECLINE_MODE == -3, "decline mode");
_Static_assert(DMESH_L7_DECLINE_SESSION_LIMIT == -4, "decline session limit");
_Static_assert(DMESH_L7_DECLINE_UNKNOWN_REPLY == -5, "decline unknown reply");

/* Function declarations are ABI too. These expressions are unevaluated, so
 * this compile-time coverage does not require the Rust static archive to be
 * linked into this small contract test. */
#define CHECK_FN(name, type)                                                   \
    _Static_assert(_Generic(&(name), type: 1, default: 0),                     \
                   "function signature: " #name)

CHECK_FN(l7_worker_run, int (*)(int, void *));
CHECK_FN(l7_conn_open,
         int (*)(int, uint64_t, const struct dmesh_l7_flow *));
CHECK_FN(l7_conn_segment,
         int (*)(int, uint64_t, const uint8_t *, uint32_t, uint32_t));
CHECK_FN(l7_conn_eof, void (*)(int, uint64_t));
CHECK_FN(l7_conn_close, void (*)(int, uint64_t));
CHECK_FN(dmesh_l7_backends, int (*)(int, int32_t, int32_t *, int));
CHECK_FN(dmesh_l7_tx_reserve,
         uint8_t *(*)(int, uint64_t, uint32_t *));
CHECK_FN(dmesh_l7_tx_commit, int (*)(int, uint64_t, int32_t, uint32_t));
CHECK_FN(dmesh_l7_tx_commit_remote,
         int (*)(int, uint64_t, const char *, uint32_t));
CHECK_FN(dmesh_l7_tx_fin,
         int (*)(int, uint64_t, int32_t, const char *));
CHECK_FN(dmesh_l7_session_failed, void (*)(int, uint64_t));
CHECK_FN(dmesh_l7_release,
         void (*)(int, uint64_t, uint32_t, uint32_t));
CHECK_FN(dmesh_l7_driver_notification_fds,
         int (*)(void *, int *, int *, int *));
CHECK_FN(dmesh_l7_driver_arm, int (*)(void *));
CHECK_FN(dmesh_l7_driver_drain, int (*)(void *, int));
CHECK_FN(dmesh_l7_driver_clear_notifications, int (*)(void *));
CHECK_FN(dmesh_l7_driver_maintenance, int (*)(void *));
CHECK_FN(dmesh_l7_driver_stopped, int (*)(void *));
CHECK_FN(dmesh_l7_driver_ready, void (*)(void *));
CHECK_FN(dmesh_l7_driver_failed, void (*)(void *));

#undef CHECK_FN

/* The decline codes are what the data plane counts a fallback by, so no two of
 * them may name the same reason. `DMESH_L7_ORIGIN` shares a value with one of
 * them and does not collide: it is a tx-commit destination, never a
 * return from l7_conn_open. */
_Static_assert(DMESH_L7_DECLINE_ERROR != DMESH_L7_DECLINE_NOT_ATTACHED &&
                   DMESH_L7_DECLINE_NOT_ATTACHED != DMESH_L7_DECLINE_MODE &&
                   DMESH_L7_DECLINE_MODE != DMESH_L7_DECLINE_SESSION_LIMIT &&
                   DMESH_L7_DECLINE_SESSION_LIMIT != DMESH_L7_DECLINE_UNKNOWN_REPLY,
               "decline codes must be distinct");

/* The connection handle, as both sides form it. `session_key_matches_c_handle`
 * in the wrapper puts the same pairs through its own implementation. */
static const struct {
    int32_t pod;
    uint16_t port;
    uint64_t handle;
} handles[] = {
    { 0, 0, UINT64_C(0x00000000) },   { 0, 1, UINT64_C(0x00000001) },
    { 1, 9092, UINT64_C(0x00012384) }, { 11, 40000, UINT64_C(0x000b9c40) },
    { 127, 65535, UINT64_C(0x007fffff) }, { 128, 1, UINT64_C(0x00800001) },
    { 255, 1, UINT64_C(0x00ff0001) },  { -1, 1, UINT64_C(0x00ff0001) },
};

int main(void)
{
    uint64_t generated = dmesh_l7_conn_handle_generation(-7, 4321, 99);
    assert(dmesh_l7_handle_pod(generated) == -7);
    assert(dmesh_l7_handle_port(generated) == 4321);
    assert(dmesh_l7_handle_generation(generated) == 99);
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        uint64_t got = dmesh_l7_conn_handle(handles[i].pod, handles[i].port);
        if (got != handles[i].handle) {
            fprintf(stderr,
                    "l7_abi_contract_test: handle(%d,%u) = 0x%llx, expected 0x%llx\n",
                    handles[i].pod, handles[i].port, (unsigned long long)got,
                    (unsigned long long)handles[i].handle);
            return 1;
        }
        CHECK(dmesh_l7_handle_port(got) == handles[i].port);
    }

    /* The pod comes back as a signed byte, so it round-trips over the range a
     * pod id actually occupies and aliases above it — which is why
     * l7_conn_open refuses a pod outside one byte rather than keying on it. */
    for (int32_t pod = 0; pod <= 127; pod++)
        CHECK(dmesh_l7_handle_pod(dmesh_l7_conn_handle(pod, 4321)) == pod);
    CHECK(dmesh_l7_handle_pod(dmesh_l7_conn_handle(255, 1)) == -1);

    /* The flow the data plane fills is the flow the layer reads: a NUL in the
     * last byte of each text field is what keeps a full-width name terminated. */
    struct dmesh_l7_flow flow;
    CHECK(sizeof(flow.workload) == 384);
    /* An identity is `<service-account>.<namespace>.serviceaccount.identity.
     * <trust-domain>`; a maximal ServiceAccount and namespace plus the fixed
     * middle and a cluster-local trust domain fit inside the field with its
     * terminator, which is what the field width is for. */
    CHECK(sizeof(flow.source_identity) == 254);
    memset(&flow, 0, sizeof(flow));
    memset(flow.source_identity, 'x', sizeof(flow.source_identity) - 1);
    CHECK(strlen(flow.source_identity) == sizeof(flow.source_identity) - 1);

    printf("l7_abi_contract_test: PASS\n");
    return 0;
}
