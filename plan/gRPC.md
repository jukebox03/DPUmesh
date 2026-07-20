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
 DmeshEndpoint ─ DmeshReactor ─ native CQ/QP ─ BlueField
```

The adapter is pinned to gRPC v1.80.0 because the endpoint injection APIs are
experimental. It uses only the public native C API.

## Ownership

| Object | Owner | Constraint |
|---|---|---|
| native channel | `DmeshRuntime` | destroyed after all reactors |
| native CQ | one `DmeshReactor` | exactly one polling thread |
| native QP | one Endpoint connection | all operations on reactor owner |
| RX completion | reactor/Endpoint handoff | copy before native credit release |
| pending write | Endpoint state | one cursor, completed exactly once |

Callbacks never run inline from transport operations. Cross-thread work enters a
reactor through its command queue. A QP marked for close is freed only after the
entire current CQ batch, because later entries can still name it.

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

Native ABI 2 batches committed posts into transport-private physical units and
submits complete units immediately. If the bounded native window fills before the
logical Write ends, the reactor forces any remaining partial, parks the cursor,
and retries after capacity is expected to have returned. The final callback is
scheduled only after the final partial flush succeeds.

`EAGAIN` currently uses a 50 µs timer because native writable notification is not
implemented. This preserves correctness but adds timer syscalls and retry work;
it is the next transport-level design target.

## Read state machine

Native RX memory cannot be retained by gRPC after credit return. For every
`DMESH_WC_RECV`, the adapter allocates an exact-size gRPC slice, copies the bytes,
then calls `dmesh_wc_release`. A pending read consumes queued slices; otherwise
the slice remains in the Endpoint queue.

Peer FIN ends the read half. Transport failure or Endpoint destruction completes
both pending directions once with an error. In L4 mode one QP must retain one
stream id; a change is treated as a transport violation.

## Server path

Every reactor can consume the channel-wide native accept queue. The CQ that
receives `DMESH_WC_CONN_REQ` becomes the permanent owner of that QP. The runtime
wraps it as an Endpoint and submits it to gRPC's `PassiveListener`. No native
listen-port call or application-level HTTP/2 parser is introduced.

## Verification contract

The maintained tests require:

- byte-exact split writes and one final flush;
- no callback before the flush boundary;
- exact cursor resume after `EAGAIN`;
- one CQ polling thread and no mid-batch QP destruction;
- RX copy before release;
- inbound QP conversion and pre-bind event replay;
- real chttp2 unary exchange over paired Endpoints;
- public-symbol linkage against `libdpumesh.so.2`.

Hardware validation additionally checks the native register/readiness barrier,
real byte exchange, FIN, `POD_QUIESCED`, and slot reuse. Those observations show
the exercised graceful path; they do not prove forced-death DMA isolation.

## Remaining work

1. Replace timer retry with a race-free native TX-ready completion.
2. Exercise streaming, cancellation/deadline, and TLS/mTLS on BlueField.
3. Integrate allocator/resource-quota policy suitable for production servers.
4. Run long-duration connection churn and memory/handle plateau tests.
5. Define platform-backed containment for host death during in-flight DMA.

Go is not addressed through LD_PRELOAD because its runtime may bypass libc
network calls. A future Go integration should bind the native API directly and
provide a `net.Conn`-compatible transport with transport-private automatic
batching, logical-write flush boundaries, and a native writable-readiness
contract.
