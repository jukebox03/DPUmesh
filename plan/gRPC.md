# gRPC over the DPUmesh Native API

## Document status

This document is the implementation, verification, and remaining-work record for
gRPC over DPUmesh. It describes the 2026-07-19 working tree, gRPC v1.80.0, and
the public C API in `include/dpumesh/dmesh.h`. It also specifies a Go integration
that can be implemented without changing generated protobuf code or forking
grpc-go.

The central conclusion is established by code and real hardware tests:

> gRPC C++ can run over the DPUmesh native API because gRPC chttp2 consumes an
> EventEngine byte-stream Endpoint. The current adapter supplies that Endpoint
> from a DPUmesh QP and injects accepted server Endpoints through a PassiveListener.
> HTTP/2, protobuf, RPC dispatch, and application handlers remain ordinary gRPC.

The implementation is functional but not production-complete. Endpoint injection,
unary RPC, real BlueField transport, graceful remote reclaim, and a focused QPS
benchmark are present. A complete hybrid EventEngine, streaming/TLS/failure
matrices, allocator integration, TX-writable notification, packaging, and
long-duration resource proof remain.

## 1. Source and version contract

| Component | Version/source | Contract |
|---|---|---|
| DPUmesh C API | current working tree, `include/dpumesh/dmesh.h` | Public API only; no adapter dependency on `dmesh_core.h` |
| DPUmesh gRPC adapter | `integrations/grpc`, version 0.2.0 | C++17 |
| gRPC | exact v1.80.0 source checkout | Required because the used EventEngine and PassiveListener APIs are experimental |
| Protocol | gRPC chttp2 / HTTP/2 | Unmodified bytes over DPUmesh L4 passthrough |

`integrations/grpc/include/dpumesh/grpc/version.h` publishes the exact gRPC source
version. CMake rejects a different source version. This is necessary because
`CreateChannelFromEndpoint` and `PassiveListener` are not a stable public ABI.

## 2. Why the integration is possible

gRPC does not require that chttp2 receive a POSIX file descriptor. At the bottom
of its transport stack it requires an object with byte-stream `Read`, `Write`,
address, and lifetime semantics:

```text
generated stub / service handler
            │ protobuf messages
            ▼
gRPC call, retry, deadline, metadata
            │ HTTP/2 frames
            ▼
grpc_event_engine::EventEngine::Endpoint
            │ ordered bytes
            ▼
socket Endpoint                 DmeshEndpoint
       │                              │
kernel TCP                      dmesh_qp_t
```

`grpc::experimental::CreateChannelFromEndpoint()` constructs a chttp2 client
channel from an already-connected Endpoint. It bypasses the usual resolver,
subchannel TCP connect, and socket EventEngine path. On the server,
`grpc::experimental::PassiveListener` accepts already-connected Endpoints and
hands them to the normal gRPC server transport.

Therefore no HTTP/2 implementation is added to DPUmesh. DPUmesh is responsible
only for an ordered, reliable, full-duplex byte stream. The current DPU L7 frame
codec must be disabled for gRPC because it is not an HTTP/2 parser and would
reinterpret arbitrary HTTP/2 bytes as repository-specific frames.

## 3. Native API to gRPC mapping

| gRPC requirement | DPUmesh mechanism | Adapter behavior |
|---|---|---|
| Process transport owner | `dmesh_channel_t` | One `DmeshRuntime` owns one channel |
| Independent polling shard | `dmesh_cq_t` | One `DmeshReactor` and owner thread per CQ |
| Connected byte stream | `dmesh_qp_t` | One `DmeshEndpoint` per QP |
| Client connect | `dmesh_create_qp(cq, service)` | Asynchronous reactor command |
| Server accept | `DMESH_WC_CONN_REQ` | Converted to an Endpoint and injected into PassiveListener |
| Receive bytes | `DMESH_WC_RECV` | Copy to allocator-owned gRPC Slice, then release RX credit |
| Peer EOF | `DMESH_WC_RECV_FIN` | Terminal failed read with `Unavailable` |
| Write bytes | `dmesh_alloc` + memcpy + `dmesh_post_send` | Slice fragments split at `post_max` |
| TX backpressure | `dmesh_alloc == NULL/EAGAIN` | Retain write and retry on CQ progress or 50 µs timer |
| Close | `dmesh_destroy_qp` | Deferred until after the CQ completion batch |
| Runtime shutdown | QP → CQ → channel | Enforced in `DmeshRuntime` destruction |

The baseline necessarily copies in both directions:

