# gRPC broker-build baseline — 2026-09-01

## Verdict

The current DPUmesh gRPC path is functionally sound within the tested
single-node scope. Release adapter tests, real chttp2 channel lifecycle tests,
the hardware shutdown/reuse gate, and all 20 routing/policy stages passed. The
75 retained performance repetitions reported no RPC failure, drop, pending
request, worker failure, retained-credit loss, EQ-budget exhaustion, or Pod
restart.

For 64-byte request and response frames, the repeated open-loop result is:

- **24,000 request/s highest clean delivered rate**. All three repetitions
  delivered at least 99% of offered load. The next point, 28,000/s, is the
  first non-clean rate.
- **16,000 request/s highest <=1 ms-p50 operating point**. Its three-run
  medians are 817 us p50, 1,790 us p99, 3.092 total Host cores, and 7.367 of
  eight DPU worker cores.
- 24,000/s is a delivery limit, not a low-latency recommendation: p50 is
  2,161 us and p99 is variable (9.3--75.6 ms across repetitions) near
  saturation.

The limiting resource is the DPU ARM L7 worker pool. Host CPU includes both
application and broker in each complete Pod cgroup and is not saturated at the
knee.

## Provenance and accounting

| Item | Value |
|---|---|
| Source baseline | `36d095da4222e7076958140ee462a58d219b8d8f` |
| Source change | `feat: add per-pod broker data path` |
| Node / scope | `rapids4`, single node, namespace `test-bench` |
| DPU geometry | `N/K/A/L=32/8/8/8` |
| gRPC | v1.80.0, C++17, chttp2 |
| Client / server reactors | 8 / 8 |
| HTTP/2 channels in open loop | 8 |
| Host placement | client Pod CPUs 18--23, server Pod CPUs 24--29 |
| Host clock | performance governor, 2.5 GHz fixed on benchmark cores |
| Host CPU metric | recursive client/server Pod cgroup; app + broker exactly once |
| DPU CPU metric | ARM data-path process, with open-loop worker breakdown |

The deployment source was the baseline commit. The worktree changes used by
the campaign are documentation and measurement-harness changes; they do not
change the deployed gRPC or data path.

## Structure under test

```text
generated stub / service handler
            |
            v
     stock gRPC chttp2
            | ordered byte stream
            v
 DmeshEndpoint -> DmeshReactor -> libdpumesh / native EQ+QP
                                      |
                         mapped arena + shared rings
                                      |
                                      v
                          DPA -> DPU ARM Linkerd

 per-Pod broker: owns DOCA device/PE/control connection and idle doorbell relay;
                 it is not a request/response byte hop.
```

The C++ adapter maps the gRPC EventEngine byte-stream seam onto one native QP.
It owns byte ordering, callback/lifetime rules and backpressure integration,
but does not parse HTTP/2 or implement routing or policy. The workload process
publishes descriptors and consumes completions through `libdpumesh`. The
broker owns privileged DOCA/control state and provides mappings. Protocol-aware
routing and policy execute in the DPU-hosted Linkerd stack.

This separation matters for optimization: adapter correctness, Host transport
cost, and DPU L7 cost are different quantities.

## Correctness gates

