/* Self-routing concurrency-depth validator.
 *
 * One thread and CQ drive client and echo roles. Byte markers detect truncation,
 * misrouting, and reordering across configurable connection and pipeline depth.
 * Command: RUN <count> <size> [zero-copy] [window] [pipeline].
 * DMESH_CQ_BATCH controls completion batch size. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <dpumesh/dmesh.h>

/* Send a best-effort control reply; failures are ignored. */
static void ctl_reply(int fd, const char *s, size_t n) { (void)!write(fd, s, n); }


#define CTRL_PORT     9092
#define MAX_CLIENTS   256
#define MAX_SERVERS   8192
#define MAX_PIPELINE  256

static dmesh_channel_t *g_s       = NULL;
static dmesh_cq_t      *g_cq      = NULL;  /* the one CQ both roles are polled from */
static const char      *g_service = "verbs-dpumesh";  /* own service NAME (self-routing) */

/* diagnostics (single-threaded, so plain counters). The allocfail pair counts only
 * PERMANENT alloc faults (EINVAL); an SQ-full EAGAIN is backpressure, not a failure,
 * and is counted separately — a non-zero eagain with ok==N is a healthy run. */
static long D_cl_sent, D_cl_recv, D_sv_recv, D_sv_sent;
static long D_cl_allocfail, D_cl_postfail, D_sv_allocfail, D_sv_postfail;
static long D_cl_eagain, D_sv_eagain;

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* One outstanding (posted, not-yet-replied) request on a client conn. */
typedef struct { uint8_t marker; double t0; } out_slot_t;

/* Per-client-conn state (attached via dmesh_qp_t::user_data). SERVER conns carry
 * a NULL user_data — they only echo. */
typedef struct {
    long        target;        /* round-trips this conn must complete */
    long        sent, acked;   /* posted / validated */
    uint32_t    size;
    int         zc;
    uint8_t     seed;          /* per-conn marker seed (unique across conns) */
    uint32_t    reqno;         /* monotonic request counter -> marker */
    out_slot_t  ring[MAX_PIPELINE];
    int         cap, head, cnt;/* FIFO ring of outstanding requests */
    int         done;          /* target met (or faulted) -> FIN, retired by the sweep */
} cstate_t;

/* Full-body deterministic byte pattern keyed by (marker, position). The echo is
 * verbatim, so a correct reply reproduces it EXACTLY at every offset — catching
 * truncation, corruption, cross-request reorder (distinct marker per outstanding
 * req) and misroute byte-for-byte, at any size (incl. 1). */
static inline uint8_t patb(uint8_t m, uint32_t j) {
    return (uint8_t)(m * 131u + j * 17u + 7u);
}

/* Post one request on a client conn. dmesh_alloc NEVER blocks: on EAGAIN the conn's
 * SQ is at its in-flight ceiling and the request simply isn't posted YET — nothing is
 * lost and nothing failed, the sweep retries it once a TX_ACK frees ring space.
 * Returns 0 posted, 1 backpressured (not a failure), -1 on a permanent send fault. */
static int post_request(dmesh_qp_t *c) {
    cstate_t *cs = (cstate_t *)c->user_data;
    uint8_t m = (uint8_t)(cs->seed + cs->reqno);

    uint8_t *b = (uint8_t *)dmesh_alloc(c, cs->size);
    if (!b) {
        if (errno == EAGAIN) { D_cl_eagain++; return 1; }
        D_cl_allocfail++; return -1;    /* EINVAL: conn not established / bad len */
    }
    if (cs->zc) {                       /* fill transport DMA memory in place */
        for (uint32_t j = 0; j < cs->size; j++) b[j] = patb(m, j);
    } else {                            /* stage then copy (still an alloc buffer) */
        static __thread uint8_t stage[65536];
        for (uint32_t j = 0; j < cs->size; j++) stage[j] = patb(m, j);
        memcpy(b, stage, cs->size);
    }
    if (dmesh_post_send(c, b, cs->size, 0, 0) != 0 || dmesh_flush(c) != 0) {
        D_cl_postfail++;
        return -1;
    }

    cs->ring[(cs->head + cs->cnt) % cs->cap] = (out_slot_t){ .marker = m, .t0 = now_sec() };
    cs->cnt++; cs->reqno++; cs->sent++; D_cl_sent++;
    return 0;
}