```text
TX: gRPC SliceBuffer → registered DPUmesh TX reservation
RX: registered DPUmesh RX completion → gRPC allocator Slice
```

TX cannot point DPA DMA at arbitrary gRPC heap slices. RX cannot be released while
gRPC still owns a slice that aliases the slot. Correctness therefore precedes any
zero-copy optimization.

## 4. Implemented C++ components

### 4.1 `DmeshApiOps`

Files: `src/dmesh_api_ops.h`, `src/dmesh_api_ops.cc`.

This virtual seam contains only public native calls. `MakeNativeDmeshApiOps()`
links the real C API; tests supply a fake implementation. The seam prevents
accidental use of internal layout from `src/dmesh_core.h` and makes lifecycle,
backpressure, and error injection deterministic in unit tests.

### 4.2 `DmeshRuntime`

Files: `src/dmesh_runtime.h`, `src/dmesh_runtime.cc`.

`DmeshRuntime::Create()` creates one channel, validates `dmesh_post_max()`, then
creates N independent CQ reactors. Connects are assigned round-robin across
reactors. An accept callback is installed on every reactor. Destruction shuts
down every reactor, which destroys all QPs and CQs, before destroying the channel.

The callback executor must outlive the runtime. The runtime must outlive every
Endpoint because each Endpoint schedules write work on its owning reactor.

### 4.3 `DmeshReactor`

Files: `src/dmesh_reactor.h`, `src/dmesh_reactor.cc`.

Each reactor owns exactly one CQ and one thread. That thread polls three fds:

```text
command eventfd     cross-thread connect/close/bind operations
CQ eventfd          inbound QP/data/FIN completions
retry timerfd       TX EAGAIN retry, default 50 µs
```

All QP operations are marshalled to this owner. `dmesh_poll_cq()` has one
consumer, and QP destruction is placed on a deferred list swept only after the
complete returned batch is handled. This satisfies the native API rule that a
later completion in the same batch may still reference a QP marked for close.

Data arriving before `DmeshEndpoint` binds its driver is copied into a bounded-by-
memory prebind queue. FIN or errors are retained as terminal prebind state. When
the driver binds, queued bytes are delivered first, followed by the terminal
event. A stream-id change on a passthrough QP is treated as a transport error,
because gRPC requires one ordered peer stream.

### 4.4 `DmeshEndpoint`

Files: `src/dmesh_endpoint.h`, `src/dmesh_endpoint.cc`.

The Endpoint has four lifetime states:

```text
OPEN ── peer FIN ──► REMOTE_EOF
  │
  ├── transport/allocator error ──► FAILED
  └── destructor/runtime stop ────► CLOSING
```

It permits one outstanding EventEngine read and one outstanding write, matching
the gRPC endpoint contract. Callbacks are always scheduled on the callback
executor and are never invoked inline from transport operations.

Read behavior:

1. If received slices are queued, append all to the caller buffer and return
   `true` synchronously.
2. Otherwise retain the callback and buffer and return `false`.
3. On native RX, allocate an exact-size gRPC slice, copy the bytes, and either
   satisfy the pending read or queue the slice.
4. On EOF, failure, or destruction, complete the pending read asynchronously with
   a terminal status.

Write behavior:

1. Empty data returns `true` synchronously.
2. Nonempty data is retained as one `PendingWrite` and returns `false`.
3. The reactor work executor walks slice index and offset, splitting only at
   `MaxPostSize()`.
4. Accepted fragments advance the cursor. `WouldBlock` retains the cursor.
5. When all fragments are posted, the adapter clears the input SliceBuffer and
   completes the callback once.
6. Error or close clears the buffer and completes once with failure.

The shared state and a separate driver object prevent use-after-free when a
reactor event races with public Endpoint destruction. Transport close is
idempotent.

### 4.5 gRPC bridge

Files: `src/dmesh_grpc_channel.cc`, `src/dmesh_grpc_runtime.h`,
`src/dmesh_grpc_runtime.cc`.

Client flow:

```text
ConnectDmeshGrpcChannel(service)
  → runtime.Connect
  → reactor dmesh_create_qp
  → DmeshEndpoint
  → grpc::experimental::CreateChannelFromEndpoint
  → ordinary generated Stub
```

Server flow:

```text
ServerBuilder.experimental().AddPassiveListener(...)
  → BuildAndStart
  → AttachDmeshGrpcServer(runtime, listener, allocator_factory)
  → runtime accept callback on DMESH_WC_CONN_REQ
  → DmeshEndpoint
  → PassiveListener::AcceptConnectedEndpoint
  → ordinary registered Service handler
```

