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

This tree holds what the L4 and L7 evaluations share. Everything specific to the
gRPC workload — its programs, images, pods, figures, and report — lives in
[`integrations/grpc/bench/`](../integrations/grpc/bench/README.md), which mirrors
this layout. `bench.sh` applies `k8s/pods.yaml` from both trees as one stream,
and `suite/l4_proxy_data.sh` drives configurations from both.

## 2. Compared transports

L4, raw framed bytes:

| Config | Client/server program | Data path |
|---|---|---|
| `envoy-permissive` | `bench_sock` / `echo_sock` | kernel TCP through Envoy `tcp_proxy` |
| `envoy-strict` | same POSIX programs | the same, with mTLS on the inter-pod leg |
| `dpumesh-preload` | same POSIX programs | socket facade over DPUmesh |
| `dpumesh-native` | `bench_dpumesh` / `echo_dpumesh` | native C API |

L7, gRPC unary over the same transports:

| Config | Client/server program | Data path |
|---|---|---|
| `grpc-envoy-permissive` | `bench_grpc` / `echo_grpc` | gRPC through Envoy `tcp_proxy` |
| `grpc-envoy-strict` | same gRPC programs | the same, with mTLS on the inter-pod leg |
| `grpc-tcp` | same gRPC programs | gRPC over kernel TCP, no mesh |
| `grpc-dpumesh` | same gRPC programs | gRPC over injected DPUmesh Endpoints |

`bench_sock` and `echo_sock` are the matched L4 baseline. Their frame contains
request id, request length, response length, and body. The response preserves the
id and requested size. Large bodies are transported as a byte sequence and may
span multiple DPUmesh receive events.

`bench_grpc` and `echo_grpc` are the matched L7 pair. They speak the generated
gRPC v1.80 `grpc.testing.BenchmarkService` unary protocol and select their
transport from the environment, so the four gRPC configurations differ in no
application code. They share this tree's control protocol, result line, frame
convention, and `apps/bench.h` latency histogram, so the collector drives L4 and
L7 paths unchanged and percentiles mean the same thing across all eight.

The two evaluations are not a single comparison. An L4 config and an L7 config
measure different applications even over the same transport, and the DPUmesh
paths differ further: `dpumesh-native` calls the C API directly while
`grpc-dpumesh` goes through the gRPC EventEngine adapter.

## 3. Reproducible deployment

Create the repository-root `.env` described in
[REPORT.md](report/REPORT.md). Disable swap before deployment:

```sh
sudo swapoff -a

DPUMESH_DPA_THREADS=32 \
DPUMESH_RINGS_PER_POD=8 \
DPUMESH_ARM_WORKERS=8 \
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

For Linkerd multi-service placement, one native client can assign its worker
threads round-robin across a CSV of destinations. The two extra echo pods are
same-service backends by default; override their advertised names to make them
independent services for this gate:

```sh
L7_BACKEND=linkerd \
DPUMESH_L7_OPAQUE_SVC=11,13,14 \
DPUMESH_L7_LINKERD_WORKER=all \
DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=4 \
BENCH_DST_SERVICES=echo-dpumesh,echo-dpumesh-13,echo-dpumesh-14 \
ECHO_13_SERVICE=echo-dpumesh-13 \
ECHO_14_SERVICE=echo-dpumesh-14 \
BENCH_DEPLOY_SCOPE=core \
./bench/bench.sh deploy
```

Run at least as many client threads as destinations, then use `l7metrics` to
require opened=closed and zero active/pending/tasks/orphaned on every worker.

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

`REPORT.md` reports how much host CPU each path costs. What that CPU is — the
share of an endpoint core spent in application code, the transport library, the
gRPC runtime, an Envoy sidecar, the kernel network stack, or being woken up — is
collected by a separate campaign:

```sh
./bench/suite/core_isolate.sh on
./bench/suite/core_campaign.sh --family l4 --out /tmp/core-l4
./bench/suite/core_report.sh /tmp/core-l4/*/ --out bench/report/core --stem l4
```

`core_campaign.sh` drives one configuration at a time over three offered rates
and three repetitions; `core_report.sh` writes the per-layer CSVs, the flame
graphs and the figures. Results, method and contract are in
[REPORT_CORE.md](report/REPORT_CORE.md).

## 5. gRPC measurements

The gRPC campaign uses the same collector, deployed with the `grpc` scope so
only the four L7 paths register:

```sh
DPUMESH_DPA_THREADS=32 DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=8 \
BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=grpc \
./bench/bench.sh deploy

CONFIGS="grpc-dpumesh grpc-envoy-permissive grpc-envoy-strict grpc-tcp" \
  ./bench/suite/l4_proxy_data.sh --no-deploy --no-perf \
  --out bench/report/data/grpc-$(date +%Y%m%d-%H%M%S)

python3 bench/suite/distill.py <dataset> measurements.csv
python3 integrations/grpc/bench/suite/plot_grpc.py measurements.csv \
    integrations/grpc/bench/report/figures
```

The deployment scope and pin profile follow the selected configurations, so a
gRPC-only `CONFIGS` list needs neither flag. Each path owns one client core and
one server core; Envoy sidecars share the endpoint cores, as at L4. `--no-perf`
is required because the collector's per-process perf mode collects no samples
inside the benchmark containers; the core-wide profile in
[`suite/core_profile.sh`](suite/core_profile.sh) does.

`./bench/bench.sh grpcbuild` builds the gRPC programs alone; `deploy` runs it
before building the images. Program syntax, control protocol, environment, and
the instrument's design constraints are documented in
[integrations/grpc/bench/README.md](../integrations/grpc/bench/README.md).
A standalone `grpc_dpumesh_qps_benchmark` is available there for closed-loop
single-machine checks that need no pods or collector.

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
6. Compare only matched semantics. Both evaluations compare DPUmesh against
   Envoy `tcp_proxy` sidecars, plaintext and mTLS; neither compares against
   Envoy HTTP connection management. L4 and L7 numbers are not interchangeable.
7. Report p50 with the tail. On the gRPC paths they move independently: p50 can
   stay flat across a range where p99 changes by an order of magnitude, so a
   capacity set by a p99 bound is a latency result, not a throughput result.
8. Record what the point was taken on: binary hashes, core affinity, NUMA
   placement, backend count, and the active topology. Request and response
   frames must match, and the generator's own schedule must have held.
9. Treat a capacity as a lower bound. The rate a ramp stops at is the first
   failure, and a path can refuse a rate and deliver a higher one; establish
   whether it recovers before quoting the number as a ceiling.
10. Measure the instrument. A load generator has its own ceiling, and a
    configuration that reports the generator's number is not being measured at
    all. The matched TCP path serves as that reference.
11. Compare cost per request only at equal load. Batching depth follows queue
    occupancy, so the offered rate moves the very thing being measured; two
    configurations compared at their own operating points are not comparable.

The evaluations are:

| Report | Covers |
|---|---|
| [report/REPORT.md](report/REPORT.md) | L4: DPUmesh against Envoy sidecars, host and ARM cost |
| [report/REPORT_L7.md](report/REPORT_L7.md) | L7: what backend-selection granularity costs |
| [report/REPORT_CORE.md](report/REPORT_CORE.md) | where the cores go, attributed per component |
| [../integrations/grpc/bench/report/REPORT_GRPC.md](../integrations/grpc/bench/report/REPORT_GRPC.md) | gRPC over DPUmesh against Envoy and TCP, across core and channel budgets |
| [../linkerd/bench/report/REPORT_LINKERD.md](../linkerd/bench/report/REPORT_LINKERD.md) | linkerd sidecar columns of the gRPC evaluation |
