# DPUmesh

DPUmesh is a service-mesh data plane built on NVIDIA BlueField DOCA Comch, DMA,
and DPA. Applications connect to a Kubernetes Service name rather than a pod
address. DPUmesh owns host-to-DPU transport, connection tracking, backend
selection, and reverse forwarding. Its default mode is L4 byte-stream
passthrough; it does not interpret the application protocol.

This document describes the 2026-07-19 working tree. The implementation is a
research prototype. In the existing single-pod L4 measurements, DPUmesh is not
faster than direct TCP or TCP+Envoy. At 1 KiB and concurrency 32, the observed
rates were 0.93 Mrps for direct TCP, 0.41 Mrps for TCP+Envoy, and 0.20 Mrps for
DPUmesh. DPUmesh reduces part of the host cost by consuming DPU ARM CPU. The
methodology and complete results are in the [performance report](bench/report/REPORT.md).

## Transport model

```text
application / gRPC HTTP/2
            │ byte stream
            ▼
libdpumesh.so ── Comch + registered DMA ── BlueField ARM/DPA
                                               │
                                      backend TCP connections
```

- L4 passthrough is the default. A connection remains pinned to one backend.
- A service configured with the `_FRAME_SVC`/`_L7_SVC` codec may load-balance at
  message boundaries. The DPU configuration selects the codec; the application
  does not.
- gRPC retains its own HTTP/2 framing. The current integration disables the
  DPUmesh L7 codec and uses DPUmesh only as a reliable byte stream.

## Public surfaces

| Surface | Intended user | Current status |
|---|---|---|
| `<dpumesh/dmesh.h>` | New C/C++ code | Native channel/CQ/QP API, zero-copy RX, registered TX |
| `libdmesh_preload.so` | Unmodified POSIX C/C++ binaries | Interposes sockets and performs the copies required by read/write |
| `integrations/grpc` | gRPC C++ v1.80 | Client Endpoint and server PassiveListener paths implemented |

The native API models one process transport as a channel, one single-consumer
polling thread as a CQ, and one full-duplex stream as a QP. `dmesh_alloc()` never
blocks and returns `EAGAIN` when TX capacity is unavailable. Every RX completion
must be returned with `dmesh_wc_release()`. See the [API whitepaper](design/API.md)
for the complete contract.

## Initialization and teardown

Pod assignment alone does not make a channel usable. Channel creation returns
only after every DPU ring import, mmap, DPA registration, and ARM egress resource
has completed and the host receives `POD_INIT_RESULT(READY)`. Graceful teardown
uses the following barrier:

```text
host destroys QPs/CQs
       │ POD_UNREGISTER
       ▼
DPU blocks routing → all-EU RING_DEL_ACK → ARM egress/inflight drain
       │ POD_QUIESCED
       ▼
host stops Comch → destroys exported mmap/device resources
```

Slot generations prevent delayed acknowledgements or forwarding completions from
touching a reused pod slot. Unexpected process loss triggers asynchronous DPU
cleanup, but production-grade containment of arbitrary in-flight host-memory DMA
during forced process death remains open work. See the [core whitepaper](design/CORE.md).

## Repository layout

```text
include/dpumesh/       public C headers
src/                   host core, native API, name resolver, preload facade
doca/                  BlueField ARM control/data plane and DPA kernel
integrations/grpc/     gRPC C++ endpoint, runtime, tests, and QPS benchmark
bench/                 deployment, matched workloads, validators, and reports
design/                API, core, and naming whitepapers
```

## Build and validation

Build the host library and benchmark programs in a DOCA development environment:

```sh
make
./bench/bench.sh deploy
./bench/bench.sh latency both
./bench/bench.sh point dpumesh 1024 1024 32 10 1000 1
```

The DPU code uses `doca/meson.build`. `bench/bench.sh deploy` performs source
synchronization, DPU build, host image creation, DPU startup, and pod startup in
the required order. Restarting only one side can desynchronize registration state,
so `deploy` is the single supported bring-up path. Keep machine-specific SSH, PCI,
and password values in the uncommitted `.env` file.

The gRPC C++ integration has an independent CMake build:

```sh
cmake -S integrations/grpc -B build/grpc \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0
cmake --build build/grpc -j2
ctest --test-dir build/grpc --output-on-failure
```

`grpc_dpumesh_qps_benchmark` runs the same generated
`grpc.testing.BenchmarkService` over either TCP or DPUmesh. Build and execution
details are in the [gRPC integration guide](integrations/grpc/README.md).

## Documentation map

- [API.md](design/API.md): public API and exact call contracts
- [CORE.md](design/CORE.md): host/DPU/DPA design and lifecycle invariants
- [NAMING.md](design/NAMING.md): service names, identity, registry, and control-plane gaps
- [bench/README.md](bench/README.md): reproducible experiment procedure
- [REPORT.md](bench/report/REPORT.md): measurements and limitations
- [STAGES.md](bench/suite/STAGES.md): evaluation coverage and interpretation rules
