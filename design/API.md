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

The current API has no race-free writable completion. The gRPC adapter therefore
arms a short timer only after a write parks on `EAGAIN`. This retry timer is not a
batching timer and does not periodically transmit messages while the QP is
writable. Replacing it with a one-shot reclaim notification is the remaining
event-model gap.

There is no internal partial-batch timer. Code that needs bounded latency calls
`dmesh_flush()` at its logical write boundary; graceful close also flushes, while
`dmesh_abort_qp()` discards the unsent partial.

## 5. RX and CQ notification

`dmesh_poll_cq()` is nonblocking and returns three completion types:

| Opcode | Meaning | Credit |
|---|---|---|
| `DMESH_WC_CONN_REQ` | New inbound QP | none |
| `DMESH_WC_RECV` | One RX fragment | held until `dmesh_wc_release()` |
| `DMESH_WC_RECV_FIN` | Peer EOF | none |

`wc.buf` points directly into the channel RX mmap. Copy it before release if the
data must outlive the completion. `dmesh_wc_release()` is idempotent and remains
valid after QP destruction because the credit belongs to the channel.

`dmesh_cq_fd()` exposes an optional eventfd for `poll`/`epoll`. Drain its counter,
poll the CQ to zero, then sleep again. Spin-polling clients do not need the fd.
The fd reports inbound completions only; it does not currently report TX
writability.

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
not interpreted by DPUmesh.

## 7. Explicit limits

- No arbitrary memory registration, rkey, one-sided READ, or one-sided WRITE.
- No application-visible send completion; protocol ACKs reclaim internal TX
  capacity.
- No native writable event yet.
- No automatic deadline for an unflushed partial batch yet.
- The registry is loaded once and is not live-reloaded.
- L4 passthrough is the supported gRPC mode; the simple message codec is not an
  HTTP/2 parser.