`DmeshGrpcServerAttachment::Detach()` first disables new accepts and then waits
for in-flight accept callbacks. The runtime and listener outlive the attachment.

## 5. Native initialization and teardown underneath gRPC

The adapter relies on the native lifecycle rather than duplicating it.

### Initialization gate

```text
dmesh_create_channel
  → Comch connection
  → POD_ASSIGNED
  → import K forward rings and host TX/RX mmaps
  → generation-tagged RING_ADD on every target EU
  → all matching RING_ADD_ACK
  → ARM egress resources ready
  → POD_INIT_RESULT(READY)
```

The host waits up to 30 seconds. Explicit `REGISTER_FAILED`, `MMAP_FAILED`, and
`DPA_FAILED` states cause construction to fail and unwind. gRPC cannot start with
a merely assigned but DMA-incomplete channel.

### Graceful remote reclaim

```text
gRPC channel/server releases Endpoints
  → reactor destroys QPs
  → runtime destroys CQs
  → channel sends POD_UNREGISTER while PE remains active
  → DPU blocks new routing and DMA for generation G
  → all target EUs return RING_DEL_ACK(G)
  → ARM egress lanes, inflight DMA, and source custody reach zero
  → DPU destroys buf arrays before imported mmaps
  → POD_QUIESCED
  → host stops Comch and destroys exports/device
```

The five-second host wait is bounded. Unexpected disconnect uses asynchronous DPU
cleanup and has recovered in hardware tests. That recovery is a liveness result,
not a proof that arbitrary already-submitted DMA is memory-safe across SIGKILL.

## 6. Verified C++ behavior

### Local build and tests

The sanitizer configuration is:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/tmp/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=ON
cmake --build build/grpc -j2
ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build/grpc --output-on-failure
```

The current working tree passes all four registered tests:

| Test | What it proves |
|---|---|
| `grpc_dpumesh_endpoint_test` | Read/write fragmentation, async callbacks, EOF/error/close, EAGAIN state |
| `grpc_dpumesh_channel_test` | Real gRPC chttp2 over paired custom Endpoints |
| `grpc_dpumesh_reactor_test` | Fake-native CQ ownership, accept, retry timer, stream checks, races, shutdown |
| `grpc_dpumesh_native_link_test` | Adapter resolves and links the public `libdpumesh` API |

ASAN leak detection is disabled because gRPC process-global allocations are not
fully torn down at test process exit; ASAN and UBSAN error detection remain on.
TSAN has a separate build option and must not be combined with ASAN/UBSAN.

### Real BlueField evidence

`grpc_dpumesh_native_grpc_smoke` uses the real native library and an ordinary
generated unary RPC on both server and client. The retained hardware campaign:

- completed 30 independent create/use/destroy rounds;
- exchanged 60 successful unary RPCs;
- repeatedly reused pod slots 10 and 11;
- observed `POD_QUIESCED` on host and DPU for every round;
- transferred three 1 MiB responses with exact byte agreement;
- recovered and registered again after an unexpected disconnect.

This is real gRPC over real DPUmesh, not an in-memory fake or the old C frame
benchmark.

## 7. Current gRPC QPS benchmark

`integrations/grpc/bench/qps_benchmark.cc` generates and implements the official
gRPC v1.80 `grpc.testing.BenchmarkService` protocol. One binary selects `tcp` or
`dmesh`, so service implementation, protobuf serialization, sync unary RPC shape,
payload construction, warmup, and statistics are identical. For DPUmesh it uses
the actual runtime and native library; for TCP it uses normal gRPC channel/listen
construction.

The client uses one channel and one synchronous loop per concurrency thread. This
matches one channel with `outstanding_rpcs_per_channel = concurrency` in a
closed-loop unary workload. It is a focused compatible harness, not the complete
upstream `qps_worker` and scenario controller.

Build and syntax:

```sh
cmake -S integrations/grpc -B build/grpc-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DDPUMESH_GRPC_SOURCE_DIR=/tmp/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=OFF \
  -DDPUMESH_GRPC_BUILD_QPS_BENCHMARK=ON \
  -DBUILD_TESTING=OFF
cmake --build build/grpc-release -j2 --target grpc_dpumesh_qps_benchmark

grpc_dpumesh_qps_benchmark server <tcp|dmesh> ENDPOINT DURATION_S [REACTORS]
grpc_dpumesh_qps_benchmark client <tcp|dmesh> TARGET WARMUP_S DURATION_S \
  CONCURRENCY REQUEST_BYTES RESPONSE_BYTES [REACTORS]
