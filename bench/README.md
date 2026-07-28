# DPUmesh Benchmark and Validation

The benchmark tree contains the deploy harness, matched transport workloads,
feature validators, and measured reports for the current working tree.
Results are meaningful only when the compared paths use the same request/reply
semantics, payloads, concurrency, warmup, duration, and CPU allocation.

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
| `tcp-direct` | `bench_sock` / `echo_sock` | kernel TCP |
| `tcp-envoy` | same POSIX programs | kernel TCP through Envoy `tcp_proxy` |
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

Create a repository-root `.env` excluded from version control with the
machine-specific values in [DEPLOY.md](report/DEPLOY.md). Deploy with the
topology explicit:

```sh
DPUMESH_DPA_THREADS=16 \
DPUMESH_ARM_WORKERS=2 \
DPUMESH_RINGS_PER_POD=8 \
DPUMESH_PROXY_L7_SVC=16 \
DPUMESH_LOG_LEVEL=40 \
./bench/bench.sh deploy
```

This command builds host and DPU artifacts, synchronizes DPU sources, imports
container images, starts the DPU process and pods in order, and applies CPU
pinning. It selects `N/K/A/L=16/8/2/2`: eight rings, two polling ARM workers, and
two RX landing stripes. A bare `deploy` selects one ARM worker. The live DPU
process environment is the measurement authority.

`deploy` reads the host BlueField PCI function's NUMA node from sysfs. Benchmark
entrypoints apply that CPU and memory policy before allocating registered DMA
memory, and the later per-pod pinning stays within the same node. This is
intentional even when automatic NUMA balancing is enabled: registered DMA
mappings were observed not to migrate.

Use `BENCH_NUMA_POLICY=auto` only for the unbound NUMA control. The default is
`local`. `BENCH_DEPLOY_SCOPE=core` starts only the three backends and one client;
it is used by low-N controls whose DPA ring capacity cannot admit every validator
pod. `BENCH_DEPLOY_SCOPE=l4` starts only the five L4 comparison paths and never
registers the two extra weighted-LB native backends; this is the scope used by
`bench/suite/l4_proxy_data.sh`.

The configuration above keeps benchmark service 11 connection-pinned L4 while
enabling L7 framing only for stream validator service 16. To exercise
per-message LB on the native request/reply benchmark too, enable both services:

```sh
DPUMESH_PROXY_L7_SVC=11,16 \
DPUMESH_DPA_THREADS=16 \
DPUMESH_ARM_WORKERS=2 \
DPUMESH_RINGS_PER_POD=8 \
./bench/bench.sh deploy
```

Run the complete repeated sweep with:

```sh
OUT=bench/report/data/sweep-final \
  ./bench/suite/sweep_final.sh --dry-run
OUT=bench/report/data/sweep-final \
  ./bench/suite/sweep_final.sh
```

The same `OUT` may be reused after interruption; completed run IDs are skipped.
The campaign validates every live topology, records TX `grow_waits`, produces
raw and summary CSVs, restores the final N32/A8/K8 local-NUMA deployment, and
runs correctness validators.

The benchmark frame is a 16-byte length prefix followed by payload, with a
128 KiB total-frame limit. The DPU routes each request frame independently and
serializes complete response frames back into the client's one QP byte stream.
Leaving service 11 out of the list measures backend-pinned L4 instead.

On the BlueField ARM, capture it before every retained run:

```sh
sudo xargs -0 -n1 -a "/proc/$(pgrep -n dpumesh_dpu)/environ" \
  | grep '^DPUMESH_' | sort
ps -ww -p "$(pgrep -n dpumesh_dpu)" -o args=
```

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
# Current repeated report campaign: native L4, two-sided Envoy, and direct TCP
./bench/suite/current_l4.sh

# Regenerate the two concise lab-meeting figures from the retained aggregates
python3 bench/suite/plot_meeting.py \
  bench/report/data/summary.csv bench/report/data/cpu_summary.csv \
  bench/report/figures

# Unloaded latency sweep
./bench/bench.sh latency both

# Payload/goodput sweep
./bench/bench.sh bandwidth both

# Thread/rate sweep
./bench/bench.sh rate both

# Exact point: transport request reply concurrency duration warmup threads
./bench/bench.sh point dpumesh 1024 1024 32 10 1000 1
./bench/bench.sh point tcp     1024 1024 32 10 1000 1

# Measure DPU main/data-worker CPU and pinned ARM cores.
./bench/bench.sh armbalance 1024 8 32 10 2 /tmp/arm-balance.csv

# Pinning presets
./bench/bench.sh pin fair
./bench/bench.sh pin hw3
```

The L4 headline comparison uses `fair`: one application core for each transport;
for Envoy, the sidecar shares the assigned pod budget. DPU CPU is measured
separately and must not be hidden when comparing total CPU/request.

Native ABI 4 automatically submits complete 8 KiB transport units. Applications
use `dmesh_flush()` at protocol or latency boundaries for a trailing partial
unit.

The native echo server is also the reference event-driven backpressure pattern.
It registers one native EQ fd with epoll, blocks without a timeout, and drains the
EQ on wake. If a reply parks on `dmesh_alloc(EAGAIN)`, the core automatically arms
that QP; `DMESH_EVENT_TX_READY` retries only the named reply. The server does not
busy-poll, run a retry timer, or scan every pending QP.

Retained measurements are recorded in [REPORT.md](report/REPORT.md).

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
Use one channel and the same concurrency/payload matrix for both paths. Record
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
