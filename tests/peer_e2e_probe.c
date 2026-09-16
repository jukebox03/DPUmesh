/* One node's half of the automatic Pod-pair inline path, end to end on real
 * hardware: the node crypto manager negotiates with its peer over a real TCP
 * control session, drives the production pair transport on real verbs, asks the
 * running dpumesh_crypto_owner to install the SAs and rules, and then exchanges
 * RDMA data over the lane the owner protects. Nothing here is a fixture stand-in
 * for the manager or the hardware adapter; only the registration/placement/
 * policy tables are fixture answers.
 *
 * Both nodes run this. Node A (client) opens one Pod-pair stream; node B just
 * runs its manager loop and accepts. When the manager reports the lane ready,
 * each side sends frames and verifies the peer's. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "doca/peer_manager.h"
#include "doca/peer_crypto_ipc.h"
#include "doca/peer_wire.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static const char *NODES[2] = {"node-a", "node-b"};
static const char *PODS[2] = {"pod-a", "pod-b"};
static unsigned SIDE;
static uint8_t KEYS[2][32];
static uint32_t LOCAL_IP, PEER_IP;   /* network order, the SF fabric IPv4 pair */
static uint16_t PEER_CTRL_PORT;
static uint32_t g_peer_ctrl_ip;

static uint64_t now_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

/* ---- fixture answers (registration/placement/policy only) --------------- */
static struct dmesh_peer_registration reg_for(unsigned side)
{
    struct dmesh_peer_registration r = {0};
    r.daemon[0] = (uint8_t)(side + 1); r.nonce[31] = (uint8_t)(side + 1); r.generation = 7; r.slot = side;
    return r;
}
static int node_binding(void *ctx, const char *node, const uint8_t **key, uint32_t *ip, uint16_t *port)
{
    (void)ctx;
    if (strcmp(node, NODES[1 - SIDE])) return 0;
    *key = KEYS[1 - SIDE]; *ip = g_peer_ctrl_ip; *port = PEER_CTRL_PORT; return 1;
}
static int node_by_key(void *ctx, const uint8_t key[32], char node[DMESH_K8S_NAME_MAX])
{
    (void)ctx;
    if (memcmp(key, KEYS[1 - SIDE], 32)) return 0;
    strcpy(node, NODES[1 - SIDE]); return 1;
}
static int pod_on_node(void *ctx, const char *uid, const char *node)
{
    (void)ctx;
    return (!strcmp(uid, PODS[0]) && !strcmp(node, NODES[0])) ||
           (!strcmp(uid, PODS[1]) && !strcmp(node, NODES[1]));
}
static int registration(void *ctx, const char *uid, struct dmesh_peer_registration *out)
{
    (void)ctx;
    if (strcmp(uid, PODS[SIDE])) return 0;
    *out = reg_for(SIDE); return 1;
}
static int authorize(void *ctx, const struct dmesh_peer_pair *p, const struct dmesh_peer_stream_open *in)
{ (void)ctx; return in->dst_port == 8080 && !strcmp(p->local_uid, PODS[SIDE]); }
static const struct dmesh_peer_ops TABLE_OPS = {.node_binding = node_binding, .pod_on_node = pod_on_node,
    .local_registration = registration, .pair_authorize = authorize};

/* Plaintext control: a crypto adapter that acknowledges every SA op without
 * touching hardware, so the manager activates the lane and RC packets flow
 * unencrypted. This isolates the inline-IPsec cost from the rest of the path. */
static struct { struct peer_crypto_completion q[64]; unsigned n; } STUB;
static int stub_submit(void *c, const struct peer_crypto_request *r)
{ (void)c; if (STUB.n >= 64) return 0; struct peer_crypto_completion *d = &STUB.q[STUB.n++];
  *d = (struct peer_crypto_completion){.token = r->token, .operation = r->operation, .action = r->action,
       .epoch = r->binding.epoch, .status = 0};
  memcpy(d->session, r->binding.session, 16); memcpy(d->association, r->binding.association, 16); return 1; }
static int stub_poll(void *c, struct peer_crypto_completion *o)
{ (void)c; if (!STUB.n) return 0; *o = STUB.q[0]; memmove(STUB.q, STUB.q + 1, --STUB.n * sizeof(*STUB.q)); return 1; }
static int stub_fd(void *c) { (void)c; return -1; }
static int stub_healthy(void *c) { (void)c; return 1; }
static const struct peer_manager_ops MANAGER_OPS = {.node_binding = node_binding, .node_by_key = node_by_key};

