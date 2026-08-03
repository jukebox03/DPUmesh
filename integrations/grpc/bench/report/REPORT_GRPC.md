# DPUmesh gRPC Validation

## Transport

The gRPC EventEngine adapter maps one endpoint to one DPUmesh QP. A logical
`Write` copies consecutive slices into native reservations, commits them with
`dmesh_post_send()`, and calls `dmesh_flush()` once at the write boundary.
`DMESH_EVENT_TX_READY` resumes a write parked on `EAGAIN`, and
`DMESH_EVENT_TX_ERROR` fails the endpoint.

Physical batching is implemented by `libdpumesh`. The adapter has no batch
state, tail list, batching timer, or QP scan. Complete physical units publish
immediately. The library publishes an idle partial immediately, retains at most
the newest busy partial, and publishes that tail at its internal deadline or an
explicit flush.

## Validation

The deployed `grpc-dpumesh` path was measured on 2026-08-04 with constant-rate
unary RPCs, eight persistent channels, one pinned client core, one pinned server
core, `N/K/A=32/8/8`, and DPU L7 disabled. Each point is the median of three
runs.

| Frame | Offered/s | Achieved/s | p50 | p95 | p99 | p99.9 | p99.99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 16,728 | 16,728 | 222 µs | 282 µs | 523 µs | 12.24 ms | 16.35 ms |
| 1 KiB | 15,813 | 15,813 | 230 µs | 293 µs | 450 µs | 8.14 ms | 15.45 ms |
| 8 KiB | 10,207 | 10,207 | 239 µs | 284 µs | 349 µs | 8.04 ms | 14.75 ms |

Every run completed with `fail=0`, `drops=0`, and `reorder=0`. The three-run
p99 ranges were 492–961 µs at 64 B, 410–488 µs at 1 KiB, and 348–884 µs at
8 KiB.

The reference dataset at the same offered rates records p99 values of 2.844 ms,
2.481 ms, and 478 µs. Current median p99 is lower by 81.6%, 81.9%, and 27.0%,
respectively.

Current run-level histogram output is stored in
[`data/batching-20260804/latency_runs.csv`](data/batching-20260804/latency_runs.csv).
It contains p50, p95, p99, p99.9, and p99.99 for every repetition. The reference
[`measurements.csv`](data/grpc-final-20260803/measurements.csv) contains only
aggregated achieved rate, CPU, p99, drop ratio, and clean status; per-request
samples and its other percentiles are not available.

## Correctness

The gRPC unit, endpoint, channel, reactor, and native-link suites pass in the
normal build. The same four CTest targets pass under ThreadSanitizer.
AddressSanitizer and UndefinedBehaviorSanitizer report no invalid access or
undefined behavior; LeakSanitizer reports a 320-byte allocation in the gRPC
v1.80 TCP connect handshaker during process teardown.

The reactor tests cover logical-write flush, split and multi-slice writes,
`EAGAIN` resume, asynchronous TX failure, RX credit ordering, FIN, close, and
multi-reactor connection dispatch.

## Reproduction

```sh
env DPUMESH_DPA_THREADS=32 DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=8 \
    DPUMESH_PROXY_L7_SVC= BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=grpc \
    ./bench/bench.sh deploy

./bench/bench.sh pin grpc
CONFIGS=grpc-dpumesh \
    ./bench/suite/l4_proxy_data.sh --no-deploy --no-perf --out /tmp/grpc-run
```

The result reported here covers DPUmesh gRPC throughput, latency, and
correctness. It does not update the Envoy/TCP comparison or CPU figures.
