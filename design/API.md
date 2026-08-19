# DPUmesh Native API

This document defines the current public contract of `<dpumesh/dmesh.h>`. The
ABI is `libdpumesh.so.4`. The interface uses RDMA-style channels, QPs, and
registered buffers with DPUmesh event queues. It is a reliable full-duplex byte
transport, not a remote-memory API.

## 1. Object and thread model

```text
process
└─ channel                         one transport and one registered-memory domain
   ├─ EQ                           one per polling thread
   │  └─ QP                        full-duplex connections owned by that EQ
   └─ TX/RX mappings               shared by the channel
```

Create objects in channel → EQ → QP order and destroy them in reverse order.
Destroying an EQ with live QPs, or a channel with live EQs, returns `EBUSY`
without partially tearing the object down.

An EQ has exactly one consumer. A QP's transmit calls form one serial stream
that may run on a different thread than its EQ's consumer — the POSIX shim
transmits from application threads, the gRPC adapter from endpoint executors —
and the caller serializes that stream against itself and against the QP's
destruction. The library serializes that stream against the buffered-tail
publication `dmesh_poll_eq()` performs. The library's own progress thread is the
single producer of EQ readiness. `qp->user_data` belongs entirely to the
application.

The public surface consists of nineteen calls:

| Group | Calls |
|---|---|
| Channel | `create_channel`, `destroy_channel`, `pod_id`, `msg_max`, `post_max` |
| EQ | `create_eq`, `destroy_eq`, `eq_fd`, `eq_next_deadline_ns` |
| QP | `create_qp`, `destroy_qp`, `abort_qp` |
| TX | `alloc`, `post_send`, `flush`, `tx_inflight` |
| Events | `poll_eq`, `release_rx_buffer` |
| Diagnostics | `get_tx_stats` |

## 2. Channel lifecycle

`dmesh_create_channel()` resolves local identity, connects Comch, registers with
the DPU, exports the data-path mappings, and waits for an end-to-end readiness
barrier:

```text
control connection established
  → POD_REGISTER / POD_ASSIGNED           the DPU assigns this process a pod id
  → K rings + TX mmap + RX mmap imported  the DPU maps the process's memory
  → RING_ADD_ACK from every target EU     each accelerator unit confirms its ring
  → POD_INIT_RESULT(READY, L)             the channel is usable; L stripes granted
```

Registration is idempotent. While assignment or readiness is pending, the host
replays `POD_REGISTER` every 100 ms; the DPU returns the same pod id and replays
any terminal result. Missing DPA ring acknowledgements cause idempotent
`RING_ADD` replay every 10 ms. A channel is never returned in a half-ready state.
The overall initialization deadline is 30 seconds.

Graceful channel destruction sends `POD_UNREGISTER` every 100 ms until
`POD_QUIESCED` or the five-second local deadline. The host retains exported
memory until that barrier. Unexpected disconnect invokes DPU-side unpublication
and cleanup. A shared DMA-context fault restarts the worker context without
changing pod readiness; a current-generation payload batch receives one ordered
retry.

The replay timers exist only inside these transitions. After READY, the channel
does not poll registration state or emit periodic control messages. Unregister is
not required for data transfer; it is the graceful remote-reclaim barrier when
the channel is destroyed.

## 3. Connections and naming

`dmesh_create_qp(eq, service_name)` resolves a Kubernetes Service name through
the immutable process registry. QP creation is local; the first outbound data is
what causes DPU routing and backend connection creation. Inbound connections
arrive as `DMESH_EVENT_CONN_REQ`; their `event.qp` is already usable and permanently
bound to the EQ that accepted it.

Every QP is one reliable full-duplex byte stream. By default the DPU forwards
those bytes untouched (L4) and pins the stream to one backend. A Service assigned
to the DPU's L7 layer instead has its frames parsed there, and complete frames may
go to different backends; that adds no identifier to this API. The application
still receives one ordered sequence of byte fragments and does its own framing and
request correlation. DPUmesh exposes no numeric service, backend, or upstream id
here.

