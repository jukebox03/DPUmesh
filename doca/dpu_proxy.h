#ifndef DPU_PROXY_H
#define DPU_PROXY_H

/* DPU L4 engine beneath the L7 routing hook.
 *
 * Ordered per-connection windows become {offset,length,destination} segments and
 * per-destination SG-DMA units. TX custody ends after egress DMA completion.
 * Requests use passthrough, frame, or service-selected L7 parsing; replies use
 * passthrough with conntrack destinations. DPUMESH_PROXY,
 * DPUMESH_PROXY_FRAME_SVC, and DPUMESH_PROXY_L7_SVC select request parsing. */

#include <stdint.h>

struct objects;

/* ---- proxy return format (design/CORE.md §5 — the deliverable) ---- */

struct dmesh_route_seg {
    uint32_t off;    /* start within this call's input window (buf) */
    uint32_t len;    /* segment length (> 0; a 0-length wire msg is the FIN) */
    int32_t  dst;    /* destination backend pod (LB already done by L7), or
                      * DMESH_SEG_DST_DEFER = let the L4 default route decide
                      * (conn stickiness, else RR over the live backend set). */
};

/* Defer sentinel — let the L4 default route (conn stickiness / RR) pick. */
#define DMESH_SEG_DST_DEFER (-2)

/* The proxy's view of one connection (one direction of one stream). The L4
 * engine owns the struct; the proxy reads it and may keep state in `user`. */
typedef struct dmesh_proxy_conn {
    int32_t  src_pod;      /* stream identity: sender pod ... */
    uint16_t src_port;     /* ... and sender port (client port, or uP) */
    int      is_reply;     /* 1 = upstream reply stream: dst is predetermined —
                            * peer_* below is the conntrack answer; the proxy
                            * confirms (returns peer_pod) rather than routing */
    int16_t  dst_service;  /* the service the client addressed (request streams) */
    int32_t  peer_pod;     /* reply streams: table-resolved destination (client) */
    uint16_t peer_port;    /* reply streams: the client's real conn port */
    void    *user;         /* proxy-owned per-conn state (L4 never touches it) */
} dmesh_proxy_conn;

/* Route an ordered contiguous input window. Segments must be ascending,
 * non-overlapping, and within [0,*consumed). Return a segment count or a
 * negative protocol error. Zero consumption requests a larger contiguous view. */
typedef int (*dmesh_proxy_route_fn)(struct objects *objs, dmesh_proxy_conn *conn,
                                    const uint8_t *buf, uint32_t avail,
                                    struct dmesh_route_seg *segs, int max,
                                    uint32_t *consumed);

/* Max segments the engine requests per proxy_route call. */
#define DMESH_PROXY_SEG_MAX 32

/* ---- engine lifecycle / hooks (called from dpu_worker.c) ---- */

/* Create the engine — ALWAYS, whatever DPUMESH_PROXY says: it is the sole DPU→host
 * reverse path, not an option. The env only picks the deploy-default REQUEST parser
 * (unset/passthru/1 → passthru, frame → frame-all). objs->proxy is non-NULL after a
 * DOCA_SUCCESS return; the caller aborts the worker on failure. Requires objs->dev +
 * objs->pe live. */
int px_init(struct objects *objs);

/* Ingest one COMP_ENTRY_FORWARD (data or FIN, request or reply).
 * `shard` selects its connection and routing state. Returns 1 = consumed, 0 = retry
 * (transient resource exhaustion; the caller keeps the completion queued),
 * -1 = dropped (sender TX_ACKed). */
int px_ingest_forward(struct objects *objs, int shard, void *entry /* dpu_comp_entry_t* */);

/* Re-parse the conns that px_ship_seg backpressured (a pool was momentarily empty, so
 * their bytes were left in the window rather than dropped). Call once per drain pass on
 * the thread owning `shard`, AFTER the egress has had a chance to free units. Returns
 * non-zero only if a conn made real progress, so an unrelieved pool lets the caller idle
 * and sleep. */
int px_drain_stalled(struct objects *objs, int shard);

/* ---- delivery counters (DIAG only; see dpu_diag_dump) ----
 * The proxy's two failure modes are invisible from the outside: a DROP loses bytes and
 * still TX_ACKs the sender, and a STALL looks exactly like a hang. Both are counted, and
 * these are the only way to read them — everything else is silent by design. */

/* Total backpressure stalls (unit + piece + upstream). A number that keeps CLIMBING
 * means a pool is chronically dry; a number that is merely non-zero is normal. */
uint64_t px_stall_total(struct objects *objs);

/* Append " px[...]" (msgs/segs/dropped-bytes/per-pool stalls) to `buf`. Returns the
 * bytes written, 0 if the proxy is not up. */
int px_diag_str(struct objects *objs, char *buf, int cap);

/* Owner shard encoded as (up_port - BASE) % M. */
int px_uport_owner(uint16_t up_port, int m);

/* One drain pass: submit per-destination SG-DMA batches, emit completed
 * batches' REV_DONE entries + custody TX_ACKs, kick credit refreshes.
 * Returns non-zero if any progress was made. */
int px_drain(struct objects *objs);

/* True only after the egress owner has stopped submitting for this dead pod,
 * every destination DMA/credit read has completed, all lane queues are empty,
 * and no worker→main completion still names the slot. The control path uses
 * this as the ARM half of POD_QUIESCED before destroying imported host mmaps. */
int px_pod_reclaim_ready(struct objects *objs, int pod_idx);

/* dpu_worker idle flushes the per-pod REV_DONE and TX_ACK batches. */

#endif /* DPU_PROXY_H */
