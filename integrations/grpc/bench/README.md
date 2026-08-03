# gRPC Benchmark Programs

This directory contains the gRPC C++ workloads used to measure the DPUmesh
transport against kernel TCP and against Envoy sidecars. All of them speak the
generated gRPC v1.80 `grpc.testing.BenchmarkService` unary protocol, and all
select their byte transport without changing RPC messages, stubs, or service
code. Results are meaningful only when the compared paths share the same frame
sizes, connection count, arrival process, warmup, duration, and CPU allocation.

## 1. Layout

```text
bench_grpc.cc            open-loop client driven by the L4 collector
echo_grpc.cc             server peer of bench_grpc
qps_benchmark.cc         standalone closed-loop client and server
docker/                  container images for the two collector peers
k8s/                     gRPC pod topology and Envoy sidecar configuration
suite/                   figure rendering for the gRPC dataset
report/                  measured gRPC evaluation
```

Everything gRPC-specific lives here. The pieces shared with the L4 evaluation
stay in [`bench/`](../../../bench/): the deploy driver `bench.sh`, the collector
`suite/l4_proxy_data.sh`, the config-agnostic `suite/distill.py`, the registry,
and `apps/bench.h`, whose frame header and latency histogram `bench_grpc`
compiles in so percentiles match the other clients. `bench.sh` applies
`k8s/pods.yaml` from both trees as one stream.

`bench_grpc` and `echo_grpc` are peers of one experiment; `qps_benchmark` is a
self-contained program that carries its own client and server. They answer
different questions and are not interchangeable.

## 2. Which program to use

| Program | Loop | Driven by | Answers |
|---|---|---|---|
| `bench_grpc` / `echo_grpc` | open, constant-rate or Poisson | `bench/suite/l4_proxy_data.sh` | capacity and host CPU under a fixed core budget, against Envoy and TCP |
| `qps_benchmark` | closed, fixed concurrency window | run by hand | latency and CPU per RPC at a chosen concurrency |

Use `bench_grpc` for anything that compares configurations. It shares the
control protocol, the result line, the frame convention, and the latency
histogram of `bench_sock` and `bench_dpumesh`, so the collector drives all
paths unchanged and percentiles mean the same thing across them.

Use `qps_benchmark` for quick single-machine checks. It needs no pods, no
collector, and no registry entry beyond a free Service name.

## 3. Build

Both are produced by the integration's CMake tree against the pinned gRPC
source. gRPC links statically, so the resulting binaries need only
`libdpumesh` and DOCA at runtime.

```sh
cmake -S integrations/grpc -B build/grpc-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
  -DDPUMESH_GRPC_ENABLE_SANITIZERS=OFF \
  -DDPUMESH_GRPC_BUILD_QPS_BENCHMARK=ON \
  -DBUILD_TESTING=OFF
cmake --build build/grpc-release -j"$(nproc)" \
  --target bench_grpc echo_grpc grpc_dpumesh_qps_benchmark
```

`./bench/bench.sh grpcbuild` runs the same configure and build, and
`./bench/bench.sh deploy` runs it before building the container images.

## 4. Collector peers

### Environment

| Variable | Program | Meaning |
|---|---|---|
| `BENCH_TRANSPORT` | both | `dmesh` or `tcp` |
| `BENCH_DST_SERVICE` | `bench_grpc` | DPUmesh Service name to dial |
| `BENCH_TARGET` | `bench_grpc` | `host:port` to dial over TCP |
| `ECHO_PORT` | `echo_grpc` | TCP listen port |
| `DPUMESH_SERVICE` | `echo_grpc` | registry identity the server advertises |
| `BENCH_REACTORS` | both | EQ reactor shards over the one native channel |
| `CTRL_PORT` | `bench_grpc` | control listener, default 9092 |

`DPUMESH_SERVICE` is read by the native library, not by the program.

### Control protocol

The client listens on `CTRL_PORT` and answers one command per connection:

