# DPUmesh Evaluation Coverage

This document indexes what the repository's evaluation has and has not measured.
It does not convert an unmeasured configuration into a
performance claim.

## Transport matrix

| Transport | Correctness exercised | Performance data | Application semantics |
|---|---|---|---|
| Direct kernel TCP | Yes | Yes | Matched POSIX frame workload |
| TCP through Envoy `tcp_proxy` | Yes | Yes | Same POSIX frame workload |
| Native DPUmesh | Yes | Yes | Matched request/reply workload |
| DPUmesh preload | Yes | Matched-C diagnostic/performance path; no headline claim yet | Same POSIX binary |
| gRPC C++ over direct TCP | Yes | Harness available; no retained comparison result | `grpc.testing.BenchmarkService` unary |
| gRPC C++ over DPUmesh | Yes | Harness available; no retained comparison result | Identical generated service and calls |
| Envoy HTTP/2 gRPC proxy | No result | No result | Not represented in existing measurements |
| gRPC Go over DPUmesh | No implementation | No result | Not represented in the working tree |

## Evidence categories

### Functional transport

The native validators cover channel/EQ/QP creation, inbound QPs, ordered data,
fragmentation, RX credit, FIN, and reverse destruction. The preload validator
covers socket interception and TCP fallback. These tests establish API behavior;
they do not establish competitive performance.

### Lifecycle and remote reclaim

The BlueField gRPC smoke ran 30 create/use/destroy rounds, reused server and
client pod slots, exchanged 60 unary RPCs, and observed `POD_QUIESCED` on both
sides each round. It also transferred three exact 1 MiB payloads and recovered
after an unexpected disconnect. This demonstrates bounded graceful reclaim and
observed liveness recovery. It does not prove memory isolation across arbitrary
host failure during in-flight DMA.

### L4 performance

The matched L4 report contains unloaded latency, throughput versus concurrency,
batching symmetry, host plus DPU ARM CPU, DPU configuration sweeps, and selected
busy-application probes. Its strongest comparison is direct TCP versus Envoy
`tcp_proxy` versus native DPUmesh on one node. It does not represent an L7 mesh.

### gRPC performance

The current harness uses one generated service, one channel, synchronous unary
RPCs, configurable concurrent client threads, separate warmup, and percentile
latency. TCP and DPUmesh use the same binary and RPC implementation. This isolates
the endpoint transport for the measured shape, but it does not cover streaming,
async/callback APIs, multiple subchannels, TLS, or an Envoy HTTP/2 proxy.

## Measurement acceptance rules

A performance point is retained only if:

- request/RPC failures are zero;
- client and server report consistent success counts;
- payload semantics are identical across transports;
- warmup is excluded and measurement duration is explicit;
- CPU pinning, DPU N/K/A topology, binary provenance, and node placement are
  recorded;
- logging is at warning level unless the run is explicitly diagnostic;
- the point is below an obvious saturation knee when used for latency comparison.

## Interpretation boundaries

- Host CPU savings are not total CPU savings. DPU ARM cost must be reported.
- DPA EUs are hardware execution resources and are not interchangeable with ARM
  process CPU percentages.
- A single repetition is directional evidence, not a stable median.
- ABI-4 native batching is mandatory; incompatible batching ablations are not
  regenerated.
- L4 Envoy `tcp_proxy` is a valid transport baseline but not a measurement of
  HTTP/2 routing, retries, telemetry, or policy cost.
- The focused gRPC harness is compatible with the official service schema but is
  not the upstream distributed `qps_worker` scenario controller.

The current evaluation is in `bench/report/REPORT.md`; its deployment is in
`bench/report/DEPLOY.md`.
