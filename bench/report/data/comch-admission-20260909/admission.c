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

#define SLOTS 8
#define MAX_CONN 512
static int server, stopping, errors;
static volatile sig_atomic_t quit;
static struct doca_pe *pe;
static struct doca_comch_server *srv;
static struct doca_comch_client *clients[SLOTS];
static struct doca_comch_connection *connections[MAX_CONN];
static unsigned char retry_disconnect[MAX_CONN];
static unsigned connects,disconnects,starts,disconnect_attempts,retries,live,peak,disconnect_accepted;
static double wall(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static double cpu(void){struct timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void sig(int s){(void)s;quit=1;}
static void check(doca_error_t r,const char *what,int line){if(r!=DOCA_SUCCESS){fprintf(stderr,"FAIL %s line=%d %s\n",what,line,doca_error_get_descr(r));exit(2);}}
#define OK(x) check(x,#x,__LINE__)
static void send_cb(struct doca_comch_task_send *t,union doca_data a,union doca_data b){(void)a;(void)b;doca_task_free(doca_comch_task_send_as_task(t));}
static void recv_cb(struct doca_comch_event_msg_recv *e,uint8_t *b,uint32_t n,struct doca_comch_connection *c){(void)e;(void)b;(void)n;(void)c;}
static void request_disconnect(unsigned i){
    disconnect_attempts++;
    doca_error_t r=doca_comch_server_disconnect(srv,connections[i]);
    if(r==DOCA_SUCCESS){retry_disconnect[i]=0;connections[i]=NULL;disconnect_accepted++;if(live)live--;}
    else if(r==DOCA_ERROR_AGAIN){retry_disconnect[i]=1;retries++;}
    else {retry_disconnect[i]=0;errors++;}
}
static void connect_cb(struct doca_comch_event_connection_status_changed *ev,struct doca_comch_connection *c,uint8_t success){
    (void)ev;if(!success){errors++;return;}
    connects++;live++;if(live>peak)peak=live;
    for(unsigned i=0;i<MAX_CONN;i++)if(!connections[i]){
        connections[i]=c;request_disconnect(i);return;
    }
    errors++;
}
static void disconnect_cb(struct doca_comch_event_connection_status_changed *ev,struct doca_comch_connection *c,uint8_t success){
    (void)ev;if(!success && !stopping)errors++;
    disconnects++;
    for(unsigned i=0;i<MAX_CONN;i++)if(connections[i]==c){if(live)live--;connections[i]=NULL;retry_disconnect[i]=0;break;}
}
static void state_cb(union doca_data d,struct doca_ctx *c,enum doca_ctx_states old,enum doca_ctx_states next){
    (void)d;(void)c;if(!server && next==DOCA_CTX_STATE_RUNNING)connects++;if(!server && !stopping && old==DOCA_CTX_STATE_RUNNING && next!=DOCA_CTX_STATE_RUNNING)disconnects++;
}
static int sfnum(struct doca_devinfo *d){
    uint32_t idx;if(doca_devinfo_get_sf_index(d,&idx)!=DOCA_SUCCESS)return -1;
    char path[180];snprintf(path,sizeof(path),"/sys/bus/auxiliary/devices/mlx5_core.sf.%u/sfnum",idx);
    FILE *f=fopen(path,"r");if(!f)return -1;
    int sf=-1;int r=fscanf(f,"%d",&sf);fclose(f);return r==1?sf:-1;
}
int main(int argc,char **argv){
    if(argc!=4)return 1;
    server=!strcmp(argv[1],"server");double seconds=strtod(argv[2],NULL),rate=strtod(argv[3],NULL);
    if(seconds<1 || seconds>180 || rate<1 || rate>1000)return 1;
    setvbuf(stdout,NULL,_IOLBF,0);signal(SIGINT,sig);signal(SIGTERM,sig);
    struct doca_devinfo **list;uint32_t count;struct doca_dev *dev=NULL;struct doca_dev_rep *rep=NULL;
    OK(doca_devinfo_create_list(&list,&count));
    for(uint32_t i=0;i<count;i++){
        char pci[DOCA_DEVINFO_PCI_ADDR_SIZE]={0};doca_devinfo_get_pci_addr_str(list[i],pci);
        if((server && !strcmp(pci,"0000:03:00.0")) || (!server && sfnum(list[i])==800)){OK(doca_dev_open(list[i],&dev));break;}
    }
    if(!dev){fprintf(stderr,"test device missing\n");return 2;}
    OK(doca_devinfo_destroy_list(list));OK(doca_pe_create(&pe));
    if(server){
        struct doca_devinfo_rep **r;uint32_t n;OK(doca_devinfo_rep_create_list(dev,DOCA_DEVINFO_REP_FILTER_NET,&r,&n));
        for(uint32_t i=0;i<n;i++){
            uint32_t sf=0,h=0,pf=0;
            if(doca_devinfo_rep_get_sf_index(r[i],&sf)!=DOCA_SUCCESS || sf!=800)continue;
            OK(doca_devinfo_rep_get_host_index(r[i],&h));OK(doca_devinfo_rep_get_pf_index(r[i],&pf));
            if(h==1 && pf==0){OK(doca_dev_rep_open(r[i],&rep));break;}
        }
        OK(doca_devinfo_rep_destroy_list(r));if(!rep)return 2;
        OK(doca_comch_server_create(dev,rep,"dpumesh-admission-test",&srv));
        OK(doca_comch_server_set_max_msg_size(srv,64));OK(doca_comch_server_set_recv_queue_size(srv,8));
        OK(doca_comch_server_task_send_set_conf(srv,send_cb,send_cb,8));
        OK(doca_comch_server_event_msg_recv_register(srv,recv_cb));
        OK(doca_comch_server_event_connection_status_changed_register(srv,connect_cb,disconnect_cb));
        OK(doca_pe_connect_ctx(pe,doca_comch_server_as_ctx(srv)));OK(doca_ctx_start(doca_comch_server_as_ctx(srv)));
    }else for(unsigned i=0;i<SLOTS;i++){
        OK(doca_comch_client_create(dev,"dpumesh-admission-test",&clients[i]));
        OK(doca_comch_client_set_max_msg_size(clients[i],64));OK(doca_comch_client_set_recv_queue_size(clients[i],8));
        OK(doca_comch_client_task_send_set_conf(clients[i],send_cb,send_cb,8));
        OK(doca_comch_client_event_msg_recv_register(clients[i],recv_cb));
        OK(doca_ctx_set_state_changed_cb(doca_comch_client_as_ctx(clients[i]),state_cb));
        OK(doca_pe_connect_ctx(pe,doca_comch_client_as_ctx(clients[i])));
    }
    printf("READY role=%s\n",server?"server":"client");
    double begin=wall(),last=begin,last_cpu=cpu(),last_tick=begin,total_progress=0,interval_progress=0;
    unsigned prev_conn=0,prev_disc=0;
    while(!quit && wall()-begin<seconds){
        double t=wall();
        double before=cpu();doca_pe_progress(pe);double spent=cpu()-before;interval_progress+=spent;total_progress+=spent;
        if(server){for(unsigned i=0;i<MAX_CONN;i++)if(connections[i] && retry_disconnect[i])request_disconnect(i);}
        else if(t-last_tick>=1.0/rate){
            for(unsigned i=0;i<SLOTS;i++){
                enum doca_ctx_states state;OK(doca_ctx_get_state(doca_comch_client_as_ctx(clients[i]),&state));
                if(state!=DOCA_CTX_STATE_IDLE)continue;
                doca_error_t r=doca_ctx_start(doca_comch_client_as_ctx(clients[i]));
                if(r!=DOCA_SUCCESS && r!=DOCA_ERROR_IN_PROGRESS)errors++;else starts++;
                last_tick=t;break;
            }
        }
        if(t-last>=1){
            double c=cpu();printf("SAMPLE elapsed=%.3f cpu_pct=%.3f progress_cpu_ms=%.3f connects=%u disconnect_events=%u pending_disconnects=%u errors=%d\n",t-begin,100*(c-last_cpu)/(t-last),1000*interval_progress,connects-prev_conn,disconnects-prev_disc,live,errors);
            last=t;last_cpu=c;prev_conn=connects;prev_disc=disconnects;interval_progress=0;
        }
        struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    printf("RESULT role=%s starts=%u connects=%u disconnect_events=%u disconnect_calls=%u disconnect_accepted=%u retries=%u peak_pending_disconnects=%u errors=%d progress_cpu_ms=%.3f\n",server?"server":"client",starts,connects,disconnects,disconnect_attempts,disconnect_accepted,retries,peak,errors,1000*total_progress);
    stopping=1;unsigned nctx=server?1:SLOTS;
    for(unsigned i=0;i<nctx;i++)doca_ctx_stop(server?doca_comch_server_as_ctx(srv):doca_comch_client_as_ctx(clients[i]));
    double stop=wall();unsigned idle;
    do{doca_pe_progress(pe);idle=0;for(unsigned i=0;i<nctx;i++){enum doca_ctx_states state;OK(doca_ctx_get_state(server?doca_comch_server_as_ctx(srv):doca_comch_client_as_ctx(clients[i]),&state));idle+=state==DOCA_CTX_STATE_IDLE;}}while(idle<nctx && wall()-stop<10);
    printf("cleanup_idle=%u/%u\n",idle,nctx);if(idle<nctx)return 3;
    if(server){OK(doca_comch_server_destroy(srv));OK(doca_dev_rep_close(rep));}
    else for(unsigned i=0;i<SLOTS;i++)OK(doca_comch_client_destroy(clients[i]));
    OK(doca_pe_destroy(pe));OK(doca_dev_close(dev));return errors?1:0;
}