```

The output includes attempts, successes, failures, QPS, p50/p90/p99/p99.9, client
CPU seconds, and average client CPU cores. Server output includes call count and
process CPU.

### 7.1 BlueField measurement on 2026-07-19

The Release binary SHA-256 was
`df74b54c371251f4656ca536e1b73f742cc5343863c0edc55309739f6b42c293`.
Both copies ran on node `rapids4`, from client pod
`bench-dpumesh-f97d94b94-n8wz4` to server pod
`echo-dpumesh-13-55c8976957-m5cxp`. The TCP target was the server pod IP
`10.244.0.143:50072`; the DPUmesh target was the separately registered service
`echo-dpumesh-13`. The same executable and generated service implementation were
used for both.

Each table row is three independent client process runs with two seconds of
warmup and five measured seconds. Every run used one channel and two DPUmesh
reactors where applicable. Values are the median, followed by the full three-run
range in parentheses. All 24 measured runs reported `failed=0`.

| Transport | Request/response | Concurrency | QPS | p50 (µs) | p99 (µs) | Client CPU cores |
|---|---:|---:|---:|---:|---:|---:|
| TCP | 64/64 B | 1 | 1,338.913 (1,318.337–1,347.219) | 773.497 (756.446–775.191) | 1,143.281 (1,131.594–1,144.740) | 0.290 (0.289–0.302) |
| DPUmesh | 64/64 B | 1 | 666.010 (651.414–677.779) | 1,562.423 (1,513.631–1,582.121) | 2,031.689 (2,013.669–2,074.119) | 0.252 (0.235–0.271) |
| TCP | 64/64 B | 8 | 9,159.643 (8,159.524–10,021.537) | 829.714 (757.907–937.167) | 1,778.673 (1,626.145–1,809.881) | 1.568 (1.447–1.660) |
| DPUmesh | 64/64 B | 8 | 8,474.683 (8,285.483–10,873.122) | 898.512 (693.708–920.272) | 1,736.434 (1,435.960–1,744.002) | 1.476 (1.220–1.530) |
| TCP | 64/64 B | 32 | 16,141.084 (16,031.115–16,159.437) | 1,996.981 (1,978.295–2,020.902) | 2,975.754 (2,956.819–2,990.842) | 1.926 (1.896–1.934) |
| DPUmesh | 64/64 B | 32 | 15,690.874 (15,472.231–15,786.137) | 2,044.733 (2,035.687–2,081.871) | 3,003.382 (2,919.423–3,006.032) | 1.951 (1.942–1.964) |
| TCP | 1,024/1,024 B | 32 | 15,738.784 (15,336.055–15,740.743) | 2,067.768 (2,027.860–2,105.362) | 3,034.749 (3,026.293–3,053.289) | 2.031 (1.998–2.033) |
| DPUmesh | 1,024/1,024 B | 32 | 15,358.362 (15,235.035–15,532.497) | 2,093.536 (2,071.894–2,113.399) | 2,978.818 (2,976.831–3,034.851) | 1.948 (1.944–1.973) |

The DPUmesh/TCP median QPS ratios were 0.497 at concurrency 1, 0.925 at
concurrency 8, 0.972 at concurrency 32 for 64 B, and 0.976 at concurrency 32
for 1 KiB. The unloaded result is therefore clear: DPUmesh delivered half the
TCP QPS and approximately twice the p50 latency. At concurrency 32 both clients
were near a two-core ceiling; the small QPS and latency gap there is a saturated
client result and does not erase the unloaded transport cost.

The concurrency-8 ranges overlap substantially. Three repetitions are enough to
show that point's instability, not enough to rank close tail values. No claim is
made that DPUmesh has a better p99 where median values cross but the ranges
overlap.

Server process summaries were 859,616 calls and 166.375 CPU seconds for the
150-second DPUmesh server process, and 899,243 calls and 129.193 CPU seconds for
the 120-second TCP server process. Their different idle windows prevent a direct
average-core comparison. A separate clean 20-second DPUmesh load at 64 B,
concurrency 32 produced 316,441/316,441 successful RPCs, 15,820.852 QPS, p50
2,026.594 µs, p99 2,948.965 µs, and 1.942 client CPU cores. During that run, two
one-second DPU samples showed the ARM main thread at 51–52% CPU and two helper
threads at 0–1% each; an immediate idle sample showed 2% on the main thread.
Thus this operating point added approximately 0.5 DPU ARM core beyond the client
and server process CPU. DPA EU consumption is not represented by ARM `%CPU`.

The DPU was in L4 passthrough with K=2, automatic N=8 after detecting 254
available EUs, one inline ARM egress engine, and warning-level DPU logging. Client
teardown logs showed RX `inbox_full=0`, `accept_full=0`, and
`POD_QUIESCED` for every measured DPUmesh run. The warning-level DPU log contained
no error during the 24-run matrix. One later CPU diagnostic was excluded because
its deliberately too-short server lifetime expired before the client; it emitted
an unroutable-segment error. The clean 20-second replacement above completed with
zero failures and is the only CPU probe retained.

Local pre-deployment validation used TCP loopback at concurrency 4 and completed
13,938/13,938 measured RPCs with zero failures. That run also exposed and fixed a
benchmark-only start barrier race: workers and the coordinator shared one
condition variable, so `notify_one` could wake another worker and strand the
coordinator. The implementation now uses `notify_all`. Every synchronous RPC also
has a five-second deadline so server loss cannot hang the benchmark after its
measurement deadline.

After the hardware matrix, the TCP server startup path was tightened to check the
v1.80 `selected_port` output and fail before printing `READY` if bind did not
succeed. This does not touch the request data path. The final rebuilt binary hash
is `cba4b6586e431f41498e83281e6fe870c77216021d06321662462341f89213e8`;
a final TCP loopback check at concurrency 4 completed 8,344/8,344 measured RPCs
with zero failures. The earlier hash remains in this section because it is the
exact binary used for the reported BlueField numbers.

Interpretation constraints:

- TCP versus DPUmesh isolates the endpoint path for this workload.
- It does not compare against Envoy HTTP/2, L7 routing, telemetry, or policy.
- Synchronous unary results do not establish streaming or callback API behavior.
- Client process CPU is not total system cost; DPU ARM CPU must be captured for a
  complete efficiency comparison.
- Short single repetitions are diagnostic. A publication result requires repeated
  medians and a recorded noise interval.

## 8. Remaining C++ work

The following items are not implemented or not demonstrated. They are ordered by
correctness and production risk rather than source-code size.

### C1. Protocol correctness matrix

Implement hardware end-to-end tests for:

- client-streaming, server-streaming, and bidirectional streaming;
- deadlines and cancellation while a read, write, EAGAIN retry, or close is in
  flight;
- GOAWAY, keepalive ping, half-close, backend close, and server restart;
- large metadata, fragmented HTTP/2 headers, and payloads across the 8 KiB RX and
  64 KiB TX boundaries;
- multiple simultaneous RPCs on one HTTP/2 connection;
- TLS and mTLS with explicit authority/SNI and certificate validation.

Acceptance requires exact payloads, zero duplicate callbacks, bounded shutdown,
no RX-credit leak, and sanitizer-clean local tests. Every terminal race must end
in one status for each pending operation.

### C2. Production connection path

The current client requires explicit Endpoint injection and therefore bypasses
normal resolver/subchannel behavior. Implement a hybrid `DmeshEventEngine` or an
equivalent supported gRPC extension that recognizes `dpumesh:///SERVICE` and
delegates all unrelated addresses and timers to gRPC's default EventEngine.

