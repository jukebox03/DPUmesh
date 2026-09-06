/* Provider-seam CPU measurement, not a NIC/fabric bandwidth benchmark.
 * Build against the preserved original implementation or the changed one.
 * Both execute the real send/recv/completion functions with mock verbs. */
#include <assert.h>
#include <time.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#ifndef PEER_RDMA_SOURCE
#define PEER_RDMA_SOURCE "../../../../doca/peer_wire_rdma.c"
#define HAVE_LEASES 1
#endif
#include PEER_RDMA_SOURCE
static struct ibv_wc completion;
static int queued;
static uintptr_t posted_addr;
static uint64_t posted_wr;
static volatile uint64_t sink;
static int send_mock(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad)
{
    (void)qp; (void)bad; posted_addr = wr->sg_list->addr; posted_wr = wr->wr_id; return 0;
}
static int recv_mock(struct ibv_qp *qp, struct ibv_recv_wr *wr, struct ibv_recv_wr **bad)
{ (void)qp; (void)wr; (void)bad; return 0; }
static int poll_mock(struct ibv_cq *cq, int max, struct ibv_wc *wc)
{ (void)cq; (void)max; if (!queued) return 0; *wc = completion; queued = 0; return 1; }
static uint64_t nanos(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static void complete_mock(struct rdma_ctx *ctx, uint64_t wr, unsigned len)
{
    completion = (struct ibv_wc){.wr_id=wr,.byte_len=len,.status=IBV_WC_SUCCESS};
    queued=1; assert(rdma_poll_cq(ctx)==1);
}
int main(void)
{
    static struct rdma_ctx ctx;
    static struct ibv_context verbs;
    static struct ibv_qp qp;
    static struct ibv_cq cq;
    static struct ibv_mr mr;
    static struct rdma_cm_id id;
    verbs.ops.post_send=send_mock; verbs.ops.post_recv=recv_mock; verbs.ops.poll_cq=poll_mock;
    qp.context=&verbs; cq.context=&verbs; ctx.cq=&cq; id.qp=&qp; mr.lkey=7;
    struct rdma_conn *c=rdma_slot_take(&ctx); assert(c);
    c->state=RC_READY; c->id=&id; c->mr=&mr;
    unsigned sizes[]={64,1024,8192,65536};
    uint8_t *source=malloc(PEER_WIRE_MSG_MAX); assert(source);
    memset(recv_slot(c,0),7,PEER_WIRE_MSG_MAX);
    assert(rdma_post_recv(c,0)==0);
    puts("size,direction,mode,rep,iterations,ns_per_message");
    for(unsigned z=0;z<4;z++) for(int rx=0;rx<2;rx++) {
        unsigned len=sizes[z], loops=(128u*1024u*1024u)/len;
#ifdef HAVE_LEASES
        int modes=2;
#else
        int modes=1;
#endif
        for(int direct=0;direct<modes;direct++) for(int rep=0;rep<4;rep++) {
            uint64_t begin=nanos();
            for(unsigned i=0;i<loops;i++) {
                if(!rx) {
#ifdef HAVE_LEASES
                    if(direct) {
                        struct peer_wire_tx_lease l; assert(rdma_tx_reserve(c,&l)==1);
                        memset(l.data,(uint8_t)i,len); __asm__ __volatile__("" ::: "memory");
                        assert(rdma_tx_commit(c,&l,len)==1);
                    } else
#endif
                    {
                        memset(source,(uint8_t)i,len); __asm__ __volatile__("" ::: "memory");
                        assert(rdma_send_msg(c,source,len)==1);
                    }
                    sink+=*(uint8_t *)posted_addr;
                    complete_mock(&ctx,posted_wr,0);
                } else {
                    complete_mock(&ctx,wr_pack(1,c->epoch,c->index,0),len);
#ifdef HAVE_LEASES
                    if(direct) {
                        struct peer_wire_rx_lease l; assert(rdma_rx_acquire(c,&l)==1);
                        sink+=l.data[0]+l.data[len-1]; __asm__ __volatile__("" ::: "memory");
                        assert(rdma_rx_release(c,&l)==0);
                    } else
#endif
                    {
                        assert(rdma_recv_msg(c,source,len)==len);
                        sink+=source[0]+source[len-1]; __asm__ __volatile__("" ::: "memory");
                    }
                }
            }
            double ns=(double)(nanos()-begin)/loops;
            if(rep) printf("%u,%s,%s,%d,%u,%.2f\n",len,rx?"rx":"tx",direct?"lease":"copy",rep,loops,ns);
        }
    }
    free(source); free(c->buf);
}
