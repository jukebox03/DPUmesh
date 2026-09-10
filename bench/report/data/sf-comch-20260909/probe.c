#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <doca_dev.h>
#include <doca_comch.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_error.h>

#define LIMIT 200
struct payload { uint32_t sf, seq; uint8_t pad[56]; };
struct endpoint {
    unsigned sf, ready, pending, sent, received;
    struct doca_dev *dev;
    struct doca_dev_rep *rep;
    struct doca_comch_server *server;
    struct doca_comch_client *client;
    struct doca_ctx *ctx;
    struct doca_comch_connection *conn;
    struct payload msg;
};
static struct endpoint ep[LIMIT];
static int is_server, errors, shutting_down;
static volatile sig_atomic_t interrupted;
static struct doca_pe *pe;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void signal_cb(int s) { (void)s; interrupted=1; }
static void check(doca_error_t e, const char *expr, int line) {
    if(e!=DOCA_SUCCESS) { fprintf(stderr,"FAIL line=%d call=%s error=%s\n",line,expr,doca_error_get_descr(e)); exit(2); }
}
#define OK(x) check((x),#x,__LINE__)
static void sent(struct doca_comch_task_send *task, union doca_data data, union doca_data ctxdata) {
    (void)data; struct endpoint *e=ctxdata.ptr;
    e->pending=0;
    if(doca_task_get_status(doca_comch_task_send_as_task(task))!=DOCA_SUCCESS && !shutting_down) errors++;
    doca_task_free(doca_comch_task_send_as_task(task));
}
static void transmit(struct endpoint *e) {
    struct doca_comch_task_send *task=NULL;
    doca_error_t r=is_server ? doca_comch_server_task_send_alloc_init(e->server,e->conn,&e->msg,sizeof(e->msg),&task)
                            : doca_comch_client_task_send_alloc_init(e->client,e->conn,&e->msg,sizeof(e->msg),&task);
    if(r!=DOCA_SUCCESS) { errors++; return; }
    r=doca_task_submit(doca_comch_task_send_as_task(task));
    if(r!=DOCA_SUCCESS) { errors++; doca_task_free(doca_comch_task_send_as_task(task)); return; }
    e->pending=1; e->sent++;
}
static void received(struct doca_comch_event_msg_recv *ev,uint8_t *buf,uint32_t size,struct doca_comch_connection *conn) {
    (void)ev; union doca_data data=doca_comch_connection_get_user_data(conn);
    struct endpoint *e=data.ptr;
    struct payload p;
    if(!e || size!=sizeof(p)) { errors++; return; }
    memcpy(&p,buf,sizeof(p));
    if(p.sf!=e->sf || p.seq!=e->received+1) { errors++; return; }
    e->received++;
    if(is_server) {
        if(e->pending) { errors++; return; }
        e->msg=p; transmit(e);
    }
}
static void connected(struct doca_comch_event_connection_status_changed *ev,struct doca_comch_connection *conn,uint8_t success) {
    (void)ev;
    union doca_data data;
    OK(doca_ctx_get_user_data(doca_comch_server_as_ctx(doca_comch_server_get_server_ctx(conn)),&data));
    struct endpoint *e=data.ptr;
    if(!success || e->ready) { errors++; return; }
    e->conn=conn; e->ready=1;
    OK(doca_comch_connection_set_user_data(conn,data));
}
static void disconnected(struct doca_comch_event_connection_status_changed *ev,struct doca_comch_connection *conn,uint8_t success) {
    (void)ev; (void)success;
    union doca_data data=doca_comch_connection_get_user_data(conn);
    struct endpoint *e=data.ptr;
    if(e) { e->ready=0; e->conn=NULL; }
}
static void state_changed(union doca_data data,struct doca_ctx *ctx,enum doca_ctx_states old,enum doca_ctx_states next) {
    (void)ctx; (void)old;
    struct endpoint *e=data.ptr;
    if(!is_server && next==DOCA_CTX_STATE_RUNNING) {
        OK(doca_comch_client_get_connection(e->client,&e->conn));
        OK(doca_comch_connection_set_user_data(e->conn,data)); e->ready=1;
    }
    if(!is_server && next==DOCA_CTX_STATE_IDLE) e->ready=0;
}
static int host_sf(struct doca_devinfo *d) {
    uint32_t aux;
    if(doca_devinfo_get_sf_index(d,&aux)!=DOCA_SUCCESS) return -1;
    char path[200]; unsigned sf;
    snprintf(path,sizeof(path),"/sys/bus/auxiliary/devices/mlx5_core.sf.%u/sfnum",aux);
    FILE *f=fopen(path,"r"); if(!f) return -1;
    int n=fscanf(f,"%u",&sf); fclose(f); return n==1?(int)sf:-1;
}
int main(int argc,char **argv) {
    if(argc!=4) { fprintf(stderr,"usage: probe server|client COUNT HOLD_SECONDS\n"); return 1; }
    is_server=!strcmp(argv[1],"server"); unsigned count=strtoul(argv[2],NULL,10); double hold=strtod(argv[3],NULL);
    if(count<1 || count>LIMIT || hold<1) return 1;
    setvbuf(stdout,NULL,_IOLBF,0); signal(SIGTERM,signal_cb); signal(SIGINT,signal_cb);
    struct doca_devinfo **list; uint32_t n;
    struct doca_dev *server_dev=NULL;
    struct doca_devinfo_rep **reps=NULL; uint32_t nr=0;
    OK(doca_pe_create(&pe)); OK(doca_devinfo_create_list(&list,&n));
    if(is_server) {
        for(uint32_t i=0;i<n;i++) { char pci[DOCA_DEVINFO_PCI_ADDR_SIZE]={0};
            doca_devinfo_get_pci_addr_str(list[i],pci);
            if(!strcmp(pci,"0000:03:00.0")) { OK(doca_dev_open(list[i],&server_dev)); break; }
        }
        if(!server_dev) { fprintf(stderr,"no DPU PF0\n"); return 2; }
        OK(doca_devinfo_rep_create_list(server_dev,DOCA_DEVINFO_REP_FILTER_NET,&reps,&nr));
        printf("representors=%u\n",nr);
    }
    for(unsigned i=0;i<count;i++) {
        struct endpoint *e=&ep[i]; e->sf=800+i;
        char name[40]; snprintf(name,sizeof(name),"dpumesh-sf-test-%u",e->sf);
        if(is_server) {
            for(uint32_t k=0;k<nr;k++) { uint32_t sf=0,host=0,pf=0;
                if(doca_devinfo_rep_get_sf_index(reps[k],&sf)!=DOCA_SUCCESS || sf!=e->sf) continue;
                OK(doca_devinfo_rep_get_host_index(reps[k],&host)); OK(doca_devinfo_rep_get_pf_index(reps[k],&pf));
                if(host!=1 || pf!=0) continue;
                OK(doca_dev_rep_open(reps[k],&e->rep)); break;
            }
            if(!e->rep) { fprintf(stderr,"no representor sf=%u\n",e->sf); return 2; }
            OK(doca_comch_server_create(server_dev,e->rep,name,&e->server));
            e->ctx=doca_comch_server_as_ctx(e->server);
            OK(doca_comch_server_set_max_msg_size(e->server,64));
            OK(doca_comch_server_set_recv_queue_size(e->server,8));
            OK(doca_comch_server_task_send_set_conf(e->server,sent,sent,8));
            OK(doca_comch_server_event_msg_recv_register(e->server,received));
            OK(doca_comch_server_event_connection_status_changed_register(e->server,connected,disconnected));
        } else {
            for(uint32_t k=0;k<n;k++) if(host_sf(list[k])==(int)e->sf) { OK(doca_dev_open(list[k],&e->dev)); break; }
            if(!e->dev) { fprintf(stderr,"no host device sf=%u\n",e->sf); return 2; }
            OK(doca_comch_cap_client_is_supported(doca_dev_as_devinfo(e->dev)));
            OK(doca_comch_client_create(e->dev,name,&e->client));
            e->ctx=doca_comch_client_as_ctx(e->client);
            OK(doca_comch_client_set_max_msg_size(e->client,64));
            OK(doca_comch_client_set_recv_queue_size(e->client,8));
            OK(doca_comch_client_task_send_set_conf(e->client,sent,sent,8));
            OK(doca_comch_client_event_msg_recv_register(e->client,received));
        }
        OK(doca_ctx_set_user_data(e->ctx,(union doca_data){.ptr=e}));
        OK(doca_ctx_set_state_changed_cb(e->ctx,state_changed));
        OK(doca_pe_connect_ctx(pe,e->ctx));
        doca_error_t r=doca_ctx_start(e->ctx);
        if(r!=DOCA_SUCCESS && r!=DOCA_ERROR_IN_PROGRESS) check(r,"ctx_start",__LINE__);
        if((i+1)%25==0 || i+1==count) printf("contexts_started=%u\n",i+1);
        doca_pe_progress(pe);
    }
    if(reps) OK(doca_devinfo_rep_destroy_list(reps));
    OK(doca_devinfo_destroy_list(list));
    double start=now(), all_since=0, tick=0,report=0;
    unsigned peak=0, min_live=count, rounds=0, ever_all=0;
    while(!interrupted && now()-start<180) {
        doca_pe_progress(pe);
        unsigned live=0;
        for(unsigned i=0;i<count;i++) live+=ep[i].ready;
        if(live>peak) peak=live;
        if(live==count && !ever_all) { ever_all=1; all_since=now(); printf("ALL_CONNECTED=%u\n",count); }
        if(ever_all && live<min_live) min_live=live;
        if(is_server && ever_all && live==0) break;
        if(ever_all && !is_server && now()-tick>=0.05) {
            unsigned done=0; for(unsigned i=0;i<count;i++) done+=!ep[i].pending && ep[i].received==ep[i].sent;
            if(done==count && now()-all_since<hold) {
                rounds++;
                for(unsigned i=0;i<count;i++) { ep[i].msg=(struct payload){.sf=ep[i].sf,.seq=rounds}; transmit(&ep[i]); }
                tick=now();
            }
        }
        if(ever_all && now()-all_since>=hold) {
            unsigned done=0; for(unsigned i=0;i<count;i++) done+=!ep[i].pending && (is_server || ep[i].received==ep[i].sent);
            if(is_server || done==count) break;
        }
        if(now()-report>=5) { printf("live=%u peak=%u errors=%d\n",live,peak,errors); report=now(); }
        struct timespec pause={.tv_nsec=50000}; nanosleep(&pause,NULL);
    }
    unsigned total_sent=0,total_recv=0,min_recv=~0u;
    for(unsigned i=0;i<count;i++) { total_sent+=ep[i].sent; total_recv+=ep[i].received; if(ep[i].received<min_recv) min_recv=ep[i].received; }
    printf("RESULT role=%s requested=%u peak=%u min_live_after_all=%u hold_s=%.3f sent=%u received=%u min_received_per_sf=%u errors=%d\n",
        is_server?"server":"client",count,peak,min_live,ever_all?now()-all_since:0,total_sent,total_recv,min_recv,errors);
    int good=ever_all && !errors && (is_server || min_live==count) && min_recv>0 && total_sent==total_recv;
    shutting_down=1;
    for(unsigned i=0;i<count;i++) doca_ctx_stop(ep[i].ctx);
    double stop=now(); unsigned idle=0;
    do { doca_pe_progress(pe); idle=0;
        for(unsigned i=0;i<count;i++) { enum doca_ctx_states state; OK(doca_ctx_get_state(ep[i].ctx,&state)); idle+=state==DOCA_CTX_STATE_IDLE; }
    } while(idle<count && now()-stop<10);
    printf("cleanup_idle=%u/%u\n",idle,count);
    if(idle<count) return 3;
    for(unsigned i=0;i<count;i++) {
        if(is_server) { OK(doca_comch_server_destroy(ep[i].server)); OK(doca_dev_rep_close(ep[i].rep)); }
        else { OK(doca_comch_client_destroy(ep[i].client)); OK(doca_dev_close(ep[i].dev)); }
    }
    OK(doca_pe_destroy(pe)); if(server_dev) OK(doca_dev_close(server_dev));
    return good?0:1;
}
