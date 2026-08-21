# gRPC workload for DPUmesh

The programs in this directory validate gRPC over the DPUmesh endpoint. The
wire stack is gRPC chttp2 → DPUmesh endpoint → Host↔DPU DMA → embedded Linkerd.
The Kubernetes manifest contains only this path.

## Build

```sh
cmake -S integrations/grpc -B build/grpc-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=OFF \
  -DDPUMESH_GRPC_BUILD_QPS_BENCHMARK=ON
cmake --build build/grpc-release -j"$(nproc)"
```

`./bench/bench.sh grpcbuild` performs the workload build, and
`BENCH_DEPLOY_SCOPE=grpc ./bench/bench.sh deploy` performs the complete DPU,
control-plane, image, Pod, and smoke-test lifecycle.

## Programs

- `bench_grpc` is the controlled client used by `bench.sh point
  grpc-dpumesh ...`.
- `echo_grpc` serves the benchmark service through the DPUmesh endpoint.
- `grpc_dpumesh_qps_benchmark` is the standalone closed-loop harness.

The deployed programs set `BENCH_TRANSPORT=dmesh`. `BENCH_DST_SERVICE` and
`DPUMESH_SERVICE` are Kubernetes Service identities resolved by the signed
DPU topology; no TCP address is used for the data path.

## Control protocol

The client accepts:

```text
PING
RUN      <req> <reply> <conc> <dur> <warmup> <threads>
OPEN     <req> <reply> <threads> <dur> <warmup> <rate> [const|poisson] [channels]
SELFTEST <payload> <threads> <dur> <rate> <const|poisson>
```

Each issued RPC remains in the live set until completion. Shutdown cancels the
set, drains the completion queue, and retains receive credit only up to the
adapter's fixed bound. The result line reports RPC failures, outstanding calls,
latency percentiles, retained-credit drops, and EQ budget exhaustion.

## Standalone harness

```text
grpc_dpumesh_qps_benchmark server dmesh SERVICE DURATION_S [REACTORS=1]
grpc_dpumesh_qps_benchmark client dmesh SERVICE WARMUP_S DURATION_S \
    CONCURRENCY REQUEST_BYTES RESPONSE_BYTES [REACTORS=1] [AUTHORITY=SERVICE] \
    [WAIT_FOR_READY=0]
```

For lifecycle validation on a real DPU, use
`./bench/bench.sh grpcshutdown`. It kills a client with a live HTTP/2 session,
requires Linkerd sessions/tasks and imported mappings to quiesce, re-registers
the recycled slot, and runs another request.