The DPUmesh branch must implement asynchronous connect, cancellation of connect,
deadline/backoff, address objects, and reconnect. The server branch must expose a
listener abstraction rather than requiring application-owned PassiveListener
attachment. Ordinary `CreateCustomChannel` and server address bootstrap should
then select DPUmesh without using private adapter headers.

Acceptance requires mixed TCP and DPUmesh channels in one process, resolver
failure propagation, connect cancellation, reconnect after backend/DPU restart,
and no global EventEngine registration side effects on unrelated channels.

### C3. Allocator and memory quota

The smoke and QPS programs use a simple allocator that always reserves the
requested maximum and does not enforce quota. Replace it with a resource-quota-
backed allocator owned by the gRPC channel/server. Propagate allocation failure as
`ResourceExhausted` without leaking the native RX credit or hanging a pending
read.

Profile the two baseline copies before attempting zero-copy. RX zero-copy would
require a slice destructor that returns exactly one native credit while ensuring
the channel outlives every slice; TX zero-copy would require gRPC to serialize
directly into registered DPUmesh memory. Neither optimization is valid as a
pointer substitution.

### C4. Native TX-writable notification

The 50 microsecond timer is needed because the C API exposes no event when a TX
reservation becomes available. Add a native notification or completion tied to
per-QP/shared-pool reclaim. The reactor should arm it only while writes are
blocked and remove periodic retry in the idle state.

