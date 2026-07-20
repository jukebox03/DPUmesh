# DPUmesh Transport Validators

These programs validate transport semantics independently of the performance
report. Build and deploy through `bench/bench.sh`; direct invocation is useful
only when the DPU process and pod registration state are already synchronized.
For fast host-only ABI, control-state, and batching-policy checks, run `make test`
as documented in [`tests/README.md`](../../tests/README.md). Those tests complement
rather than replace the hardware validators below.

| Validator | Programs | Contract exercised |
|---|---|---|
| Loopback | `loopback_dpumesh.c` | One process registers, connects to its own Service, exchanges data, and closes |
| Verbs-shaped | `verbs_dpumesh.c` | Channel/CQ/QP lifecycle, windowed commit/flush sends, polling, and RX credit release |
| Stream/L7 | `stream_dpumesh.c` | Fragmented framed messages through the optional frame codec |
| POSIX preload | `preload_runner.c`, `tcp_echo.c`, `tcp_client.c` | Unmodified socket connect/listen/accept/read/write behavior and TCP fallback |
| Matched-C preload | `preload_sock.Dockerfile`, `bench_sock`, `echo_sock` | Same L4 benchmark workload over the socket facade; control TCP stays kernel, data uses DPUmesh |

Run them through the common entry point:

```sh
./bench/bench.sh loopback 1000 1024 0
./bench/bench.sh verbs    1000 1024 0 32 4
./bench/bench.sh stream   1000 1024 1
./bench/bench.sh preload  1000 1024 8
```

The matched-C preload transport is included in `bench/suite/run_suite.sh` as
`dpumesh-preload` after `bench.sh deploy`; its control endpoint is the
`preload-bench` service. The ordinary `preload` command remains the focused
correctness/churn validator.

A passing data test requires exact byte and request-id agreement, zero failed
operations, correct EOF delivery, and successful reverse-order destruction. A
process exit without a crash is not sufficient: inspect the DPU log for DMA,
generation, ring-ACK, egress, or cleanup warnings.

All native validators use ABI-2 semantics: `post_send` commits and automatically
submits complete transport units, while explicit `flush` forces each logical
request, response batch, or large-write tail. A pass exercises both automatic
full-unit submission and byte correctness.

The L7 stream validator is not a gRPC validator. Its simple frame codec has a
repository-specific message format and must not be enabled for HTTP/2. gRPC uses
L4 passthrough and is tested by the C++ tests and
`grpc_dpumesh_qps_benchmark` under `integrations/grpc`.

Sanitizer validation and performance validation are separate. Use ASAN/UBSAN
builds for memory correctness, and use optimized non-sanitized binaries for QPS
or latency results.
