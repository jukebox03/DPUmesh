# gRPC Integration Design Record

This is the compact design record for the implemented gRPC C++ adapter. Current
build instructions and support status live in
[`integrations/grpc/README.md`](../integrations/grpc/README.md); native transport
contracts live in [`design/API.md`](../design/API.md).

## Decision

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

The adapter is pinned to gRPC v1.80.0 because the endpoint injection APIs are
experimental. It uses only the public native C API.

## Ownership

| Object | Owner | Constraint |
|---|---|---|
| native channel | `DmeshRuntime` | destroyed after all reactors |
| native EQ | one `DmeshReactor` | exactly one polling thread |
| native QP lifecycle | reactor owner thread | destroyed under the connection's transmit lock |
| native QP transmit | Endpoint work executor | serialized by the connection's transmit lock |
| RX event | reactor/Endpoint handoff | copy before credit release; credit held above the queue mark |
| pending write | Endpoint state | one cursor, completed exactly once |
| callback executor | `DmeshRuntime` default or caller | default = one thread paired per reactor |

Each reactor is paired with one dedicated thread that runs its connections'
endpoint completions (and therefore chttp2) and their write pumps, so all
per-connection work shards with the EQ shards, an idle runtime consumes no spin
cycles, and a write reaches the transport on the thread it is already running
on. That thread claims its whole queue per wake, and a completion is queued as
a callback beside the status it completes with, so the per-RPC path adds no
type-erased wrapper. Callbacks never run inline from transport operations.

Transmit serialization is a per-connection lock, not a thread: reserve, commit
and flush take it on the work executor, and the reactor takes the same lock
before destroying that connection's QP, so a transmit that races a close either
completes against a live QP or observes it gone. The lock is never held across
a driver call — the endpoint locks its own state first and then calls the
transport, so the opposite order would deadlock. Cross-thread work enters a
reactor through its command queue; only an empty→non-empty queue transition
writes the command eventfd. A QP marked for close is freed only after the
entire current EQ batch, because later entries can still name it.

Endpoint addresses carry native identity: a loopback address whose host octet is
the DPU-assigned pod id and whose port is the connection's native port. A client
QP has no peer pod, because the DPU selects and pins its backend.

## Write state machine

One EventEngine Write may contain many slices and may exceed one native
reservation. The Endpoint retains `(slice_index, slice_offset)` and advances it
only over bytes a reservation has taken.

```text
cursor bytes, capped at dmesh_post_max
  → dmesh_alloc (one reservation spanning consecutive slices)
  → copy each slice fragment into it
  → dmesh_post_send (commit + complete-unit submission)
  → next reservation
  → publish the trailing partial at the logical Write boundary
```

The pump runs on the connection's paired thread, which is where chttp2 issued
the Write, so posting costs no thread handoff. A reservation carries every
remaining byte of the logical Write that fits, so an HTTP/2 frame header and
its payload cost one native post rather than one each. Native ABI 4 batches
committed posts into transport-private physical units and submits complete
units immediately. A pump run takes a bounded number of reservations and
reschedules itself, so one large Write cannot monopolise its shard's thread. If
the bounded native window fills before the logical Write ends, the pump forces
any remaining partial, parks the cursor, and retries only after native capacity
reclamation identifies that QP as ready. The final callback is scheduled only
after the final partial flush succeeds.

The trailing partial unit is a policy choice, because a successor write can share
the descriptor it would otherwise take alone. A configured retention delay holds
that tail while `dmesh_tx_inflight()` reports an outstanding unit, and the
reactor bounds the wait with the tail's own deadline. The delay is zero for
chttp2: it already merges concurrent streams into one logical Write, so no
successor exists until the current write completes, and retention would only add
latency.

`dmesh_alloc(EAGAIN)` automatically arms a one-shot `DMESH_EVENT_TX_READY`
event on the QP's EQ. The Endpoint retains the exact cursor and marks the write
parked; the reactor returns to its two-fd event loop — one command eventfd and
one native EQ eventfd, bounded only by the nearest retained tail's deadline —
and owns no timerfd and no scan of pending writes. On TX-ready it forwards the
hint to the named connection's endpoint, which resumes its parked write and
drops a stale hint. The hint does not reserve shared capacity; a repeated
`EAGAIN` rearms the next transition.

A departed peer stops returning the credits a parked write is waiting for, so
the Endpoint fails a parked write when EOF arrives, and a Reserve that blocks
after the FIN flag is set fails instead of parking. Every accepted EventEngine
Write therefore completes even when the peer vanishes mid-transfer.

## Read state machine

Native RX memory cannot be retained by gRPC after credit return. For every
`DMESH_EVENT_RECV`, the adapter allocates an exact-size gRPC slice, copies the bytes,
then calls `dmesh_release_rx_buffer`. A pending read consumes queued slices; otherwise
the slice remains in the Endpoint queue.

That queue is what the transport must not fill without bound. Above the
Endpoint's high-water mark the reactor keeps the receive credit instead of
returning it, so the DPU stops landing bytes for that connection until a read
drains the queue and the Endpoint asks its reactor to release what it holds. A
per-connection cap bounds the retention, because those credits belong to a
landing ring shared with the other connections on the shard. The mark sits well
above the depth an ordinary pipeline reaches between reads, so backpressure
answers a stalled reader rather than a busy one.

Peer FIN ends the read half. Transport failure or Endpoint destruction completes
both pending directions once with an error. Each Endpoint QP is one byte stream;
HTTP/2 framing and multiplexing remain entirely inside chttp2.

## Server path

Every reactor can consume the channel-wide native accept queue. The EQ that
receives `DMESH_EVENT_CONN_REQ` becomes the permanent owner of that QP. The runtime
wraps it as an Endpoint and submits it to gRPC's `PassiveListener`. No native
listen-port call or application-level HTTP/2 parser is introduced.

## Verification contract

The maintained tests require:

- byte-exact split writes and one final flush;
- consecutive slices coalesced into one reservation;
- no callback before the flush boundary;
- exact cursor resume only after `DMESH_EVENT_TX_READY`, with no timer retry;
- peer FIN failing a parked write, and post-FIN backpressure failing a write;
- byte-exact completion of writes that span multiple pump runs;
- a retained tail publishing both on an idle transport and at its deadline;
- one EQ polling thread and no mid-batch QP destruction;
- RX copy before release, credit held above the queue mark and released on read;
- inbound QP conversion and pre-bind event replay;
- real chttp2 unary exchange over paired Endpoints;
- public-symbol linkage against `libdpumesh.so.4`.

Hardware validation additionally checks the native register/readiness barrier,
real byte exchange, FIN, `POD_QUIESCED`, and slot reuse. Those observations show
the exercised graceful path; they do not prove forced-death DMA isolation.

## Status and remaining work

Race-free native TX-ready publication and event-driven retry are implemented.
The remaining integration work is:

1. Exercise streaming, cancellation/deadline, and TLS/mTLS on BlueField.
2. Integrate allocator/resource-quota policy suitable for production servers.
3. Run long-duration connection churn and memory/handle plateau tests.
4. Define platform-backed containment for host death during in-flight DMA.

Go is not addressed through LD_PRELOAD because its runtime may bypass libc
network calls. A future Go integration should bind the native API directly and
provide a `net.Conn`-compatible transport with transport-private automatic
batching, logical-write flush boundaries, and a native writable-readiness
contract.
