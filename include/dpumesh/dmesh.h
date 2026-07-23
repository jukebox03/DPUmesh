/* DPUmesh native C API.
 *
 * The API exposes channels, single-consumer event queues, full-duplex QPs,
 * registered TX reservations, zero-copy RX events, and one-shot TX-ready
 * notifications. See design/API.md for the complete contract. */
#ifndef DMESH_H
#define DMESH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "dmesh_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal transport context. */
struct dpumesh_ctx;

/* ===== Handles ===== */

/* One transport channel per process. */
typedef struct dmesh_channel {
    struct dpumesh_ctx *ctx;
    int            pod_id;
    int            slot_size;   /* max body per RECV event (wire DMA cap) */
    int            block_size;  /* max contiguous alloc/post reservation */
} dmesh_channel_t;

/* Single-consumer event queue. */
typedef struct dmesh_eq dmesh_eq_t;

/* One persistent full-duplex service byte stream. */
typedef struct dmesh_qp {
    dmesh_channel_t *ep;
    dmesh_eq_t      *eq;       /* fixed event queue */
    void     *user_data;       /* application-owned context */
    int       role;            /* DMESH_ROLE_CLIENT (connect) | DMESH_ROLE_SERVER (accept) */

    /* addressing */
    uint16_t  local_port;      /* my port (this conn's id) */
    int16_t   dst_service;     /* peer service (the service connected to / caller's) */
    int16_t   remote_pod;      /* SERVER: the DPU-facing peer pod (learned at accept).
                                * CLIENT: always DMESH_POD_BLANK (Model B never pins). */
    uint16_t  remote_port;     /* SERVER: the peer uP (learned at accept); CLIENT: 0. */
    /* Independent inbound and outbound close state. */
    uint8_t   peer_closed;     /* INBOUND: the peer's FIN landed -> EOF (sticky) */
    uint8_t   fin_sent;        /* OUTBOUND: our FIN is on the wire (sticky) */
    uint16_t  seq;             /* per-QP outbound descriptor counter */

    /* inbound view (rx_slot>=0 => one fragment is held on the conn; the compat
     * layer's partial-read cursor lives here too) */
    int            rx_slot;    /* landing byte-offset in host RX buffer; -1 = none */
    const uint8_t *rx_buf;
    uint32_t       rx_len;
    uint32_t       rx_pos;

    /* Outbound bytes live in the per-conn TX byte-ring (keyed by local_port);
     * the conn holds no TX buffer state of its own. */
} dmesh_qp_t;

/* ===== Events ===== */

typedef enum {
    DMESH_EVENT_RECV     = 1, /* one inbound byte-stream fragment; holds an RX credit */
    DMESH_EVENT_RECV_FIN = 2, /* peer closed the conn (EOF); no credit held */
    DMESH_EVENT_CONN_REQ = 3, /* new inbound conn (server side); no credit held */
    DMESH_EVENT_TX_READY = 4, /* an EAGAIN-blocked QP should retry dmesh_alloc */
} dmesh_event_type_t;

typedef struct dmesh_event {
    dmesh_qp_t         *qp;       /* the QP this event belongs to (== the handle from
                                   * dmesh_create_qp / CONN_REQ; use qp->user_data for
                                   * your per-conn context) */
    dmesh_event_type_t  type;
    const uint8_t      *buf;      /* RECV: points INTO the RX mmap (zero-copy); valid
                                   * until dmesh_release_rx_buffer. Else NULL. */
    uint32_t            len;      /* RECV: byte-stream fragment length. Else 0. */
    int32_t             _rx_token; /* internal release token; -1 = nothing held */
} dmesh_event_t;

/* ===== Channel lifecycle (ibv_open_device + PD) ===== */

/* Create a channel using $DPUMESH_SERVICE identity. NULL indicates failure. */
dmesh_channel_t *dmesh_create_channel(void);

/* Destroy an idle channel. Returns EBUSY while an EQ remains. Safe on NULL. */
int dmesh_destroy_channel(dmesh_channel_t *s);

int dmesh_pod_id(dmesh_channel_t *s);      /* this node's DPU-assigned pod_id */
int dmesh_msg_max(dmesh_channel_t *s);     /* max length arriving as ONE RECV (slot_size) */
int dmesh_post_max(dmesh_channel_t *s);    /* max length of one alloc/post (block size) */

/* ===== Event queue lifecycle ===== */

/* Create an EQ for one polling thread. Returns ENOMEM or EMFILE on failure. */
dmesh_eq_t *dmesh_create_eq(dmesh_channel_t *ch);

/* Destroy an idle EQ. Returns EBUSY while a QP remains. Safe on NULL. */
int dmesh_destroy_eq(dmesh_eq_t *eq);

/* Optional eventfd. Drain it on wake, then call dmesh_poll_eq() until empty. */
int dmesh_eq_fd(dmesh_eq_t *eq);

/* ===== Connection setup (rdma_cm) ===== */

/* Create a client QP for a registered service name. Returns ENOENT or ENOMEM. */
dmesh_qp_t *dmesh_create_qp(dmesh_eq_t *eq, const char *service_name);

/* Flush, send FIN, release RX credit, and destroy the QP. The pointer is invalid
 * on return. Defer destruction until the current EQ batch is fully dispatched. */
int dmesh_destroy_qp(dmesh_qp_t *c);

/* Discard unsent bytes, send FIN when connected, and destroy the QP. */
int dmesh_abort_qp(dmesh_qp_t *c);

/* ===== Send (ibv_post_send) ===== */

/* Reserve `len` contiguous bytes in registered TX memory. Returns NULL with:
 *   EAGAIN   QP window or shared block pool exhausted. DMESH_EVENT_TX_READY is armed.
 *   EINVAL   invalid length or unestablished connection. */
void *dmesh_alloc(dmesh_qp_t *c, uint32_t len);

/* Commit bytes from the current dmesh_alloc() reservation and submit complete
 * transport batches. `buf` must match the reservation and `len` must fit it.
 * Returns EINVAL for invalid input or EBADMSG for a submission fault. Ownership
 * transfers to the transport on success. */
int dmesh_post_send(dmesh_qp_t *c, const void *buf, uint32_t len);

/* Submit all committed bytes, including the trailing partial batch. A descriptor
 * fault returns EBADMSG; no pending data is a no-op. */
int dmesh_flush(dmesh_qp_t *c);

/* ===== Diagnostics ===== */

/* Cumulative TX block-pool allocation, return, reuse, wait, and padding counters. */
typedef struct dmesh_tx_stats {
    unsigned long long pool_grabs;
    unsigned long long pool_returns;
    unsigned long long recycle_hits;
    unsigned long long grow_waits;
    unsigned long long block_pads;
} dmesh_tx_stats_t;
void dmesh_get_tx_stats(dmesh_channel_t *s, dmesh_tx_stats_t *out);

/* ===== Event polling ===== */

/* Nonblocking single-consumer poll. Returns up to max_events events, 0 when empty,
 * or EINVAL. Per-connection order is preserved across partial batches. */
int dmesh_poll_eq(dmesh_eq_t *eq, dmesh_event_t *events, int max_events);

/* Release the zero-copy RX buffer held by a RECV event and invalidate event->buf.
 * Idempotent, valid after QP closure, and a no-op for other event types. */
void dmesh_release_rx_buffer(dmesh_channel_t *s, dmesh_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DMESH_H */
