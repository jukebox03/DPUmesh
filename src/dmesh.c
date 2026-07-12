/*
 * dmesh.c — implementation of the socket/epoll-style façade declared in
 * <dpumesh/dmesh.h>.
 *
 * Previously the façade was header-only (all functions `static inline` in
 * dmesh.h). It now lives here, compiled into libdpumesh.so, so the façade follows
 * the same header-declares / src-implements split as the core dpumesh_* API
 * (dmesh_core.h → dmesh_core.c) and the LD_PRELOAD shim (dmesh_preload.c). The
 * two structs and all prototypes stay public in dmesh.h; only conn_free_rx and
 * dmesh_emit_desc are file-local (they are façade internals).
 */
#include <dpumesh/dmesh.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* ===== internal helpers (file-local) ===== */

/* Return the held RX-landing credit and clear the inbound view. */
static void conn_free_rx(dmesh_conn_t *c) {
    if (c->rx_slot >= 0) dpumesh_rx_free(c->ep->ctx, c->rx_slot);
    c->rx_slot = -1; c->rx_buf = NULL; c->rx_len = 0; c->rx_pos = 0;
}

/* Build the oriented tuple for one outbound descriptor of this conn (client →
 * service with per-message LB, or server → its learned peer), stamped with the
 * conn's route-affinity group. `moff` = byte offset in the shared TX mmap, `len`
 * = descriptor length. seq++. Returns 0, or -1 (EBADMSG) on enqueue fault. */
static int dmesh_emit_desc(dmesh_conn_t *c, size_t moff, uint32_t len) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    c->seq++;
    sw_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.body_buf_slot = (int32_t)moff;                 /* BYTE offset into the TX mmap */
    d.body_len      = len;
    d.src_port      = c->local_port;
    d.seq           = c->seq;
    d.dst_service   = c->dst_service;
    if (c->role == DMESH_ROLE_CLIENT) { d.dst_pod = DMESH_POD_BLANK; d.dst_port = DMESH_PORT_BLANK; }
    else                              { d.dst_pod = c->remote_pod;   d.dst_port = c->remote_port; }
    d.route_group = c->pin_group;                    /* 0 = per-message LB per descriptor */
    d.valid = 1;
    if (dpumesh_enqueue(ctx, &d) < 0) { errno = EBADMSG; return -1; }
    return 0;
}

/* ===== Endpoint lifecycle ===== */

dmesh_channel_t *dmesh_create_channel(int service_id) {
    dmesh_channel_t *s = (dmesh_channel_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    dpumesh_config_t cfg = DPUMESH_CONFIG_DEFAULT;
    if (dpumesh_init(&s->ctx, service_id, &cfg) != 0 || !s->ctx) {
        free(s);
        return NULL;
    }
    s->pod_id    = dpumesh_get_pod_id(s->ctx);   /* DPU-assigned (valid after init) */
    s->slot_size = dpumesh_get_slot_size(s->ctx);
    return s;
}

void dmesh_destroy_channel(dmesh_channel_t *s) {
    if (!s) return;
    if (s->ctx) dpumesh_destroy(s->ctx);
    free(s);
}

int dmesh_pod_id(dmesh_channel_t *s)  { return s->pod_id; }
int dmesh_msg_max(dmesh_channel_t *s) { return s->slot_size; }
int dmesh_event_fd(dmesh_channel_t *s) { return dpumesh_get_event_fd(s->ctx); }

/* ===== Connection setup ===== */

dmesh_conn_t *dmesh_accept(dmesh_channel_t *s) {
    sw_descriptor_t req;
    if (dpumesh_dequeue(s->ctx, &req, 0) < 0 || !req.valid) {
        errno = EAGAIN;
        return NULL;
    }
    dmesh_conn_t *c = (dmesh_conn_t *)calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; dpumesh_rx_free(s->ctx, req.body_buf_slot); return NULL; }
    /* Model B: the PE created a SERVER_PENDING slot at message-1 delivery (port =
     * req.dst_port = uP, with any pipelined messages 2..P already coalesced in its
     * inbox). Promote it to a live SERVER conn and attach THIS handle so
     * dmesh_next_ready returns it. */
    uint16_t ps = dpumesh_accept_port(s->ctx, req.dst_port, c);
    if (ps == 0) { dpumesh_rx_free(s->ctx, req.body_buf_slot); free(c); errno = ENOMEM; return NULL; }

    c->ep          = s;
    c->role        = DMESH_ROLE_SERVER;
    c->local_port  = ps;                 /* == req.dst_port == uP */
    c->remote_pod  = req.src_pod;        /* learned peer (for replies + further sends) */
    c->remote_port = req.src_port;
    c->dst_service = req.src_service;
    c->seq         = 0;
    c->rx_slot     = req.body_buf_slot;  /* the first message (held; read returns it) */
    c->rx_buf      = dpumesh_rx_buf(s->ctx, req.body_buf_slot);
    c->rx_len      = req.body_len;
    c->rx_pos      = 0;
    return c;
}