Acceptance requires no missed wakeup, no busy loop, bounded retry after every
reclaim path, and lower or equal p99/CPU under an EAGAIN-heavy benchmark.

### C5. Long-duration lifecycle and failure containment

Run at least 100,000 QP cycles and repeated channel cycles while sampling host
RSS/fd count, DPU imported mmap/buf-array counts, pod-slot generations, egress
lanes, and DPA ring tables. Values must plateau after quiescence.

Inject failures at each initialization and unregister stage, including lost or
late ring ACK, egress DMA error, DPU disconnect, client SIGKILL, server SIGKILL,
and host container restart. Graceful failures must preserve the barrier. Forced
death requires an explicit containment mechanism before claiming safety for
already-submitted DMA into host memory.

### C6. Performance coverage

Extend measurement beyond the focused sync-unary harness:

- three or more repetitions per point and reported median/spread;
- multiple channels/subchannels and reactor-count scaling;
- async and callback clients/servers;
- streaming and payload/metadata sweeps;
- direct TCP, DPUmesh, and Envoy HTTP/2 with matched features;
- client, server, Envoy, host progress, DPU ARM, DPA, and DMA counters;
- copy/allocator, CQ, retry-timer, and SG-DMA profiles.

Do not call a DPUmesh result a sidecar replacement result unless the Envoy path
uses comparable HTTP/2 routing and enabled features.

### C7. Distribution and compatibility

Install public adapter headers, export CMake package targets, define library
SONAME/version policy, add an example package, and test a compiler/gRPC matrix.
Private gRPC API dependence must remain exact-version gated. A stable public
release either needs an upstream-supported endpoint/listener extension or a
maintained compatibility layer for each supported gRPC version.

## 9. C++ execution phases

| Phase | Deliverable | Exit gate |
|---|---|---|
| C++-A | Streaming, deadlines, cancellation, large boundaries | Local sanitizers plus real BlueField exact-data tests |
| C++-B | TLS/mTLS and authority behavior | Certificate-positive and certificate-negative tests |
| C++-C | Hybrid EventEngine/resolver/listener | Normal channel/server bootstrap, mixed TCP/DPUmesh, reconnect |
| C++-D | Quota allocator and native writable event | Exhaustion correctness and no timer polling while idle |
| C++-E | Long churn and failure injection | Resource plateau and bounded teardown evidence |
| C++-F | Packaging and full performance matrix | Reproducible install plus repeated matched measurements |

Phases A and B protect protocol correctness before a larger integration surface.
The writable event and quota allocator can be developed independently after
their native API contracts are fixed. Zero-copy is considered only after profiles
show that the baseline copies dominate.

## 10. Go gRPC design

### 10.1 Feasibility and boundary

The C implementation of DPUmesh is not a problem. A stable `extern "C"` API is a
normal cgo boundary. The C++ EventEngine adapter cannot be reused because grpc-go
has its own transport implementation and expects Go `net.Conn`/`net.Listener`
objects rather than C++ EventEngine Endpoints.

The least invasive Go integration is:

```text
grpc-go
  ├─ client: grpc.WithContextDialer(dmeshDialer)
  └─ server: grpcServer.Serve(dmeshListener)
                         │
                    Go net.Conn
                         │ cgo
                    DPUmesh C shim
                         │
                 libdpumesh.so / dmesh.h
```

Generated `.pb.go` and `_grpc.pb.go` files, messages, stubs, interceptors,
handlers, and business logic remain unchanged. Only client dial bootstrap and
server listener bootstrap change. grpc-go itself does not need to be forked.
LD_PRELOAD is unsuitable because Go networking often uses raw runtime syscalls
instead of libc symbols.

### 10.2 Repository structure

Create an independent integration with this shape:

```text
integrations/grpc-go/
├─ go.mod
├─ dmesh/
│  ├─ runtime.go          channel ownership and CQ reactor shards
│  ├─ conn.go             net.Conn
│  ├─ listener.go         net.Listener
│  ├─ deadline.go         deadline/cancellation wake state
│  ├─ errors.go           errno to net.Error mapping
│  ├─ shim.h              narrow cgo-safe ABI
│  ├─ shim.c              public dmesh.h calls and stable opaque wrappers
│  └─ cgo.go              build/link directives and C declarations
├─ internal/fakeshim/     deterministic non-DOCA test implementation
├─ cmd/smoke/             unary/streaming hardware smoke
└─ cmd/qps/               same benchmark service and matrix
```