`dmesh_destroy_qp()` is graceful close: it submits the buffered tail, waits until
the DPU has finished reading every submitted byte, and only then sends FIN. It returns
any held RX credit and always frees the local QP. `dmesh_abort_qp()` instead
discards bytes that have not yet been submitted, waits for already-submitted bytes
to leave the DPU, then sends FIN when a peer exists so remote state can be
reclaimed. Already-submitted bytes cannot be recalled. That wait is bounded;
on a broken transport the close returns `-1/EBADMSG` without sending an overtaking
FIN. Either call may return `-1/EBADMSG`, and the pointer is invalid on every return.
Because one EQ poll can return several entries that name the same QP, defer
destruction until the whole returned batch has been processed.

## 4. TX: buffered sending and backpressure

The send API separates buffer reservation from submission so applications can
produce data directly in transport-owned memory:

```c
void *p = dmesh_alloc(qp, len);              /* reserve registered bytes */
memcpy(p, source, len);                       /* or produce in place */
dmesh_post_send(qp, p, len);                 /* commit the bytes */

/* repeat alloc/post as useful */
dmesh_flush(qp);                              /* submit all buffered bytes now */
```

`dmesh_alloc()` reserves one contiguous region of at most `dmesh_post_max()`
and opens a transmit call that `dmesh_post_send()` closes. Only one may be open
per QP: a second `dmesh_alloc()` or a `dmesh_flush()` while one is open returns
`EDEADLK`, and otherwise only `dmesh_destroy_qp()` or `dmesh_abort_qp()` ends
it. `dmesh_post_send()` requires the exact pointer returned by that allocation
and rejects an oversized post, a repeated post, or one with no open transmit
call. After a successful post, the application must no longer access the
committed bytes until the transport makes that storage available through a
later allocation.

The transport combines adjacent posts without creating message boundaries: a
QP remains an ordered byte stream. Complete transport units submit immediately.
An idle stream also submits its first partial unit immediately; while an earlier
unit is in flight, only the newest partial may be retained, and it is submitted
by a bounded internal deadline. `dmesh_flush()` forces that remainder earlier.
Applications do not drive this policy, must not depend on a particular physical
unit size, and have no `SEND_MORE` mode. `dmesh_tx_inflight()` reports the
stream's outstanding bytes for diagnostics and is not an input to application
batching policy.

Each QP has bounded outstanding-send capacity, and QPs also share the channel's
overall transmit capacity. The transport recovers capacity as previously
submitted data completes. These limits affect admission and readiness only; how
the transport partitions or submits the underlying memory is not part of the
API contract. Submission waits for a descriptor slot, so `dmesh_post_send()` and
`dmesh_flush()` may pause briefly while the DPU drains one; `dmesh_alloc()` never
does. If committed data cannot be submitted because of a transport fault,
`dmesh_post_send()` or `dmesh_flush()` returns `-1/EBADMSG`. A submission that
makes no progress for five seconds retires that descriptor queue for the
remaining life of the channel; every QP carried on it then fails the same way.

### Backpressure

`dmesh_alloc()` never sleeps or flushes. It returns:

| Result | Meaning |
|---|---|
| pointer | Reservation succeeded |
| `NULL/EAGAIN` | QP or channel transmit capacity is temporarily exhausted |
| `NULL/EINVAL` | Invalid length or QP |
| `NULL/EDEADLK` | A transmit call is already open on this QP |
| `NULL/ENOMEM` | Transport bookkeeping memory could not be allocated |

If allocation reaches backpressure while committed bytes remain buffered, the
library expedites their publication. For multi-QP reactors, park the write and
continue servicing other QPs rather than spinning on one connection.

`EAGAIN` may mean that the QP has reached its own outstanding-send limit or that
the channel's shared transmit capacity is temporarily exhausted. The API does
not distinguish these cases because the application handles both in the same
way: wait for progress, then retry the allocation.

Every `dmesh_alloc()` that returns `EAGAIN` automatically requests one readiness
notification for that QP; there is no separate arm call. When relevant capacity
becomes available, the owning EQ receives `DMESH_EVENT_TX_READY`. Closing the QP or
successfully allocating on a direct retry cancels an obsolete request or hint.

TX readiness is a one-shot retry hint, not a guarantee that a particular
allocation will succeed. The event names the QP whose parked write should
retry, but shared capacity may be consumed before that retry. If it returns
`EAGAIN` again, that call has already requested the next notification.

