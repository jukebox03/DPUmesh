# DPUmesh

DPUmesh is a BlueField service-mesh transport built with DOCA Comch, DPA, and
DMA. Applications address a Kubernetes Service; the DPU owns backend selection,
connection tracking, host-to-DPU forwarding, and reverse DMA. The default mode
is an ordered L4 byte stream. DPUmesh does not terminate TLS or interpret HTTP/2.

This repository is a research prototype. The evaluation contract and
measurements are in the [performance report](bench/report/REPORT.md).

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

The shared libdpumesh send core batches all three surfaces. `dmesh_alloc()`
reserves registered bytes and `dmesh_post_send()` commits them into one ordered
stream. A post automatically submits every newly complete transport batch;
`dmesh_flush()`
forces only the newest partial batch. An idle tail publishes immediately; a busy
tail is combined and published at a bounded deadline. The physical unit and
timing are internal data-plane choices, not application tuning parameters.
Graceful close flushes the trailing partial unit; abort discards it. Native
publication writes the shared descriptor ring polled by the DPA. The preload and
gRPC layers only adapt POSIX and EventEngine semantics; neither keeps a physical
batch queue or batching timer.

Every public QP is one full-duplex byte stream. Optional DPU L7 framing is an
internal routing policy and does not expose backend or stream ids through native
events. The in-tree L7 validator uses a simple length-prefixed benchmark
frame; gRPC continues to use backend-pinned L4 passthrough so chttp2 owns HTTP/2.

Backpressure remains nonblocking. If `dmesh_alloc()` returns `NULL/EAGAIN`, it
also arms that QP internally. Capacity returned by a QP ACK or by the channel's
shared registered-block pool produces one `DMESH_EVENT_TX_READY` event on the
QP's EQ and wakes the same optional EQ fd used for receive events. Applications
park only the named write and retry it from the event; there is no separate arm
call and no per-QP fd. Readiness is a one-shot retry hint rather than a capacity
reservation, so another `EAGAIN` arms the next transition.

## Lifecycle

Channel creation returns only after a replayable two-phase barrier:

```text
POD_REGISTER → POD_ASSIGNED → mmap/ring import → all DPA RING_ADD_ACKs
             → POD_INIT_RESULT(READY, L)
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

Per-slot DMA generations reject delayed work from a prior registration. A
worker-level DMA fault restarts the shared context without unpublishing healthy
pods. Current-generation payload batches receive one ordered retry; control-path
disconnect remains authoritative for pod removal and mapping teardown.

## Repository

```text
include/dpumesh/       public C API
src/                   host core, native facade, resolver, preload facade
doca/                  BlueField ARM process and DPA kernel
integrations/grpc/     gRPC C++ runtime, reactor, tests, benchmark
linkerd/               DPU-side L7 layer: contract, consumers, port submodule
bench/                 deployment, workloads, validators, measurement records
tests/                 fast host-only ABI and state-machine regression tests
design/                current API, core, naming, L7, and gRPC whitepapers
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
DPUMESH_ARM_WORKERS=2 \
DPUMESH_RINGS_PER_POD=8 \
./bench/bench.sh deploy
./bench/bench.sh latency both
```

A bare deploy selects one ARM data worker. Each polling worker owns its DPA
consumer PE, connection state, SG-DMA context, completion callbacks, and
reverse-ring producers. `K` controls rings, `A` controls ARM workers, and the
64 MiB RX mapping uses `L=A` landing stripes. `K` and `N` must be multiples of
`A`; an incompatible worker count is reduced at startup and reported in the DPU
log.

`DPUMESH_DPA_THREADS` accepts up to 32 EUs; automatic selection uses up to 32.
`DPUMESH_ARM_WORKERS` sets the number of ARM data workers.

The gRPC adapter has an independent CMake build:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0
cmake --build build/grpc -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/grpc --output-on-failure
```

LeakSanitizer is disabled; AddressSanitizer and the functional test suite run.
Clients use a Service-name target and ordinary gRPC channel arguments. Each
connection attempt creates a QP; established L4 streams remain backend-pinned.

## Documentation

Design documents define the current contracts; benchmark documents say how a
number was produced; reports carry the numbers.

**Design**

| Document | Defines |
|---|---|
| [design/API.md](design/API.md) | the native `<dpumesh/dmesh.h>` contract: lifecycle, batching, EQ, errors |
| [design/CORE.md](design/CORE.md) | host/DPA/ARM custody, rings, and replay barriers |
| [design/CONTROL.md](design/CONTROL.md) | naming, identity, registry, and the node boundary |
| [design/L7.md](design/L7.md) | the L7 layer: modes, custody, egress arena, backend selection |
| [design/GRPC.md](design/GRPC.md) | how the gRPC adapter maps chttp2 onto the transport |

**Integrations**

| Document | Covers |
|---|---|
| [integrations/grpc/README.md](integrations/grpc/README.md) | building and running gRPC over DPUmesh |
| [linkerd/README.md](linkerd/README.md) | the DPU-side L7 layer: layout, modes, consumers |
| [linkerd/CONTRACT.md](linkerd/CONTRACT.md) | the normative datapath/port interface and its ABI |

**Measurement**

| Document | Covers |
|---|---|
| [bench/README.md](bench/README.md) | deployment, the experiment commands, and the measurement rules |
| [bench/validators/README.md](bench/validators/README.md) | hardware validators and what each one exercises |
| [tests/README.md](tests/README.md) | host-only contract tests, no hardware needed |
| [integrations/grpc/bench/README.md](integrations/grpc/bench/README.md) | the gRPC workloads and collectors |
| [linkerd/bench/README.md](linkerd/bench/README.md) | the linkerd sidecar columns of the gRPC evaluation |

**Reports**

| Report | Question it answers |
|---|---|
| [bench/report/REPORT.md](bench/report/REPORT.md) | what DPUmesh costs at L4 against Envoy sidecars |
| [bench/report/REPORT_L7.md](bench/report/REPORT_L7.md) | what backend-selection granularity costs on the DPU |
| [bench/report/REPORT_CORE.md](bench/report/REPORT_CORE.md) | where the cores go, attributed per component |
| [integrations/grpc/bench/report/REPORT_GRPC.md](integrations/grpc/bench/report/REPORT_GRPC.md) | whether the L4 win carries to gRPC, and how it scales with cores and channels |
| [linkerd/bench/report/REPORT_LINKERD.md](linkerd/bench/report/REPORT_LINKERD.md) | linkerd as a sidecar, against Envoy and DPUmesh |
