# DPUmesh

DPUmesh is a BlueField service mesh built with DOCA Comch, DPA, DMA, and an
embedded Linkerd proxy. Applications address a Kubernetes Service; the DPU owns
policy, discovery, backend selection, connection state, and Host↔DPU transfer.
Native and preload connections use Linkerd's opaque byte-stream path. gRPC uses
its HTTP/2 path. Local endpoints are reached by DMA and remote endpoints by the
RDMA peer channel.

This repository is a research prototype. The design documents below define its
transport, proxy, control-plane, and API contracts.

## Architecture

A sending application writes into memory the transport registered with the DPU
and publishes a descriptor. The DPU reads those bytes, chooses the backend, and
writes them straight into the receiving application's registered memory. No copy
travels through the host kernel, and no proxy process runs beside either pod.

```text
           host                        BlueField DPU                      host
 +-----------------------+       +----------------------+       +-----------------------+
 | client app or gRPC    |       | DPA execution unit   |       | backend app or gRPC   |
 | libdpumesh.so.5       |       | ARM data worker      |       | libdpumesh.so.5       |
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

Every QP is one full-duplex byte stream into the DPU-hosted Linkerd proxy and
does not expose backend or proxy-session ids through native events. Native and
preload Services enter Linkerd as opaque streams; the gRPC Service enters its
HTTP/2 path. Linkerd's selected backend is then reached through DMA on this node
or the RDMA-backed peer channel across nodes.

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
controller/            cluster controller: topology generation, workload scope
integrations/grpc/     gRPC C++ runtime, reactor, tests, benchmark
linkerd/               DPU-side L7 layer: adapter ABI, consumers, port submodule
bench/                 deployment, workloads, validators, measurement records
tests/                 fast host-only ABI and state-machine regression tests
design/                current API, data-plane, control-plane and gRPC whitepapers
```

## Build and test

In a DOCA development environment:

```sh
make -j2
make test
```

The build produces `build/lib/libdpumesh.so.5`, the preload library, and native
bench/validator binaries. The library target tracks all source and header inputs;
public-header changes therefore rebuild both ABI and consumers.

`make test` runs the host-only native contract suite without requiring a DPU.
Its scope and relationship to hardware validation are documented in
[`bench/README.md`](bench/README.md#6-correctness-validation).

The BlueField program is built from `doca/meson.build`. The supported benchmark
bring-up path rebuilds and deploys both sides together:

```sh
./bench/bench.sh deploy
./bench/bench.sh latency dpumesh
```

The deployment geometry is `N/K/A/L=32/8/8/8`: 32 DPA execution units, eight
rings per Pod, eight ARM data workers, and eight receive landing stripes. Each
Pod exposes one ring to each worker; each worker owns four DPA EUs, its
connection shard, DMA engine, reverse-ring producer, and Linkerd runtime. A
connection remains on that shard for its lifetime.

The gRPC adapter has an independent CMake build:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0
cmake --build build/grpc -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/grpc --output-on-failure
```

LeakSanitizer is disabled; AddressSanitizer and the functional test suite run.
Clients use a Service-name target and ordinary gRPC channel arguments. Each
connection attempt creates a QP and a session-local HTTP/2 transport in the
DPU-hosted Linkerd proxy; its selected backend remains pinned for that session.

## Documentation

Design documents define the contracts; benchmark documents define the
DPU-hosted Linkerd/DMA deployment and its validation method.

**Design**

| Document | Defines |
|---|---|
| [design/API.md](design/API.md) | the native `<dpumesh/dmesh.h>` contract: lifecycle, batching, EQ, errors |
| [design/DATA.md](design/DATA.md) | the data plane: host/DPA/ARM custody, rings, replay barriers, and the DPU-side Linkerd runtime that rides on them |
| [design/CONTROL.md](design/CONTROL.md) | the control plane at both scopes: naming, trusted registration, signed feeds, the Linkerd control plane, the cluster controller and the peer channel |
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