The shim must not expose internal DPUmesh structures to Go. It should use opaque
handles and fixed-width value structs for completions. It must return an explicit
error number for every operation instead of relying on a later cgo call reading
thread-local `errno`.

No Go pointer may be retained by C. Do not store a Go pointer in `qp->user_data`.
Use a C-allocated numeric connection token and a Go map owned by the reactor.

### 10.3 Runtime and CQ ownership

One Go `Runtime` owns one C channel. It creates one or more reactor shards; each
shard owns one C CQ and exactly one locked or dedicated goroutine that performs
all poll and QP lifecycle calls for that CQ.

```text
Go callers                      CQ owner goroutine
   │ command channel/eventfd         │
   ├────────────────────────────────►│ create/close/write commands
   │                                 │ dmesh_poll_cq (only consumer)
   │◄────────────────────────────────┤ RX/FIN/accept state + wake channels
```

Do not call potentially owner-sensitive QP operations concurrently from arbitrary
goroutines. A `net.Conn.Write` request is copied into Go-owned pending state and
executed on the CQ owner. The owner defers QP destruction until the full poll
batch has been processed, exactly as the C++ reactor does.

Runtime close order is:

```text
reject new dial/accept
→ wake and fail blocked Conn operations
→ destroy all QPs on their CQ owners
→ stop/join reactor goroutines
→ destroy CQs
→ destroy channel and wait for POD_QUIESCED
```

### 10.4 `net.Conn` contract

`DmeshConn` must implement:

```go
Read([]byte) (int, error)
Write([]byte) (int, error)
Close() error
LocalAddr() net.Addr
RemoteAddr() net.Addr
SetDeadline(time.Time) error
SetReadDeadline(time.Time) error
SetWriteDeadline(time.Time) error
```

Required semantics:

- One concurrent reader and one concurrent writer are supported, as required by
  `net.Conn`; multiple concurrent readers or writers are serialized.
- `Read` presents a continuous byte stream. It maintains a cursor across native
  fragments and may return fewer bytes than requested.
- RX data is copied into Go-owned memory before the CQ owner calls
  `dmesh_wc_release()`. A Go slice must never alias a released native slot.
- `Write` returns only after all input bytes have been copied into accepted native
  TX reservations, or returns the count accepted before a terminal error.
- Large writes split at `dmesh_post_max()` without inserting message boundaries
  visible to grpc-go.
- `EAGAIN` parks the writer and resumes on a native writable event when available;
  the baseline may use a bounded timer plus CQ progress.
- Peer FIN becomes `io.EOF` only after buffered RX bytes are consumed.
- `Close` is idempotent, wakes blocked read/write calls, and requests owner-thread
  QP destruction.

Deadlines apply to operations that are already blocked as well as future calls.
Use monotonically evaluated timers and generation tokens so replacing a deadline
cannot let an older timer fail a newer operation. Timeout errors must satisfy
`net.Error` with `Timeout() == true` and `Temporary() == true` only where grpc-go
expects retryable behavior.

### 10.5 `net.Listener` contract

`DmeshListener` owns a bounded Go accept queue populated from
`DMESH_WC_CONN_REQ`. `Accept()` blocks until a connection, listener close, or
terminal runtime error. `Close()` rejects future native QPs, wakes all blocked
accept calls, and does not close connections already returned to grpc-go.

```go
lis, err := dmesh.Listen(runtime)
grpcServer := grpc.NewServer()
pb.RegisterBenchmarkServiceServer(grpcServer, service)
err = grpcServer.Serve(lis)
```

If the accept queue is full, the owner must close the new QP rather than block CQ
progress. Queue overflow needs an observable counter and warning-level summary,
not a per-event hot-path log.

### 10.6 Client bootstrap

```go
rt, err := dmesh.NewRuntime(options)
conn, err := grpc.NewClient(
    "passthrough:///echo-dpumesh",
    grpc.WithContextDialer(rt.DialContext),
    grpc.WithTransportCredentials(insecure.NewCredentials()),
)
client := pb.NewBenchmarkServiceClient(conn)
```

`DialContext` extracts or is configured with the DPUmesh Service name, submits a
QP create command, and respects context cancellation before and after QP
creation. A QP that finishes after cancellation is destroyed and never returned.
The dialer returns a `net.Conn`; grpc-go continues with its normal HTTP/2 client
transport.

