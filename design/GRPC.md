# DPUmesh gRPC Integration

This document defines how the gRPC C++ adapter maps chttp2 onto the native
transport: ownership, threading, the transmit and receive state machines, and
the contract its tests hold. Application bootstrap and build commands live in
[`integrations/grpc/README.md`](../integrations/grpc/README.md); the native
transport contract lives in [`API.md`](API.md).

## Model

gRPC chttp2 consumes an EventEngine byte-stream Endpoint, not necessarily a
POSIX socket. DPUmesh therefore supplies an Endpoint backed by one native QP and
injects accepted Endpoints through `PassiveListener`. HTTP/2, protobuf, service
dispatch, metadata, and security remain ordinary gRPC concerns.

```text
generated stub / handler
        │ protobuf + gRPC semantics
        ▼
      chttp2
        │ ordered bytes
        ▼
 DmeshEndpoint ─ DmeshReactor ─ native EQ/QP ─ BlueField
```

The adapter uses only the public native C API.

## Endpoint injection

gRPC exposes two seams for a non-socket transport, and the adapter uses a
different one on each side.

**Client.** `CreateDmeshChannel` constructs a `DmeshClientEventEngine`, installs
it as `GRPC_ARG_EVENT_ENGINE` through `grpc_event_engine_arg_vtable()`, and
calls `grpc::CreateCustomChannel` with the synthetic target `ipv4:127.0.0.1:1`.
The channel argument holds a shared pointer to the engine. The configured
Service name stays in the engine; the authority reaches chttp2 as
`GRPC_ARG_DEFAULT_AUTHORITY`, defaulted to the Service name when the caller
omits it. A caller-supplied `GRPC_ARG_EVENT_ENGINE` is rejected.

`DmeshClientEventEngine` implements `Connect` and delegates `Run`, `RunAfter`,
`Cancel`, `CreateListener`, and `GetDNSResolver` to the process default engine.
`Connect` ignores the resolved address, calls `DmeshRuntime::Connect` with the
Service name, and completes gRPC's `OnConnectCallback` with a `DmeshEndpoint`
over the returned QP. The connect deadline is a `RunAfter` timer on the
delegate; the timer and the connect callback both hold a weak reference to the
engine.

**Server.** No EventEngine is replaced. The application registers a listener
with `ServerBuilder::experimental().AddPassiveListener`, and
`AttachDmeshGrpcServer` installs a runtime accept callback. Each
`DMESH_EVENT_CONN_REQ` is wrapped as a `DmeshEndpoint` with an allocator from
the caller's factory and submitted through
`PassiveListener::AcceptConnectedEndpoint`. `Detach()` clears the accept
callback and returns after in-flight injections finish.

## Threads

| Thread | Owner | Count | Waits on | Runs |
|---|---|---|---|---|
| application | the application | application | — | `Endpoint::Write`, which records a cursor and returns |
| gRPC default engine pool | gRPC | gRPC | its own poller | deadlines, DNS, and every delegated `Run`/`RunAfter` |
| reactor owner | `DmeshReactor` | one per shard | `ppoll` on two fds | `dmesh_poll_eq`, QP lifecycle, event delivery to endpoints |
| executor worker | `DmeshRuntime` | one per shard | condition variable | write pumps and chttp2 callbacks |

`DmeshEndpoint` and `DmeshClientEventEngine` own no thread. A further host
thread belongs to the native channel and drains DOCA completions and the
reverse rings; it and the DPU-side workers are specified in
[`CORE.md`](CORE.md).

An accepted Write is queued on the caller's thread, posted on the executor
worker, and resumed from an event the reactor owner delivers. A receive is
copied on the reactor owner and parsed on the executor worker.

## Ownership

| Object | Owner | Constraint |
|---|---|---|
| native channel | `DmeshRuntime` | destroyed after all reactors |
| native EQ | one `DmeshReactor` | exactly one polling thread |
| native QP lifecycle | reactor owner thread | destroyed under the connection's transmit lock |
| native QP transmit | Endpoint work executor | serialized by the connection's transmit lock |
| RX batch run | reactor/Endpoint handoff | one slice per run, copied before credit release; credit held above the queue mark |
| pending write | Endpoint state | one cursor, completed exactly once |
| callback executor | `DmeshRuntime` and its endpoints | shared; default = one thread paired per reactor |
| runtime | application, channel and server attachment | shared; outlives what gRPC still holds |

Each reactor is paired with one dedicated thread that runs its connections'
endpoint completions (and therefore chttp2) and their write pumps. That thread
claims its whole queue per wake, and a completion is queued as a callback beside
the status it completes with. Callbacks never run inline from transport
operations. An endpoint holds a shared reference to both its executors, so
neither is destroyed while the endpoint can still schedule on it; a
`ThreadExecutor` released by a task on its own worker detaches that worker.
`DmeshRuntime::Create` returns a shared pointer, and the client EventEngine and
the server attachment each hold one, so the reactors and their threads outlive
every channel and listener gRPC has not yet released.

Transmit is serialized by a per-connection lock. One post — reserve, fill and
submit — holds it on the work executor, as does flush, and the reactor takes
the same lock before destroying that connection's QP. Lock order is Endpoint
state then transmit lock; the transmit lock is never held across a driver call.
Cross-thread work enters a reactor through its command queue; only an
empty→non-empty queue transition writes the command eventfd. One loop iteration
consumes a bounded number of poll batches and then returns to that queue without
blocking, so a saturated EQ still yields to commands and to the stop request. A
QP marked for close is freed only after the entire current EQ batch.

