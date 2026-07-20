# DPUmesh Native API

This document defines the public contract of `<dpumesh/dmesh.h>` as implemented
on 2026-07-20. The ABI is `libdpumesh.so.2`. The interface borrows the useful
shape of RDMA verbs—channel, completion queue, QP, registered buffers—but it is
a reliable full-duplex byte transport, not a remote-memory API.

## 1. Object and thread model

```text
process
└─ channel                         one transport and one registered-memory domain
   ├─ CQ                           one per polling thread
   │  └─ QP                        full-duplex connections owned by that CQ
   └─ TX/RX mappings               shared by the channel
```

Create objects in channel → CQ → QP order and destroy them in reverse order.
Destroying a CQ with live QPs, or a channel with live CQs, returns `EBUSY`
without partially tearing the object down.

A CQ has exactly one consumer. QP operations and CQ polling should run on that
CQ's owner thread; create more CQs to scale across threads. The transport PE is a
separate single producer of CQ-ready edges. `qp->user_data` belongs entirely to
the application.

The public surface consists of seventeen calls:

| Group | Calls |
|---|---|
| Channel | `create_channel`, `destroy_channel`, `pod_id`, `msg_max`, `post_max` |
| CQ | `create_cq`, `destroy_cq`, `cq_fd` |
| QP | `create_qp`, `destroy_qp`, `abort_qp` |
| TX | `alloc`, `post_send`, `flush` |
| RX | `poll_cq`, `wc_release` |
| Diagnostics | `get_tx_stats` |

## 2. Channel lifecycle

`dmesh_create_channel()` resolves local identity, connects Comch, registers with
the DPU, exports the data-path mappings, and waits for an end-to-end readiness
barrier:

```text
Comch RUNNING
  → POD_REGISTER / POD_ASSIGNED
  → K rings + TX mmap + RX mmap imported
  → generation-matched RING_ADD_ACK from every target EU
  → POD_INIT_RESULT(READY)
```

Registration is idempotent. While assignment or readiness is pending, the host
replays `POD_REGISTER` every 100 ms; the DPU returns the same pod id and replays
any terminal result. Missing DPA ring acknowledgements cause idempotent
`RING_ADD` replay every 10 ms. A channel is never returned in a half-ready state.
The overall initialization deadline is 30 seconds.

Graceful channel destruction sends `POD_UNREGISTER` every 100 ms until
`POD_QUIESCED` or the five-second local deadline. The host retains exported
memory until that barrier. Unexpected disconnect still invokes DPU-side cleanup,
but forced process death during an already-issued DMA is outside the graceful
safety claim.

The replay timers exist only inside these transitions. After READY, the channel
does not poll registration state or emit periodic control messages. Unregister is
not required for data transfer; it is the graceful remote-reclaim barrier when
the channel is destroyed.

## 3. Connections and naming

`dmesh_create_qp(cq, service_name)` resolves a Kubernetes Service name through
the immutable process registry. QP creation is local; the first outbound data is
what causes DPU routing and backend connection creation. Inbound connections
arrive as `DMESH_WC_CONN_REQ`; their `wc.qp` is already usable and permanently
bound to the CQ that accepted it.

Default L4 passthrough pins a connection to one backend and preserves byte order.
A service codec may route messages to several upstreams; in that mode the
application reassembles independently by `wc.stream`. DPUmesh does not expose a
numeric service id in this API.

`dmesh_destroy_qp()` is graceful close: it submits the buffered tail before FIN,
returns any held RX credit, and always frees the local QP. `dmesh_abort_qp()`
instead discards bytes that have not yet been submitted, then sends FIN when a
peer exists so remote state can be reclaimed. Already-submitted bytes cannot be
recalled. Either call may return `-1/EBADMSG`, and the pointer is invalid on every
return. Because one CQ poll can return several entries that name the same QP,
defer destruction until the whole returned batch has been processed.

## 4. TX: batching is the default

The send protocol is deliberately three-stage:

