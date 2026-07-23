/* Native allocation, posting, event polling, and RX-buffer release API.
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

/* ===== Event queue ===== */

/* Emit one event for a landed message descriptor. A 0-length body is the
 * wire FIN marker: its landing credit is returned here (nothing to hand out) and
 * the conn latches EOF. */
static void eq_emit(dmesh_channel_t *s, dmesh_qp_t *c, dmesh_event_t *event,
                    int32_t slot, uint32_t body_len) {
    event->qp = c;
    if (body_len == 0) {                     /* FIN → EOF event */
        dpumesh_rx_free(s->ctx, slot);
        c->peer_closed = 1;
        event->type      = DMESH_EVENT_RECV_FIN;
        event->buf       = NULL;
        event->len       = 0;
        event->_rx_token = -1;
        return;
    }
    event->type      = DMESH_EVENT_RECV;
    event->buf       = dpumesh_rx_buf(s->ctx, slot);
    event->len       = body_len;
    event->_rx_token = slot;                 /* held until release_rx_buffer */
}

/* Drain one connection into events[]. `drained` reports whether its inbox emptied.
 * An accept-held first message is emitted before queued messages. */
static int eq_drain_conn(dmesh_channel_t *s, dmesh_qp_t *c,
                         dmesh_event_t *events, int max_events, int *drained) {
    int n = 0;
    *drained = 0;

    if (c->rx_slot >= 0) {                   /* accept-held first message */
        if (n >= max_events) return n;
        eq_emit(s, c, &events[n++], c->rx_slot, c->rx_len);
        c->rx_slot = -1; c->rx_buf = NULL; c->rx_len = 0; c->rx_pos = 0;
    }

    for (;;) {
        if (n >= max_events) return n;       /* events[] full; inbox may hold more */
        sw_descriptor_t d;
        if (!dpumesh_conn_recv(s->ctx, c->local_port, &d)) break;
        eq_emit(s, c, &events[n++], d.body_buf_slot, d.body_len);
    }
    *drained = 1;
    return n;
}

int dmesh_poll_eq(dmesh_eq_t *eq, dmesh_event_t *events, int max_events) {
    if (!eq || !events || max_events <= 0) { errno = EINVAL; return -1; }
    dmesh_channel_t *s = eq->ch;
    int n = 0, drained;

    /* 1. Resume the conn cut off by events[] filling up last call. Its inbox never went
     * empty, so the ready list holds no fresh edge for it — this cursor is the only
     * path back (dmesh_destroy_qp clears the cursor if the conn dies). */
    if (eq->drain_cur) {
        dmesh_qp_t *c = eq->drain_cur;
        n += eq_drain_conn(s, c, events + n, max_events - n, &drained);
        if (!drained) return n;
        eq->drain_cur = NULL;
    }

    /* 2. New inbound conns off the SHARED accept queue (SPMC — sibling EQs may be
     * popping it too; whichever wins one owns it). One CONN_REQ, then the conn's
     * already-landed messages (held first message + any pipelined ones coalesced while
     * it was SERVER_PENDING — those predate the promote, so they too never re-edge). */
    while (n < max_events) {
        errno = 0;
        dmesh_qp_t *c = dmesh_accept(eq);
        if (!c) {
            if (errno == ENOMEM) continue;   /* dropped entry consumed; keep draining */
            break;                           /* EAGAIN: accept queue empty */
        }
        events[n].qp        = c;
        events[n].type      = DMESH_EVENT_CONN_REQ;
        events[n].buf       = NULL;
        events[n].len       = 0;
        events[n]._rx_token = -1;
        n++;
        n += eq_drain_conn(s, c, events + n, max_events - n, &drained);
        if (!drained) { eq->drain_cur = c; return n; }
    }

    /* 3. QPs whose automatically armed alloc(EAGAIN) became retryable. TX readiness
     * is deliberately emitted before bulk RX so a read-heavy EQ cannot starve writes.
     * It is a hint, not a reservation; the application retries only the named QP. */
    while (n < max_events) {
        dmesh_qp_t *c = (dmesh_qp_t *)dpumesh_next_tx_ready(eq);
        if (!c) break;
        events[n].qp        = c;
        events[n].type      = DMESH_EVENT_TX_READY;
        events[n].buf       = NULL;
        events[n].len       = 0;
        events[n]._rx_token = -1;
        n++;
    }

    /* 4. This EQ's established conns with inbound (edge-armed by the PE). A spurious
     * entry (inbox already drained via 1/2) emits nothing — harmless. */
    while (n < max_events) {
        dmesh_qp_t *c = dmesh_next_ready(eq);
        if (!c) break;
        n += eq_drain_conn(s, c, events + n, max_events - n, &drained);
        if (!drained) { eq->drain_cur = c; return n; }
    }
    return n;
}

void dmesh_release_rx_buffer(dmesh_channel_t *s, dmesh_event_t *event) {
    if (!s || !event || event->_rx_token < 0) return;
    dpumesh_rx_free(s->ctx, event->_rx_token);
    event->_rx_token = -1;                  /* idempotent */
    event->buf = NULL;
}
