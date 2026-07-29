# DPUmesh Evaluation Coverage

## L4 performance

The current L4 campaign measures four primary configurations with one workload
contract:

| Configuration | Application API | Data path |
|---|---|---|
| `envoy-permissive` | POSIX | two Envoy TCP sidecars, plaintext |
| `envoy-strict` | POSIX | two Envoy TCP sidecars, mutual TLS |
| `dpumesh-preload` | POSIX | DPUmesh L4 through `LD_PRELOAD` |
| `dpumesh-native` | native DPUmesh | DPUmesh L4 through the native API |

All configurations use the same frame, arrival schedule, frame-size matrix,
connection count, host-core budget, and single backend. The POSIX paths use the
same application binaries. Host and DPU ARM CPU are reported separately.

The complete contract, deployment settings, results, and reproduction commands
are in [`bench/report/REPORT.md`](../report/REPORT.md).

## Correctness

Host-only tests cover native API state, batching, writable notification,
preload readiness and publication, topology, and collector analysis contracts.
Run them with:

```sh
make test
```

Hardware validators cover native loopback, verbs-shaped lifecycle, fragmented
streams, POSIX preload behavior, byte agreement, EOF, and reverse destruction.
Their commands and acceptance criteria are in
[`bench/validators/README.md`](../validators/README.md).

## gRPC

The C++ adapter uses one generated unary service over direct TCP or DPUmesh L4.
Its build, functional tests, and benchmark command are in
[`integrations/grpc/README.md`](../../integrations/grpc/README.md). It is not
part of the L4 campaign.

## Acceptance

A retained performance point requires:

- zero request, reorder, and overflow failures;
- matching request and response frame sizes;
- explicit warmup and measurement duration;
- recorded binary hashes, core affinity, NUMA placement, backend count, and
  DPU topology;
- valid generator scheduling and a clean SLA point;
- separate host and DPU ARM CPU accounting.

Envoy `tcp_proxy` represents L4 proxy cost. It does not represent HTTP routing,
retry, telemetry, or policy processing.
