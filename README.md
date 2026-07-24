# DPUmesh

DPUmesh is a BlueField service-mesh transport built with DOCA Comch, DPA, and
DMA. Applications address a Kubernetes Service; the DPU owns backend selection,
connection tracking, host-to-DPU forwarding, and reverse DMA. The default mode
is an ordered L4 byte stream. DPUmesh does not terminate TLS or interpret HTTP/2.

This repository is a research prototype, not a claim that offload is already
faster than direct TCP. The measured baselines and limitations are kept in the
[performance report](bench/report/REPORT.md).

## Architecture

```text
C/C++ application or gRPC
          │
          ▼
libdpumesh.so.4 ─ registered TX/RX memory ─ BlueField DPA + ARM ─ backend TCP
          ▲
          └─ EQ events and optional epoll fd
```

The host exposes three integration surfaces:

| Surface | Purpose |
|---|---|
| `<dpumesh/dmesh.h>` | Native channel/EQ/QP API with registered TX and zero-copy RX |
| `libdmesh_preload.so` | POSIX socket compatibility for libc-based C/C++ binaries |
| `integrations/grpc` | gRPC C++ v1.80 Endpoint and PassiveListener integration |

The native send path batches by default. `dmesh_alloc()` reserves registered
bytes and `dmesh_post_send()` commits them into one ordered stream. A post
automatically submits every newly complete transport batch; `dmesh_flush()`
forces only the newest partial batch. The physical unit is an internal data-plane
choice, not an application tuning parameter. A future one-shot batching timer may
bound partial-batch latency; ABI 4 currently relies on flush or graceful close for
that tail, while abort discards it. Native publication writes a shared descriptor
ring that the DPA already polls; it is not a socket syscall or a per-flush control
message.

Every public QP is one full-duplex byte stream. Optional DPU L7 framing is an
internal routing policy and does not expose backend or stream ids through native
events. The in-tree L7 validator uses a simple length-prefixed benchmark
frame; gRPC continues to use backend-pinned L4 passthrough so chttp2 owns HTTP/2.

Backpressure remains nonblocking. If `dmesh_alloc()` returns `NULL/EAGAIN`, it
also arms that QP internally. Capacity returned by a QP ACK or by the channel's
shared registered-block pool produces one `DMESH_EVENT_TX_READY` event on the
QP's EQ and wakes the same optional EQ fd used for receive events. Applications
park only the named write and retry it from the event; they need no explicit
arm call, per-QP fd, retry timer, busy-poll, or scan of all QPs. Readiness is a
one-shot retry hint rather than a capacity reservation, so another `EAGAIN`
simply arms the next transition.

## Lifecycle

Channel creation returns only after a replayable two-phase barrier:

```text
POD_REGISTER → POD_ASSIGNED → mmap/ring import → all DPA RING_ADD_ACKs
             → POD_INIT_RESULT(READY)
```

The host retries registration while either assignment or readiness is pending;
the DPU treats identical registration as idempotent. Missing DPA add ACKs are
also retried. Graceful destruction similarly retries `POD_UNREGISTER` until the
DPU has removed every ring, drained ARM DMA custody, destroyed imported handles,
and replied `POD_QUIESCED`.

Those retries are phase-local. A ready channel does not periodically send
registration heartbeats, and unregister traffic starts only when channel
destruction begins. The steady-state data plane uses the imported rings and
reverse DMA path.

Per-slot DMA generations reject delayed work from a prior registration. Forced
process death during an already-issued DMA remains outside the graceful reclaim
guarantee.

## Repository

```text
include/dpumesh/       public C API
src/                   host core, native facade, resolver, preload facade
doca/                  BlueField ARM process and DPA kernel
integrations/grpc/     gRPC C++ runtime, reactor, tests, benchmark
bench/                 deployment, workloads, validators, measurement records
tests/                 fast host-only ABI and state-machine regression tests
design/                current API, core, and naming whitepapers
```

## Build and test

In a DOCA development environment:

```sh
make -j2
make test
```

The build produces `build/lib/libdpumesh.so.4`, the preload library, and native
bench/validator binaries. The library target tracks all source and header inputs;
public-header changes therefore rebuild both ABI and consumers.

`make test` runs the host-only native contract suite without requiring a DPU.
Its scope and relationship to hardware validation are documented in
[`tests/README.md`](tests/README.md).

The BlueField program is built from `doca/meson.build`. The supported benchmark
bring-up path rebuilds and deploys both sides together:

```sh
DPUMESH_DPA_THREADS=16 \
DPUMESH_INGEST_SHARDS=2 \
DPUMESH_RINGS_PER_POD=2 \
DPUMESH_ARM_PIN=1 \
./bench/bench.sh deploy
./bench/bench.sh latency both
```

A bare deploy selects one ARM data worker. With `A>=2`, each worker owns its DPA
consumer PE, connection/conntrack shard, parser/routing state, and matching
SG-DMA engine. `K` and `N` must be multiples of `A`; an incompatible requested
worker count is reduced at startup and reported in the DPU log.

`DPUMESH_DPA_THREADS` accepts up to 32 EUs; automatic selection uses up to 16.
`DPUMESH_INGEST_SHARDS` sets the number of homogeneous ARM data workers.

The gRPC adapter has an independent CMake build:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0
cmake --build build/grpc -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/grpc --output-on-failure
```

LeakSanitizer is disabled only because ptrace/sandboxed test execution makes its
process probe fail; AddressSanitizer and the functional test suite still run.
Clients use a Service-name target and ordinary gRPC channel arguments. Each
connection attempt creates a QP; established L4 streams remain backend-pinned.

## Documentation

- [Native API](design/API.md): exact lifecycle, batching, EQ, and error contracts
- [Core architecture](design/CORE.md): host/DPA/ARM custody and replay barriers
- [Naming](design/NAMING.md): registry, service identity, and routing meaning
- [gRPC integration](integrations/grpc/README.md): build and application bootstrap
- [Benchmark guide](bench/README.md): deployment and experiment commands
- [Native contract tests](tests/README.md): fast host-only regression coverage
- [Performance report](bench/report/REPORT.md): current topology evaluation
