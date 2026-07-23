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
| native QP | one Endpoint connection | all operations on reactor owner |
| RX event | reactor/Endpoint handoff | copy before native credit release |
| pending write | Endpoint state | one cursor, completed exactly once |

Callbacks never run inline from transport operations. Cross-thread work enters a
reactor through its command queue. A QP marked for close is freed only after the
entire current EQ batch, because later entries can still name it.

## Write state machine

One EventEngine Write may contain many slices and may exceed one native post.
The Endpoint retains `(slice_index, slice_offset)` and advances it only after an
accepted native post.

```text
slice bytes
  → dmesh_alloc
  → copy into registered reservation
  → dmesh_post_send (commit + complete-unit submission)
  → next fragment
  → dmesh_flush trailing partial at logical Write completion
```

Native ABI 4 batches committed posts into transport-private physical units and
submits complete units immediately. If the bounded native window fills before the
logical Write ends, the reactor forces any remaining partial, parks the cursor,
and retries only after native capacity reclamation identifies that QP as ready.
The final callback is scheduled only after the final partial flush succeeds.

`dmesh_alloc(EAGAIN)` automatically arms a one-shot `DMESH_EVENT_TX_READY`
event on the QP's EQ. The reactor retains the exact cursor and returns to its
two-fd event loop: one command eventfd and one native EQ eventfd. It does not own a
timerfd or scan all pending writes. On TX-ready it resumes only the named QP and
ignores the hint if that connection no longer has a parked write. The hint does not
reserve shared capacity; a repeated `EAGAIN` rearms the next transition.

## Read state machine

Native RX memory cannot be retained by gRPC after credit return. For every
`DMESH_EVENT_RECV`, the adapter allocates an exact-size gRPC slice, copies the bytes,
then calls `dmesh_release_rx_buffer`. A pending read consumes queued slices; otherwise
the slice remains in the Endpoint queue.

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
- no callback before the flush boundary;
- exact cursor resume only after `DMESH_EVENT_TX_READY`, with no timer retry;
- one EQ polling thread and no mid-batch QP destruction;
- RX copy before release;
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
