# Linkerd low-risk cleanup hardware regression (2026-08-17)

This run checks the first behavior-preserving cleanup after the frozen Linkerd
baseline: direct `SessionToken` backend lookup, allocation-free shutdown walks,
the last-session close fast path, and synchronous stack-build timing. It asks
whether those changes caused a performance regression on the target DPU.

## Method

The current working tree at `c8f50f0` was rebuilt on and deployed to the DPU.
The deployment used the same measured geometry as the frozen baseline:

- `N/K/A=32/8/4`, `DPUMESH_L7_LINKERD_WORKER=all`;
- opaque services 11, 13 and 14, reservation output path;
- 1,024-byte request, 8-byte reply, one client thread;
- client/backend pods pinned to cores 18, 19, 24 and 25 at 2.5 GHz;
- three repetitions per point.

The steady-state points repeat the reservation arm of
`REPORT_L7_TX_AB.md`. The churn points repeat the connection-build half of
`REPORT_L7_SESSION_COST.md`: concurrency 8 for twenty measured seconds, with
the reconnect period controlling sessions per second.

Raw current results are in
`bench/report/data/l7-lowrisk-20260817-175243/{steady,churn}`. The comparison
uses `bench/report/data/l7-tx-ab-20260817/points.csv` and
`bench/report/data/l7-session-cost-20260817/churn.csv` as the frozen baseline.

## Steady state

ARM cost is the median across three repetitions. Ranges are the full three-run
range; delta is current relative to the frozen baseline.

| concurrency | baseline ARM us/req | current ARM us/req | delta | current Mrps delta | ranges |
|---:|---:|---:|---:|---:|---|
| 1 | 156.897 | 153.748 | **-2.0%** | -0.7% | 156.624–162.407 vs 151.262–155.736, disjoint |
| 32 | 11.554 | 10.994 | **-4.8%** | +0.05% | 11.346–11.627 vs 10.630–11.272, disjoint |
| 128 | 3.791 | 3.677 | -3.0% | +1.8% | 3.735–3.849 vs 3.662–3.833, overlapping |

There is no steady-state regression. The first two ARM ranges separate in the
better direction; concurrency 128 has the same sign but overlaps, so it is not
independent evidence of an improvement. Latency has no consistent movement:
p50 changes by less than 1%, and the noisy concurrency-128 p99 ranges overlap.

## Session churn

| reconnect period | baseline sessions/s | current sessions/s | baseline ARM us/req | current ARM us/req | delta | ranges |
|---:|---:|---:|---:|---:|---:|---|
| never | 0.0 | 0.0 | 51.946 | 51.629 | -0.6% | overlapping |
| 4000 | 2.5 | 2.5 | 53.171 | 52.173 | **-1.9%** | disjoint |
| 1000 | 10.0 | 10.1 | 55.921 | 54.201 | **-3.1%** | disjoint |
| 250 | 36.1 | 36.3 | 63.899 | 62.446 | **-2.3%** | disjoint |
| 60 | 109.7 | 110.1 | 92.455 | 90.295 | **-2.3%** | disjoint |

The fitted build/teardown slope falls from **1,200 to 1,154 ARM core-us per
session**, a 3.8% reduction. Throughput at the four nonzero-churn points rises
by 0.4% to 0.7%; median p99 changes by less than 1%. The no-churn point overlaps
the baseline, which is the expected shape for a session-lifecycle cleanup.

## New stack-build split

After 9,565 opens/closes distributed across four workers, the worker metrics
report these average synchronous costs per frontend stack:

| phase | mean us/build | share of fitted session cost |
|---|---:|---:|
| clone and configure template | 5.938 | 0.5% |
| construct outbound layers | 107.833 | 9.3% |
| instantiate target service | 34.680 | 3.0% |
| **synchronous total** | **148.451** | **12.9%** |

Therefore the synchronous `outbound.mk` boundary explains only about one
eighth of the 1,154 core-us slope. The remaining cost is in lazy discovery and
policy work, task execution/teardown, and the surrounding DPUmesh lifecycle;
the next split must instrument those asynchronous boundaries rather than
assuming all 1,127 Linkerd-added microseconds are inside the synchronous call.

## Correctness and conclusion

Every point completed with `fail=0`, `drops=0`, `reorder=0` and
`worker_fail=0`. At quiescence the four workers reported respectively
2,363/2,401/2,401/2,400 opens with exactly matching closes, and zero active
sessions, pending registrations, live tasks and orphans. Every emitted L7
fallback audit line, and the over-release and stray-release counters, stayed
zero.

**The cleanup is safe to keep.** Hardware shows no performance loss and a small,
repeatable 2%–4% reduction in session-related ARM cost. Treat the result as a
regression comparison, not a perfect source-only A/B: the frozen run's
provenance records an older commit plus a dirty Linkerd worktree, while this run
records `c8f50f0` plus the current dirty worktree. The magnitude is useful, but
should not be attributed to one individual line without a two-build interleaved
A/B.