```text
PING
RUN      <req> <reply> <conc> <dur> <warmup> <threads>
OPEN     <req> <reply> <threads> <dur> <warmup> <rate> [const|poisson]
SELFTEST <payload> <threads> <dur> <rate> <const|poisson>
```

`req` and `reply` are body bytes; the reported frame adds the 16-byte benchmark
header so `reqframe` and `respframe` match the other clients at the same
nominal frame size. `threads` is the connection count: one channel per
connection.

`SELFTEST` runs the real issuer timeline with the transport removed and reports
`schedule_ratio` and `drop_ratio`. The collector records it as the `grpc`
generator kind.

### Result line

`OK` followed by `key=value` pairs in the shape the collector parses:
throughput, `p50` through `p9999`, `rcnt`, `scheduled`, `pending`, `fail`,
`drops`, `reqframe`, `respframe`, `offered_mrps`, and the transport counters
`grabs`, `rets`, `recyc`, `waits`, `pads`, `credit_hold_dropped`, and
`eq_budget_exhausted`. The last two are the adapter's own bounds: receives whose
credit was returned at the retention cap, and drains that ended on the poll
budget with the EQ non-empty. `dist` is `NA`; per-backend attribution is not
collected here.

Reading a run: `drops` is admission, `fail` is RPC failure, `pending` is
outstanding at the deadline. A point with `achieved == offered` and `fail=0` but
a large `p99` is a latency result, not a throughput result, and should be
reported as one.

## 5. Instrument design

The issue path and the completion path run on separate threads. One issuer owns
a single global departure timeline and round-robins over the channels; one
completer per channel blocks in `CompletionQueue::Next()` with no deadline.
Every issued call remains in the worker's locked live set until completion.
Teardown cancels that set under the same lock and then drains the completion
queue. Latency is measured from each request's intended arrival; scheduler
delay is therefore included in the reported value.

## 6. Standalone harness

```text
grpc_dpumesh_qps_benchmark server <tcp|dmesh> ENDPOINT DURATION_S [REACTORS=1]
grpc_dpumesh_qps_benchmark client <tcp|dmesh> TARGET WARMUP_S DURATION_S \
    CONCURRENCY REQUEST_BYTES RESPONSE_BYTES [REACTORS=1] [AUTHORITY=TARGET] \
    [WAIT_FOR_READY=0]
```

`TARGET` is a TCP address or a DPUmesh Service name. `AUTHORITY` maps to
`GRPC_ARG_DEFAULT_AUTHORITY` and defaults to `TARGET`. The client is
synchronous with one channel and `CONCURRENCY` threads, discards warmup
samples, and prints one JSON line with QPS, failures, p50/p90/p99/p99.9, and
client process CPU.

Process CPU here is `getrusage(RUSAGE_SELF)`: it excludes softirq serviced on
the core and excludes DPU ARM cores entirely. It is not a substitute for the
collector's core-busy accounting when comparing transports.

The DPUmesh side needs no pods. Run the server under a free registry Service
name, keep `DPUMESH_RINGS_PER_POD` equal to the DPU's value, and let the server
exit on its duration argument rather than killing it — a signal during DMA
leaves the DPU reclaiming mappings it cannot destroy.

## 7. Measurement rules

1. Use a Release build for performance and the sanitizer build for correctness.
2. Verify `fail=0` and `drops=0` before accepting any throughput or latency.
3. Separate warmup from measurement and report a median over at least three
   repetitions.
4. Report p50 with the tail. On these paths they move independently: p50 can
   stay flat across a range where p99 changes by an order of magnitude.
5. Record host application CPU, sidecar CPU when present, DPU ARM cores, and
   the live `N/K/A/L`. Host-only CPU is not total system cost.
6. Compare only matched semantics. A gRPC path and a raw-frame path measure
   different applications even over the same transport.

The gRPC evaluation and its contract are in
[report/REPORT_GRPC.md](report/REPORT_GRPC.md); the L4 evaluation it
parallels is in [REPORT.md](../../../bench/report/REPORT.md).
