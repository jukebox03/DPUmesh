/* Exercise the actual RDMA slot machine with deterministic verbs completions.
 * No RDMA device is required; post addresses, lkeys and teardown order are real
 * assertions at the provider seam, not a second implementation of the pool. */
#include <assert.h>
#include <time.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
static int send_fail, recv_fail, posts, reposts, teardown;
static struct ibv_wc completions[64];
static int completion_count;
static uintptr_t last_addr;
static uint32_t last_lkey;
static uint64_t last_wr;
static int post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad)
{
    (void)qp; (void)bad;
    assert(wr->num_sge == 1 && wr->opcode == IBV_WR_SEND);
    assert(wr->send_flags == IBV_SEND_SIGNALED);
    posts++; last_addr = wr->sg_list->addr; last_lkey = wr->sg_list->lkey;
    last_wr = wr->wr_id;
    return send_fail;
}
static int post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad)
{
    (void)qp; (void)wr; (void)bad; reposts++; return recv_fail;
}
static int poll_cq(struct ibv_cq *cq, int max, struct ibv_wc *out)
{
    (void)cq;
    int n = completion_count < max ? completion_count : max;
    for (int i = 0; i < n; i++) out[i] = completions[i];
    completion_count = 0; return n;
}
static void destroy_qp(struct rdma_cm_id *id) { assert(teardown++ == 0); id->qp = NULL; }
static int destroy_id(struct rdma_cm_id *id) { (void)id; assert(teardown++ == 1); return 0; }
static int dereg_mr(struct ibv_mr *mr) { (void)mr; assert(teardown++ == 2); return 0; }
#define rdma_destroy_qp destroy_qp
#define rdma_destroy_id destroy_id
#define ibv_dereg_mr dereg_mr
#include "../doca/peer_wire_rdma.c"

