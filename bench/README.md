# DPUmesh Benchmark and Validation

The benchmark tree contains the deploy harness, matched transport workloads,
feature validators, and measured reports for the 2026-07-19 working tree.
Results are meaningful only when the compared paths use the same request/reply
semantics, payloads, concurrency, warmup, duration, and CPU allocation.

## 1. Layout

```text
bench.sh                 build, deploy, run, pin, inspect, and clean entry point
apps/                    matched native-DPUmesh and POSIX benchmark applications
k8s/                     namespace and pod topology
docker/                  benchmark container images
envoy/                   L4 tcp_proxy configuration
validators/              transport correctness programs
suite/                   evaluation matrix and status
report/                  deployment record and interpreted measurements
RESULT.md                 condensed engineering experiment record
```

The gRPC C++ workload is built from `integrations/grpc`. It uses the generated
gRPC v1.80 `grpc.testing.BenchmarkService` protocol and can select direct TCP or
DPUmesh without changing RPC messages or service code.

## 2. Compared transports

| Name | Client/server program | Data path |
|---|---|---|
| `tcp-direct` | `bench_sock` / `echo_sock` | kernel TCP |
| `tcp-envoy` | same POSIX programs | kernel TCP through Envoy `tcp_proxy` |
| `dpumesh-native` | `bench_dpumesh` / `echo_dpumesh` | native C API |
| `dpumesh-preload` | same POSIX programs | socket facade over DPUmesh |
| `grpc-tcp` | `grpc_dpumesh_qps_benchmark` | gRPC C++ over kernel TCP |
| `grpc-dpumesh` | same gRPC program | gRPC C++ over injected DPUmesh Endpoint |

`bench_sock` and `echo_sock` are the matched L4 baseline. Their frame contains
request id, request length, response length, and body. The response preserves the
id and requested size. Large bodies are transported as a byte sequence and may
span multiple DPUmesh completions.

The gRPC benchmark is a closed-loop synchronous unary workload with one channel
and a configurable number of client threads/outstanding RPCs. Warmup samples are
discarded. It reports successful QPS, failures, p50/p90/p99/p99.9 latency, and
client process CPU. It is protocol-compatible with the official service schema;
it is a focused transport harness rather than the upstream multi-scenario
`qps_worker` driver.

## 3. Reproducible deployment

Create an uncommitted repository-root `.env` containing the machine-specific
values described in [DEPLOY.md](report/DEPLOY.md), then use the only supported
bring-up command:

```sh
./bench/bench.sh deploy
```

This command builds host and DPU artifacts, synchronizes the DPU sources, imports
container images, starts the DPU process and pods in order, and applies the fair
CPU pinning. Do not restart only the DPU process or only the pods; doing so can
invalidate registration state.

Useful read-only checks are:

```sh
./bench/bench.sh status
./bench/bench.sh logs
./bench/bench.sh dpulog 500
./bench/bench.sh dpucpu
```

Routine deployments use warning-level logging. Raise the log level only for a
bounded diagnostic run, then restore warning-level output before measurement.

## 4. L4 measurements

```sh
# Unloaded latency sweep
./bench/bench.sh latency both

# Payload/goodput sweep
./bench/bench.sh bandwidth both

# Thread/rate sweep
./bench/bench.sh rate both

# Exact point: transport request reply concurrency duration warmup threads
./bench/bench.sh point dpumesh 1024 1024 32 10 1000 1
./bench/bench.sh point tcp     1024 1024 32 10 1000 1

# Pinning presets
./bench/bench.sh pin fair
./bench/bench.sh pin hw3
```

The L4 headline comparison uses `fair`: one application core for each transport;
for Envoy, the sidecar shares the assigned pod budget. DPU CPU is measured
separately and must not be hidden when comparing total CPU/request.

For a point that exercises batching, apply the same application-level batching
to every transport. Report native DPUmesh coalescing separately from the matched
comparison because asymmetric batching changes the workload.

## 5. gRPC QPS measurements

Build a Release binary against the pinned gRPC checkout:

```sh
cmake -S integrations/grpc -B build/grpc-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=OFF \
  -DDPUMESH_GRPC_BUILD_QPS_BENCHMARK=ON \
  -DBUILD_TESTING=OFF
cmake --build build/grpc-release -j2 --target grpc_dpumesh_qps_benchmark
```

The binary syntax is:

```text
grpc_dpumesh_qps_benchmark server <tcp|dmesh> ENDPOINT DURATION_S [REACTORS]
grpc_dpumesh_qps_benchmark client <tcp|dmesh> TARGET WARMUP_S DURATION_S \
    CONCURRENCY REQUEST_BYTES RESPONSE_BYTES [REACTORS]
```

For a transport comparison, copy the identical binary to the same client and
server pods. Run `server tcp 0.0.0.0:PORT ...` with a client target of the server
pod IP, then run `server dmesh SERVICE ...` and target the same Service name.
Use one channel and the same concurrency/payload matrix for both paths. Record
the binary hash, pod/node identity, DPU knobs, success/failure counts, process CPU,
and DPU logs with the output.

## 6. Correctness validators

```sh
./bench/bench.sh loopback 1000 1024 0
./bench/bench.sh verbs    1000 1024 0 32 4
./bench/bench.sh stream   1000 1024 1
./bench/bench.sh preload  1000 1024 8
```

The validator-specific contracts are in [validators/README.md](validators/README.md).
The C++ gRPC tests are executed separately with CTest and include fake-native
reactor tests, a paired real gRPC HTTP/2 channel test, native symbol linkage, and
an optional BlueField client/server smoke binary.

## 7. Measurement rules

1. Record the exact git commit plus whether the working tree is dirty.
2. Use a Release build for performance and sanitizer builds for correctness.
3. Verify zero RPC/request failures before accepting throughput or latency.
4. Separate warmup from measurement and use at least three repetitions for a
   reported median when the environment permits.
5. Report p50 and tail latency with QPS; a saturated point can improve QPS while
   invalidating latency comparison.
6. Capture host application CPU, Envoy CPU when present, DPU ARM CPU, and active
   DPA configuration. Host-only CPU is not total system cost.
7. Compare only matched semantics. The current gRPC comparison is DPUmesh versus
   direct TCP, not DPUmesh versus Envoy HTTP connection management.

Interpreted results and their provenance are in [REPORT.md](report/REPORT.md).