```c
void *p = dmesh_alloc(qp, len);              /* reserve registered bytes */
memcpy(p, source, len);                       /* or produce in place */
dmesh_post_send(qp, p, len, 0, 0);           /* commit; full units auto-submit */

/* repeat alloc/post as useful */
dmesh_flush(qp);                              /* force the newest partial now */
```

`dmesh_alloc()` reserves one contiguous region of at most `dmesh_post_max()`.
Only one unposted allocation may exist per QP. `dmesh_post_send()` requires the
exact pointer returned by that allocation, rejects an oversized or repeated post,
and requires `flags == 0`. `wr_id` is reserved. A successful post transfers byte
ownership to the transport and automatically submits every newly complete
transport batch. Usually one newest fillable partial remains buffered.

`dmesh_flush()` forces every remaining committed byte, which normally means just
that trailing partial. Small adjacent posts are batched automatically, but callers
choose flush boundaries by latency or protocol semantics rather than by transport
size. The current data plane uses 8 KiB physical units internally; that value is
not a send-side application contract. There is no `SEND_MORE` mode.

Submission is descriptor publication to a memory ring continuously consumed by
the DPA. `post_send()` and `flush()` do not make a send syscall, wake a transport
thread, or send a separate doorbell message. Batching changes how many adjacent
bytes one descriptor covers, not whether a syscall is issued.

The registered TX mapping is divided into physical blocks. A QP borrows blocks
from a channel-wide pool as its logical byte ring grows and reuses or returns them
after ACK reclamation. A block is allocation and reclamation storage, not an
application message and not necessarily one DPA descriptor: several posts and
several 8 KiB transport units can occupy one block. The block count per QP is a
fixed configured ceiling, not a dynamically tuned target.

The per-QP block window is clamped so its worst-case physical-unit carve fits the
64-entry reclaim FIFO. Consequently a successfully committed window is always
flushable without another per-QP admission step. The shared host-to-DPU ring may
still apply bounded backoff; a ring stalled for its deadline returns
`-1/EBADMSG`.

### Backpressure

`dmesh_alloc()` never sleeps or flushes. It returns:

| Result | Meaning |
|---|---|
| pointer | Reservation succeeded |
| `NULL/EAGAIN` | Per-QP window or process block pool has no space now |
| `NULL/EINVAL` | Invalid length, QP, or outstanding-allocation state |
| `NULL/ENOMEM` | Lazy per-QP bookkeeping allocation failed |

Complete batches already generate ACKs. If allocation still reaches backpressure
with a trailing partial buffered, flush that tail before waiting for more space.
For multi-QP reactors, park the write and continue servicing other QPs rather
than spinning on one connection.

`EAGAIN` has two resource causes. The QP may have reached its configured block
window, in which case its own forward ACK must make a whole block reusable or
fully drain the window. Alternatively, every registered block in the channel's
shared pool may currently be held by other QPs, even though this QP remains below
its own ceiling. These causes are deliberately not exposed as separate errors;
the correct application action is identical.

Every `dmesh_alloc()` that returns `EAGAIN` automatically arms one notification
for that QP. There is no public `dmesh_arm_writable()` call. The core snapshots
the failed allocation state, publishes the arm, and rechecks the relevant ACK or
pool generation, closing the reclaim-before-arm race. A qualifying QP reclaim
queues `DMESH_WC_TX_READY`; a returned shared block wakes at most one valid pool
waiter because it represents one new unit of capacity. Close and a successful
direct retry cancel a stale arm or queued hint.

TX readiness is one-shot and edge-driven, not a permanent statement that a QP is
writable. The completion names the QP whose parked write should retry, but does
not reserve a block for it. Shared capacity can be consumed before the retry; if
the retry returns `EAGAIN`, the same call has already armed the next notification.
Polling applications may retry opportunistically without waiting, while sleeping
event loops need neither a timer nor a scan of all QPs.

There is no internal partial-batch timer. Code that needs bounded latency calls
`dmesh_flush()` at its logical write boundary; graceful close also flushes, while
`dmesh_abort_qp()` discards the unsent partial.

## 5. RX and CQ notification