| Gate | Result |
|---|---|
| `make test-hostfree` | PASS, including the new sweep analyzer regression test |
| Release CTest | 4/4 PASS: endpoint, real chttp2 channel, reactor/runtime, native symbol linkage |
| Clang 14 Debug ASAN+UBSAN | 4/4 PASS with `ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer is unavailable under the environment's ptrace policy |
| Endpoint cases | 15 PASS: reads, EOF, write splitting/coalescing, backpressure, failures and destruction |
| Reactor/runtime cases | 30 PASS: ordering, credits, EAGAIN, FIN/error closure, bridge, reconnect, churn, ownership and MPSC connects |
| Real chttp2 cases | PASS: unary RPC, channel churn, concurrent channels, GOAWAY, client/server FIN |
| `grpcshutdown` | 8 opened / 8 closed, post-reuse RPC PASS, no failure/drop/credit/EQ loss |
| Policy/routing surfaces | 20/20 PASS |
| Post-campaign Pods | client and server Running, restart count 0 |
| Post-campaign DPU | every worker OPENED=CLOSED; ACTIVE=PENDING=TASKS=0 |

The policy receipt covers baseline service, request timeout plus its DPU
counter, retry plus its DPU counter, HTTP method/header matching, GRPCRoute
method matching, route-targeted authorization allow/deny, failure accrual and
endpoint withdrawal, plus the tree's HTTP/1 control. See
[`stages.csv`](../policy-route-20260901-164531/stages.csv).

`ORPHANED` in the final DPU metrics is cumulative accounting, not live
residue; the live active, pending and task gauges are all zero.

The first sanitizer run caught a test-fixture lifetime violation rather than a
deployed-path defect. Four public-Channel lifecycle tests used a stack-backed
`UnownedExecutor`, although gRPC can destroy an internal Endpoint after the
public Channel handle is gone. The tests now use `DmeshRuntime`'s production
owned executor, and the complete sanitizer suite passes. No production adapter
or data-path source was changed for that repair.

## Open-loop capacity

Each rate has three repetitions. A repetition is clean only when
`achieved/offered >= 0.99`, all failure/loss counters are zero, all fields are
valid, and no restart is observed. A rate needs at least three valid
repetitions and a clean majority. The reported knee is the monotonic clean
prefix, so an isolated later success cannot hide an earlier bad rate.

| Offered rps | Achieved rps | Ratio | p50 us | p99 us | p999 us | Host cores | DPU worker cores |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 8,000 | 8,000 | 1.0000 | 737 | 1,223 | 2,990 | 2.023 | 4.277 |
| 12,000 | 11,999 | 0.9999 | 717 | 1,441 | 4,617 | 2.644 | 6.126 |
| 16,000 | 15,999 | 0.9999 | 817 | 1,790 | 4,558 | 3.092 | 7.367 |
| 20,000 | 19,999 | 1.0000 | 1,225 | 2,895 | 4,106 | 3.334 | 7.750 |
| 24,000 | 23,995 | 0.9998 | 2,161 | 24,650 | 123,475 | 3.141 | 7.875 |
| 28,000 | 26,874 | 0.9598 | 11,298 | 2,698,863 | 2,698,863 | 2.485 | 7.840 |
| 32,000 | 30,105 | 0.9408 | 146,036 | 2,627,371 | 2,627,371 | 2.193 | 7.746 |

Values are three-run medians. At overload the client cannot keep the requested
open-loop rate and queues for seconds; those rows demonstrate the right side of
the knee and are not acceptable operating points.

## Closed-loop payload/concurrency shape

`total concurrency = 8 workers * window per worker`. These points describe
shape and saturation; they are not published as open-loop capacity.

| Frame | Total concurrency | Achieved rps | p50 us | p99 us | DPU ARM cores |
|---:|---:|---:|---:|---:|---:|
| 64 B | 8 | 9,167 | 803 | 1,546 | 5.347 |
| 64 B | 64 | 24,588 | 2,571 | 3,668 | 7.805 |
| 64 B | 256 | 28,323 | 8,940 | 12,000 | 7.956 |
| 1 KiB | 8 | 8,870 | 837 | 1,568 | 5.246 |
| 1 KiB | 64 | 23,708 | 2,660 | 3,808 | 7.788 |
| 1 KiB | 256 | 26,778 | 9,471 | 12,346 | 7.976 |
| 8 KiB | 8 | 8,227 | 903 | 1,668 | 5.496 |
| 8 KiB | 64 | 19,210 | 3,306 | 4,647 | 8.028 |
| 8 KiB | 256 | 19,791 | 12,793 | 16,006 | 8.013 |

The saturation plateaus are approximately 28.3k, 26.8k and 19.8k request/s for
64 B, 1 KiB and 8 KiB. At 8 KiB, raising total concurrency from 64 to 256 adds
only 3% throughput while p50 grows 3.9x. With equal request and response sizes,
the 8 KiB plateau is about 324 MB/s of aggregate application-frame traffic
(about 1.30 Gbit/s in each direction).

## PLAN optimization decision

- **O1 is not the first steady-state action.** It instruments the remaining
  0.4--0.5 ms per-session setup cost and belongs to a connection-churn arm.
- **O2 is the only current steady-state candidate.** Direct Linkerd
  `AsyncWrite` reservation can remove the remaining intermediate tx queue,
  but the historical reservation-versus-copy gain was only
  0.265 ARM us/request against a current path that still spends hundreds of
  ARM us/request. Implement it only as a same-build A/B.
- **O6 is out of scope.** Incremental topology generations reduce control-plane
  churn amplification, not live HTTP/2 request cost.
- **O5 follows later.** The fair ARM/x86 full-stack study is valuable for the
  paper but is not required to establish this DPUmesh baseline.

The O2 acceptance arm should repeat the 12k/16k/20k/24k open-loop points and
the 64 B and 8 KiB closed-loop saturation points with unchanged placement,
geometry and clocks. Retain it only if copy bytes or arena publications explain
an ARM CPU/request improvement with no p99, ordering, failure, credit or
shutdown regression.

## Receipts

- [`open-points.csv`](open-points.csv): all 21 open-loop repetitions
- [`open-knees.csv`](open-knees.csv): analyzer output
- [`open-crashes.csv`](open-crashes.csv): header only; no restart
- [`open-sweep.log`](open-sweep.log): execution log
- [`closed-points.csv`](closed-points.csv): all 54 closed-loop repetitions
- [`closed-medians.csv`](closed-medians.csv): grouped three-run medians
- [`closed-crashes.csv`](closed-crashes.csv): header only; no restart
- [`closed-sweep.log`](closed-sweep.log): execution log

## Limits

This is a current-build, single-node DPUmesh gRPC result. It is not a
DPUmesh-versus-stock-TCP comparison, an ARM-versus-x86 equivalence result, or a
two-node receipt. Those require matched same-day comparison arms; older
2026-08-25 measurements remain historical receipts for their own build.
