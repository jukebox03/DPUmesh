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

An EQ has exactly one consumer. QP operations and EQ polling should run on that
EQ's owner thread; create more EQs to scale across threads. The transport PE is a
separate single producer of EQ-ready edges. `qp->user_data` belongs entirely to
the application.

The public surface consists of seventeen calls:

| Group | Calls |
|---|---|
| Channel | `create_channel`, `destroy_channel`, `pod_id`, `msg_max`, `post_max` |
| EQ | `create_eq`, `destroy_eq`, `eq_fd` |
| QP | `create_qp`, `destroy_qp`, `abort_qp` |
| TX | `alloc`, `post_send`, `flush` |
| Events | `poll_eq`, `release_rx_buffer` |
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

`dmesh_create_qp(eq, service_name)` resolves a Kubernetes Service name through
the immutable process registry. QP creation is local; the first outbound data is
what causes DPU routing and backend connection creation. Inbound connections
arrive as `DMESH_EVENT_CONN_REQ`; their `event.qp` is already usable and permanently
bound to the EQ that accepted it.

Every QP is one reliable full-duplex byte stream. Default L4 passthrough pins that
stream to one backend. A service-selected DPU codec may recognize application
frames and route complete frames to different upstreams, but this does not add a
public stream identifier: the application still receives one ordered sequence of
byte fragments and performs its own framing and request correlation. DPUmesh does
not expose numeric service, backend, or upstream ids in this API.

`dmesh_destroy_qp()` is graceful close: it submits the buffered tail before FIN,
returns any held RX credit, and always frees the local QP. `dmesh_abort_qp()`
instead discards bytes that have not yet been submitted, then sends FIN when a
peer exists so remote state can be reclaimed. Already-submitted bytes cannot be
recalled. Either call may return `-1/EBADMSG`, and the pointer is invalid on every
return. Because one EQ poll can return several entries that name the same QP,
defer destruction until the whole returned batch has been processed.

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

`dmesh_alloc()` reserves one contiguous region of at most `dmesh_post_max()`.
Only one unposted allocation may exist per QP. `dmesh_post_send()` requires the
exact pointer returned by that allocation and rejects an oversized or repeated
post. After a successful post, the application must no longer access the committed
bytes until the transport makes that storage available through a later allocation.

The transport may combine adjacent posts before submitting them. This does not
create message boundaries: a QP remains an ordered byte stream. `dmesh_flush()`
asks the transport to submit all committed bytes currently buffered on the QP,
so callers should flush at latency-sensitive or protocol-defined write
boundaries. Applications must not depend on a particular batching size, and
there is no `SEND_MORE` mode.

Each QP has bounded outstanding-send capacity, and QPs also share the channel's
overall transmit capacity. The transport recovers capacity as previously
submitted data completes. These limits affect admission and readiness only; how
the transport partitions or submits the underlying memory is not part of the
API contract. If committed data cannot be submitted because of a transport
fault, `dmesh_post_send()` or `dmesh_flush()` returns `-1/EBADMSG`.

### Backpressure

`dmesh_alloc()` never sleeps or flushes. It returns:

| Result | Meaning |
|---|---|
| pointer | Reservation succeeded |
| `NULL/EAGAIN` | QP or channel transmit capacity is temporarily exhausted |
| `NULL/EINVAL` | Invalid length, QP, or outstanding-allocation state |
| `NULL/ENOMEM` | Transport bookkeeping memory could not be allocated |

If allocation reaches backpressure while committed bytes remain buffered, flush
them before waiting for more space. For multi-QP reactors, park the write and
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
`EAGAIN` again, that call has already requested the next notification. Polling
applications may retry opportunistically, while sleeping event loops can rely on
the EQ notification without running a timer or scanning every QP.

DPUmesh does not flush buffered data on a timer. Code that needs bounded latency
calls `dmesh_flush()` at its logical write boundary; graceful close also flushes,
while `dmesh_abort_qp()` discards buffered data that has not been submitted.

## 5. RX and EQ notification

`dmesh_poll_eq()` is nonblocking and returns four event types:

| Type | Meaning | Credit |
|---|---|---|
| `DMESH_EVENT_CONN_REQ` | New inbound QP | none |
| `DMESH_EVENT_RECV` | One RX fragment | held until `dmesh_release_rx_buffer()` |
| `DMESH_EVENT_RECV_FIN` | Peer EOF | none |
| `DMESH_EVENT_TX_READY` | An `EAGAIN`-blocked QP should retry allocation | none |

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
        }
    }
    destroy_qps_marked_during_this_batch();
}
```

For `DMESH_EVENT_TX_READY`, `event.qp` is the retry target, `buf == NULL`,
`len == 0`, and `_rx_token == -1`. Calling `dmesh_release_rx_buffer()` on it is
a harmless no-op.
The application should ignore a stale hint if that QP no longer has a parked
write.

## 6. POSIX and gRPC facades

`libdmesh_preload.so` is a POSIX adapter over the native data/event contract.
It uses `dmesh_alloc`/`dmesh_post_send`/`dmesh_flush` for TX and consumes
`DMESH_EVENT_RECV`, `DMESH_EVENT_RECV_FIN`, and `DMESH_EVENT_TX_READY` from
`dmesh_poll_eq`; it does not use the internal raw-ring or ready-list APIs. Narrow
in-tree hooks remain for ClusterIP resolution, numeric QP creation, and transport
FIN because those operations are specific to socket interception rather than the
public service-name API.

The preload path copies because POSIX `read`/`write` require application buffers;
it flushes once at each POSIX write boundary. If one write exceeds the bounded
native TX window, it may flush an accepted prefix to make forward progress. On
`EAGAIN`, its real kernel fd suppresses `EPOLLOUT` until the EQ delivers
`DMESH_EVENT_TX_READY`; blocking writes park on that fd, without a retry timer. The
POSIX application never selects or observes the physical batch size.

The gRPC C++ adapter maps one runtime to channels, reactor shards to EQs, and
EventEngine endpoints to QPs. Client bootstrap accepts a Service-name target,
credentials, and `grpc::ChannelArguments`; absent authority defaults to the
target. Each EventEngine `Connect` creates a targeted QP. Established L4 streams
remain backend-pinned and terminate when that backend is lost. TLS and HTTP/2
remain end-to-end.

The adapter commits all slices of one EventEngine Write and flushes once at the
logical write boundary. RX is copied into gRPC slices before the native credit
is returned. A reactor parks its exact slice cursor on `EAGAIN` and resumes only
that connection when its EQ produces `DMESH_EVENT_TX_READY`; the adapter has no
retry timer.

## 7. Explicit limits

- No arbitrary memory registration, rkey, one-sided READ, or one-sided WRITE.
- No application-visible send completion; protocol ACKs reclaim internal TX
  capacity.
- No automatic deadline for an unflushed partial batch yet.
- The registry is loaded once and is not live-reloaded.
- Dynamic instances require a Service already present in the registry.
- L4 passthrough is the supported gRPC mode. The in-tree L7 validator uses a
  bounded 16-byte length-prefixed benchmark frame, not HTTP/2.
