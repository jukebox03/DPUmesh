/* Real pair channel + worker/control mailboxes; injectable verbs completion
 * failures. This test does not claim hardware encryption or peer authorization. */
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "doca/peer_pair_transport.h"

struct fixture {
    struct peer_pair_transport *rt;
    struct dmesh_peer_table table;
    struct dmesh_peer_channel *ch;
    struct peer_pair_command cmd;
    uint8_t key[32];
    struct dmesh_peer_registration reg;
    uint64_t now;
    int prepared, enabled, closed, freed, sends, dma, drained, close_fail;
    int revoked, placed, allow, authorizations;
};
static uint64_t now(void *v) { return ((struct fixture *)v)->now; }
static int binding(void *v, const char *n, const uint8_t **k, uint32_t *ip, uint16_t *p)
{ struct fixture *f=v; if (strcmp(n,"node-b")) return 0; *k=f->key; *ip=1; *p=7000; return 1; }
static int placed(void *v, const char *u, const char *n)
{ return ((struct fixture *)v)->placed && ((!strcmp(u,"pod-a") && !strcmp(n,"node-a")) || (!strcmp(u,"pod-b") && !strcmp(n,"node-b"))); }
static int registration(void *v, const char *u, struct dmesh_peer_registration *r)
{ if (strcmp(u,"pod-a")) return 0; *r=((struct fixture *)v)->reg; return 1; }
static int authorize(void *v, const struct dmesh_peer_pair *p, const struct dmesh_peer_stream_open *in)
{ struct fixture *f=v; assert(!strcmp(p->local_uid,"pod-a")); f->authorizations++; return f->allow && in->dst_port==8080; }
static const struct dmesh_peer_ops AUTH = {.node_binding=binding,.pod_on_node=placed,.local_registration=registration,.pair_authorize=authorize,.now_ns=now};
static int prepare(void *v, void **out, struct peer_verbs_endpoint *e)
{ struct fixture *f=v; f->prepared++; *out=f; *e=(struct peer_verbs_endpoint){.qpn=100,.psn=20,.mtu=1024}; e->gid[15]=1; return 0; }
static int activate(void *v, const struct peer_verbs_endpoint *e, int hw)
{ struct fixture *f=v; assert(hw==1 && e->qpn==101 && e->psn==21 && e->gid[15]==2); f->enabled++; return 0; }
static int drained(void *v) { return ((struct fixture *)v)->drained; }
static int close_wire(void *v) { struct fixture *f=v; if(f->close_fail) return -1; f->closed++; return 0; }
static void unexpected_close(void *v) { (void)v; assert(0); }
static int dma(void *v, struct dmesh_peer_channel *c, uint32_t inc)
{ struct fixture *f=v; assert(c==f->ch && c->incarnation==inc); return f->dma; }
static int wire_progress(void *v, void **a, int cap, int *count)
{ (void)v; (void)a; (void)cap; *count=0; return 0; }
static int wire_fault(void *v) { (void)v; return 0; }
static int wire_send(void *v, const void *b, size_t n)
{ struct fixture *f=v; assert(f->enabled && b && n); f->sends++; return 1; }
static long wire_recv(void *v, void *b, size_t n) { (void)v; (void)b; (void)n; return 0; }
static int wire_epfd(void *v) { (void)v; return -1; }
static void wire_free(void *v) { ((struct fixture *)v)->freed++; }
static const struct peer_wire_ops WIRE = {.progress=wire_progress,.faulted=wire_fault,.send_msg=wire_send,.recv_msg=wire_recv,.epfd=wire_epfd,.close=unexpected_close,.ctx_free=wire_free};
static void progress(struct fixture *f) { assert(peer_pair_transport_ops()->progress(f->rt)>=0); }
static struct peer_pair_event event(struct fixture *f)
{ struct peer_pair_event e; assert(peer_pair_manager_poll(f->rt,&e)==1); return e; }
static int response(struct fixture *f, uint64_t op)
{
    struct peer_pair_event e;
    while(peer_pair_manager_poll(f->rt,&e)==1) {
        if(e.type==PEER_PAIR_REVOKE) { f->revoked++; continue; }
        assert(e.type==PEER_PAIR_COMPLETED && e.operation==op); return e.status;
    }
    return 99;
}
static int command(struct fixture *f, enum peer_pair_command_type t)
{ f->cmd.type=t; f->cmd.operation++; assert(peer_pair_manager_submit(f->rt,&f->cmd)==1); progress(f); return response(f,f->cmd.operation); }
static struct fixture *init(void)
{
    struct fixture *f=calloc(1,sizeof(*f)); assert(f);
    f->now=1; f->dma=f->drained=f->placed=f->allow=1; memset(f->key,3,32);
    f->reg.daemon[0]=1; f->reg.nonce[0]=2; f->reg.generation=7;
    struct peer_pair_transport_config cfg={.worker=0,.connections=1,.setup_timeout_ns=100,.now_ns=now,.now_ctx=f,.dma_fenced=dma,.dma_ctx=f};
    struct peer_pair_driver d={.wire=&WIRE,.ctx=f,.prepare=prepare,.activate=activate,.send_drained=drained,.close=close_wire};
    assert(!peer_pair_transport_new_driver(&cfg,&d,&f->rt));
    dmesh_peer_table_init(&f->table,"node-a",f->key,peer_pair_transport_ops(),f->rt,&AUTH,f);
    peer_pair_transport_attach(f->rt,&f->table);
    struct dmesh_peer_stream_open in={.src_generation=7,.dst_port=8080};
    strcpy(in.src_pod_uid,"pod-a"); strcpy(in.dst_pod_uid,"pod-b"); strcpy(in.src_service_key,"ns/service");
    enum dmesh_peer_refusal why;
    f->ch=dmesh_peer_open_stream(&f->table,"node-b",&in,&why); assert(f->ch && !why);
    struct peer_pair_event e=event(f); assert(e.type==PEER_PAIR_OPEN && !f->prepared);
    f->cmd.ref=e.ref; f->cmd.offer=e.offer;
    f->cmd.offer.pair.remote=f->reg; f->cmd.offer.pair.remote.generation=8;
    f->cmd.offer.pair.association[0]=9;
    f->cmd.offer.pair.lane_id=f->cmd.offer.pair.lane_generation=1;
    for(unsigned i=0;i<3;i++) f->cmd.deadline[i]=1000;
    f->cmd.association=(struct peer_assoc_token){.manager=20,.generation=30,.slot=0};
    f->cmd.epoch=1; f->cmd.hardware_ready=1;
    f->cmd.lane=(struct peer_sec_lane){.id=1,.generation=1,.qpn={100,101},.psn={20,21},.mtu=1024};
    f->cmd.lane.gid[0][15]=1; f->cmd.lane.gid[1][15]=2;
    f->cmd.grant=(struct peer_assoc_grant){.token=f->cmd.association,.revision=1,.epoch=1,.pair=f->cmd.offer.pair,.lane=f->cmd.lane};
    for(unsigned i=0;i<3;i++) { f->cmd.grant.deadline[i]=1000; f->cmd.grant.lease_generation[i]=1; }
    return f;
}
static void ready(struct fixture *f)
{
    struct dmesh_peer_pair p;
    assert(peer_pair_transport_ops()->pair_ready(f->ch->conn,&p)==0);
    assert(!command(f,PEER_PAIR_PREPARE) && f->prepared==1 && !f->enabled);
    assert(!command(f,PEER_PAIR_ENABLE) && f->enabled==1 && !f->sends);
    assert(peer_pair_transport_ops()->pair_ready(f->ch->conn,&p)==0);
    assert(!command(f,PEER_PAIR_GRANT));
    assert(!dmesh_peer_channel_progress(&f->table,f->ch,1) && f->ch->state==DMESH_PEER_OPEN);
}
static void release_pair(struct fixture *f)
{
    /* Before ENABLE the worker has no association token. */
    if(!f->enabled) memset(&f->cmd.association,0,sizeof(f->cmd.association));
    assert(!command(f,PEER_PAIR_BLOCK));
    f->dma=1; f->close_fail=0; f->cmd.hardware_ready=1;
    assert(!command(f,PEER_PAIR_DESTROY));
    assert(!command(f,PEER_PAIR_FORGET));
    struct peer_pair_event e; while(peer_pair_manager_poll(f->rt,&e)==1) assert(e.type==PEER_PAIR_REVOKE);
}
static void retire(struct fixture *f)
{
    release_pair(f);
    dmesh_peer_table_fini(&f->table);
    assert(!peer_pair_transport_free(f->rt) && f->freed==1); free(f);
}
static void incoming_and_stale_reference(void)
{
    struct fixture *f=init(); ready(f);
    struct peer_pair_ref old=f->cmd.ref;
    release_pair(f);
    memset(&f->cmd.ref,0,sizeof(f->cmd.ref));
    strcpy(f->cmd.offer.intent.src_pod_uid,"pod-b");
    strcpy(f->cmd.offer.intent.dst_pod_uid,"pod-a");
    f->cmd.offer.intent.src_generation=8; f->cmd.offer.incarnation=42;
    f->cmd.type=PEER_PAIR_PREPARE; f->cmd.operation++;
    assert(peer_pair_manager_submit(f->rt,&f->cmd)==1); progress(f);
    struct peer_pair_event e=event(f);
    assert(!e.status && e.ref.generation>old.generation && e.ref.runtime==old.runtime);
    f->cmd.ref=e.ref;
    assert(!command(f,PEER_PAIR_ENABLE));
    assert(!command(f,PEER_PAIR_GRANT));
    assert(f->ch->incarnation==42 && f->ch->state==DMESH_PEER_OPEN && f->ch->retirement_refs==1);
    struct peer_pair_command stale=f->cmd; stale.ref=old; stale.operation++; stale.type=PEER_PAIR_BLOCK;
    assert(peer_pair_manager_submit(f->rt,&stale)==1); progress(f); e=event(f);
    assert(e.status==1 && f->ch->state==DMESH_PEER_OPEN);
    /* A live retirement pin must still allow idle reset to start teardown. */
    f->now=DMESH_CHANNEL_IDLE_NS+2; dmesh_peer_evict_idle(&f->table);
    assert(f->ch->state==DMESH_PEER_CLOSED && f->ch->in_use && f->ch->retirement_refs==1);
    retire(f);
}
static void barriers_and_rekey(void)
{
    struct fixture *f=init(); ready(f);
    f->drained=0; assert(command(f,PEER_PAIR_QUIESCE)==99);
    assert(!peer_pair_transport_ops()->pair_admit(f->ch->conn));
    uint64_t quiesce=f->cmd.operation;
    f->drained=1; f->dma=0; progress(f); assert(response(f,quiesce)==99);
    f->dma=1; progress(f); assert(response(f,quiesce)==0);
    f->cmd.grant.revision++; assert(command(f,PEER_PAIR_GRANT)==1);
    assert(!peer_pair_transport_ops()->pair_admit(f->ch->conn));
    f->cmd.epoch=2; assert(!command(f,PEER_PAIR_RESUME));
    assert(!peer_pair_transport_ops()->pair_admit(f->ch->conn));
    f->cmd.grant.epoch=2; assert(!command(f,PEER_PAIR_GRANT));
    assert(peer_pair_transport_ops()->pair_admit(f->ch->conn));
    assert(command(f,PEER_PAIR_GRANT)==1); /* replay cannot change ACTIVE */
    retire(f);
}
static void delayed_destroy(void)
{
    struct fixture *f=init(); ready(f); f->dma=0;
    assert(!command(f,PEER_PAIR_BLOCK));
    assert(f->ch->state==DMESH_PEER_CLOSED && !f->closed);
    assert(command(f,PEER_PAIR_DESTROY)==99 && f->closed==1);
    assert(f->ch->retirement_refs == 1);
    assert(dmesh_peer_table_fini(&f->table) == -1 && f->ch->in_use);
    uint64_t destroying=f->cmd.operation;
    progress(f); assert(response(f,destroying)==99);
    assert(peer_pair_transport_free(f->rt)==-1);
    f->dma=1; progress(f); assert(response(f,destroying)==0);
    retire(f); assert(1);
}
static void failures(void)
{
    struct fixture *f=init();
    f->allow=0; assert(command(f,PEER_PAIR_PREPARE)==-1 && !f->prepared); retire(f);
    f=init();
    assert(!command(f,PEER_PAIR_PREPARE)); f->cmd.hardware_ready=0;
    assert(command(f,PEER_PAIR_ENABLE)==-1 && !f->enabled); retire(f);
    f=init(); ready(f); assert(!command(f,PEER_PAIR_BLOCK)); f->close_fail=1;
    assert(command(f,PEER_PAIR_DESTROY)==-2 && !f->closed);
    assert(command(f,PEER_PAIR_FORGET)==-1); retire(f);
    f=init(); ready(f); f->reg.nonce[31]++; progress(f);
    assert(f->ch->state==DMESH_PEER_CLOSED && !f->closed); retire(f);
    f=init(); ready(f); f->now=1000; progress(f);
    assert(f->ch->state==DMESH_PEER_CLOSED && !f->closed); retire(f);
    f=init(); ready(f); f->cmd.grant.revision++; f->cmd.grant.deadline[0]++;
    assert(command(f,PEER_PAIR_GRANT)==-1); retire(f); /* renewal needs new generation */
}
static void backpressure(void)
{
    struct fixture *f=init();
    struct peer_pair_command stale={.type=PEER_PAIR_BLOCK,.operation=1,.ref={.runtime=1,.generation=1}};
    for(unsigned i=0;i<PEER_PAIR_MAILBOX_CAP;i++) assert(peer_pair_manager_submit(f->rt,&stale)==1);
    assert(peer_pair_manager_submit(f->rt,&stale)==0);
    progress(f); assert(peer_pair_transport_ops()->pending(f->rt));
    struct pollfd fd={.fd=peer_pair_manager_fd(f->rt),.events=POLLIN};
    for(unsigned i=0;i<PEER_PAIR_MAILBOX_CAP-1;i++) {
        assert(poll(&fd,1,0)==1); struct peer_pair_event e=event(f); assert(e.status==1);
    }
    progress(f); struct peer_pair_event e=event(f); assert(e.status==1);
    assert(poll(&fd,1,0)==0 && !f->prepared); retire(f);
}
static void preallocation_query(void)
{
    struct fixture *f=init();
    struct peer_pair_command q=f->cmd; q.type=PEER_PAIR_AUTHORIZE; q.operation=1;
    memset(&q.ref,0,sizeof(q.ref)); memset(&q.offer.pair.local,0,sizeof(q.offer.pair.local));
    strcpy(q.offer.intent.src_pod_uid,"pod-b"); strcpy(q.offer.intent.dst_pod_uid,"pod-a"); q.offer.intent.src_generation=8;
    assert(peer_pair_manager_submit(f->rt,&q)==1); progress(f);
    struct peer_pair_event e=event(f);
    assert(e.status==0 && e.command==PEER_PAIR_AUTHORIZE && !e.ref.runtime);
    assert(dmesh_peer_registration_equal(&e.offer.pair.local,&f->reg));
    assert(!f->prepared && !f->enabled);
    f->allow=0; q.operation++;
    assert(peer_pair_manager_submit(f->rt,&q)==1); progress(f); e=event(f);
    assert(e.status==-1 && !e.offer.node[0] && !f->prepared);
    retire(f);
}
struct threaded { struct fixture *f; atomic_int stop; };
static void *worker_main(void *v)
{
    struct threaded *t=v;
    while(!atomic_load_explicit(&t->stop,memory_order_acquire)) {
        progress(t->f); sched_yield();
    }
    return NULL;
}
static void concurrent_mailboxes(void)
{
    struct fixture *f=init(); struct threaded t={.f=f}; atomic_init(&t.stop,0);
    pthread_t worker; assert(!pthread_create(&worker,NULL,worker_main,&t));
    unsigned sent=0, received=0; const unsigned count=20000;
    struct timespec start, current; assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    struct peer_pair_command c={.type=PEER_PAIR_BLOCK,.ref={.runtime=UINT64_MAX,.generation=1}};
    struct pollfd fd={.fd=peer_pair_manager_fd(f->rt),.events=POLLIN};
    while(received<count) {
        if(sent<count) { c.operation=sent+1; if(peer_pair_manager_submit(f->rt,&c)==1) sent++; }
        struct peer_pair_event e;
        if(peer_pair_manager_poll(f->rt,&e)==1) {
            assert(e.type==PEER_PAIR_COMPLETED && e.status==1 && e.operation==++received);
        } else if(sent==count) (void)poll(&fd,1,1);
        assert(!clock_gettime(CLOCK_MONOTONIC,&current) && current.tv_sec-start.tv_sec<15);
    }
    atomic_store_explicit(&t.stop,1,memory_order_release); assert(!pthread_join(worker,NULL));
    assert(sent==count && !f->prepared); retire(f);
}
int main(void)
{
    barriers_and_rekey(); delayed_destroy(); failures(); backpressure(); preallocation_query(); incoming_and_stale_reference();
    concurrent_mailboxes();
    puts("peer_pair_transport_test: PASS (activation, rekey, DMA delay, quarantine, identity/lease expiry, mailbox pressure)");
    return 0;
}
