# DPUmesh deployment and validation

This directory is the deployment surface for the supported architecture:

```text
native API ─┐
LD_PRELOAD ─┼─ Host↔DPU DMA ─ embedded Linkerd proxy ─ DMA/RDMA ─ backend
gRPC ───────┘
```

Native, preload, and gRPC workloads are application adapters over the same
DPU-hosted Linkerd mesh.

## One-shot deployment

Create the repository-root `.env` with the DPU/Host connection settings, then:

```sh
BENCH_DEPLOY_SCOPE=all ./bench/bench.sh deploy
```

The command deploys `N/K/A/L=32/8/8/8`. Each Pod has one ring and one RX
landing stripe per ARM worker, and all eight workers host an embedded Linkerd
runtime. Port affinity keeps each connection, proxy session, DMA engine, and
reverse-ring producer on one worker.

The command performs the complete lifecycle in order:

1. provisions the signed registration, topology, policy, identity, and service
   target feeds;
2. builds the Host library, preload shim, native workloads, and gRPC adapter;
3. builds the embedded Linkerd static library and the DPU binary;
4. starts the controller, node agent, DPU process, and API workloads;
5. pins the workloads to the BlueField-local NUMA node;
6. runs a real request through native, preload, and gRPC and fails deployment if
   any adapter does not traverse the embedded Linkerd path.

Narrow diagnostic scopes are `native`, `preload`, and `grpc`; each deploys the
corresponding application adapter over the same mesh.

## Supported workloads

| API | Client/server | Deployment |
|---|---|---|
| native | `bench_dpumesh` / `echo_dpumesh` | `bench-dpumesh`, `echo-dpumesh` |
| preload | `bench_sock` / `echo_sock` under `libdmesh_preload.so` | `preload-bench`, `preload-echo` |
| gRPC | `bench_grpc` / `echo_grpc` using the DPUmesh endpoint | `bench-grpc-dpumesh`, `echo-grpc-dpumesh` |

The loopback, verbs, and preload validator deployments exercise supported API
contracts on the same DMA mesh. Extra native echo pods validate Service
fan-out and endpoint pinning.

The native and preload workloads preserve the same application submission
boundary. Each client submits one logical request per native post or POSIX
`write()`, and each server submits one logical response the same way. A partial
POSIX write retains only the unfinished suffix of that frame. Complete transport
units and partial-tail deadlines are formed exclusively by the shared native
transport batching policy below both adapters.

## Validation commands

```sh
./bench/bench.sh point dpumesh 1024 8 1 5 100 1
./bench/bench.sh point preload 1024 8 1 5 100 1
./bench/bench.sh point grpc-dpumesh 1024 8 1 5 100 1

./bench/bench.sh loopback
./bench/bench.sh verbs
./bench/bench.sh preload
./bench/bench.sh grpcshutdown
./bench/bench.sh l7metrics
```

The standard latency, bandwidth, and rate families accept exactly one of
`dpumesh`, `preload`, or `grpc-dpumesh` and write CSVs under
`$OUT` (default `/tmp/dpumesh-bench`).

## Lifecycle invariants

- `deploy` is the only supported way to bring up meshed Pods. It starts the
  DPU and all selected Pods as one registration lifecycle.
- `restart` is only for a quiescent deployment with no meshed Pod.
- DPU admission is fail-closed. Applications receive no signing keys or
  authoritative placement claims.
- Linkerd policy, destination, and identity services are reached through the
  node-agent relay. Linkerd is linked into `dpumesh_dpu`; it is never injected
  beside a workload.
- Workload templates carry Linkerd's control-plane label for policy indexing
  and skip their DMA ports for Pod-local inbound interception; the DPU remains
  the sole proxy and enforcement point.
- `DPUMESH_L7_SVC` selects HTTP/1 and HTTP/2 services and
  `DPUMESH_L7_OPAQUE_SVC` selects opaque TCP services.
- Cross-node routing uses the authenticated peer-channel seam. RDMA supplies
  that seam's ordered reliable transport; its stream, custody, and policy
  semantics are identical to the local DMA path.

## Useful controls

```text
BENCH_DEPLOY_SCOPE=all|native|preload|grpc
BENCH_NUMA_POLICY=local|auto
BENCH_GRPC_BUILD=release|asan
```

Operational commands are `status`, `logs`, `dpulog`, `dpucpu`,
`armbalance`, `rotate-identity`, `admission`, and `cleanup`.
