#ifndef DMESH_L7_H
#define DMESH_L7_H

/* Adapter contract between the DPUmesh datapath and the DPU-side L7 layer.
 *
 * Every entry point is called on one ARM worker thread and never crosses to
 * another: `worker_id` names that thread, and state reached through it must
 * stay thread-local. `conn` is an opaque handle issued by DPUmesh; it is not a
 * pointer, because connection objects are recycled and a late call must not
 * name a different connection.
 *
 * Symbols named `l7_*` are implemented by the L7 layer. Symbols named
 * `dmesh_l7_*` are implemented by DPUmesh.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How far this connection involves the L7 layer, decided per service by
 * DPUmesh. `decision` connections deliver no payload: they are answered once by
 * l7_resolve. `opaque` carries a byte stream with no message boundaries;
 * `full` carries one whose boundaries the L7 layer parses. */
#define DMESH_L7_MODE_DECISION 1
#define DMESH_L7_MODE_OPAQUE   2
#define DMESH_L7_MODE_FULL     3

/* Why l7_conn_open declined. Any negative value declines and the data plane
 * forwards at L4; these name the cause, so a fallback is counted by reason
 * rather than as one total. A layer that has no reason to give returns
 * DMESH_L7_DECLINE_ERROR. */
#define DMESH_L7_DECLINE_ERROR          (-1)  /* the layer could not take it */
#define DMESH_L7_DECLINE_NOT_ATTACHED   (-2)  /* no runtime on this worker */
#define DMESH_L7_DECLINE_MODE           (-3)  /* the mode carries nothing for it */
#define DMESH_L7_DECLINE_SESSION_LIMIT  (-4)  /* the layer already holds this session */
#define DMESH_L7_DECLINE_UNKNOWN_REPLY  (-5)  /* no open session this reply belongs to */

/* One connection's identity. The L7 layer routes on socket addresses; DPUmesh
 * routes on pod and service identifiers, so both are carried. */
struct dmesh_l7_flow {
    uint32_t src_ip,   dst_ip;      /* host byte order; dst = original destination */
    uint16_t src_port, dst_port;
    int32_t  src_pod;               /* DPUmesh routing key */
    int32_t  dst_service;
    /* The other end of this session. A request names the pod it came from in
     * `src_pod`; its replies arrive as a separate connection, and `peer_pod`
     * with `dst_port` is what lets the L7 layer recognise them as the same
     * session it already opened. */
    int32_t  peer_pod;
    uint8_t  mode;                  /* DMESH_L7_MODE_* */
    uint8_t  is_reply;              /* the backend-to-client direction */
    char     workload[64];          /* NUL-terminated; fixed array, never a pointer */
};

/* The `conn` handle, as DPUmesh forms it: the sending pod in the high bits, its
 * port in the low. A reply names the same pair through `peer_pod` and
 * `dst_port`, which is how the L7 layer recognises the session it already
 * opened. Both sides compute the handle, so the formula lives here rather than
 * in either one of them. */
static inline uint64_t dmesh_l7_conn_handle(int32_t pod, uint16_t port) {
    return ((uint64_t)(uint8_t)pod << 16) | (uint64_t)port;
}
static inline int32_t dmesh_l7_handle_pod(uint64_t conn) {
    return (int32_t)(int8_t)(conn >> 16);
}
static inline uint16_t dmesh_l7_handle_port(uint64_t conn) {
    return (uint16_t)conn;
}

/* Leave the choice of backend to the data plane's own balancer. A reply
 * direction is always routed by conntrack and ignores any choice. */
#define DMESH_L7_BACKEND_ANY (-1)

/* Return the bytes to where this connection came from instead of forwarding
 * them onward. Both directions carry the connection's own delivery sequence,
 * so one connection may mix them. */
#define DMESH_L7_ORIGIN (-2)

/* ---- DPUmesh calls, the L7 layer implements ---- */

/* Build this worker's runtime. Negative on failure. */
int  l7_worker_attach(int worker_id);

/* Advance the runtime by one step. 1 if progress was made, else 0. The worker
 * loop owns the iteration and the 1 ms backstop; this must not block, and the
 * L7 layer must not arm a timer of its own. */
int  l7_worker_step(int worker_id);

/* Open a connection. Negative rejects it, and the datapath falls back to L4
 * forwarding for that connection; DMESH_L7_DECLINE_* names the cause. */
int  l7_conn_open(int worker_id, uint64_t conn, const struct dmesh_l7_flow *flow);

/* Arrived payload at `base + pos`, `len` bytes. The region is shared staging:
 * it stays valid until the matching dmesh_l7_release and must not be retained
 * past it. Returns the number of bytes taken, in [0, len]; the remainder is
 * offered again later. Negative poisons the connection. */
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);

void l7_conn_eof  (int worker_id, uint64_t conn);   /* peer half-closed */
void l7_conn_close(int worker_id, uint64_t conn);   /* drop every reference to this conn */
void l7_worker_detach(int worker_id);

/* ---- The L7 layer calls, DPUmesh implements ---- */

/* The backends currently live for a service, so the L7 layer can choose among
 * them. Returns the count written, at most `max`. */
int  dmesh_l7_backends(int worker_id, int32_t service, int32_t *out, int max);

/* Send produced bytes to a chosen backend. Returns the number of bytes
 * accepted, in [0, len]; 0 means the egress arena is momentarily full and the
 * caller should retry. Negative is terminal for the connection.
 *
 * One call becomes one delivery, so a message handed over whole is delivered
 * whole. A short accept splits it, which the receiver sees as two deliveries. */
int  dmesh_l7_send(int worker_id, uint64_t conn, int32_t backend_pod,
                   const uint8_t *buf, size_t len);

/* Borrow egress memory and encode into it directly, so bytes the L7 layer
 * produces are never copied again. Returns NULL when the arena is momentarily
 * full; *cap receives the writable size. One reservation per connection at a
 * time, released by the commit below. */
uint8_t *dmesh_l7_tx_reserve(int worker_id, uint64_t conn, uint32_t *cap);

/* Publish the first `len` bytes of the reservation, or return it unused when
 * `len` is zero. Returns the bytes published, or negative on failure. */
int  dmesh_l7_tx_commit(int worker_id, uint64_t conn, int32_t backend_pod,
                        uint32_t len);

/* Report consumption of a prefix of the outstanding segments, in the order they
 * were handed over. Until this call the bytes stay valid and the sender's slot
 * is not returned, so omitting it stalls the connection. */
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);

/* ---- `decision` mode ---- */

/* Answer one query per connection instead of carrying its payload. */
struct dmesh_l7_verdict { int allow; int32_t backend_pod; };

int  l7_resolve(int worker_id, const struct dmesh_l7_flow *flow,
                struct dmesh_l7_verdict *out);

/* Per-connection load, so balancer state stays accurate for connections whose
 * bytes never traversed the L7 layer. */
void l7_report (int worker_id, uint64_t conn, uint64_t bytes_in,
                uint64_t bytes_out, uint64_t duration_ns, int reason);

#ifdef __cplusplus
}
#endif

#endif /* DMESH_L7_H */
