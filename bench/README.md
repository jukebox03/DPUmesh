# DPUmesh Benchmark and Validation

The benchmark tree contains the deploy harness, matched transport workloads,
feature validators, and measured reports for the current working tree.
Results are meaningful only when the compared paths use the same request/reply
semantics, frame sizes, concurrency, warmup, duration, and CPU allocation.

## 1. Layout

```text
bench.sh                 build, deploy, run, pin, inspect, and clean entry point
apps/                    matched native-DPUmesh and POSIX benchmark applications
docker/                  benchmark container images
k8s/                     pod topology and embedded L4 tcp_proxy configuration
validators/              transport correctness programs
suite/                   evaluation matrix and status
report/                  deployment record and interpreted measurements
```

The gRPC C++ workload is built from `integrations/grpc`. It uses the generated
gRPC v1.80 `grpc.testing.BenchmarkService` protocol and can select direct TCP or
DPUmesh without changing RPC messages or service code.

## 2. Compared transports

| Name | Client/server program | Data path |
|---|---|---|
| `tcp-envoy` | `bench_sock` / `echo_sock` | kernel TCP through Envoy `tcp_proxy` |
| `dpumesh-native` | `bench_dpumesh` / `echo_dpumesh` | native C API |
| `dpumesh-preload` | same POSIX programs | socket facade over DPUmesh |
| `grpc-tcp` | `grpc_dpumesh_qps_benchmark` | gRPC C++ over kernel TCP |
| `grpc-dpumesh` | same gRPC program | gRPC C++ over injected DPUmesh Endpoint |

`bench_sock` and `echo_sock` are the matched L4 baseline. Their frame contains
request id, request length, response length, and body. The response preserves the
id and requested size. Large bodies are transported as a byte sequence and may
span multiple DPUmesh receive events.

The gRPC benchmark is a closed-loop synchronous unary workload with one channel
and a configurable number of client threads/outstanding RPCs. Warmup samples are
discarded. It reports successful QPS, failures, p50/p90/p99/p99.9 latency, and
client process CPU. It is protocol-compatible with the official service schema;
it is a focused transport harness rather than the upstream multi-scenario
`qps_worker` driver.

## 3. Reproducible deployment

Create the repository-root `.env` described in
[REPORT.md](report/REPORT.md). Disable swap before deployment:

```sh
sudo swapoff -a

DPUMESH_DPA_THREADS=32 \
DPUMESH_RINGS_PER_POD=8 \
DPUMESH_ARM_WORKERS=8 \
DPUMESH_PROXY_L7_SVC= \
DPUMESH_LOG_LEVEL=40 \
BENCH_NUMA_POLICY=local \
BENCH_DEPLOY_SCOPE=l4 \
./bench/bench.sh deploy
```

This selects `N/K/A/L=32/8/8/8`, connection-pinned L4, and PCI-local NUMA
placement. The L4 collector uses eight persistent connections, one for each
connection-affine DPU shard. `BENCH_NUMA_POLICY=auto` selects the unbound
NUMA control.

Read-only deployment checks are:

```sh
./bench/bench.sh status
./bench/bench.sh logs
./bench/bench.sh dpulog 500
./bench/bench.sh dpucpu
```

## 4. L4 measurements

```sh
# Inspect the matrix and duration.
./bench/suite/l4_proxy_data.sh --dry-run

# Validate deployment and invariants.
./bench/suite/l4_proxy_data.sh \
  --preflight-only --out /tmp/l4-preflight

# Collect the complete dataset.
./bench/suite/l4_proxy_data.sh \
  --target-verify \
  --out bench/report/data/l4-$(date +%Y%m%d-%H%M%S)

# Rebuild derived tables.
python3 bench/suite/analyze_saturation.py <dataset>
python3 bench/suite/summarize_l4.py <dataset>
```

The four primary configurations are Envoy plaintext, Envoy mTLS, DPUmesh
preload, and DPUmesh native. Each owns one client core and one server
core. Envoy sidecars share the endpoint cores. DPU ARM CPU is a separate metric.
The analysis reports both maximum clean throughput under this fixed budget and
the throughput where either physical endpoint core reaches 0.95 utilization,
including the retained-point clean count. System-wide softirq is recorded
separately. Cgroup CPU is process attribution only; saturation is determined
from each pinned physical core across the highest-clean/first-bad boundary.

Capacity discovery has no fixed RPS ceiling. It stops at the configured
per-direction frame-rate bound or an explicit `SCOUT_MAX_RPS`. The L4 matrix
uses symmetric 64 B, 1 KiB, and 8 KiB request/response frames, including the
16 B benchmark header. POSIX and native generator ramps include frame
construction. Every path knee requires at least 1.25× generator headroom.

The complete metric and analysis contract is in
[REPORT.md](report/REPORT.md).

## 5. gRPC QPS measurements

Build a Release binary against the pinned gRPC source tree:

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
    CONCURRENCY REQUEST_BYTES RESPONSE_BYTES [REACTORS] [AUTHORITY=TARGET] \
    [WAIT_FOR_READY=0]
```

For a transport comparison, copy the identical binary to the same client and
server pods. Run `server tcp 0.0.0.0:PORT ...` with a client target of the server
pod IP, then run `server dmesh SERVICE ...` and target the same Service name.
Use one channel and the same concurrency/frame-size matrix for both paths. Record
the binary hash, pod/node identity, DPU knobs, success/failure counts, process CPU,
and DPU logs with the output.

`TARGET` is a TCP address or a DPUmesh Service name. `AUTHORITY` maps to
`GRPC_ARG_DEFAULT_AUTHORITY` and defaults to `TARGET`; `WAIT_FOR_READY` applies
to each RPC. The result JSON records all three values.

## 6. Correctness validators

Run the host-only native contract suite before deployment:

```sh
make test
```

Its scope is documented in [tests/README.md](../tests/README.md). Then exercise
the real registration, DMA, byte-transfer, FIN, and cleanup paths on BlueField:

```sh
./bench/bench.sh loopback 1000 1024 0
./bench/bench.sh verbs    1000 1024 0 32 4
./bench/bench.sh stream   1000 1024 1
./bench/bench.sh preload  1000 1024 8
```

The validator-specific contracts are in [validators/README.md](validators/README.md).
The C++ gRPC tests are executed separately with CTest and include fake-native
reactor tests, event-gated writable retry, a paired real gRPC HTTP/2 channel
test, native symbol linkage, and an optional BlueField client/server smoke binary.

## 7. Measurement rules

1. Use a Release build for performance and sanitizer builds for correctness.
2. Verify zero RPC/request failures before accepting throughput or latency.
3. Separate warmup from measurement and use at least three repetitions for a
   reported median when the environment permits.
4. Report p50 and tail latency with QPS; a saturated point can improve QPS while
   invalidating latency comparison.
5. Capture host application CPU, Envoy CPU when present, DPU ARM CPU, and active
   `N/K/A/L` topology. Host-only CPU is not total system cost.
6. Compare only matched semantics. The current gRPC comparison is DPUmesh versus
   direct TCP, not DPUmesh versus Envoy HTTP connection management.

The current evaluation is in [REPORT.md](report/REPORT.md).
