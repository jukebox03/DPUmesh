/* The RDMA carrier behind `peer_wire_ops`.
 *
 * A queue pair already moves whole messages — one posted send arrives as one
 * completion carrying its own byte count — so unlike the TCP carrier there is
 * no length prefix here and nothing to reassemble. What replaces them is
 * buffer ownership: a posted work request names memory the adapter reads or
 * writes on its own schedule, so a message rides in a slot this file owns
 * until the completion for it comes back.
 *
 * Connection setup runs on librdmacm, which reports address resolution, route
 * resolution, and establishment as events instead of as a blocking call.
 * Everything is driven from `progress`, and the two descriptors it drains —
 * the connection-manager event channel and the completion channel — are what a
 * worker's event loop sleeps on. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peer_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* One per peer node per worker, plus the inbound connections still proving who
 * they are. Lower than the TCP carrier's bound because every connection here
 * pins registered memory for its rings. */
#define RDMA_CONN_MAX    128u
/* Send depth absorbs completion latency. The layer above offers the next frame
 * as soon as this one is accepted, and accepted here means posted, not
 * delivered — with a single slot every frame would wait on the previous one's
 * completion. */
#define RDMA_SEND_RING   16u
/* Receive depth stays above a peer's send depth, so a peer sending flat out
 * never finds this side without a buffer posted. */
#define RDMA_RECV_RING   32u
#define RDMA_SLOT        PEER_WIRE_MSG_MAX
#define RDMA_CQ_DEPTH    ((int)(RDMA_CONN_MAX * (RDMA_SEND_RING + RDMA_RECV_RING)))
#define RDMA_RESOLVE_MS  2000
#define RDMA_POLL_BATCH  32
#define RDMA_BACKLOG     8

/* Retry counts are the maximum the wire format encodes. A receiver momentarily
 * without a posted buffer is back-pressure rather than a fault, so the sender
 * keeps retrying instead of tearing the connection down; the layer above has
 * its own timeouts for a peer that never comes back. */
#define RDMA_RETRY_COUNT 7
#define RDMA_RNR_RETRY   7
#define RDMA_ACK_TIMEOUT 14

enum { RC_FREE = 0, RC_RESOLVING, RC_CONNECTING, RC_READY, RC_DEAD };

struct rdma_ctx;

struct rdma_conn {
    struct rdma_ctx   *ctx;
    struct rdma_cm_id *id;
    struct ibv_mr     *mr;
    uint8_t           *buf;          /* the send ring, then the receive ring */
    uint32_t           index;
    uint32_t           epoch;
    int                state;
    uint8_t            in_use;
    uint8_t            inbound;
    uint8_t            handout_queued;
    uint8_t            handed_out;
    uint8_t            send_busy[RDMA_SEND_RING];
    uint32_t           send_next;    /* where the free-slot scan starts */
    /* Receives that completed and have not been handed up yet, oldest first. */
    uint16_t           rq_slot[RDMA_RECV_RING];
    uint32_t           rq_len[RDMA_RECV_RING];
    uint32_t           rq_head;
    uint32_t           rq_count;
};

struct rdma_ctx {
    int                        epfd;
    struct rdma_event_channel *ec;
    struct rdma_cm_id         *listener;
    struct ibv_context        *verbs;
    struct ibv_pd             *pd;
    struct ibv_comp_channel   *cc;
    struct ibv_cq             *cq;
    uint32_t                   bind_ip;
    uint16_t                   port;
    uint8_t                    armed;
    uint8_t                    dead;
    uint8_t                    ec_nonblock;
    uint32_t                   epoch_next;
    /* Connections that finished arriving but did not fit the caller's batch. */
    uint16_t                   handout[RDMA_CONN_MAX];
    uint32_t                   handout_head;
    uint32_t                   handout_count;
    struct rdma_conn           conns[RDMA_CONN_MAX];
};

/* ---- work-request identity --------------------------------------------- */

/* Completions outlive the connection that posted them: one queue destroyed
 * while its work is still in a shared completion queue leaves entries behind.
 * A slot index alone would name a struct a later peer now owns, so the epoch
 * the slot carried when it posted rides along and a stale completion is
 * recognised and dropped. */
