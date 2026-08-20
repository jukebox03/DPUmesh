# DPUmesh

DPUmesh is a BlueField service-mesh transport built with DOCA Comch, DPA, and
DMA. Applications address a Kubernetes Service; the DPU owns backend selection,
connection tracking, host-to-DPU forwarding, and reverse DMA. The default mode
is an ordered L4 byte stream; in that mode DPUmesh does not terminate TLS or
interpret HTTP/2. Services explicitly assigned to the embedded L7 layer are the
exception described below.

This repository is a research prototype. The evaluation contract and
measurements are in the [performance report](bench/report/REPORT.md).

## Architecture

A sending application writes into memory the transport registered with the DPU
and publishes a descriptor. The DPU reads those bytes, chooses the backend, and
writes them straight into the receiving application's registered memory. No copy
travels through the host kernel, and no proxy process runs beside either pod.

```text
           host                        BlueField DPU                      host
 +-----------------------+       +----------------------+       +-----------------------+
 | client app or gRPC    |       | DPA execution unit   |       | backend app or gRPC   |
 | libdpumesh.so.4       |       | ARM data worker      |       | libdpumesh.so.4       |
 | registered memory     |       | staging and routing  |       | registered memory     |
 +-----------------------+       +----------------------+       +-----------------------+

 1. forward ring   the client publishes a descriptor for the bytes it wrote
 2. DPA + ARM      the DPU stages those bytes and selects the backend
 3. DMA            a scatter-gather DMA writes them into the backend's memory
 4. reverse ring   completions report delivered bytes and return send capacity
```

### Terms

The rest of this repository uses these names. The first three are borrowed from
RDMA verbs, because the native API has the same shape.

| Term | Meaning |
|---|---|
| channel | one process's transport: its DPU registration, its registered memory, and its rings |
| QP | one full-duplex byte stream on that channel — the equivalent of a TCP connection |
| EQ (event queue) | what one application thread polls for the events of the QPs it owns |
| forward ring | host→DPU descriptor queue; a pod has `K` of them |
| reverse ring | DPU→host completion queue: bytes delivered, and send capacity returned |
| DPA | the BlueField data-path accelerator; its execution units (EUs) drain forward rings |
| ARM data worker | a DPU CPU thread that owns routing, DMA and reverse publication for its connections |
| staging | DPU memory holding arrived bytes until the DPU has finished sending them on |
| `N` / `K` / `A` / `L` | DPA execution units / forward rings per pod / ARM data workers / receive-side landing stripes |

The host exposes three integration surfaces:

| Surface | Purpose |
|---|---|
| `<dpumesh/dmesh.h>` | Native channel/EQ/QP API with registered TX and zero-copy RX |
| `libdmesh_preload.so` | POSIX socket compatibility for libc-based C/C++ binaries |
| `integrations/grpc` | gRPC C++ v1.80 Endpoint and PassiveListener integration |

The shared libdpumesh send core batches all three surfaces. `dmesh_alloc()`
reserves registered bytes and `dmesh_post_send()` commits them into one ordered
stream: complete transport units submit at once, and a partial tail is published
at a bounded deadline unless `dmesh_flush()` forces it earlier. The size of that
unit and its timing are internal, not application tuning parameters, and the
preload and gRPC layers keep no batch queue or timer of their own.

Every QP is one full-duplex byte stream. Optional DPU L7 framing is an internal
routing policy and does not expose backend or stream ids through native events.
The in-tree L7 validator uses a simple length-prefixed benchmark frame; gRPC
uses backend-pinned L4 passthrough unless its service is assigned to the L7
layer, which terminates HTTP/2 on the DPU.

Backpressure is nonblocking. `dmesh_alloc()` returning `NULL/EAGAIN` arms that QP
itself, and returned capacity produces one `DMESH_EVENT_TX_READY` on its EQ:
applications park the named write and retry it from the event. Readiness is a
one-shot hint, not a reservation. [design/API.md](design/API.md) is the contract.

## Lifecycle

Channel creation returns only after a replayable two-phase barrier:

```text
POD_REGISTER → POD_ASSIGNED → memory and ring import → all DPA RING_ADD_ACKs
             → POD_INIT_RESULT(READY, L)
```