dmesh_conn_t *dmesh_connect(dmesh_channel_t *s, int dst_service_id) {
    dmesh_conn_t *c = (dmesh_conn_t *)calloc(1, sizeof(*c));
    if (!c) { errno = ENOMEM; return NULL; }
    uint16_t pc = dpumesh_alloc_port(s->ctx, DMESH_ROLE_CLIENT, c);   /* c = the port's handle */
    if (pc == 0) { free(c); errno = ENOMEM; return NULL; }
    c->ep          = s;
    c->role        = DMESH_ROLE_CLIENT;
    c->local_port  = pc;
    c->dst_service = (int16_t)dst_service_id;
    c->remote_pod  = DMESH_POD_BLANK;
    c->remote_port = DMESH_PORT_BLANK;
    c->seq         = 0;
    c->rx_slot     = -1;
    return c;
}

void dmesh_pin_route(dmesh_conn_t *c) {
    if (c->pin_group != 0) return;
    uint32_t g = __atomic_fetch_add(&c->ep->next_group, 1u, __ATOMIC_RELAXED);
    c->pin_group = (uint8_t)((g % 255u) + 1u);           /* 1..255, never 0 */
}

dmesh_conn_t *dmesh_next_ready(dmesh_channel_t *s) {
    return (dmesh_conn_t *)dpumesh_next_ready(s->ctx);
}

/* ===== read / write / send ===== */

ssize_t dmesh_read(dmesh_conn_t *c, void *buf, size_t len) {
    if (c->peer_closed) return 0;             /* EOF is sticky once the FIN arrived */
    if (c->rx_slot < 0) {                     /* no message loaded → fetch the next */
        sw_descriptor_t d;
        if (!dpumesh_conn_recv(c->ep->ctx, c->local_port, &d)) { errno = EAGAIN; return -1; }
        if (d.body_len == 0) {                /* FIN marker → EOF: reclaim its landing, latch closed */
            dpumesh_rx_free(c->ep->ctx, d.body_buf_slot);
            c->peer_closed = 1;
            return 0;
        }
        c->rx_slot = d.body_buf_slot;
        c->rx_buf  = dpumesh_rx_buf(c->ep->ctx, d.body_buf_slot);
        c->rx_len  = d.body_len;
        c->rx_pos  = 0;
        /* Model B: a CLIENT does NOT learn/pin a peer — it keeps addressing its
         * service (dst_pod=BLANK) and the DPU owns the upstream. A SERVER conn
         * already learned its peer (client_pod, uP) at accept. So there is no
         * learn-on-read here. */
    }
    size_t avail = c->rx_len - c->rx_pos;
    size_t n = (len < avail) ? len : avail;
    if (n && c->rx_buf) memcpy(buf, c->rx_buf + c->rx_pos, n);
    c->rx_pos += (uint32_t)n;
    if (c->rx_pos >= c->rx_len)           /* message consumed → free credit, next read fetches a new one */
        conn_free_rx(c);
    return (ssize_t)n;
}

