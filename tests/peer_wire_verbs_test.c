/* Real manual-QP code with a deterministic verbs provider. No device is opened. */
#include <assert.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <string.h>
static struct ibv_context device;
static struct ibv_qp qp;
static struct ibv_mr mr;
static struct ibv_wc completion;
static int init_count, rtr_count, rts_count, receives, sends, destroyed, deregistered;
static int fail_rts, fail_destroy, fail_dereg, completion_ready;
static uint64_t send_id;
static struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
    (void)pd;
    assert(attr->qp_type == IBV_QPT_RC && attr->cap.max_send_wr == 16 && attr->cap.max_recv_wr == 32);
    qp.context = &device; qp.qp_num = 101; return &qp;
}
static int modify_qp(struct ibv_qp *q, struct ibv_qp_attr *a, int mask)
{
    assert(q == &qp && (mask & IBV_QP_STATE));
    if (a->qp_state == IBV_QPS_INIT) { assert(a->qp_access_flags == 0); init_count++; }
    else if (a->qp_state == IBV_QPS_RTR) {
        assert(receives == 32 && a->dest_qp_num == 202 && a->rq_psn == 303);
        assert(a->max_dest_rd_atomic == 0 && a->ah_attr.is_global && a->ah_attr.grh.sgid_index == 1);
        rtr_count++;
    } else {
        assert(a->qp_state == IBV_QPS_RTS && rtr_count == 1 && a->max_rd_atomic == 0);
        rts_count++; if (fail_rts) return EIO;
    }
    return 0;
}
static struct ibv_mr *reg_mr(struct ibv_pd *pd, void *addr, size_t len, int access)
{
    (void)pd; assert(len == 48u * 73728u && access == IBV_ACCESS_LOCAL_WRITE);
    mr.addr = addr; mr.lkey = 999; return &mr;
}
static int destroy_qp(struct ibv_qp *q)
{ assert(q == &qp); if (fail_destroy) return EBUSY; destroyed++; return 0; }
static int dereg_mr(struct ibv_mr *m)
{ assert(m == &mr && destroyed == 1); if (fail_dereg) return EBUSY; deregistered++; return 0; }
static int recv_wr(struct ibv_qp *q, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad)
{ (void)q; (void)bad; assert(wr->num_sge == 1); receives++; return 0; }
static int send_wr(struct ibv_qp *q, struct ibv_send_wr *wr, struct ibv_send_wr **bad)
{
    (void)q; (void)bad; assert(rts_count && wr->opcode == IBV_WR_SEND && wr->sg_list->lkey == 999);
    sends++; send_id = wr->wr_id; return 0;
}
static int poll_cq(struct ibv_cq *cq, int n, struct ibv_wc *out)
{
    (void)cq; assert(n > 0);
    if (!completion_ready) return 0;
    *out = completion; completion_ready = 0; return 1;
}
#define ibv_create_qp create_qp
#define ibv_modify_qp modify_qp
#undef ibv_reg_mr
#define ibv_reg_mr reg_mr
#define ibv_destroy_qp destroy_qp
#define ibv_dereg_mr dereg_mr
#include "../doca/peer_wire_rdma.c"

static void test_manual_lifecycle(int fail_activation)
{
    struct rdma_ctx *ctx = calloc(1, sizeof(*ctx)); assert(ctx);
    struct ibv_pd pd = {0}; struct ibv_cq cq = {0}; cq.context = &device;
    memset(&device, 0, sizeof(device)); device.ops.post_recv = recv_wr;
    device.ops.post_send = send_wr; device.ops.poll_cq = poll_cq;
    init_count = rtr_count = rts_count = receives = sends = destroyed = deregistered = 0;
    fail_rts = fail_activation; fail_destroy = fail_dereg = completion_ready = 0;
    ctx->manual = 1; ctx->verbs = &device; ctx->pd = &pd; ctx->cq = &cq;
    ctx->verbs_port = 1; ctx->gid_index = 1; ctx->mtu = 1024;
    ctx->gid[10] = ctx->gid[11] = 0xff; ctx->gid[15] = 1;
    struct peer_verbs_endpoint local, remote = {.qpn = 202, .psn = 303, .mtu = 1024};
    memcpy(remote.gid, ctx->gid, 16); remote.gid[15] = 2;
    void *wire = NULL;
    assert(!peer_wire_verbs_prepare(ctx, &wire, &local));
    assert(local.qpn == 101 && local.psn <= 0xffffff && init_count == 1 && receives == 32);
    assert(!RDMA_OPS.established(wire));
    assert(RDMA_OPS.send_msg(wire, "data", 4) == 0 && sends == 0);
    assert(peer_wire_verbs_activate(wire, &remote, 0) < 0 && rtr_count == 0);
    remote.qpn = 0x1000000;
    assert(peer_wire_verbs_activate(wire, &remote, 1) < 0 && rtr_count == 0); remote.qpn = 202;
    if (fail_activation) {
        assert(peer_wire_verbs_activate(wire, &remote, 1) < 0);
        assert(RDMA_OPS.faulted(wire));
    } else {
        assert(!peer_wire_verbs_activate(wire, &remote, 1)); assert(RDMA_OPS.established(wire));
        assert(peer_wire_verbs_activate(wire, &remote, 1) < 0); /* no duplicate transitions */
        assert(peer_wire_verbs_send_drained(wire) == 1);
        assert(RDMA_OPS.send_msg(wire, "data", 4) == 1);
        assert(peer_wire_verbs_send_drained(wire) == 0);
        completion = (struct ibv_wc){.wr_id = send_id, .status = IBV_WC_SUCCESS}; completion_ready = 1;
        assert(rdma_poll_cq(ctx) == 1); assert(peer_wire_verbs_send_drained(wire) == 1);
    }
    fail_destroy = 1;
    assert(peer_wire_verbs_close(wire) < 0 && !destroyed && !deregistered);
    struct rdma_conn *c = wire; assert(c->in_use && c->buf && c->mr && c->manual_qp);
    fail_destroy = 0; fail_dereg = 1;
    assert(peer_wire_verbs_close(wire) < 0 && destroyed == 1 && !deregistered);
    assert(c->in_use && c->buf && c->mr && !c->manual_qp);
    fail_dereg = 0;
    assert(!peer_wire_verbs_close(wire) && destroyed == 1 && deregistered == 1);
    assert(!c->in_use); free(ctx);
}
int main(void)
{
    test_manual_lifecycle(0); test_manual_lifecycle(1);
    puts("peer_wire_verbs_test: PASS (INIT gate, RTR/RTS, SEND drain, failed teardown retains memory)"); return 0;
}