#define WR_KIND_SHIFT  63
#define WR_EPOCH_SHIFT 32
#define WR_CONN_SHIFT  16
#define WR_EPOCH_MASK  0x7fffffffu

static uint64_t wr_pack(int recv, uint32_t epoch, uint32_t conn, uint32_t slot)
{
    return ((uint64_t)(recv & 1) << WR_KIND_SHIFT) |
           ((uint64_t)(epoch & WR_EPOCH_MASK) << WR_EPOCH_SHIFT) |
           ((uint64_t)(conn & 0xffffu) << WR_CONN_SHIFT) |
           (uint64_t)(slot & 0xffffu);
}

/* ---- slots -------------------------------------------------------------- */

static uint8_t *send_slot(struct rdma_conn *c, uint32_t slot)
{
    return c->buf + (size_t)slot * RDMA_SLOT;
}

static uint8_t *recv_slot(struct rdma_conn *c, uint32_t slot)
{
    return c->buf + ((size_t)RDMA_SEND_RING + slot) * RDMA_SLOT;
}

static struct rdma_conn *rdma_slot_take(struct rdma_ctx *ctx)
{
    for (uint32_t i = 0; i < RDMA_CONN_MAX; i++) {
        struct rdma_conn *c = &ctx->conns[i];
        if (c->in_use)
            continue;
        uint8_t *buf = malloc((size_t)(RDMA_SEND_RING + RDMA_RECV_RING) * RDMA_SLOT);
        if (!buf)
            return NULL;
        memset(c, 0, sizeof(*c));
        c->ctx = ctx;
        c->index = i;
        c->epoch = (++ctx->epoch_next) & WR_EPOCH_MASK;
        c->buf = buf;
        c->in_use = 1;
        c->state = RC_RESOLVING;
        return c;
    }
    return NULL;
}

static void rdma_release(struct rdma_conn *c)
{
    if (!c->in_use)
        return;
    if (c->id) {
        /* An explicit rdma_disconnect would create a DISCONNECTED event that
         * must be read and acknowledged before rdma_destroy_id. This carrier
         * closes synchronously, so destroy the QP instead; the peer observes
         * that loss and no unacknowledged local event is manufactured here. */
        if (c->id->qp)
            rdma_destroy_qp(c->id);
        rdma_destroy_id(c->id);
    }
    if (c->mr)
        ibv_dereg_mr(c->mr);
    free(c->buf);
    memset(c, 0, sizeof(*c));
}

/* The connection an event names, or NULL when the event belongs to the
 * listener. A newly requested connection can inherit the listener's context
 * pointer, so the identity is only trusted when the connection points back. */
static struct rdma_conn *conn_of(struct rdma_ctx *ctx, struct rdma_cm_id *id)
{
    if (!id || id == ctx->listener)
        return NULL;
    struct rdma_conn *c = id->context;
    if (!c || c < ctx->conns || c >= ctx->conns + RDMA_CONN_MAX)
        return NULL;
    if (!c->in_use || c->id != id)
        return NULL;
    return c;
}

/* ---- queue pair --------------------------------------------------------- */