static struct rdma_ctx ctx;
static struct ibv_context verbs;
static struct ibv_qp qp;
static struct rdma_cm_id id;
static struct ibv_mr mr;
static struct ibv_cq cq;
static struct rdma_conn *setup(void)
{
    memset(&ctx, 0, sizeof(ctx)); memset(&verbs, 0, sizeof(verbs));
    verbs.ops.post_send = post_send; verbs.ops.post_recv = post_recv;
    verbs.ops.poll_cq = poll_cq; qp.context = &verbs; cq.context = &verbs;
    ctx.cq = &cq; id.qp = &qp; mr.lkey = 12345;
    struct rdma_conn *c = rdma_slot_take(&ctx);
    assert(c); c->state = RC_READY; c->id = &id; c->mr = &mr;
    send_fail = recv_fail = posts = reposts = completion_count = teardown = 0;
    return c;
}
static void finish(struct rdma_conn *c) { rdma_release(c); assert(teardown == 3); }
static void complete(struct rdma_conn *c, int rx, unsigned slot, unsigned len)
{
    completions[0] = (struct ibv_wc) { .wr_id = wr_pack(rx, c->epoch, c->index, slot),
        .status = IBV_WC_SUCCESS, .byte_len = len };
    completion_count = 1; assert(rdma_poll_cq(&ctx) == 1);
}
static void ready_rx(struct rdma_conn *c, unsigned slot, const char *s)
{
    assert(rdma_post_recv(c, slot) == 0);
    memcpy(recv_slot(c, slot), s, strlen(s)); complete(c, 1, slot, strlen(s));
}
static void tx_contract(void)
{
    struct rdma_conn *c = setup();
    struct peer_wire_tx_lease l, extra;
    assert(rdma_tx_reserve(c, &l) == 1 && l.cap == PEER_WIRE_MSG_MAX);
    struct peer_wire_tx_lease old = l;
    assert(rdma_tx_reserve(c, &extra) == 0 && !extra.data);
    assert(rdma_send_msg(c, "later", 5) == 0);
    struct peer_wire_tx_lease bad = l; bad.token.kind = PEER_WIRE_RX;
    assert(rdma_tx_cancel(c, &bad) == -1 && c->tx_leased);
    bad = l; bad.token.conn_epoch++;
    assert(rdma_tx_commit(c, &bad, 1) == -1 && c->tx_leased);
    assert(rdma_tx_cancel(c, &l) == 0 && !l.data);
    assert(rdma_tx_reserve(c, &l) == 1);
    assert(rdma_tx_cancel(c, &old) == -1 && c->tx_leased);
    uintptr_t address = (uintptr_t)l.data; memcpy(l.data, "direct", 6);
    unsigned slot = l.token.slot;
    assert(rdma_tx_commit(c, &l, 6) == 1 && !l.data);
    assert(last_addr == address && last_lkey == mr.lkey);
    assert(c->send_busy[slot] == TX_POSTED);
    assert(rdma_tx_commit(c, &old, 6) == -1);
    for (unsigned i = 1; i < RDMA_SEND_RING; i++) assert(rdma_send_msg(c, "x", 1) == 1);
    assert(rdma_tx_reserve(c, &l) == 0 && !l.data);
    assert(memcmp(send_slot(c, slot), "direct", 6) == 0);
    complete(c, 0, slot, 0);
    assert(rdma_tx_reserve(c, &l) == 1);
    assert(rdma_tx_cancel(c, &l) == 0);
    // A completion from an earlier connection cannot release the current slot.
    assert(rdma_tx_reserve(c, &l) == 1);
    completions[0] = (struct ibv_wc) { .wr_id = wr_pack(0, c->epoch - 1, c->index, l.token.slot), .status = IBV_WC_SUCCESS };
    completion_count = 1; assert(rdma_poll_cq(&ctx) == 0); assert(c->tx_leased);
    assert(rdma_tx_cancel(c, &l) == 0);
    finish(c);
}
static void rx_contract(void)
{
    struct rdma_conn *c = setup();
    ready_rx(c, 0, "first"); ready_rx(c, 1, "second");
    struct peer_wire_rx_lease l, next;
    assert(rdma_rx_acquire(c, &l) == 1 && l.data == recv_slot(c, 0));
    assert(l.len == 5 && memcmp(l.data, "first", 5) == 0);
    assert(c->rq_count == 1 && c->recv_state[0] == RX_LEASED);
    int was = reposts;
    assert(rdma_rx_acquire(c, &next) == 0 && !next.data);
    char buf[32]; assert(rdma_recv_msg(c, buf, sizeof(buf)) == 0);
    assert(reposts == was);
    struct peer_wire_rx_lease old = l, bad = l; bad.len++;
    assert(rdma_rx_release(c, &bad) == -1 && c->rx_leased);
    assert(rdma_rx_release(c, &l) == 0 && !l.data && reposts == was + 1);
    assert(rdma_rx_release(c, &old) == -1);
    assert(rdma_recv_msg(c, buf, sizeof(buf)) == 6 && memcmp(buf, "second", 6) == 0);
    ready_rx(c, 0, "reuse"); assert(rdma_rx_acquire(c, &l) == 1);
    assert(rdma_rx_release(c, &old) == -1 && c->rx_leased);
    recv_fail = 1; assert(rdma_rx_release(c, &l) == -1 && !l.data);
    assert(c->state == RC_DEAD && !c->rx_leased);
    finish(c);
}
static void rx_full_pool(void)
{
    struct rdma_conn *c = setup();
    for (unsigned i = 0; i < RDMA_RECV_RING; i++) ready_rx(c, i, "queued");
    struct peer_wire_rx_lease l;
    for (unsigned i = 0; i < RDMA_RECV_RING; i++) {
        assert(rdma_rx_acquire(c, &l) == 1 && l.token.slot == i);
        assert(c->rq_count == RDMA_RECV_RING - i - 1);
        unsigned posted = 0, ready = 0, leased = 0;
        for (unsigned j = 0; j < RDMA_RECV_RING; j++) {
            posted += c->recv_state[j] == RX_POSTED;
            ready += c->recv_state[j] == RX_READY;
            leased += c->recv_state[j] == RX_LEASED;
        }
        assert(posted == i && ready == RDMA_RECV_RING - i - 1 && leased == 1);
        assert(rdma_rx_release(c, &l) == 0);
    }
    assert(rdma_rx_acquire(c, &l) == 0 && !l.data);
    finish(c);
}
static void failures(void)
{
    struct peer_wire_tx_lease l;
    struct rdma_conn *c = setup(); assert(rdma_tx_reserve(c, &l) == 1);
    send_fail = 1; assert(rdma_tx_commit(c, &l, 1) == -1 && !l.data);
    assert(c->state == RC_DEAD && !c->tx_leased); finish(c);
    c = setup(); assert(rdma_tx_reserve(c, &l) == 1);
    assert(rdma_tx_commit(c, &l, PEER_WIRE_MSG_MAX + 1) == -1 && !l.data && posts == 0); finish(c);
    c = setup(); c->tx_generation[0] = UINT64_MAX;
    assert(rdma_tx_reserve(c, &l) == -1 && !l.data); finish(c);
    c = setup(); ready_rx(c, 0, "too-long"); char tiny;
    assert(rdma_recv_msg(c, &tiny, 1) == -1 && !c->rx_leased); finish(c);
    c = setup(); assert(rdma_tx_reserve(c, &l) == 1);
    complete(c, 0, l.token.slot, 0); /* Unposted slot cannot receive a SEND CQ. */
    assert(c->state == RC_DEAD); assert(rdma_tx_cancel(c, &l) == -1 && !l.data); finish(c);
    c = setup(); assert(rdma_tx_reserve(c, &l) == 1);
    struct peer_wire_tx_lease stale = l;
    assert(rdma_tx_cancel(c, &l) == 0); finish(c);
    c = setup(); assert(rdma_tx_reserve(c, &l) == 1);
    assert(rdma_tx_cancel(c, &stale) == -1 && c->tx_leased);
    assert(rdma_tx_cancel(c, &l) == 0); finish(c);
    c = setup(); ready_rx(c, 0, "wrap"); c->rx_generation[0] = UINT64_MAX;
    struct peer_wire_rx_lease rx;
    assert(rdma_rx_acquire(c, &rx) == -1 && !rx.data); finish(c);
    struct peer_wire_ops ops = RDMA_OPS;
    assert(peer_wire_ops_valid(&ops)); ops.tx_commit = NULL; assert(!peer_wire_ops_valid(&ops));
    ops = RDMA_OPS; ops.rx_release = NULL; assert(!peer_wire_ops_valid(&ops));
}
static volatile uint64_t sink;
static uint64_t nanos(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
static void microbench(void)
{
    const unsigned sizes[] = {64, 1024, 8192, 65536};
    uint8_t *source = malloc(PEER_WIRE_MSG_MAX);
    puts("size,direction,mode,rep,iterations,ns_per_message");
    for (unsigned z = 0; z < 4; z++) {
        unsigned len = sizes[z], loops = (256u * 1024u * 1024u) / len;
        for (int rx = 0; rx < 2; rx++) for (int direct = 0; direct < 2; direct++) {
            struct rdma_conn *c = setup();
            memset(recv_slot(c, 0), 7, len);
            for (int rep = 0; rep < 4; rep++) {
                uint64_t start = nanos();
                for (unsigned i = 0; i < loops; i++) {
                    if (!rx) {
                        if (direct) {
                            struct peer_wire_tx_lease l; assert(rdma_tx_reserve(c, &l) == 1);
                            memset(l.data, (uint8_t)i, len);
                            __asm__ __volatile__("" ::: "memory");
                            assert(rdma_tx_commit(c, &l, len) == 1);
                        } else {
                            memset(source, (uint8_t)i, len);
                            __asm__ __volatile__("" ::: "memory");
                            assert(rdma_send_msg(c, source, len) == 1);
                        }
                        sink += *(uint8_t *)last_addr;
                        complete(c, 0, (unsigned)(last_wr & 0xffff), 0);
                    } else {
                        c->recv_state[0] = RX_POSTED;
                        complete(c, 1, 0, len);
                        if (direct) {
                            struct peer_wire_rx_lease l; assert(rdma_rx_acquire(c, &l) == 1);
                            sink += l.data[0] + l.data[len - 1];
                            __asm__ __volatile__("" ::: "memory");
                            assert(rdma_rx_release(c, &l) == 0);
                        } else {
                            assert(rdma_recv_msg(c, source, len) == len);
                            sink += source[0] + source[len - 1];
                            __asm__ __volatile__("" ::: "memory");
                        }
                    }
                }
                double ns = (double)(nanos() - start) / loops;
                if (rep) printf("%u,%s,%s,%d,%u,%.2f\n", len, rx ? "rx" : "tx",
                    direct ? "lease" : "copy", rep, loops, ns);
            }
            finish(c);
        }
    }
    free(source);
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) { microbench(); return 0; }
    tx_contract(); rx_contract(); rx_full_pool(); failures(); puts("peer_wire_lease_test: PASS");
}