A node agent's signed grant precedes every `POD_REGISTER`, and the DPU admits
only the Service that grant authorizes.
[design/CONTROL.md](design/CONTROL.md) is the contract.

The host retries registration while either assignment or readiness is pending;
the DPU treats identical registration as idempotent. Missing DPA add ACKs are
also retried. Graceful destruction similarly retries `POD_UNREGISTER` until the
DPU has removed every ring, finished every DMA that reads the pod's memory,
destroyed the mappings it imported, and replied `POD_QUIESCED`.

Those retries are phase-local. A ready channel does not periodically send
registration heartbeats, and unregister traffic starts only when channel
destruction begins. The steady-state data plane uses the imported rings and
reverse DMA path.

Each registration of a pod slot carries a generation number, so a DMA completion
that arrives late cannot be attributed to the slot's next occupant. A DMA fault
restarts that worker's DMA engine without unpublishing healthy pods, and a
current-generation payload batch is retried once, in order. Removing a pod and
tearing its mappings down remains the control connection's decision.

## Repository

```text
include/dpumesh/       public C API
src/                   host core, native facade, resolver, preload facade
doca/                  BlueField ARM process and DPA kernel
integrations/grpc/     gRPC C++ runtime, reactor, tests, benchmark
linkerd/               DPU-side L7 layer: adapter ABI, consumers, port submodule
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
[`bench/README.md`](bench/README.md#6-correctness-validation).

The BlueField program is built from `doca/meson.build`. The supported benchmark
bring-up path rebuilds and deploys both sides together:

```sh
DPUMESH_DPA_THREADS=16 \
DPUMESH_ARM_WORKERS=2 \
DPUMESH_RINGS_PER_POD=8 \
./bench/bench.sh deploy
./bench/bench.sh latency both
```

A bare deploy selects one ARM data worker. Each data worker owns the completion
queue it drains, the state of its connections, its DMA engine, and the reverse
rings it publishes to. `K` sets forward rings per pod, `A` sets ARM data
workers, and the 64 MiB receive mapping is divided into `L=A` landing stripes —
the disjoint regions the DPU writes into, one per worker. `K` and `N` must be
multiples of `A`; an incompatible worker count is reduced at startup and
reported in the DPU log.

`DPUMESH_DPA_THREADS` sets `N` and `DPUMESH_ARM_WORKERS` sets `A`; both are
clamped at startup, `N` to 32 EUs and `A` to 8 workers.

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
| [design/CONTROL.md](design/CONTROL.md) | naming, trusted registration, signed feeds, the Linkerd control plane, and node scope |
| [design/L7.md](design/L7.md) | the DPU-side Linkerd runtime: ownership, custody, the adapter ABI, and limits |
| [design/GRPC.md](design/GRPC.md) | how the gRPC adapter maps chttp2 onto the transport |

**Integrations**

| Document | Covers |
|---|---|
| [integrations/grpc/README.md](integrations/grpc/README.md) | building and running gRPC over DPUmesh |
| [linkerd/README.md](linkerd/README.md) | building and deploying the embedded Linkerd consumer |

**Measurement**

| Document | Covers |
|---|---|
| [bench/README.md](bench/README.md) | deployment, the experiment commands, the measurement rules, and the host-only and hardware validation gates |
| [integrations/grpc/bench/README.md](integrations/grpc/bench/README.md) | the gRPC workloads and collectors |
| [linkerd/bench/README.md](linkerd/bench/README.md) | the linkerd sidecar columns of the gRPC evaluation |

**Reports**

| Report | Question it answers |
|---|---|
| [bench/report/REPORT.md](bench/report/REPORT.md) | what DPUmesh costs at L4 against Envoy sidecars |
| [bench/report/REPORT_L7.md](bench/report/REPORT_L7.md) | what the DPU's L7 layer costs per message, per connection and per request |
| [bench/report/REPORT_CORE.md](bench/report/REPORT_CORE.md) | where the cores go, attributed per component |
| [integrations/grpc/bench/report/REPORT_GRPC.md](integrations/grpc/bench/report/REPORT_GRPC.md) | whether the L4 win carries to gRPC, and how it scales with cores and channels |
| [linkerd/bench/report/REPORT_LINKERD.md](linkerd/bench/report/REPORT_LINKERD.md) | linkerd as a sidecar, against Envoy and DPUmesh |
