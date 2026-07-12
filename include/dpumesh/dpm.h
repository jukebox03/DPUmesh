/*
 * dpm.h — socket/epoll-style façade over the DPUmesh C API.
 *
 * DECLARATIONS ONLY; the implementation lives in src/dpm.c (compiled into
 * libdpumesh.so), matching the header-declares / src-implements split of the
 * core dpumesh_* API (dpumesh.h → dpumesh_doca.c). Include this header and link
 * -ldpumesh.
 *
 *   socket()/bind()/listen()  ->  dmesh_create_channel()      (registers a service)
 *   accept()                  ->  dmesh_accept()              (one new conn)
 *   connect()                 ->  dmesh_connect(svc)          (one new conn)
 *   read()/recv()             ->  dmesh_read()                (next inbound message)
 *   write()/send()            ->  dmesh_write() + dmesh_flush()  (buffer, then ship)
 *   close()                   ->  dmesh_close()
 *   epoll on ONE channel fd   ->  native epoll on dmesh_event_fd(s)   (the only fd)
 *   epoll_wait readiness list ->  dmesh_accept() (new conns) + dmesh_next_ready() (data)
 *
 * CONNECTION-ORIENTED, FULL-DUPLEX (TCP-like), NOT request/response:
 *   - A `dmesh_conn_t` is a persistent connection, alive until close. The transport
 *     delivers ALL inbound messages on a conn to the app — it does NO request↔
 *     response matching (that's the app's job, if it wants RPC semantics).
 *   - MODEL B (the DPU owns every connection): a CLIENT addresses only a SERVICE.
 *     `dmesh_connect(svc)` is local (no round-trip, no pod chosen). Every
 *     `write+flush` ships with dst_pod=BLANK and the DPU load-balances it PER
 *     MESSAGE to a backend, owning the upstream. The client NEVER learns or pins a
 *     pod — it keeps addressing the service. A backend `dmesh_accept()`s a conn the
 *     DPU created to it (learning its DPU-facing peer), replies to it, and the DPU
 *     maps the reply back to the client.
 *   - Ordering: in send order only on a connection to ONE backend. Under per-message
 *     LB a single conn's replies can arrive OUT OF ORDER, so if you keep several
 *     requests outstanding, carry a req-id in the body and match on it (never on
 *     arrival order). Single-outstanding (write→flush→read one at a time) is
 *     structurally paired and needs no correlation.
 *   - Teardown: dmesh_close() sends a FIN (a zero-length message on the same conn),
 *     which rides behind all prior data and makes the peer's read() return 0 (EOF);
 *     the peer then closes, reclaiming its slot. read()==0 ⇒ peer closed ⇒ close.
 *     (A user 0-length send is a no-op; zero length on the wire is the FIN alone.)
 *     Concurrent close is safe: a FIN landing on an already-freed conn is dropped.
 *   - Each message is one whole <= slot_size (8 KB) body, delivered atomically.
 *   - NON-BLOCKING: read/accept/next_ready return EAGAIN/NULL when nothing is ready;
 *     sleep on ONE channel fd (dmesh_event_fd). On wake, drain dmesh_accept() (new
 *     conns) then dmesh_next_ready() (conns with inbound — the PE names them, so no
 *     scan, no per-conn fd). SEND IS EXPLICIT: write buffers, flush ships.
 */
#ifndef DPM_H
#define DPM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#include "dpumesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Handles ===== */

/* Endpoint — one per process. Wraps one DOCA device context + this node's service. */
typedef struct dmesh_channel {
    dpumesh_ctx_t *ctx;
    int            pod_id;
    int            slot_size;   /* cached max body size */
    uint32_t       next_group;  /* GLOBAL rolling route-affinity id source (atomic across
                                 * all conns → 1..255): concurrent large messages get
                                 * DISTINCT groups. A per-conn counter would collide —
                                 * every conn's first large message would pick id 1. */
} dmesh_channel_t;