For TLS, return the raw DPUmesh `net.Conn` and let grpc-go perform its normal TLS
handshake. The target authority and `credentials.Config` server name must be set
explicitly because the DPUmesh service name is not automatically a DNS identity.

### 10.7 Error mapping

| Native condition | Go surface |
|---|---|
| Unknown service / `ENOENT` | Dial error wrapping an unavailable destination |
| `ENOMEM`, `EMFILE`, `ENOSPC` | Resource exhaustion error |
| TX `EAGAIN` | Internal parked write, not returned as a permanent error |
| `EBADMSG`, DMA/control failure | `net.OpError` and connection termination |
| Peer FIN after buffered data | `io.EOF` |
| Local close | `net.ErrClosed` |
| Read/write deadline | timeout `net.Error` |
| Context canceled during dial | `context.Canceled` |
| Dial deadline | `context.DeadlineExceeded` |

Error strings include operation and service/token but never credentials, payload
bytes, pointer values, or registry secrets.

### 10.8 Go testing

The non-hardware test suite must run under `go test -race ./...` and use a fake C
shim or build-tagged fake backend. It covers:

- partial reads across multiple native completions;
- large writes split across post maximum;
- EAGAIN park/resume without busy looping;
- simultaneous one-reader/one-writer traffic;
- read, write, and combined deadline replacement races;
- FIN after buffered data, local close, double close, and runtime close;
- accept queue overflow and listener close;
- dial cancellation before and after native QP creation;
- deferred QP destruction after a completion batch;
- no Go pointer retained by C and no callback after object reclamation.

Run the C shim/native library under ASAN/UBSAN independently because the Go race
detector cannot detect C memory corruption. Use a TSAN-compatible C build only in
a separate configuration.

Hardware tests use ordinary grpc-go unary, all streaming forms, deadlines,
cancellation, keepalive, large payloads, TLS/mTLS, 30+ channel lifecycle rounds,
unexpected disconnect recovery, and exact payload verification. Resource counts
must plateau after repeated runtime creation/destruction.

### 10.9 Go performance comparison

Use grpc-go's benchmark service definitions and the same server implementation
over two listeners:

- normal TCP `net.Listener` and default dialer;
- `DmeshListener` and `DialContext`.

Record QPS, percentiles, failures, Go process CPU/RSS/GC, cgo calls per RPC,
reactor CPU, DPU ARM CPU, DPA configuration, and native EAGAIN counters. Sweep
concurrency, channels, payload, streaming, reactor count, and GOMAXPROCS. A C++
comparison is informative only after matching RPC shape, payload, channel count,
and transport security.

## 11. Go execution phases

| Phase | Deliverable | Exit gate |
|---|---|---|
| Go-A | Narrow cgo shim and fake backend | C ABI tests and explicit error capture |
| Go-B | Runtime, one CQ owner, `net.Conn` | `net.Pipe`-style stream tests and `go test -race` |
| Go-C | Listener and grpc-go unary | Local fake end-to-end RPC |
| Go-D | Real BlueField unary and lifecycle | Exact bytes, quiesced teardown, reconnect |
| Go-E | Streaming, deadlines, cancellation, TLS | Full grpc-go transport correctness matrix |
| Go-F | Multi-reactor scaling and QPS | Repeated matched TCP/DPUmesh data with CPU accounting |

## 12. Definition of production readiness

The C++ or Go integration is not production-ready until all of these statements
are backed by retained tests and measurement artifacts:

- every RPC form works across payload and metadata fragmentation boundaries;
- TLS/mTLS identity, authority, deadlines, cancellation, keepalive, and GOAWAY
  behave like the language's normal TCP gRPC transport;
- backpressure is event-driven or demonstrated to consume no unacceptable idle
  CPU and has no missed wakeup;
- graceful teardown reaches remote quiescence before host unmap;
- forced process/container failure has an explicit, tested in-flight DMA safety
  model;
- repeated channel/QP churn reaches a stable resource plateau;
- public build/install targets and exact version compatibility are documented;
- performance is reported against matched direct TCP and matched Envoy HTTP/2,
  including host and DPU resource cost;
- routine logs remain warning-level and diagnostic verbosity is opt-in.

Until then, the accurate statement is: the C++ proof is a real working gRPC
transport integration with demonstrated unary correctness and graceful lifecycle;
it is not yet a transparent general-purpose replacement for every gRPC socket
path. The Go integration is a bounded new adapter over the same C API, not a
rewrite of DPUmesh or grpc-go.
