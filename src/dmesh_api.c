/*
 * dmesh_api.c — implementation of the native API declared in <dpumesh/dmesh.h>.
 *
 * This file is ONLY the verbs-shaped data path: alloc, post_send, poll_cq,
 * wc_release. Everything else the header declares (channel + connection
 * lifecycle, flush, close) is transport and lives in dmesh_core.c, shared with
 * the LD_PRELOAD shim — the two surfaces are siblings on the core, neither
 * built on the other.
 *
 * Nothing here adds a thread, a lock, a syscall or a copy: every function is a
 * caller-thread re-arrangement of the core. RECV hands out a pointer straight
 * into the RX mmap; SEND hands out a pointer straight into the TX ring.
 */
#include "dmesh_core.h"

#include <errno.h>

/* ===== Send ===== */

void *dmesh_alloc(dmesh_qp_t *c, uint32_t len) {
    if (!c) { errno = EINVAL; return NULL; }
    return dpumesh_tx_reserve(c->ep->ctx, c->local_port, len);   /* NULL+EAGAIN if SQ full */
}

int dmesh_post_send(dmesh_qp_t *c, const void *buf, uint32_t len,
                    uint64_t wr_id, unsigned flags) {
    (void)buf;      /* the ring position is implied by the alloc contract */
    (void)wr_id;    /* reserved for future send-completion support */
    if (!c || len == 0) { errno = EINVAL; return -1; }
    dpumesh_tx_commit(c->ep->ctx, c->local_port, len);   /* finalize the alloc'd bytes */
    if (flags & DMESH_SEND_MORE) return 0;               /* doorbell deferred (WR batching) */
    return dmesh_flush(c);                               /* ship ALL committed, in order */
}

/* ===== Completion queue ===== */

/* Emit one completion for a landed message descriptor. A 0-length body is the
 * wire FIN marker: its landing credit is returned here (nothing to hand out) and
 * the conn latches EOF. */
static void cq_emit(dmesh_channel_t *s, dmesh_qp_t *c, dmesh_wc_t *w,
                    int32_t slot, uint32_t body_len) {
    w->qp = c;
    if (body_len == 0) {                     /* FIN → EOF completion */
        dpumesh_rx_free(s->ctx, slot);
        c->peer_closed = 1;
        w->opcode  = DMESH_WC_RECV_FIN;
        w->buf     = NULL;
        w->len     = 0;
        w->rx_slot = -1;
        return;
    }
    w->opcode  = DMESH_WC_RECV;
    w->buf     = dpumesh_rx_buf(s->ctx, slot);
    w->len     = body_len;
    w->rx_slot = slot;                       /* credit held until wc_release */
}

/* Drain one conn into wc[0..nwc). Returns the count emitted; *drained = 1 iff the
 * inbox was emptied (0 = wc[] filled first → the caller parks the conn in cq->vq_cur
 * and the next poll_cq resumes it, preserving per-conn order).
 *
 * The conn-held RX view goes FIRST: dmesh_accept returns the conn HOLDING its first
 * message (delivered before the promote, so the ready list will never re-edge for it
 * — the same trap dmesh_preload.c documents). Emitting it here transfers ownership to
 * the wc, so the view is cleared (a later dmesh_destroy_qp must not double-free the
 * credit). */
static int cq_drain_conn(dmesh_channel_t *s, dmesh_qp_t *c,
                         dmesh_wc_t *wc, int nwc, int *drained) {
    int n = 0;
    *drained = 0;

    if (c->rx_slot >= 0) {                   /* accept-held first message */
        if (n >= nwc) return n;
        cq_emit(s, c, &wc[n++], c->rx_slot, c->rx_len);
        c->rx_slot = -1; c->rx_buf = NULL; c->rx_len = 0; c->rx_pos = 0;
    }

    for (;;) {
        if (n >= nwc) return n;              /* wc[] full; inbox may hold more */
        sw_descriptor_t d;
        if (!dpumesh_conn_recv(s->ctx, c->local_port, &d)) break;
        cq_emit(s, c, &wc[n++], d.body_buf_slot, d.body_len);
    }
    *drained = 1;
    return n;
}

int dmesh_poll_cq(dmesh_cq_t *cq, dmesh_wc_t *wc, int nwc) {
    if (!cq || !wc || nwc <= 0) { errno = EINVAL; return -1; }
    dmesh_channel_t *s = cq->ch;
    int n = 0, drained;

    /* 1. Resume the conn cut off by wc[] filling up last call. Its inbox never went
     * empty, so the ready list holds no fresh edge for it — this cursor is the only
     * path back (dmesh_destroy_qp clears the cursor if the conn dies). */
    if (cq->vq_cur) {
        dmesh_qp_t *c = cq->vq_cur;
        n += cq_drain_conn(s, c, wc + n, nwc - n, &drained);
        if (!drained) return n;
        cq->vq_cur = NULL;
    }

    /* 2. New inbound conns off the SHARED accept queue (SPMC — sibling CQs may be
     * popping it too; whichever wins one owns it). One CONN_REQ, then the conn's
     * already-landed messages (held first message + any pipelined ones coalesced while
     * it was SERVER_PENDING — those predate the promote, so they too never re-edge). */
    while (n < nwc) {
        errno = 0;
        dmesh_qp_t *c = dmesh_accept(cq);
        if (!c) {
            if (errno == ENOMEM) continue;   /* dropped entry consumed; keep draining */
            break;                           /* EAGAIN: accept queue empty */
        }
        wc[n].qp    = c;
        wc[n].opcode  = DMESH_WC_CONN_REQ;
        wc[n].buf     = NULL;
        wc[n].len     = 0;
        wc[n].rx_slot = -1;
        n++;
        n += cq_drain_conn(s, c, wc + n, nwc - n, &drained);
        if (!drained) { cq->vq_cur = c; return n; }
    }

    /* 3. This CQ's established conns with inbound (edge-armed by the PE). A spurious
     * entry (inbox already drained via 1/2) emits nothing — harmless. */
    while (n < nwc) {
        dmesh_qp_t *c = dmesh_next_ready(cq);
        if (!c) break;
        n += cq_drain_conn(s, c, wc + n, nwc - n, &drained);
        if (!drained) { cq->vq_cur = c; return n; }
    }
    return n;
}

void dmesh_wc_release(dmesh_channel_t *s, dmesh_wc_t *wc) {
    if (!s || !wc || wc->rx_slot < 0) return;
    dpumesh_rx_free(s->ctx, wc->rx_slot);
    wc->rx_slot = -1;                        /* idempotent */
    wc->buf     = NULL;
}