/* Connection — a persistent full-duplex link to one peer. */
typedef struct dmesh_conn {
    dmesh_channel_t *ep;
    void     *user_data;       /* APP-OWNED (like epoll's data.ptr): you set it, and
                                * dmesh_next_ready hands this conn back so you read it.
                                * The transport never touches it. */
    int       role;            /* DMESH_ROLE_CLIENT (connect) | DMESH_ROLE_SERVER (accept) */

    /* addressing */
    uint16_t  local_port;      /* my port (this conn's id) */
    int16_t   dst_service;     /* peer service (the service connected to / caller's) */
    int16_t   remote_pod;      /* SERVER: the DPU-facing peer pod (learned at accept).
                                * CLIENT: always DMESH_POD_BLANK (Model B never pins). */
    uint16_t  remote_port;     /* SERVER: the peer uP (learned at accept); CLIENT: 0. */
    uint8_t   peer_closed;      /* received the peer's FIN → reads return EOF (sticky) */
    uint16_t  seq;             /* per-conn OUTBOUND message counter */

    /* inbound (the message currently being read out; rx_slot>=0 ⇒ one is loaded) */
    int            rx_slot;    /* landing byte-offset in host RX buffer; -1 = none */
    const uint8_t *rx_buf;
    uint32_t       rx_len;
    uint32_t       rx_pos;

    /* Outbound bytes live in the CORE per-conn TX byte-ring (keyed by local_port):
     * dmesh_write reserves+commits into it; dmesh_flush ships committed bytes as
     * ≤slot_size descriptors. The conn holds no TX buffer state of its own. */

    /* CONNECTION-level route affinity (dmesh_pin_route): 0 = per-message LB (default,
     * bit-identical to the pre-pin behavior). Non-zero = a route_group stamped on
     * EVERY outbound message of this conn (and its FIN), so the DPU pins the whole
     * conn to the ONE backend picked for its first message — restoring socket-like
     * total order on the conn (per-message LB is forgone by design). Group ids are a
     * per-channel rolling 255-space, so unrelated conns/channels reuse a byte; the
     * DPU keys its pin table by (dst_service, id), so a collision can only merge
     * SAME-SERVICE traffic onto one backend (balance skew, ordering intact) — it can
     * never redirect a conn to another service's backend. */
    uint8_t   pin_group;
} dmesh_conn_t;

/* ===== Endpoint lifecycle ===== */

/* service_id = the service this node advertises (DMESH_SVC_NONE for a pure
 * client). The node's pod_id (its address) is ASSIGNED BY THE DPU at register —
 * the caller never picks it; dmesh_pod_id() returns the assigned value. Returns
 * NULL on init failure. */
dmesh_channel_t *dmesh_create_channel(int service_id);

/* Release the channel + all DOCA resources. Safe on NULL. */
void dmesh_destroy_channel(dmesh_channel_t *s);

int dmesh_pod_id(dmesh_channel_t *s);   /* this node's DPU-assigned pod_id */
int dmesh_msg_max(dmesh_channel_t *s);  /* max body size (slot_size) */

/* The ONE channel fd: readable when a NEW connection is pending (accept) OR any
 * conn has inbound (next_ready). Calling it enables readiness delivery. */
int dmesh_event_fd(dmesh_channel_t *s);

/* ===== Connection setup ===== */

/* accept(): NON-BLOCKING. Pops the next NEW connection from the accept queue,
 * allocates a SERVER conn that learns the peer (pod,port), and returns it holding
 * the first message body. NULL+EAGAIN if none pending; NULL+ENOMEM on alloc
 * failure (the message is dropped, its RX credit reclaimed). */
dmesh_conn_t *dmesh_accept(dmesh_channel_t *s);

/* connect(): bind a CLIENT conn to a logical SERVICE, addressed by its service_id
 * (the same id the backend passed to dmesh_create_channel). Local; no round-trip.
 * The conn is established (peer learned) on its first inbound. NULL+ENOMEM on OOM. */
dmesh_conn_t *dmesh_connect(dmesh_channel_t *s, int dst_service_id);

/* Pin this connection's outbound routing to ONE backend (connection-level LB).
 * Claims a route-affinity group from the channel's global id source and stamps it
 * on every subsequent message (and the FIN): the DPU routes the first message by
 * normal LB, records the pick in its route_group table, and every later message
 * reuses it (dpu_route) — so replies arrive in send order, like a socket. Call it
 * right after dmesh_connect, before any write. Idempotent. Meaningless on a
 * SERVER conn (it already sends to its learned peer). Used by the LD_PRELOAD
 * socket shim, where byte-stream total order is part of the contract. */
void dmesh_pin_route(dmesh_conn_t *c);

/* Pop the next conn that has inbound, from the channel's ready list (the PE puts
 * ready conns here, so there is NO scan and NO per-conn fd). Returns the SAME conn
 * handle you created at accept/connect, or NULL when drained. After waking on
 * dmesh_event_fd(s), loop dmesh_next_ready() and drain each returned conn to EAGAIN.
 * Single-consumer: call it from your one event-loop thread. */
dmesh_conn_t *dmesh_next_ready(dmesh_channel_t *s);

/* ===== read / write / send ===== */

/* read(): return up to `len` bytes of the NEXT inbound message on this conn.
 * One message is atomic (<= slot_size); a single read with a big enough buffer
 * returns the whole message. When a message is fully consumed its RX credit is
 * freed and the next read fetches a new message. Learns the peer on first inbound.
 *   >0 bytes, 0 = EOF (peer closed the conn via FIN; sticky), -1 = EAGAIN.
 * A zero-length inbound message IS the FIN marker (user 0-length sends are no-ops),
 * so read()==0 means the peer closed — close this conn (like a BSD socket EOF). */
ssize_t dmesh_read(dmesh_conn_t *c, void *buf, size_t len);