Retry on that notification rather than on an application clock: park the blocked
QP, keep servicing the others, and resume it on `DMESH_EVENT_TX_READY`.

Retention stamps a deadline once and never moves it, and it holds until the
stream has nothing in flight. Full units still publish during that interval.
`dmesh_post_send()` publishes an overdue tail before retaining a new one, and
`dmesh_poll_eq()` publishes every overdue tail on its EQ on entry; a QP whose
owner is inside a transmit call keeps its retention for a later pass.
Acknowledgement reclaims capacity, and the one that leaves a QP with nothing in
flight also arms a tail that is still retained, so a stream that stops writing
still reaches its deadline. Explicit flush, allocation pressure, and graceful
close force it.

`dmesh_eq_next_deadline_ns()` reports when the loop must next run. A channel
timer writes the readiness fd of an EQ whose earliest tail has come due, at most
once per tick per EQ; it touches no transmit state and parks while no tail is
retained.

A synchronous submission fault is returned by `dmesh_post_send()` or
`dmesh_flush()`. A deferred tail fault latches a sticky error on the QP and
emits one `DMESH_EVENT_TX_ERROR`; subsequent TX calls fail with that error until
the QP is destroyed.

## 5. RX and EQ notification

`dmesh_poll_eq()` is nonblocking and returns five event types:

| Type | Meaning | Credit |
|---|---|---|
| `DMESH_EVENT_CONN_REQ` | New inbound QP | none |
| `DMESH_EVENT_RECV` | One RX fragment | held until `dmesh_release_rx_buffer()` |
| `DMESH_EVENT_RECV_FIN` | Peer EOF | none |
| `DMESH_EVENT_TX_READY` | An `EAGAIN`-blocked QP should retry allocation | none |
| `DMESH_EVENT_TX_ERROR` | Deferred tail submission failed; QP TX is terminal | none |

`event.buf` points directly into the channel RX mmap. An event is a transport
fragment, not an application message boundary; parsers must retain framing state
across events and may decode several frames from one logical byte stream.
Copy bytes before release if they must outlive the event.
`dmesh_release_rx_buffer()` is idempotent and remains valid after QP destruction because
the credit belongs to the channel.

The event fields have one meaning across all event types:

| Field | Contract |
|---|---|
| `qp` | QP that owns the event; the newly accepted QP for `CONN_REQ` |
| `type` | Event kind from the table above |
| `buf` | RX-mmap view for `RECV`; otherwise `NULL` |
| `len` | Fragment length for `RECV`; otherwise zero |
| `_rx_token` | Opaque release token; applications neither read nor modify it |

`dmesh_eq_fd()` exposes an optional eventfd for `poll`/`epoll`. Drain its counter,
poll the EQ to zero, then sleep again. Spin-polling clients do not need the fd.
The fd reports new connections, RX/FIN, and armed TX-ready transitions for every
QP owned by that EQ. It is one fd per EQ, not one fd per QP. Calling
`dmesh_eq_fd()` also self-kicks once so work queued while the EQ was poll-only
cannot be stranded when the application first goes to sleep.

A minimal event-driven owner loop has the following shape. Application-specific
connection state is normally stored in `qp->user_data`; closing a QP is deferred
until the entire returned batch has been dispatched.

```c
int eqfd = dmesh_eq_fd(eq);
add_to_epoll_once(eqfd, EPOLLIN);

/* When epoll reports EPOLLIN for eqfd: */
uint64_t counter;
if (read(eqfd, &counter, sizeof(counter)) < 0 && errno != EAGAIN)
    handle_fd_error();

dmesh_event_t events[64];
int n;
while ((n = dmesh_poll_eq(eq, events, 64)) > 0) {
    for (int i = 0; i < n; ++i) {
        switch (events[i].type) {
        case DMESH_EVENT_CONN_REQ:
            bind_connection(events[i].qp);
            break;
        case DMESH_EVENT_RECV:
            consume(events[i].qp, events[i].buf, events[i].len);
            dmesh_release_rx_buffer(channel, &events[i]);
            break;
        case DMESH_EVENT_RECV_FIN:
            mark_peer_eof(events[i].qp);
            break;
        case DMESH_EVENT_TX_READY:
            retry_parked_write(events[i].qp);
            break;
        case DMESH_EVENT_TX_ERROR:
            mark_transport_failed(events[i].qp);
            break;
        }
    }
    destroy_qps_marked_during_this_batch();
}
```