## Write state machine

One EventEngine Write may contain many slices and may exceed one native post.
The Endpoint retains `(slice_index, slice_offset)` and advances it only over
bytes a post has taken.

```text
cursor bytes, capped at dmesh_post_max
  → dmesh_alloc (one reservation spanning consecutive slices)
  → copy each slice fragment into it
  → dmesh_post_send (commit + complete-unit submission)
  → next post
  → publish the trailing partial at the logical Write boundary
```

The pump runs on the connection's paired thread and holds the connection's
transmit lock from `dmesh_alloc` through `dmesh_post_send`, matching the one
live reservation a QP holds. One post spans every remaining byte of the logical
Write that fits, so an HTTP/2 frame header and its payload cost one native
post. Native ABI 4 batches
committed posts into transport-private physical units and submits complete
units immediately. A pump run takes a bounded number of posts and reschedules
itself. If the bounded native window fills before the logical Write ends, the
pump forces any remaining partial and parks the cursor, resuming only after
native capacity reclamation identifies that QP as ready. The final callback is
scheduled only after the final partial flush succeeds.

`DmeshReactor::Options::tail_flush_delay` retains the trailing partial unit
while `dmesh_tx_inflight()` reports an outstanding unit, and the reactor bounds
its loop wait with the nearest tail deadline. The delay is zero by default and
the tail publishes at every write boundary.

`dmesh_alloc(EAGAIN)` automatically arms a one-shot `DMESH_EVENT_TX_READY`
event on the QP's EQ. The Endpoint retains the exact cursor and marks the write
parked; the reactor returns to its two-fd event loop — one command eventfd and
one native EQ eventfd, bounded by the nearest retained tail's deadline and not
waiting at all while a poll budget is outstanding — and owns no timerfd and no
scan of pending writes. On TX-ready it forwards the
hint to the named connection's Endpoint, which resumes its parked write and
drops a stale hint. The hint does not reserve shared capacity; a repeated
`EAGAIN` rearms the next transition.

The Endpoint fails a parked write when peer EOF arrives, and a post that blocks
after the FIN flag is set fails instead of parking. Every accepted EventEngine
Write completes even when the peer vanishes mid-transfer.

## Read state machine

Native RX memory cannot be retained by gRPC after credit return. One
`dmesh_poll_eq` batch returns a connection's receives consecutively, and the
reactor consumes that run as a unit: it allocates one exact-size gRPC slice for
the run's total length, copies each event into it, hands the Endpoint one slice,
then calls `dmesh_release_rx_buffer` for every event. One slice, one Endpoint
lock acquisition and one queue entry cost one per run. A run ends at any other
event for that QP, so terminal events stay ordered against the byte stream. A
pending read consumes queued slices; otherwise the slice remains in the Endpoint
queue.

Above the Endpoint's high-water mark the reactor keeps the receive credit
instead of returning it, and the DPU lands no further bytes for that connection
until a read drains the queue and the Endpoint asks its reactor to release what
it holds. Retention is capped per connection, the credits belonging to a landing
ring shared across the shard; past that cap the credit returns with the copy and
`Stats::receive_credit_hold_dropped` advances.

Peer FIN ends the read half. Transport failure or Endpoint destruction completes
both pending directions once with an error. Each Endpoint QP is one byte stream;
HTTP/2 framing and multiplexing remain entirely inside chttp2.

## Server path

Every reactor can consume the channel-wide native accept queue. The EQ that
receives `DMESH_EVENT_CONN_REQ` becomes the permanent owner of that QP. No
native listen-port call or application-level HTTP/2 parser is introduced.

## Verification contract

The maintained tests require:

- byte-exact split writes and one final flush;
- consecutive slices coalesced into one post;
- no callback before the flush boundary;
- exact cursor resume only after `DMESH_EVENT_TX_READY`, with no timer retry;
- peer FIN failing a parked write, and post-FIN backpressure failing a write;
- byte-exact completion of writes that span multiple pump runs;
- a retained tail publishing both on an idle transport and at its deadline;
- one EQ polling thread and no mid-batch QP destruction;
- a batch run coalesced into one slice, ended by any other event for that QP;
- RX copy before release, credit held above the queue mark and released on read;
- inbound QP conversion and pre-bind event replay;
- real chttp2 unary exchange over paired Endpoints;
- public-symbol linkage against `libdpumesh.so.4`.

Hardware validation additionally checks the native register/readiness barrier,
real byte exchange, FIN, `POD_QUIESCED`, and slot reuse. Those observations show
the exercised graceful path; they do not prove forced-death DMA isolation.

The bounded poll budget, shared executor ownership, and shared runtime
ownership are not covered by the maintained tests. `DmeshReactor::Stats` is
reported only by the hardware runtime smoke.

## Remaining work

1. Exercise streaming, cancellation/deadline, and TLS/mTLS on BlueField.
2. Integrate allocator/resource-quota policy suitable for production servers.
3. Run long-duration connection churn and memory/handle plateau tests.
4. Define platform-backed containment for host death during in-flight DMA.

Go is not addressed through LD_PRELOAD; its runtime may bypass libc network
calls. A future Go integration binds the native API directly and provides a
`net.Conn`-compatible transport with transport-private automatic batching,
logical-write flush boundaries, and a native writable-readiness contract.