/* write(): APPEND outbound body bytes to this conn's TX byte-ring (shipped by flush).
 * Any length — the bytes just accumulate in the ring; dmesh_flush later carves the
 * committed run into <=slot_size wire descriptors (coalescing). There is no size
 * limit and no EMSGSIZE. Ordering on a connection to ONE backend is send order;
 * across backends (only when a service has several) use dmesh_pin_route for socket
 * order. The receiver is a plain dmesh_read loop (it frames its own length). */
ssize_t dmesh_write(dmesh_conn_t *c, const void *buf, size_t len);

/* sendfile(): append up to count bytes from in_fd into the current message. */
ssize_t dmesh_sendfile(dmesh_conn_t *c, int in_fd, off_t *offset, size_t count);

/* flush(): SHIP the conn's committed-but-unsent bytes. REQUIRED to send. The core
 * byte-ring carves [send, commit) into ≤slot_size descriptors — coalescing many small
 * writes into fewer, bigger host→DPU DMAs — and this posts each with the conn's tuple
 * + route group, recording it so its DPU TX_ACK reclaims the ring bytes. A CLIENT ships
 * dst_pod=BLANK (per-message LB per descriptor; pin the conn for socket order); a SERVER
 * ships to its learned peer. Nothing committed → NO-OP.
 *   0 = sent (or nothing to send); -1 EBADMSG = descriptor fault (close the conn). */
int dmesh_flush(dmesh_conn_t *c);

/* ===== Zero-copy TX (dmesh_alloc / dmesh_commit) =====
 * Fill transport DMA memory directly instead of memcpy'ing through dmesh_write. */

/* dmesh_alloc(c, len): reserve `len` CONTIGUOUS bytes in this conn's TX byte-ring and
 * return a pointer into transport DMA memory to fill DIRECTLY (no memcpy). Then
 * dmesh_commit(c, actual_len) + dmesh_flush(c). Busy-spins under backpressure; NULL if
 * len==0, len exceeds the per-conn ring, or the conn is not established. Zero-copy
 * counterpart of dmesh_write — the same byte-ring, filled in place. */
void *dmesh_alloc(dmesh_conn_t *c, size_t len);

/* dmesh_commit(c, len): finalize `len` (<= the alloc'd len) bytes at the write head as
 * committed message bytes, ready for dmesh_flush. Returns 0. */
int dmesh_commit(dmesh_conn_t *c, size_t len);

/* Low-level: send a FIN — a zero-length message addressed to the established peer.
 * It rides the SAME conn-shard ring as this conn's data (src_port), so it arrives
 * AFTER every prior message (ordering preserved). The peer's PE delivers it to the
 * conn inbox as a 0-length descriptor → the peer's read() returns EOF → the peer
 * closes → its port slot is reclaimed (no cross-run conn accumulation).
 * Best-effort: if no TX slot is free we skip the FIN; the peer/DPU then keep that
 * conn + upstream until the port/uP is reused (there is NO idle reaper — apply your
 * own wall-clock timeout for a service that never answers). A CLIENT FINs to its
 * service (dst_pod=BLANK; the DPU frees the upstream it created); a SERVER FINs to
 * its learned peer. Called by dmesh_close(); also exposed for the LD_PRELOAD shim's
 * shutdown(SHUT_WR) half-close. */
void dmesh_send_fin(dmesh_conn_t *c);

/* close(): graceful close. Drops any buffered-but-unflushed bytes, then (if established)
 * sends a FIN so the peer + DPU-owned upstream tear down (no leak across runs). Shipped-
 * but-un-ACKed bytes are reclaimed by their own TX_ACKs (the region returns once drained).
 * Reclaims held RX credit. Safe on NULL. Returns 0. */
int dmesh_close(dmesh_conn_t *c);

/* ===== Large messages (> slot_size) — transparent, NO special API =====
 * There is no write_large/read_large. A payload larger than one slot is handled by
 * plain dmesh_write + dmesh_flush: the bytes buffer in the conn's TX byte-ring and
 * flush ships them as <=slot_size wire descriptors, IN SEND ORDER on a connection to
 * one backend. The receiver is a plain dmesh_read LOOP: read chunks in arrival order
 * and concatenate until you have the length YOUR protocol declares (framing is the
 * app's job, like a byte stream). If a service load-balances across SEVERAL backends,
 * dmesh_pin_route(c) first so the whole conn stays on one backend and the chunks stay
 * in order. bench/bench_sock.c (mode=3) is the worked example. */

/* ===== Event-loop integration =====
 * ONE fd: dmesh_event_fd(s). Register it in a vanilla epoll set; it becomes readable
 * when a new conn is pending OR any conn has inbound. On EPOLLIN: drain the fd
 * (read() a uint64_t), then loop dmesh_accept() for new conns and dmesh_next_ready()
 * for conns with data (drain each to EAGAIN). No per-conn fds, no dmesh_epoll_*
 * wrappers — the PE-published ready list replaces both the scan and the per-conn fd. */

#ifdef __cplusplus
}
#endif

#endif /* DPM_H */
