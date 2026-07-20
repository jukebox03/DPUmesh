# gRPC C++ over DPUmesh

This directory adapts the DPUmesh native byte stream to gRPC C++ EventEngine.
It is an independent CMake project and depends only on public
`<dpumesh/dmesh.h>` plus `libdpumesh.so.2`; no DPUmesh internal header crosses
the integration boundary.

## Version contract

- gRPC source: exactly v1.80.0
- language: C++17
- transport ABI: `libdpumesh.so.2`

The used EventEngine endpoint-injection APIs are experimental. A gRPC upgrade is
therefore an explicit source migration with a full test rebuild.

## Mapping

```text
gRPC chttp2
    │ EventEngine Read / Write
    ▼
DmeshEndpoint
    │
DmeshReactor: one owner thread + one native CQ
    │
dmesh_qp_t ── registered byte stream ── BlueField
```

One `DmeshRuntime` owns the native channel and a configurable set of reactor
shards. Each reactor owns one single-consumer CQ. A client endpoint owns one QP;
an inbound `DMESH_WC_CONN_REQ` becomes a server Endpoint delivered through
gRPC's `PassiveListener`.

RX bytes are copied into allocator-owned gRPC slices before the native RX credit
is returned. TX slices are copied into registered native reservations. This copy
boundary is intentional: arbitrary gRPC heap memory is not DPA-registered, and a
native RX slot cannot be released while gRPC still references it.

Writes use native default batching. Every slice fragment is committed with
`dmesh_post_send`, then the adapter calls `dmesh_flush` once when the complete
EventEngine Write has been accepted. If a write is larger than the bounded
native batch window, the committed prefix is flushed explicitly before the QP
is parked. Complete physical units may already have been submitted by post; the
final flush is a logical latency boundary that forces only the remaining tail.

## Reactor and backpressure

Each reactor polls:

| fd | Purpose |
|---|---|
| command eventfd | cross-thread connect, bind, close, shutdown |
| native CQ eventfd | inbound connection, data, FIN |
| timerfd | retry a QP whose `dmesh_alloc` returned `EAGAIN` |

All QP calls run on the CQ owner. QP destruction is deferred until the complete
native completion batch has been dispatched. Data arriving before Endpoint bind
is copied and replayed in order, followed by any retained FIN/error.

The retry timer defaults to 50 µs because ABI 2 still lacks native writable
notification. It is a correctness fallback, not the desired final readiness
model. A future native TX-ready completion can remove the timer without changing
the Endpoint write state machine.

## Build and tests

Build the native library first, then configure against the pinned gRPC source:

```sh
make lib
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DBUILD_TESTING=ON
cmake --build build/grpc -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/grpc --output-on-failure
```

The test suite covers Endpoint read/write state, one-flush-per-logical-write
batching, EAGAIN retry, exact callback completion, CQ ownership, deferred close,
inbound QPs, native symbol linkage, and paired real gRPC HTTP/2 over fake native
transport. The BlueField smoke executable exercises the same runtime against the
real native API.

Sanitizers can be selected independently:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=ON

# separate build: TSan cannot be combined with ASan
cmake -S integrations/grpc -B build/grpc-tsan \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_TSAN=ON
```

LeakSanitizer may fail under ptrace-restricted test runners; disabling leak
detection does not disable ASan's memory-access checks.

## Application bridge

`ConnectDmeshGrpcChannel` creates a client chttp2 channel from a DPUmesh Endpoint.
`AttachDmeshGrpcServer` attaches inbound DPUmesh Endpoints to an ordinary gRPC
server. Generated protobuf types, service implementations, HTTP/2 framing,
metadata, and TLS bytes remain unchanged.

The adapter must use L4 passthrough. The repository's optional simple frame
codec is not an HTTP/2 parser and must not be enabled for a gRPC service.

## Current boundary

Implemented: client and server endpoint injection, unary chttp2, multi-reactor
CQ ownership, native lifecycle barriers, default TX batching, focused QPS
benchmark, fake and hardware smoke paths.

Not yet established: native writable events, streaming/cancellation/TLS hardware
matrices, production resource-quota allocator policy, long-duration resource
plateau evidence, and forced-death isolation for already-issued host-memory DMA.
