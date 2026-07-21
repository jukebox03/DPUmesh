/* Native allocation, posting, polling, and completion-release API.
 * Operations run on the caller thread and expose registered RX/TX memory. */
#include "dmesh_core.h"

#include <errno.h>

/* ===== Send ===== */

void *dmesh_alloc(dmesh_qp_t *c, uint32_t len) {
    if (!c) { errno = EINVAL; return NULL; }
    return dpumesh_tx_reserve(c->ep->ctx, c->local_port, len);
}

int dmesh_post_send(dmesh_qp_t *c, const void *buf, uint32_t len) {
    if (!c || len == 0) { errno = EINVAL; return -1; }
    /* Commit first, then publish every newly complete transport slot. The newest
     * partial stays buffered until more posts fill it or dmesh_flush forces it. */
    if (dpumesh_tx_commit(c->ep->ctx, c->local_port, buf, len) != 0) {
        errno = EINVAL;
        return -1;
    }
    return dmesh_flush_full(c);
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
        w->_rx_token = -1;
        return;
    }
    w->opcode  = DMESH_WC_RECV;
    w->buf     = dpumesh_rx_buf(s->ctx, slot);
    w->len     = body_len;
    w->_rx_token = slot;                     /* credit held until wc_release */
}

/* Drain one connection into wc[]. `drained` reports whether its inbox emptied.
 * An accept-held first message is emitted before queued messages. */
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
        wc[n].qp      = c;
        wc[n].opcode  = DMESH_WC_CONN_REQ;
        wc[n].buf     = NULL;
        wc[n].len     = 0;
        wc[n]._rx_token = -1;
        n++;
        n += cq_drain_conn(s, c, wc + n, nwc - n, &drained);
        if (!drained) { cq->vq_cur = c; return n; }
    }

    /* 3. QPs whose automatically armed alloc(EAGAIN) became retryable. TX readiness
     * is deliberately emitted before bulk RX so a read-heavy CQ cannot starve writes.
     * It is a hint, not a reservation; the application retries only the named QP. */
    while (n < nwc) {
        dmesh_qp_t *c = (dmesh_qp_t *)dpumesh_next_tx_ready(cq);
        if (!c) break;
        wc[n].qp      = c;
        wc[n].opcode  = DMESH_WC_TX_READY;
        wc[n].buf     = NULL;
        wc[n].len     = 0;
        wc[n]._rx_token = -1;
        n++;
    }

    /* 4. This CQ's established conns with inbound (edge-armed by the PE). A spurious
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
    if (!s || !wc || wc->_rx_token < 0) return;
    dpumesh_rx_free(s->ctx, wc->_rx_token);
    wc->_rx_token = -1;                      /* idempotent */
    wc->buf     = NULL;
}