`dmesh_poll_cq()` is nonblocking and returns four completion types:

| Opcode | Meaning | Credit |
|---|---|---|
| `DMESH_WC_CONN_REQ` | New inbound QP | none |
| `DMESH_WC_RECV` | One RX fragment | held until `dmesh_wc_release()` |
| `DMESH_WC_RECV_FIN` | Peer EOF | none |
| `DMESH_WC_TX_READY` | An `EAGAIN`-blocked QP should retry allocation | none |

`wc.buf` points directly into the channel RX mmap. Copy it before release if the
data must outlive the completion. `dmesh_wc_release()` is idempotent and remains
valid after QP destruction because the credit belongs to the channel.

`dmesh_cq_fd()` exposes an optional eventfd for `poll`/`epoll`. Drain its counter,
poll the CQ to zero, then sleep again. Spin-polling clients do not need the fd.
The fd reports new connections, RX/FIN, and armed TX-ready transitions for every
QP owned by that CQ. It is one fd per CQ, not one fd per QP. Calling
`dmesh_cq_fd()` also self-kicks once so work queued while the CQ was poll-only
cannot be stranded when the application first goes to sleep.

A minimal event-driven owner loop has the following shape. Application-specific
connection state is normally stored in `qp->user_data`; closing a QP is deferred
until the entire returned batch has been dispatched.

```c
int cqfd = dmesh_cq_fd(cq);
add_to_epoll_once(cqfd, EPOLLIN);

/* When epoll reports EPOLLIN for cqfd: */
uint64_t counter;
if (read(cqfd, &counter, sizeof(counter)) < 0 && errno != EAGAIN)
    handle_fd_error();

dmesh_wc_t wc[64];
int n;
while ((n = dmesh_poll_cq(cq, wc, 64)) > 0) {
    for (int i = 0; i < n; ++i) {
        switch (wc[i].opcode) {
        case DMESH_WC_CONN_REQ:
            bind_connection(wc[i].qp);
            break;
        case DMESH_WC_RECV:
            consume(wc[i].qp, wc[i].buf, wc[i].len, wc[i].stream);
            dmesh_wc_release(channel, &wc[i]);
            break;
        case DMESH_WC_RECV_FIN:
            mark_peer_eof(wc[i].qp);
            break;
        case DMESH_WC_TX_READY:
            retry_parked_write(wc[i].qp);
            break;
        }
    }
    destroy_qps_marked_during_this_batch();
}
```

For `DMESH_WC_TX_READY`, `wc.qp` is the retry target and `buf == NULL`,
`len == 0`, `stream == 0`, and `rx_slot == -1`. Calling
`dmesh_wc_release()` on it is a harmless no-op. The application should ignore a
stale hint if that QP no longer has a parked write.

## 6. POSIX and gRPC facades

`libdmesh_preload.so` and the native API are sibling facades over the same core.
The preload path copies because POSIX `read`/`write` require application buffers;
it flushes once at each POSIX write boundary. If one write exceeds the bounded
native TX window, it may flush an accepted prefix to make forward progress. The
POSIX application never selects or observes the physical batch size.

The gRPC C++ adapter maps one runtime to a channel, reactor shards to CQs, and
EventEngine endpoints to QPs. It commits all slices of one EventEngine Write and
flushes once at the logical write boundary. RX is copied into gRPC slices before
the native credit is returned. TLS and HTTP/2 framing remain end-to-end and are
not interpreted by DPUmesh. A reactor parks its exact slice cursor on `EAGAIN`
and resumes only that connection when its CQ produces `DMESH_WC_TX_READY`; the
adapter has no retry timer.

## 7. Explicit limits

- No arbitrary memory registration, rkey, one-sided READ, or one-sided WRITE.
- No application-visible send completion; protocol ACKs reclaim internal TX
  capacity.
- No automatic deadline for an unflushed partial batch yet.
- The registry is loaded once and is not live-reloaded.
- L4 passthrough is the supported gRPC mode; the simple message codec is not an
  HTTP/2 parser.