ssize_t dmesh_write(dmesh_conn_t *c, const void *buf, size_t len) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t cap = (uint32_t)c->ep->slot_size;         /* reserve in <= slot_size runs */
    size_t done = 0;
    while (done < len) {
        uint32_t chunk = (len - done > cap) ? cap : (uint32_t)(len - done);
        uint8_t *dst = dpumesh_tx_reserve(ctx, c->local_port, chunk);   /* busy-spins on backpressure */
        if (!dst) return done ? (ssize_t)done : -1;    /* no region (conn not established) */
        memcpy(dst, p + done, chunk);
        dpumesh_tx_commit(ctx, c->local_port, chunk);  /* append to the send stream */
        done += chunk;
    }
    return (ssize_t)len;
}

ssize_t dmesh_sendfile(dmesh_conn_t *c, int in_fd, off_t *offset, size_t count) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    uint32_t cap = (uint32_t)c->ep->slot_size;
    if (count > cap) count = cap;                      /* one reserve <= slot_size */
    if (count == 0) { errno = EMSGSIZE; return -1; }
    uint8_t *dst = dpumesh_tx_reserve(ctx, c->local_port, (uint32_t)count);
    if (!dst) { errno = EAGAIN; return -1; }
    ssize_t n = offset ? pread(in_fd, dst, count, *offset) : read(in_fd, dst, count);
    if (n <= 0) return n;                              /* reserved but not committed → space reused */
    dpumesh_tx_commit(ctx, c->local_port, (uint32_t)n);
    if (offset) *offset += n;
    return n;
}

int dmesh_flush(dmesh_conn_t *c) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    size_t moff; uint32_t len;
    while (dpumesh_tx_next_send(ctx, c->local_port, &moff, &len)) {
        if (dmesh_emit_desc(c, moff, len) < 0) return -1;    /* EBADMSG; bytes stay committed */
        dpumesh_tx_sent(ctx, c->local_port, c->seq, len);    /* c->seq = the seq emit_desc used */
    }
    return 0;
}

/* ===== Zero-copy TX (dmesh_alloc / dmesh_commit) ===== */

void *dmesh_alloc(dmesh_conn_t *c, size_t len) {
    return dpumesh_tx_reserve(c->ep->ctx, c->local_port, (uint32_t)len);
}

int dmesh_commit(dmesh_conn_t *c, size_t len) {
    dpumesh_tx_commit(c->ep->ctx, c->local_port, (uint32_t)len);
    return 0;
}

void dmesh_send_fin(dmesh_conn_t *c) {
    dpumesh_ctx_t *ctx = c->ep->ctx;
    c->seq++;
    sw_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.body_buf_slot = 0;                                   /* 0-length FIN: offset unused (0-byte DMA) */
    d.body_len      = 0;                                   /* FIN marker (0-length) */
    d.src_port      = c->local_port;
    d.seq           = c->seq;
    d.dst_service   = c->dst_service;
    d.dst_pod       = c->remote_pod;                       /* the learned peer conn */
    d.dst_port      = c->remote_port;
    d.route_group   = c->pin_group;
    d.valid         = 1;
    /* Best-effort: rides after all prior data (seq order). Holds NO ring bytes, so it
     * needs no send-unit / reclaim — its TX_ACK is a harmless FIFO no-op. */
    dpumesh_enqueue(ctx, &d);
}

int dmesh_close(dmesh_conn_t *c) {
    if (!c) return 0;
    dpumesh_ctx_t *ctx = c->ep->ctx;
    dpumesh_tx_discard_unsent(ctx, c->local_port);        /* buffered, never flushed → drop */
    if (!c->peer_closed && (c->role == DMESH_ROLE_SERVER || c->seq > 0))
        dmesh_send_fin(c);
    conn_free_rx(c);                                       /* return the held RX credit */
    if (c->local_port) dpumesh_free_port(ctx, c->local_port);
    free(c);
    return 0;
}