static int rdma_post_recv(struct rdma_conn *c, uint32_t slot)
{
    struct ibv_sge sge = {
        .addr   = (uintptr_t)recv_slot(c, slot),
        .length = RDMA_SLOT,
        .lkey   = c->mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id   = wr_pack(1, c->epoch, c->index, slot),
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(c->id->qp, &wr, &bad) == 0 ? 0 : -1;
}

/* Build the queue pair and fill its receive ring. Both sides do this before
 * the connection is established, because a peer may send the moment it is. */
static int rdma_arm_conn(struct rdma_conn *c)
{
    struct rdma_ctx *ctx = c->ctx;
    /* One protection domain and one completion queue serve every connection
     * here, and both belong to a single device context. */
    if (c->id->verbs != ctx->verbs)
        return -1;
    struct ibv_qp_init_attr attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 0,
        .cap = {
            .max_send_wr  = RDMA_SEND_RING,
            .max_recv_wr  = RDMA_RECV_RING,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    if (rdma_create_qp(c->id, ctx->pd, &attr) != 0)
        return -1;
    c->mr = ibv_reg_mr(ctx->pd, c->buf,
                       (size_t)(RDMA_SEND_RING + RDMA_RECV_RING) * RDMA_SLOT,
                       IBV_ACCESS_LOCAL_WRITE);
    if (!c->mr)
        return -1;
    for (uint32_t i = 0; i < RDMA_RECV_RING; i++)
        if (rdma_post_recv(c, i) != 0)
            return -1;
    return 0;
}

static void rdma_conn_param(struct rdma_conn_param *cp)
{
    memset(cp, 0, sizeof(*cp));
    cp->responder_resources = 0;     /* nothing here reads the peer's memory */
    cp->initiator_depth = 0;
    cp->retry_count = RDMA_RETRY_COUNT;
    cp->rnr_retry_count = RDMA_RNR_RETRY;
}

static void rdma_set_ack_timeout(struct rdma_cm_id *id)
{
    uint8_t t = RDMA_ACK_TIMEOUT;
    /* Advisory: a kernel without the option keeps its own default. */
    rdma_set_option(id, RDMA_OPTION_ID, RDMA_OPTION_ID_ACK_TIMEOUT, &t, sizeof(t));
}

/* ---- transfer ----------------------------------------------------------- */

static int rdma_send_msg(void *wc, const void *buf, size_t len)
{
    struct rdma_conn *c = wc;
    if (!c || !c->in_use || c->state == RC_DEAD)
        return -1;
    if (len == 0 || len > PEER_WIRE_MSG_MAX)
        return -1;
    if (c->state != RC_READY)
        return 0;

    uint32_t slot = RDMA_SEND_RING;
    for (uint32_t i = 0; i < RDMA_SEND_RING; i++) {
        uint32_t s = (c->send_next + i) % RDMA_SEND_RING;
        if (!c->send_busy[s]) {
            slot = s;
            break;
        }
    }
    if (slot == RDMA_SEND_RING)
        return 0;                    /* every slot is still with the adapter */

    uint8_t *p = send_slot(c, slot);
    memcpy(p, buf, len);
    struct ibv_sge sge = {
        .addr   = (uintptr_t)p,
        .length = (uint32_t)len,
        .lkey   = c->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = wr_pack(0, c->epoch, c->index, slot),
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        /* Signalled because the completion is what returns the slot. */
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(c->id->qp, &wr, &bad) != 0) {
        c->state = RC_DEAD;
        return -1;
    }
    c->send_busy[slot] = 1;
    c->send_next = (slot + 1) % RDMA_SEND_RING;
    return 1;
}

static long rdma_recv_msg(void *wc, void *buf, size_t cap)
{
    struct rdma_conn *c = wc;
    if (!c || !c->in_use || c->state == RC_DEAD)
        return -1;
    if (c->rq_count == 0)
        return 0;
    uint32_t slot = c->rq_slot[c->rq_head];
    uint32_t len  = c->rq_len[c->rq_head];
    if (len > cap) {
        c->state = RC_DEAD;
        return -1;
    }
    memcpy(buf, recv_slot(c, slot), len);
    c->rq_head = (c->rq_head + 1) % RDMA_RECV_RING;
    c->rq_count--;
    if (rdma_post_recv(c, slot) != 0) {
        c->state = RC_DEAD;
        return -1;
    }
    return (long)len;
}

/* ---- completions -------------------------------------------------------- */

/* Consume whatever notifications are queued. They carry no information beyond
 * "look at the queue", which `progress` is about to do anyway; what matters is
 * that they leave the descriptor, since one left unread keeps a level-triggered
 * event loop awake. */
static void rdma_drain_comp(struct rdma_ctx *ctx)
{
    unsigned acked = 0;
    for (;;) {
        struct ibv_cq *cq = NULL;
        void *cookie = NULL;
        if (ibv_get_cq_event(ctx->cc, &cq, &cookie) != 0)
            break;
        acked++;
        ctx->armed = 0;
    }
    if (acked)
        ibv_ack_cq_events(ctx->cq, acked);   /* one lock for the whole batch */
}

static int rdma_poll_cq(struct rdma_ctx *ctx)
{
    struct ibv_wc wcs[RDMA_POLL_BATCH];
    int progressed = 0;
    for (;;) {
        int n = ibv_poll_cq(ctx->cq, RDMA_POLL_BATCH, wcs);
        if (n < 0) {
            ctx->dead = 1;
            for (uint32_t i = 0; i < RDMA_CONN_MAX; i++)
                if (ctx->conns[i].in_use)
                    ctx->conns[i].state = RC_DEAD;
            return 1;
        }
        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            uint64_t id = wcs[i].wr_id;
            uint32_t ci = (uint32_t)((id >> WR_CONN_SHIFT) & 0xffffu);
            uint32_t slot = (uint32_t)(id & 0xffffu);
            uint32_t epoch = (uint32_t)((id >> WR_EPOCH_SHIFT) & WR_EPOCH_MASK);
            int recv = (int)((id >> WR_KIND_SHIFT) & 1);
            if (ci >= RDMA_CONN_MAX)
                continue;
            struct rdma_conn *c = &ctx->conns[ci];
            if (!c->in_use || c->epoch != epoch)
                continue;            /* the slot belongs to a later connection */
            progressed = 1;
            if (wcs[i].status != IBV_WC_SUCCESS) {
                c->state = RC_DEAD;
                continue;
            }
            if (recv) {
                if (slot >= RDMA_RECV_RING || wcs[i].byte_len == 0 ||
                    wcs[i].byte_len > RDMA_SLOT ||
                    c->rq_count >= RDMA_RECV_RING) {
                    c->state = RC_DEAD;
                    continue;
                }
                uint32_t tail = (c->rq_head + c->rq_count) % RDMA_RECV_RING;
                c->rq_slot[tail] = (uint16_t)slot;
                c->rq_len[tail] = wcs[i].byte_len;
                c->rq_count++;
            } else if (slot < RDMA_SEND_RING) {
                c->send_busy[slot] = 0;
            }
        }
        if (n < RDMA_POLL_BATCH)
            break;
    }
    return progressed;
}

/* ---- connection manager ------------------------------------------------- */

static int rdma_handout_push(struct rdma_ctx *ctx, struct rdma_conn *c)
{
    if (ctx->handout_count >= RDMA_CONN_MAX)
        return -1;
    uint32_t tail = (ctx->handout_head + ctx->handout_count) % RDMA_CONN_MAX;
    ctx->handout[tail] = (uint16_t)c->index;
    ctx->handout_count++;
    c->handout_queued = 1;
    return 0;
}

static void rdma_on_connect_request(struct rdma_ctx *ctx,
                                    struct rdma_cm_id *id,
                                    struct rdma_cm_id **orphan)
{
    struct rdma_conn *c = ctx->dead ? NULL : rdma_slot_take(ctx);
    if (!c) {
        rdma_reject(id, NULL, 0);    /* at the pool bound; the peer retries */
        *orphan = id;
        return;
    }
    c->id = id;
    c->inbound = 1;
    id->context = c;
    struct rdma_conn_param cp;
    rdma_conn_param(&cp);
    rdma_set_ack_timeout(id);
    if (rdma_arm_conn(c) != 0 || rdma_accept(id, &cp) != 0) {
        rdma_reject(id, NULL, 0);
        /* The queue pair and its registration go now, while the identifier is
         * still in hand; the identifier itself waits for the acknowledgement. */
        if (id->qp)
            rdma_destroy_qp(id);
        if (c->mr)
            ibv_dereg_mr(c->mr);
        c->id = NULL;
        c->mr = NULL;
        c->state = RC_DEAD;
        rdma_release(c);
        *orphan = id;
        return;
    }
    c->state = RC_CONNECTING;
}

static int rdma_drain_cm(struct rdma_ctx *ctx)
{
    int progressed = 0;
    for (;;) {
        struct rdma_cm_event *ev = NULL;
        if (rdma_get_cm_event(ctx->ec, &ev) != 0)
            break;
        /* An identifier may only be destroyed once every event naming it has
         * been acknowledged, so a rejected request is parked here and torn
         * down after the acknowledgement below. */
        struct rdma_cm_id *orphan = NULL;
        struct rdma_conn *c = conn_of(ctx, ev->id);

        /* Shutdown still has to consume requests already queued on the event
         * channel. Reject them and acknowledge every other event without
         * starting more asynchronous work. */
        if (ctx->dead) {
            progressed = 1;
            if (ev->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
                rdma_reject(ev->id, NULL, 0);
                orphan = ev->id;
            } else if (c) {
                c->state = RC_DEAD;
            }
            rdma_ack_cm_event(ev);
            if (orphan)
                rdma_destroy_id(orphan);
            continue;
        }

        switch (ev->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            progressed = 1;
            if (c && rdma_resolve_route(c->id, RDMA_RESOLVE_MS) != 0)
                c->state = RC_DEAD;
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED: {
            progressed = 1;
            if (!c)
                break;
            struct rdma_conn_param cp;
            rdma_conn_param(&cp);
            rdma_set_ack_timeout(c->id);
            if (rdma_arm_conn(c) != 0 || rdma_connect(c->id, &cp) != 0)
                c->state = RC_DEAD;
            else
                c->state = RC_CONNECTING;
            break;
        }
        case RDMA_CM_EVENT_CONNECT_REQUEST:
            progressed = 1;
            rdma_on_connect_request(ctx, ev->id, &orphan);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            progressed = 1;
            if (!c)
                break;
            c->state = RC_READY;
            if (c->inbound && !c->handout_queued && !c->handed_out &&
                rdma_handout_push(ctx, c) != 0)
                c->state = RC_DEAD;
            break;
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
            progressed = 1;
            ctx->dead = 1;
            for (uint32_t i = 0; i < RDMA_CONN_MAX; i++)
                if (ctx->conns[i].in_use)
                    ctx->conns[i].state = RC_DEAD;
            break;
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            break;                   /* the identifier is already released */
        default:
            /* Everything else that arrives for a connection ends it: address
             * and route failures, rejection, unreachability, disconnect. */
            progressed = 1;
            if (c)
                c->state = RC_DEAD;
            break;
        }

        rdma_ack_cm_event(ev);
        if (orphan)
            rdma_destroy_id(orphan);
    }
    return progressed;
}

/* ---- ops ---------------------------------------------------------------- */

static int rdma_connect_op(void *wctx, uint32_t ip_be, uint16_t port, void **wc)
{
    struct rdma_ctx *ctx = wctx;
    if (!wc)
        return -1;
    *wc = NULL;
    if (!ctx || ctx->dead)
        return -1;
    struct rdma_conn *c = rdma_slot_take(ctx);
    if (!c)
        return -1;
    if (rdma_create_id(ctx->ec, &c->id, c, RDMA_PS_TCP) != 0) {
        rdma_release(c);
        return -1;
    }
    /* The source address is bound too, so resolution settles on the device
     * this runtime's protection domain and completion queue belong to. */
    struct sockaddr_in src = { 0 }, dst = { 0 };
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = ctx->bind_ip;
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ip_be;
    dst.sin_port = htons(port);
    if (rdma_resolve_addr(c->id, (struct sockaddr *)&src,
                          (struct sockaddr *)&dst, RDMA_RESOLVE_MS) != 0) {
        rdma_release(c);
        return -1;
    }
    *wc = c;
    return 0;
}

static int rdma_progress(void *wctx, void **accepted, int max, int *n_accepted)
{
    struct rdma_ctx *ctx = wctx;
    int progressed = 0;
    if (n_accepted)
        *n_accepted = 0;
    if (!ctx)
        return 0;

    progressed |= rdma_drain_cm(ctx);
    rdma_drain_comp(ctx);
    /* The notification is only wanted when the queue has run dry, and it must
     * be requested before the poll: a completion already sitting in the queue
     * when the request lands produces no notification of its own. */
    if (!ctx->armed && ibv_req_notify_cq(ctx->cq, 0) == 0)
        ctx->armed = 1;
    progressed |= rdma_poll_cq(ctx);

    while (accepted && n_accepted && *n_accepted < max && ctx->handout_count) {
        struct rdma_conn *c = &ctx->conns[ctx->handout[ctx->handout_head]];
        ctx->handout_head = (ctx->handout_head + 1) % RDMA_CONN_MAX;
        ctx->handout_count--;
        if (!c->in_use || !c->handout_queued)
            continue;
        c->handout_queued = 0;
        if (c->state == RC_DEAD) {
            rdma_release(c);
            continue;
        }
        c->handed_out = 1;
        accepted[(*n_accepted)++] = c;
        progressed = 1;
    }
    /* Inbound failures before ESTABLISHED never entered the handout queue and
     * have no upper owner that could close them. Reclaim them here. */
    for (uint32_t i = 0; i < RDMA_CONN_MAX; i++) {
        struct rdma_conn *c = &ctx->conns[i];
        if (c->in_use && c->inbound && c->state == RC_DEAD &&
            !c->handout_queued && !c->handed_out)
            rdma_release(c);
    }
    return progressed;
}

static int rdma_established(void *wc)
{
    struct rdma_conn *c = wc;
    return c && c->in_use && c->state == RC_READY;
}

static int rdma_faulted(void *wc)
{
    struct rdma_conn *c = wc;
    return !c || !c->in_use || c->state == RC_DEAD;
}

static void rdma_close(void *wc)
{
    struct rdma_conn *c = wc;
    if (c)
        rdma_release(c);
}

static int rdma_epfd(void *wctx)
{
    struct rdma_ctx *ctx = wctx;
    return ctx ? ctx->epfd : -1;
}

static void rdma_ctx_free(void *wctx)
{
    struct rdma_ctx *ctx = wctx;
    if (!ctx)
        return;
    /* Stop creating work, then reject requests already queued before removing
     * the listener. A CONNECT_REQUEST owns a child id, so merely acknowledging
     * it would leak that id. */
    ctx->dead = 1;
    if (ctx->ec && ctx->ec_nonblock)
        (void)rdma_drain_cm(ctx);
    if (ctx->listener) {
        rdma_destroy_id(ctx->listener);
        ctx->listener = NULL;
    }
    if (ctx->ec && ctx->ec_nonblock)
        (void)rdma_drain_cm(ctx);
    for (uint32_t i = 0; i < RDMA_CONN_MAX; i++)
        rdma_release(&ctx->conns[i]);
    /* Destroying QPs flushes their work requests and may produce one last CQ
     * notification. A CQ destroy waits for every delivered event to be
     * acknowledged, so consume and acknowledge those before tearing it down. */
    if (ctx->cc && ctx->cq) {
        rdma_drain_comp(ctx);
        (void)rdma_poll_cq(ctx);
        rdma_drain_comp(ctx);         /* an allowed extra event may have no WC */
    }
    if (ctx->cq)
        ibv_destroy_cq(ctx->cq);
    if (ctx->cc)
        ibv_destroy_comp_channel(ctx->cc);
    if (ctx->pd)
        ibv_dealloc_pd(ctx->pd);
    if (ctx->ec)
        rdma_destroy_event_channel(ctx->ec);
    if (ctx->epfd >= 0)
        close(ctx->epfd);
    free(ctx);
}

static const struct peer_wire_ops RDMA_OPS = {
    .connect     = rdma_connect_op,
    .progress    = rdma_progress,
    .send_msg    = rdma_send_msg,
    .recv_msg    = rdma_recv_msg,
    .established = rdma_established,
    .faulted     = rdma_faulted,
    .close       = rdma_close,
    .epfd        = rdma_epfd,
    .ctx_free    = rdma_ctx_free,
};

/* ---- construction ------------------------------------------------------- */

static int fd_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int epoll_add(int epfd, int fd)
{
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = fd } };
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int peer_wire_rdma_new(uint32_t bind_ip_be, uint16_t port,
                       const struct peer_wire_ops **ops, void **wctx,
                       char *error, size_t error_len)
{
    if (error && error_len)
        error[0] = '\0';
    if (!ops || !wctx)
        return -1;
    *ops = NULL;
    *wctx = NULL;
    /* A wildcard bind leaves the identifier without a device, and the
     * protection domain, completion queue, and completion channel all have to
     * come from one. */
    if (bind_ip_be == INADDR_ANY) {
        snprintf(error, error_len,
                 "peer wire: rdma needs the local address of an rdma device");
        return -1;
    }

    struct rdma_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        snprintf(error, error_len, "peer wire: out of memory");
        return -1;
    }
    ctx->epfd = -1;
    ctx->bind_ip = bind_ip_be;

    char ipstr[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &bind_ip_be, ipstr, sizeof(ipstr));

    ctx->ec = rdma_create_event_channel();
    if (!ctx->ec) {
        snprintf(error, error_len, "peer wire: rdma_create_event_channel: %s",
                 strerror(errno));
        goto fail;
    }
    if (fd_nonblock(ctx->ec->fd) != 0) {
        snprintf(error, error_len, "peer wire: event channel nonblocking: %s",
                 strerror(errno));
        goto fail;
    }
    ctx->ec_nonblock = 1;
    if (rdma_create_id(ctx->ec, &ctx->listener, NULL, RDMA_PS_TCP) != 0) {
        snprintf(error, error_len, "peer wire: rdma_create_id: %s",
                 strerror(errno));
        goto fail;
    }
    int one = 1;
    rdma_set_option(ctx->listener, RDMA_OPTION_ID, RDMA_OPTION_ID_REUSEADDR,
                    &one, sizeof(one));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = bind_ip_be;
    addr.sin_port = htons(port);
    if (rdma_bind_addr(ctx->listener, (struct sockaddr *)&addr) != 0) {
        snprintf(error, error_len, "peer wire: rdma_bind_addr %s:%u: %s",
                 ipstr, port, strerror(errno));
        goto fail;
    }
    ctx->verbs = ctx->listener->verbs;
    if (!ctx->verbs) {
        snprintf(error, error_len, "peer wire: %s is not on an rdma device",
                 ipstr);
        goto fail;
    }
    ctx->pd = ibv_alloc_pd(ctx->verbs);
    if (!ctx->pd) {
        snprintf(error, error_len, "peer wire: ibv_alloc_pd: %s", strerror(errno));
        goto fail;
    }
    ctx->cc = ibv_create_comp_channel(ctx->verbs);
    if (!ctx->cc) {
        snprintf(error, error_len, "peer wire: ibv_create_comp_channel: %s",
                 strerror(errno));
        goto fail;
    }
    if (fd_nonblock(ctx->cc->fd) != 0) {
        snprintf(error, error_len, "peer wire: completion channel nonblocking: %s",
                 strerror(errno));
        goto fail;
    }
    ctx->cq = ibv_create_cq(ctx->verbs, RDMA_CQ_DEPTH, ctx, ctx->cc, 0);
    if (!ctx->cq) {
        snprintf(error, error_len, "peer wire: ibv_create_cq(%d): %s",
                 RDMA_CQ_DEPTH, strerror(errno));
        goto fail;
    }
    if (ibv_req_notify_cq(ctx->cq, 0) == 0)
        ctx->armed = 1;
    if (rdma_listen(ctx->listener, RDMA_BACKLOG) != 0) {
        snprintf(error, error_len, "peer wire: rdma_listen: %s", strerror(errno));
        goto fail;
    }
    struct sockaddr *bound = rdma_get_local_addr(ctx->listener);
    if (bound && bound->sa_family == AF_INET)
        ctx->port = ntohs(((struct sockaddr_in *)bound)->sin_port);
    else
        ctx->port = port;

    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) {
        snprintf(error, error_len, "peer wire: epoll_create1: %s", strerror(errno));
        goto fail;
    }
    if (epoll_add(ctx->epfd, ctx->ec->fd) != 0 ||
        epoll_add(ctx->epfd, ctx->cc->fd) != 0) {
        snprintf(error, error_len, "peer wire: epoll_ctl: %s", strerror(errno));
        goto fail;
    }

    *ops = &RDMA_OPS;
    *wctx = ctx;
    return 0;
fail:
    rdma_ctx_free(ctx);
    return -1;
}

uint16_t peer_wire_rdma_port(void *wctx)
{
    struct rdma_ctx *ctx = wctx;
    return ctx ? ctx->port : 0;
}