/* ---- server side: echo every looped-back request verbatim ---- */

/* Per-server-connection FIFO for echoes waiting on TX capacity. Bodies are copied
 * so RX credits can be released while the queue waits. */
typedef struct { uint8_t *b; uint32_t len; } smsg_t;
typedef struct { smsg_t *q; int cap, head, cnt, dead; } sstate_t;

static int sq_push(sstate_t *ss, const uint8_t *b, uint32_t n) {
    if (ss->cnt == ss->cap) {                       /* grow, re-linearizing the FIFO */
        int cap = ss->cap ? ss->cap * 2 : 16;
        smsg_t *q = (smsg_t *)malloc((size_t)cap * sizeof *q);
        if (!q) return -1;
        for (int i = 0; i < ss->cnt; i++) q[i] = ss->q[(ss->head + i) % ss->cap];
        free(ss->q); ss->q = q; ss->cap = cap; ss->head = 0;
    }
    uint8_t *m = (uint8_t *)malloc(n);
    if (!m) return -1;
    memcpy(m, b, n);
    ss->q[(ss->head + ss->cnt) % ss->cap] = (smsg_t){ .b = m, .len = n };
    ss->cnt++;
    return 0;
}

/* Ship as much of the backlog as the SQ will take. Returns messages posted; a
 * permanent fault (non-EAGAIN alloc / descriptor enqueue fault) flags the conn. */
static int sq_drain(dmesh_qp_t *c) {
    sstate_t *ss = (sstate_t *)c->user_data;
    int posted = 0;
    while (ss && !ss->dead && ss->cnt) {
        smsg_t *m = &ss->q[ss->head];
        uint8_t *b = (uint8_t *)dmesh_alloc(c, m->len);
        if (!b) { if (errno != EAGAIN) { D_sv_allocfail++; ss->dead = 1; } else D_sv_eagain++; break; }
        memcpy(b, m->b, m->len);
        if (dmesh_post_send(c, b, m->len, 0, 0) != 0) { D_sv_postfail++; ss->dead = 1; break; }
        free(m->b);
        ss->head = (ss->head + 1) % ss->cap; ss->cnt--;
        D_sv_sent++; posted++;
    }
    if (posted && dmesh_flush(c) != 0) { D_sv_postfail++; ss->dead = 1; }
    return posted;
}

/* Echo one inbound request. Returns messages posted. A fault only FLAGS the conn: it
 * must stay alive until the whole wc[] batch is dispatched, since later entries may
 * still name it. */
static int srv_recv(dmesh_qp_t *c, const dmesh_wc_t *w) {
    sstate_t *ss = (sstate_t *)c->user_data;
    if (!ss || ss->dead) return 0;                  /* untracked / already faulted */
    if (ss->cnt == 0) {                             /* fast path: RX mmap -> TX ring */
        uint8_t *b = (uint8_t *)dmesh_alloc(c, w->len);
        if (b) {
            memcpy(b, w->buf, w->len);
            if (dmesh_post_send(c, b, w->len, 0, 0) != 0 || dmesh_flush(c) != 0) {
                D_sv_postfail++;
                ss->dead = 1;
                return 0;
            }
            D_sv_sent++;
            return 1;
        }
        if (errno != EAGAIN) { D_sv_allocfail++; ss->dead = 1; return 0; }
        D_sv_eagain++;
    }
    if (sq_push(ss, w->buf, w->len) != 0) ss->dead = 1;  /* SQ full -> owe the bytes */
    return 0;
}