For `DMESH_EVENT_TX_READY` and `DMESH_EVENT_TX_ERROR`, `event.qp` is the affected
QP, `buf == NULL`, `len == 0`, and `_rx_token == -1`. Calling
`dmesh_release_rx_buffer()` on either is a harmless no-op.
The application should ignore a stale hint if that QP no longer has a parked
write.

## 6. POSIX and gRPC facades

`libdmesh_preload.so` is a POSIX adapter over the native data/event contract.
It uses `dmesh_alloc`/`dmesh_post_send` for TX and consumes
`DMESH_EVENT_RECV`, `DMESH_EVENT_RECV_FIN`, `DMESH_EVENT_TX_READY`, and
`DMESH_EVENT_TX_ERROR` from
`dmesh_poll_eq`. Internal hooks provide ClusterIP resolution, numeric QP
creation, transport FIN, and temporary EQ notification suppression.

The preload path copies POSIX `read`/`write` application buffers but owns no
batching list, scan, or timer. Its dispatcher waits on readiness edges alone.
Backpressure maps to kernel descriptor non-writability; shutdown forces pending
bytes before FIN, and graceful close uses the native close contract.

One process-wide EQ serves the mapped descriptors. Dispatcher and waiter drains
are serialized. A blocking receive may consume two EQ batches before parking.
Receive readiness is signalled once per descriptor per drained batch. EQ eventfd
writes are suppressed during waiter drains and restored when pending work remains.

On `EAGAIN`, the kernel descriptor suppresses `EPOLLOUT` until
`DMESH_EVENT_TX_READY`. Blocking writes park on that descriptor. A
`DMESH_EVENT_TX_ERROR` becomes a sticky socket I/O error. The POSIX application
does not select or observe the physical batch size.

The gRPC C++ adapter maps one runtime to channels, reactor shards to EQs, and
EventEngine endpoints to QPs. Client bootstrap accepts a Service-name target,
credentials, and `grpc::ChannelArguments`; absent authority defaults to the
target. Each EventEngine `Connect` creates a targeted QP. Established L4 streams
remain backend-pinned and terminate when that backend is lost. TLS and HTTP/2
remain end-to-end unless the Service is assigned to the DPU's L7 layer.

The adapter uses `dmesh_alloc`/`dmesh_post_send` for TX, calls `dmesh_flush`
once at each EventEngine Write boundary, and consumes
`DMESH_EVENT_RECV`, `DMESH_EVENT_RECV_FIN`, `DMESH_EVENT_CONN_REQ`,
`DMESH_EVENT_TX_READY`, and `DMESH_EVENT_TX_ERROR` from `dmesh_poll_eq`. One
EventEngine Write commits every slice; consecutive slices share one reservation.
The flush is a logical force operation; physical batch state and deadlines remain
in libdpumesh, and the reactor bounds its poll wait with
`dmesh_eq_next_deadline_ns()`. Receives are copied
out before `dmesh_release_rx_buffer`, and the credit is withheld, up to a
per-connection cap, while the endpoint's queued bytes exceed its high-water mark.
On `EAGAIN` the adapter parks the write and resumes it from
`DMESH_EVENT_TX_READY`; it has no retry or batching timer.
`DMESH_EVENT_TX_ERROR` fails the endpoint and closes the QP after the current
event batch.

Adapter-internal ownership and threading are specified in
[`GRPC.md`](GRPC.md).

## 7. Explicit limits

- No arbitrary memory registration, rkey, one-sided READ, or one-sided WRITE.
- No application-visible send completion; protocol ACKs reclaim internal TX
  capacity.
- The registry is loaded once and is not live-reloaded.
- Dynamic instances require a Service already present in the registry.
- L4 passthrough is the default gRPC mode; a Service assigned to the L7 layer is
  terminated there. The in-tree L7 validator uses a bounded 16-byte
  length-prefixed benchmark frame, not HTTP/2.
