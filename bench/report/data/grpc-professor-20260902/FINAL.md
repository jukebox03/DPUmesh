> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# DPUmesh gRPC correctness and performance

One node, one BlueField-3, `N/K/A=32/8/8`, nine cores each for the host client
and server Pods. The comparison arm runs the same client and server binaries
behind a per-Pod Linkerd sidecar (`edge-26.8.1`, stock `LINKERD2_PROXY_CORES=1`,
mTLS confirmed). The 64 B / 1 KiB / 8 KiB frame sizes are the request and the
response size each. The procedure is [`EXPERIMENT.md`](EXPERIMENT.md), the raw
data is `*-raw.csv`, and the table values come from `*-summary.csv`.

## Conclusions

1. **Every correctness gate passes**: contract tests, sanitizers, a real DPU
   shutdown and slot reuse, and 19 gRPC policy and routing stages. §1.
2. **Peak RPS is 4.3x / 4.6x / 2.2x the per-Pod Linkerd arm** (64 B / 1 KiB /
   8 KiB) with 0 RPC failures and 0 drops. §2.
3. **At the same 10k RPS, p50 is +0.2 ms (64 B and 1 KiB) and p99 is at or below
   Linkerd.** Only 8 KiB shows a 3x p50 (1.55 ms against 0.51 ms), with p99
   +10%. §2.

## 1. Correctness

| Gate | Result |
|---|---:|
| host transport/ABI/fault contract tests (`make test-hostfree`) | PASS |
| real DPU lane and SG-DMA queue contract | PASS |
| gRPC cHTTP2 adapter CTest, release | 4/4 |
| the same CTest under Clang ASAN+UBSAN | 4/4 |
| embedded Linkerd (Rust) adapter tests | 38/38 |
| real DPU channel shutdown and slot reuse | opened = closed 22/22, 0 exchanges lost after reuse |
| gRPC policy and routing (timeout, retry, method/header match, GRPCRoute, AuthorizationPolicy, circuit breaker) | 19/19 |
| Pod restarts / residual sessions and tasks at the end of measurement | 0 / 0 |

Raw output [`correctness.txt`](correctness.txt), per-stage verdicts
[`policy-stages.csv`](policy-stages.csv), acceptance criteria
[`design/GRPC.md`](../../../../design/GRPC.md#verification-contract).

## 2. Performance

![Summary](graphs/00_summary.png)

| | 64 B | 1 KiB | 8 KiB |
|---|---:|---:|---:|
| DPUmesh peak RPS | 106.8k | 89.1k | 34.3k |
| Linkerd peak RPS | 25.0k | 19.3k | 15.2k |
| p50 at 10k RPS, DPUmesh / Linkerd | 611 / 403 µs | 625 / 433 µs | 1,552 / 514 µs |
| p99 at 10k RPS, DPUmesh / Linkerd | 965 / 1,162 µs | 1,148 / 1,220 µs | 1,864 / 1,691 µs |

Peak RPS is the median of three closed-loop runs (8 threads x 8 channels, 1,024
in flight, 10 s) with 0 failures in both arms
([`mesh-closed-summary.csv`](mesh-closed-summary.csv)). Latency is the median of
three open-loop runs at 10k RPS, both arms achieving 10k with 0 failures
([`mesh-cpu-summary.csv`](mesh-cpu-summary.csv)). DPUmesh uses eight Arm workers;
Linkerd uses one core per sidecar.

## Reproducing

```sh
bash bench/suite/grpc_correctness.sh all                       # §1
python3 bench/report/data/grpc-professor-20260902/plot.py     # graphs/00_summary
```

The deploy, pin and sweep commands for each measurement arm are rows C, P4, P5,
R1 and R2 of [`EXPERIMENT.md`](EXPERIMENT.md).