int main(int argc, char **argv)
{
    if (argc != 9 && argc != 10) {
        fprintf(stderr, "usage: %s SIDE(0|1) RDMA_DEV GID_INDEX LOCAL_IP PEER_IP CTRL_PORT OWNER_SOCKET ROUNDS [MSG_BYTES]\n"
                        "  MSG_BYTES set -> latency ping-pong + throughput at that fixed size; unset -> 256/64KiB functional.\n", argv[0]);
        return 2;
    }
    size_t msg_bytes = argc == 10 ? (size_t)strtoul(argv[9], NULL, 10) : 0;
    int plaintext = getenv("E2E_PLAINTEXT") != NULL;   /* control: stub adapter, no owner */
    SIDE = (unsigned)atoi(argv[1]);
    const char *dev_name = argv[2];
    uint32_t gid = (uint32_t)strtoul(argv[3], NULL, 10);
    struct in_addr a, b; CHECK(inet_pton(AF_INET, argv[4], &a) == 1 && inet_pton(AF_INET, argv[5], &b) == 1);
    LOCAL_IP = a.s_addr; PEER_IP = b.s_addr; g_peer_ctrl_ip = b.s_addr;
    uint16_t ctrl_port = (uint16_t)atoi(argv[6]);
    PEER_CTRL_PORT = ctrl_port;   /* both nodes bind the same control port on their own fabric IP */
    const char *owner_sock = argv[7];
    unsigned rounds = (unsigned)atoi(argv[8]);
    CHECK(SIDE <= 1);

    for (unsigned i = 0; i < 2; i++) {
        struct peer_tls_ctx *c; uint8_t seed[32] = {0}; seed[0] = (uint8_t)(i + 1);
        CHECK(!peer_tls_ctx_new(seed, NODES[i], &c, NULL, 0));
        peer_tls_ctx_public_key(c, KEYS[i]); peer_tls_ctx_free(c);
    }

    /* Real verbs device for the pair transport. */
    int n = 0; struct ibv_device **list = ibv_get_device_list(&n); CHECK(list);
    struct ibv_context *verbs = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(ibv_get_device_name(list[i]), dev_name)) verbs = ibv_open_device(list[i]);
    ibv_free_device_list(list); CHECK(verbs);

    struct peer_pair_transport_config tcfg = {.worker = 0, .connections = 4, .setup_timeout_ns = 30ull * 1000000000ull,
        .dma_fenced = dmesh_peer_channel_dma_fenced};
    struct peer_pair_transport *rt; char err[256];
    if (peer_pair_transport_new(&tcfg, verbs, 1, gid, 1024, &rt, err, sizeof(err))) { fprintf(stderr, "pair transport: %s\n", err); return 1; }
    const struct dmesh_peer_transport *ops = peer_pair_transport_ops();
    static struct dmesh_peer_table table;
    dmesh_peer_table_init(&table, NODES[SIDE], KEYS[SIDE], ops, rt, &TABLE_OPS, NULL);
    peer_pair_transport_attach(rt, &table);

    /* Real crypto owner over its socket, or the stub for the plaintext control. */
    struct peer_crypto_ipc *ipc = NULL;
    struct peer_crypto_adapter crypto;
    if (plaintext) crypto = (struct peer_crypto_adapter){.ctx = &STUB, .submit = stub_submit,
        .poll = stub_poll, .fd = stub_fd, .healthy = stub_healthy};
    else { CHECK(!peer_crypto_ipc_new(owner_sock, &ipc, err, sizeof(err))); crypto = peer_crypto_ipc_adapter(ipc); }

    /* Real TCP control carrier, bound to this node's fabric IP:port. */
    const struct peer_wire_ops *wire_ops; void *wire_ctx;
    CHECK(!peer_wire_tcp_new(LOCAL_IP, ctrl_port, &wire_ops, &wire_ctx, err, sizeof(err)));

    struct peer_manager_config mcfg = {.seed = (const uint8_t *)"\1", .workers = 1, .mtu = 1024,
        .wire = wire_ops, .wire_ctx = wire_ctx, .crypto = crypto,
        .limits = {.pairs = 8, .lanes = 32, .sas = 64, .setup_timeout_ns = 20ull * 1000000000ull, .overlap_ns = 2000000000ull},
        .lease_ns = 120ull * 1000000000ull, .control_lease_ns = 20ull * 1000000000ull,
        .setup_timeout_ns = 20ull * 1000000000ull, .ops = &MANAGER_OPS, .ops_ctx = NULL};
    uint8_t seed[32] = {0}; seed[0] = (uint8_t)(SIDE + 1); mcfg.seed = seed;
    strcpy(mcfg.node, NODES[SIDE]); strcpy(mcfg.cluster, "e2e-cluster"); mcfg.boot[0] = (uint8_t)(SIDE + 9);
    struct peer_manager *m; CHECK(!peer_manager_new(&mcfg, &m, err, sizeof(err)));
    CHECK(!peer_manager_attach_worker(m, 0, rt));
    CHECK(peer_manager_notify_authority(m, 1, now_ns() + 300ull * 1000000000ull) == 1);

    /* The client opens one Pod-pair stream; the server just runs its loop. */
    struct dmesh_peer_channel *ch = NULL;
    if (SIDE == 0) {
        struct dmesh_peer_stream_open in = {.src_generation = 7, .dst_port = 8080, .source_token = 1};
        strcpy(in.src_pod_uid, PODS[0]); strcpy(in.dst_pod_uid, PODS[1]); strcpy(in.src_service_key, "ns/service");
        enum dmesh_peer_refusal why;
        ch = dmesh_peer_open_stream(&table, NODES[1], &in, &why);
        CHECK(ch && !why);
    }

    /* Drive both owners' loops: manager (control + registry) and transport. */
    uint64_t deadline = now_ns() + 60ull * 1000000000ull;
    struct dmesh_peer_pair cert;
    void *conn = NULL;
    for (;;) {
        CHECK(ops->progress(rt) >= 0);
        CHECK(peer_manager_progress(m, now_ns()) >= 0);
        if (SIDE == 0) { conn = ch->conn; if (conn && ops->pair_ready(conn, &cert) == 1) break; }
        else { struct dmesh_peer_channel *c = dmesh_peer_find_pair(&table, NODES[0], PODS[1], PODS[0]);
               if (c && c->conn && ops->pair_ready(c->conn, &cert) == 1) { ch = c; conn = c->conn; break; } }
        CHECK(now_ns() < deadline);
        usleep(2000);
    }
    fprintf(stderr, "LANE_READY side=%u\n", SIDE);

    uint8_t *tx = malloc(PEER_WIRE_MSG_MAX), *rx = malloc(PEER_WIRE_MSG_MAX);
    CHECK(tx && rx);
    /* Build one DATA frame of `bytes` at a chosen sequence into tx. */
    #define BUILD(seq, bytes) do { \
        struct peer_sec_data_header _h = {.type = PEER_SEC_DATA, .body_len = (uint32_t)(bytes), \
            .lane_id = cert.lane_id, .lane_generation = cert.lane_generation, .handle = 1, \
            .stream_generation = 1, .sequence = (seq)}; \
        memcpy(_h.association, cert.association, 16); CHECK(!peer_sec_data_encode(&_h, tx)); \
        for (size_t _i = 0; _i < (bytes); _i++) tx[64 + _i] = (uint8_t)(_i + (seq)); \
    } while (0)
    #define SEND(bytes) do { long _r; do { CHECK(ops->progress(rt) >= 0); CHECK(peer_manager_progress(m, now_ns()) >= 0); \
        _r = ops->pair_admit(conn) == 1 ? ops->send(conn, tx, 64 + (bytes)) : 0; CHECK(_r >= 0); CHECK(now_ns() < deadline); \
        } while (!_r); CHECK(_r == (long)(64 + (bytes))); } while (0)
    #define RECV(bytes) do { long _r; do { CHECK(ops->progress(rt) >= 0); CHECK(peer_manager_progress(m, now_ns()) >= 0); \
        _r = ops->recv(conn, rx, PEER_WIRE_MSG_MAX); CHECK(_r >= 0); CHECK(now_ns() < deadline); \
        } while (!_r); CHECK(_r == (long)(64 + (bytes))); } while (0)

    if (!msg_bytes) {
        /* Functional: alternating 256/64KiB, both directions, verify bytes. */
        deadline = now_ns() + 60ull * 1000000000ull;
        unsigned sent = 0, recv = 0;
        uint8_t *exp = malloc(PEER_WIRE_MSG_MAX); CHECK(exp);
        while (sent < rounds || recv < rounds) {
            CHECK(ops->progress(rt) >= 0); CHECK(peer_manager_progress(m, now_ns()) >= 0);
            if (sent < rounds && ops->pair_admit(conn) == 1) {
                size_t bytes = sent % 2 ? 65536 : 256; BUILD(sent + 1, bytes);
                for (size_t i = 0; i < bytes; i++) tx[64 + i] = (uint8_t)(i * 31 + sent * 7 + SIDE);
                long r = ops->send(conn, tx, 64 + bytes); CHECK(r >= 0);
                if (r) { CHECK(r == (long)(64 + bytes)); sent++; }
            }
            long r = ops->recv(conn, rx, PEER_WIRE_MSG_MAX); CHECK(r >= 0);
            if (r) { size_t bytes = recv % 2 ? 65536 : 256; CHECK(r == (long)(64 + bytes));
                for (size_t i = 0; i < bytes; i++) exp[64 + i] = (uint8_t)(i * 31 + recv * 7 + (1 - SIDE));
                CHECK(!memcmp(rx + 64, exp + 64, bytes)); recv++; }
            CHECK(now_ns() < deadline); usleep(500);
        }
        free(exp);
        printf("E2E_PASS side=%u sent=%u recv=%u bytes=256,65536\n", SIDE, sent, recv);
    } else {
        CHECK(msg_bytes >= 1 && msg_bytes <= PEER_SEC_DATA_MAX);
        deadline = now_ns() + 120ull * 1000000000ull;
        BUILD(1, msg_bytes);
        /* Latency: closed-loop ping-pong. A sends then waits for the echo; B
         * waits then echoes. One RTT per round; report p50/p99/mean over rounds. */
        uint64_t *lat = calloc(rounds, sizeof(*lat)); CHECK(lat);
        for (unsigned i = 0; i < rounds; i++) {
            if (SIDE == 0) { uint64_t t0 = now_ns(); SEND(msg_bytes); RECV(msg_bytes); lat[i] = now_ns() - t0; }
            else { RECV(msg_bytes); SEND(msg_bytes); }
        }
        if (SIDE == 0) {
            for (unsigned i = 0; i + 1 < rounds; i++) for (unsigned j = 0; j + 1 < rounds - i; j++)
                if (lat[j] > lat[j+1]) { uint64_t t = lat[j]; lat[j] = lat[j+1]; lat[j+1] = t; }
            uint64_t sum = 0; for (unsigned i = 0; i < rounds; i++) sum += lat[i];
            printf("E2E_LAT side=0 msg=%zu rounds=%u rtt_p50_ns=%llu rtt_p99_ns=%llu rtt_mean_ns=%llu\n",
                   msg_bytes, rounds, (unsigned long long)lat[rounds/2],
                   (unsigned long long)lat[(rounds*99)/100], (unsigned long long)(sum/rounds));
        }
        free(lat);
        /* Throughput: A streams `rounds` messages back to back; B drains and
         * counts. Report A's send-side goodput; the pair transport bounds
         * outstanding sends via TX credits, so this is closed by the wire. */
        uint64_t t0 = now_ns();
        if (SIDE == 0) { for (unsigned i = 0; i < rounds; i++) { BUILD(i + 2, msg_bytes); SEND(msg_bytes); } }
        else { for (unsigned i = 0; i < rounds; i++) RECV(msg_bytes); }
        uint64_t dt = now_ns() - t0;
        printf("E2E_BW side=%u msg=%zu rounds=%u ns=%llu %s_MBps=%.1f\n", SIDE, msg_bytes, rounds,
               (unsigned long long)dt, SIDE == 0 ? "tx" : "rx",
               dt ? (double)rounds * msg_bytes / 1e6 / ((double)dt / 1e9) : 0.0);
    }
    fflush(stdout);

    /* Retire: the client's unregister ends the pair; both drain to teardown. */
    free(tx); free(rx);
    if (SIDE == 0) CHECK(peer_manager_notify_unregister(m, PODS[0]) == 1);
    deadline = now_ns() + 30ull * 1000000000ull;
    while (ch->state != DMESH_PEER_CLOSED && now_ns() < deadline) {
        CHECK(ops->progress(rt) >= 0);
        CHECK(peer_manager_progress(m, now_ns()) >= 0);
        usleep(2000);
    }
    peer_manager_free(m);
    if (ipc) peer_crypto_ipc_free(ipc);
    dmesh_peer_table_fini(&table);
    peer_pair_transport_free(rt);
    ibv_close_device(verbs);
    printf("E2E_DONE side=%u channel_closed=%d\n", SIDE, ch->state == DMESH_PEER_CLOSED);
    return 0;
}
