# gRPC over DPUmesh

This directory implements the gRPC C++ EventEngine transport described in
[`plan/gRPC.md`](../../plan/gRPC.md). It deliberately remains in the DPUmesh
repository because the production transport requires coordinated changes to
both this adapter and the native DPUmesh API.

The directory is an independent CMake project. It does not include DPUmesh
internal headers and it does not make the main `Makefile` depend on gRPC.

## Version contract

- gRPC: exactly `v1.80.0`
- C++: C++17 or newer
- DPUmesh: public `<dpumesh/dmesh.h>` and `libdpumesh.so.1`

EventEngine is experimental. A gRPC upgrade is therefore a deliberate source
and test migration, not an unbounded compatible dependency update.

## Local build

Prepare the DPUmesh library first:

```sh
make lib
```

Use an exact gRPC v1.80.0 source checkout with its required submodules:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DBUILD_TESTING=ON
cmake --build build/grpc -j
ctest --test-dir build/grpc --output-on-failure
```

Adapter tests can be rebuilt with ASAN and UBSAN without rebuilding gRPC:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=ON
cmake --build build/grpc -j
ctest --test-dir build/grpc --output-on-failure
```

When tests run under `ptrace`, LeakSanitizer may be unavailable. In that
environment use `ASAN_OPTIONS=detect_leaks=0`; AddressSanitizer and UBSan
checks remain active.

ThreadSanitizer uses a separate configuration because it cannot be combined
with ASan:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=OFF \
  -DDPUMESH_GRPC_ENABLE_TSAN=ON
cmake --build build/grpc -j
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/grpc --output-on-failure
```

If gRPC v1.80.0 is installed as a CMake package,
`DPUMESH_GRPC_SOURCE_DIR` may be omitted.

## Current implementation boundary

Implemented through the BlueField unary/reclaim phase:

- exact gRPC version and public-header build gate
- testable wrapper for the public DPUmesh C API
- `EventEngine::Endpoint` Read, Write, FIN, failure and destruction state
- asynchronous work/callback separation
- deterministic fake transport tests, including TX backpressure
- one process channel with configurable round-robin CQ reactor shards
- single-consumer `poll`/eventfd owner thread per CQ
- mutex-protected MPSC command queue and command eventfd
- client QP creation with asynchronous status delivery
- registered TX allocation, copy, post-size splitting and immediate doorbell
- exact pending-TX set with timerfd retry after `EAGAIN`
- RX copy before `dmesh_wc_release`, FIN and CQ-failure delivery
- passthru stream-ID latch and fail-close on a stream change
- post-CQ-drain deferred QP destruction and CQ-before-channel shutdown
- pre-endpoint-bind receive/FIN buffering and ordered replay
- real BlueField runtime smoke using the actual native channel/CQ/QP API
- two-phase native channel initialization (`POD_ASSIGNED` then `POD_INIT_READY`)
- generation-safe per-EU `RING_ADD_ACK` barrier before native channel readiness
- native init-failure propagation and ordered Comch/mmap/PE/device cleanup
- client endpoint injection with gRPC v1.80
  `grpc::experimental::CreateChannelFromEndpoint`
- server endpoint injection with gRPC v1.80 `PassiveListener`
- native inbound `DMESH_WC_CONN_REQ` conversion to an accepted endpoint
- production client/server bridge helpers (`ConnectDmeshGrpcChannel` and the
  RAII `DmeshGrpcServerAttachment` returned by `AttachDmeshGrpcServer`)
- real chttp2 unary echo over paired `DmeshEndpoint` instances, including a
  4 KiB payload and forced 137-byte native post fragmentation
- real gRPC chttp2 unary echo over native DPUmesh on BlueField hardware
- host `POD_UNREGISTER` / DPU `POD_QUIESCED` graceful teardown barrier
- generation-checked, flushed per-EU `RING_DEL_ACK` before imported-handle
  destruction
- ARM SG-DMA lane/inflight/source-reference quiescence before remote reclaim
- generation on queued `FWD_DONE` work so a recycled pod slot cannot consume a
  previous tenant's completion
- idempotent delete retry and coalesced DPA WAKE messages
- DPU pod slot reuse after graceful teardown and unexpected disconnect

Not implemented yet:

- TX-writable notification and nonblocking flush API additions
- streaming, deadline/cancellation and TLS/mTLS tests on BlueField hardware
- a production resource-quota-backed allocator factory; the bridge currently
  requires the embedding application to supply an EventEngine allocator
- long-duration 100k+ lifecycle churn and resource-plateau measurement
- a defined isolation/recovery guarantee for process death with host-memory DMA
  already in flight

The reactor has been executed against a real-eventfd fake `DmeshApiOps`, under
ASan/UBSan and ThreadSanitizer. It has also run on BlueField through the real
`libdpumesh.so.1`: 2 reactor shards, 16 QPs, 1,120 byte-exact messages, and
29,220,480 wire bytes passed. A deliberately mismatched rings-per-pod setting
failed during native initialization and the same DPU pod slot was then reused by
a successful run. Both first-EU startup and existing-EU `ADD_RING` paths were
observed on hardware; readiness followed exactly one successful ACK per target
EU. `plan/gRPC.md` section 31 preserves that earlier bench-framing phase and its
then-open delete/quiesce limitation; section 33 documents the final ACK-based
reclaim implementation and supersedes that limitation.

The final hardware run used the real gRPC GenericStub/AsyncGenericService path,
not repository bench framing. Thirty consecutive server/client lifecycle rounds
passed without a DPU restart: 60 unary RPCs, the same slots 10/11 reused every
round, and `POD_QUIESCED` observed on both sides every round. A subsequent
3 x 1 MiB byte-exact run also passed. The final warning-level DPU log contained
no runtime errors. An unexpected-disconnect injection was followed by successful
reuse of the reclaimed slot. See `plan/gRPC.md` section 33 for the protocol
diagram, exact safety invariants, commands, evidence, and limitations.

Those items are intentionally layered on the tested endpoint state machine
rather than mixed into its first implementation.