/* Drop a server conn: its backlog dies with it (the peer is gone). */
static void srv_retire(dmesh_qp_t **servers, int *nserv, dmesh_qp_t *c) {
    sstate_t *ss = (sstate_t *)c->user_data;
    if (ss) {
        for (int i = 0; i < ss->cnt; i++) free(ss->q[(ss->head + i) % ss->cap].b);
        free(ss->q); free(ss); c->user_data = NULL;
    }
    dmesh_destroy_qp(c);
    for (int i = 0; i < *nserv; i++)
        if (servers[i] == c) { servers[i] = servers[--(*nserv)]; break; }
}

/* ==================================================================== RUN */
static void run_verbs(int conn_fd, long N, uint32_t size, int zc,
                      int window, int pipeline, int batch) {
    char reply[192];
    int msgmax = dmesh_msg_max(g_s);
    if (N < 1 || size < 1 || (int)size > msgmax ||
        window < 1 || window > MAX_CLIENTS ||
        pipeline < 1 || pipeline > MAX_PIPELINE || batch < 1) {
        int n = snprintf(reply, sizeof reply,
                         "ERR bad args (1<=size<=%d, 1<=window<=%d, 1<=pipe<=%d, batch>=1)\n",
                         msgmax, MAX_CLIENTS, MAX_PIPELINE);
        ctl_reply(conn_fd, reply, (size_t)n); return;
    }
    D_cl_sent = D_cl_recv = D_sv_recv = D_sv_sent = 0;
    D_cl_allocfail = D_cl_postfail = D_sv_allocfail = D_sv_postfail = 0;
    D_cl_eagain = D_sv_eagain = 0;

    int cqfd = dmesh_cq_fd(g_cq);
    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = cqfd } };
    epoll_ctl(epfd, EPOLL_CTL_ADD, cqfd, &ev);

    double *lat = (double *)malloc((size_t)N * sizeof(double));
    dmesh_wc_t *wc = (dmesh_wc_t *)malloc((size_t)batch * sizeof(dmesh_wc_t));
    dmesh_qp_t **clients = (dmesh_qp_t **)calloc((size_t)window, sizeof(*clients));
    cstate_t *cst = (cstate_t *)calloc((size_t)window, sizeof(*cst));
    /* SERVER conns the DPU loops back to us — tracked so teardown closes them all. */
    static dmesh_qp_t *servers[MAX_SERVERS]; int nserv = 0;
    if (!lat || !wc || !clients || !cst) {
        ctl_reply(conn_fd, "ERR oom\n", 8);
        free(lat); free(wc); free(clients); free(cst); close(epfd); return;
    }

    long ok = 0, fail = 0, served = 0, nlat = 0;
    long completed = 0;                 /* ok + fail (client round-trips finished) */

    /* Open `window` CLIENT conns and pin each (RC-like ordered replies). Distribute N
     * round-trips across the conns; the sweep below primes every pipeline on the first
     * pass, so a conn backpressured at prime time is topped up rather than failed. */
    for (int i = 0; i < window; i++) {
        dmesh_qp_t *c = dmesh_create_qp(g_cq, g_service);
        if (!c) {                                  /* can't even connect -> abort */
            for (int j = 0; j < window; j++) if (clients[j]) dmesh_destroy_qp(clients[j]);
            fail += N; goto report;
        }
        cstate_t *cs = &cst[i];
        cs->target = N / window + (i < (N % window) ? 1 : 0);
        cs->size = size; cs->zc = zc; cs->cap = pipeline;
        cs->seed = (uint8_t)(0x40u + i);           /* distinct per conn */
        c->user_data = cs;
        clients[i] = c;
    }

    double deadline = now_sec() + 60.0;            /* global anti-hang guard */

    while (completed < N || nserv > 0) {
        int n = dmesh_poll_cq(g_cq, wc, batch);
        if (n < 0) { break; }                      /* only EINVAL — impossible here */
        for (int i = 0; i < n; i++) {
            dmesh_qp_t *c = wc[i].qp;
            switch (wc[i].opcode) {

            /* NB: nothing in this batch loop may close a conn — dmesh_destroy_qp frees it,
             * and poll_cq emits CONN_REQ together with that conn's already-landed
             * messages, so a later entry in THIS batch can still name it. Deaths are
             * flagged and collected by the sweep below. */
            case DMESH_WC_CONN_REQ:                /* the DPU looped our request back */
                c->user_data = nserv < MAX_SERVERS ? calloc(1, sizeof(sstate_t)) : NULL;
                if (c->user_data) servers[nserv++] = c;
                break;                             /* untracked -> never echoes -> client times out */

            case DMESH_WC_RECV:
                if (c->role == DMESH_ROLE_SERVER) {          /* a request -> echo verbatim */
                    D_sv_recv++;
                    served += srv_recv(c, &wc[i]);
                    dmesh_wc_release(g_s, &wc[i]);
                } else {                                     /* a reply -> validate FIFO */
                    D_cl_recv++;
                    cstate_t *cs = (cstate_t *)c->user_data;
                    int bad = 1;
                    if (cs && cs->cnt > 0 && wc[i].len == cs->size) {
                        out_slot_t os = cs->ring[cs->head];
                        cs->head = (cs->head + 1) % cs->cap; cs->cnt--;
                        bad = 0;                                 /* full byte-exact compare */
                        for (uint32_t j = 0; j < cs->size; j++)
                            if (wc[i].buf[j] != patb(os.marker, j)) { bad = 1; break; }
                        if (!bad && nlat < N) lat[nlat++] = (now_sec() - os.t0) * 1e6;
                    }
                    dmesh_wc_release(g_s, &wc[i]);
                    if (bad) fail++; else ok++;
                    completed++;
                    if (cs) {
                        cs->acked++;
                        if (cs->sent < cs->target) {         /* keep the pipe full */
                            int r = post_request(c);         /* r > 0: SQ full — the sweep retries */
                            if (r < 0) { fail++; completed++; cs->done = 1; }
                        } else if (cs->acked >= cs->target && cs->cnt == 0) {
                            cs->done = 1;                    /* -> FIN, sent by the sweep */
                        }
                    }
                }
                break;

            case DMESH_WC_RECV_FIN:                /* peer closed — role picks the state type */
                if (c->role == DMESH_ROLE_SERVER) {
                    if (c->user_data) ((sstate_t *)c->user_data)->dead = 1;
                } else if (c->user_data) ((cstate_t *)c->user_data)->done = 1;
                break;
            }
        }

        /* Sweep, every pass: ship what backpressure refused — client pipes below their
         * window, echoes the server SQ turned away — and retire the conns flagged during
         * the batch. Coming back here IS the EAGAIN retry: SQ space frees silently (there
         * are no send completions), so allocating again is the only way to find out. */
        int posted = 0;
        for (int i = 0; i < window; i++) {
            dmesh_qp_t *c = clients[i];
            if (!c) continue;
            cstate_t *cs = &cst[i];
            if (cs->done) { dmesh_destroy_qp(c); clients[i] = NULL; continue; }
            while (cs->sent < cs->target && cs->cnt < cs->cap) {
                int r = post_request(c);
                if (r > 0) break;                            /* SQ full — retry next pass */
                if (r < 0) { fail++; completed++; cs->done = 1; break; }
                posted++;
            }
        }
        for (int i = 0; i < nserv; ) {
            dmesh_qp_t *c = servers[i];
            int p = sq_drain(c);
            served += p; posted += p;
            if (((sstate_t *)c->user_data)->dead) { srv_retire(servers, &nserv, c); continue; }
            i++;
        }

        if (now_sec() > deadline) break;
        if (n == 0 && posted == 0) {
            /* idle: sleep on the CQ fd (drain counter FIRST, per the edge rule),
             * then loop back to poll. 200ms tick doubles as the deadline check. */
            uint64_t cnt; while (read(cqfd, &cnt, sizeof cnt) > 0) { }
            struct epoll_event evs; epoll_wait(epfd, &evs, 1, 200);
        }
    }

    /* Anything still outstanding at the deadline counts as failed. */
    if (completed < N) {
        fail += (N - completed);
        fprintf(stderr, "[verbs] WEDGE DIAG: cl_sent=%ld sv_recv=%ld sv_sent=%ld cl_recv=%ld "
                "| fwd_gap(cl_sent-sv_recv)=%ld rev_gap(sv_sent-cl_recv)=%ld "
                "| cl_allocfail=%ld cl_postfail=%ld sv_allocfail=%ld sv_postfail=%ld "
                "| cl_eagain=%ld sv_eagain=%ld | nserv=%d\n",
                D_cl_sent, D_sv_recv, D_sv_sent, D_cl_recv,
                D_cl_sent - D_sv_recv, D_sv_sent - D_cl_recv,
                D_cl_allocfail, D_cl_postfail, D_sv_allocfail, D_sv_postfail,
                D_cl_eagain, D_sv_eagain, nserv);
        for (int i = 0; i < window; i++) {
            cstate_t *cs = &cst[i];
            if (cs->acked < cs->target)
                fprintf(stderr, "[verbs]   client[%d] target=%ld sent=%ld acked=%ld outstanding=%d%s\n",
                        i, cs->target, cs->sent, cs->acked, cs->cnt,
                        clients[i] ? "" : " (closed)");
        }
    }

    /* Teardown: close any client conn that never finished, then any server conn
     * whose FIN we never observed (best-effort — mirrors loopback's close loop). */
    for (int i = 0; i < window; i++) if (clients[i]) dmesh_destroy_qp(clients[i]);
    while (nserv > 0) srv_retire(servers, &nserv, servers[0]);

