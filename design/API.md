# DPUmesh Native API

This document defines the current public contract of `<dpumesh/dmesh.h>`. The
ABI is `libdpumesh.so.3`. The interface borrows the useful
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
return. Because one CQ poll can return several entries that name the same QP,
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
becomes available, the owning CQ receives `DMESH_WC_TX_READY`. Closing the QP or
successfully allocating on a direct retry cancels an obsolete request or hint.

TX readiness is a one-shot retry hint, not a guarantee that a particular
allocation will succeed. The completion names the QP whose parked write should
retry, but shared capacity may be consumed before that retry. If it returns
`EAGAIN` again, that call has already requested the next notification. Polling
applications may retry opportunistically, while sleeping event loops can rely on
the CQ notification without running a timer or scanning every QP.

DPUmesh does not flush buffered data on a timer. Code that needs bounded latency
calls `dmesh_flush()` at its logical write boundary; graceful close also flushes,
while `dmesh_abort_qp()` discards buffered data that has not been submitted.

## 5. RX and CQ notification

`dmesh_poll_cq()` is nonblocking and returns four completion types:

| Opcode | Meaning | Credit |
|---|---|---|
| `DMESH_WC_CONN_REQ` | New inbound QP | none |
| `DMESH_WC_RECV` | One RX fragment | held until `dmesh_wc_release()` |
| `DMESH_WC_RECV_FIN` | Peer EOF | none |
| `DMESH_WC_TX_READY` | An `EAGAIN`-blocked QP should retry allocation | none |

`wc.buf` points directly into the channel RX mmap. A completion is a transport
fragment, not an application message boundary; parsers must retain framing state
across completions and may decode several frames from one logical byte stream.
Copy bytes before release if they must outlive the completion.
`dmesh_wc_release()` is idempotent and remains valid after QP destruction because
the credit belongs to the channel.

The completion fields have one meaning across all opcodes:

| Field | Contract |
|---|---|
| `qp` | QP that owns the event; the newly accepted QP for `CONN_REQ` |
| `opcode` | Event kind from the table above |
| `buf` | RX-mmap view for `RECV`; otherwise `NULL` |
| `len` | Fragment length for `RECV`; otherwise zero |
| `_rx_token` | Opaque release token; applications neither read nor modify it |

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
            consume(wc[i].qp, wc[i].buf, wc[i].len);
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

For `DMESH_WC_TX_READY`, `wc.qp` is the retry target, `buf == NULL`, `len == 0`,
and `_rx_token == -1`. Calling `dmesh_wc_release()` on it is a harmless no-op.
The application should ignore a stale hint if that QP no longer has a parked
write.

## 6. POSIX and gRPC facades

`libdmesh_preload.so` is a POSIX adapter over the native data/completion contract.
It uses `dmesh_alloc`/`dmesh_post_send`/`dmesh_flush` for TX and consumes
`DMESH_WC_RECV`, `DMESH_WC_RECV_FIN`, and `DMESH_WC_TX_READY` from
`dmesh_poll_cq`; it does not use the internal raw-ring or ready-list APIs. Narrow
in-tree hooks remain for ClusterIP resolution, numeric QP creation, and transport
FIN because those operations are specific to socket interception rather than the
public service-name API.

The preload path copies because POSIX `read`/`write` require application buffers;
it flushes once at each POSIX write boundary. If one write exceeds the bounded
native TX window, it may flush an accepted prefix to make forward progress. On
`EAGAIN`, its real kernel fd suppresses `EPOLLOUT` until the CQ delivers
`DMESH_WC_TX_READY`; blocking writes park on that fd, without a retry timer. The
POSIX application never selects or observes the physical batch size.

The gRPC C++ adapter maps one runtime to channels, reactor shards to CQs, and
EventEngine endpoints to QPs. Client bootstrap accepts a Service-name target,
credentials, and `grpc::ChannelArguments`; absent authority defaults to the
target. Each EventEngine `Connect` creates a targeted QP. Established L4 streams
remain backend-pinned and terminate when that backend is lost. TLS and HTTP/2
remain end-to-end.

The adapter commits all slices of one EventEngine Write and flushes once at the
logical write boundary. RX is copied into gRPC slices before the native credit
is returned. A reactor parks its exact slice cursor on `EAGAIN` and resumes only
that connection when its CQ produces `DMESH_WC_TX_READY`; the adapter has no
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