report:
    qsort(lat, (size_t)nlat, sizeof(double), cmp_d);
    double p50 = nlat ? lat[nlat / 2] : 0.0;
    int rn = snprintf(reply, sizeof reply, "OK %ld %ld %ld %.1f\n", ok, fail, served, p50);
    ctl_reply(conn_fd, reply, (size_t)rn);
    fprintf(stderr, "[verbs] DONE N=%ld size=%u zc=%d window=%d pipe=%d batch=%d "
            "ok=%ld fail=%ld served=%ld p50=%.1fus\n",
            N, size, zc, window, pipeline, batch, ok, fail, served, p50);
    free(lat); free(wc); free(clients); free(cst); close(epfd);
}

/* ============================================================ control TCP */
static void handle_ctrl(int fd) {
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';

    char cmd[16] = {0}; long N = 0; uint32_t size = 0;
    int zc = 0, window = 1, pipeline = 1, batch = 16;
    int m = sscanf(buf, "%15s %ld %u %d %d %d %d", cmd, &N, &size, &zc, &window, &pipeline, &batch);
    if (m < 3 || strcmp(cmd, "RUN") != 0) {
        ctl_reply(fd, "ERR use: RUN <N> <SIZE> [ZC] [WINDOW] [PIPELINE] [BATCH]\n", 56);
        close(fd); return;
    }
    run_verbs(fd, N, size, zc, window, pipeline, batch);
    close(fd);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (getenv("DPUMESH_SERVICE")) g_service = getenv("DPUMESH_SERVICE");

    g_s = dmesh_create_channel();
    if (!g_s) { fprintf(stderr, "[verbs] create_channel failed\n"); return 1; }
    g_cq = dmesh_create_cq(g_s);
    if (!g_cq) { fprintf(stderr, "[verbs] create_cq failed\n"); return 1; }
    fprintf(stderr, "[verbs] ready: pod_id=%d own_service=%s msg_max=%d (verbs façade)\n",
            dmesh_pod_id(g_s), g_service, dmesh_msg_max(g_s));

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(CTRL_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 4) < 0) {
        perror("listen"); return 1;
    }
    fprintf(stderr, "[verbs] control LISTEN on :%d\n", CTRL_PORT);
    for (;;) {
        int conn = accept(srv, NULL, NULL);
        if (conn < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        handle_ctrl(conn);
    }
}
