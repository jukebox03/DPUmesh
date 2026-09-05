# Broker lifecycle and worker progress evidence — 2026-09-04

This record covers the broker-failure and worker-progress fixes exercised on
`rapids4` with the BlueField DPU. The code under test was the working tree based
on commit `d2b42a2`; each receipt records its capture time and scenario inputs.

## Worker progress

The first instrumented deployment exposed a stale diagnostic gauge after a
connection had already closed: worker 1 had `opened=1`, `closed=1`, every queue
at zero, but `stalled_conns=1`. The retained
[baseline summary](data/worker-progress-20260904T055114Z-baseline-post-deploy/worker-summary.csv)
therefore failed strict quiescence. After fixing connection removal to decrement
the gauge, the same state reached
[strict quiescence](data/worker-progress-20260904T055550Z-quiescent-fixed/worker-summary.csv)
with all session, queue, DMA, retry, and stall gauges at zero.

## Broker lifecycle scenarios

| Scenario | Result | Evidence |
|---|---|---|
| B1 — broker `SIGKILL` | PASS; restart `0 -> 1`, fresh PID/starttime, stable recovery in 12 s | [recovery](data/broker-lifecycle-20260904T061145Z-b1-broker-sigkill/recovery.env), [post-fault point](data/broker-lifecycle-20260904T061145Z-b1-broker-sigkill/point.log) |
| B2 — application `SIGKILL` | PASS; restart `1 -> 2`, old broker removed, stable recovery in 25 s | [recovery](data/broker-lifecycle-20260904T061258Z-b2-application-sigkill/recovery.env), [post-fault point](data/broker-lifecycle-20260904T061258Z-b2-application-sigkill/point.log) |
| B3 — node-agent rollout | PASS; all broker PID/starttime tuples unchanged, forged state rejected, fresh registration and teardown succeeded | [before](data/broker-lifecycle-20260904T062218Z-b3-agent-re-adoption/brokers-before.txt), [after](data/broker-lifecycle-20260904T062218Z-b3-agent-re-adoption/brokers-after.txt), [fresh registration](data/broker-lifecycle-20260904T062218Z-b3-agent-re-adoption/fresh-registration.log) |
| B4 — DPU restart | PASS; workloads were drained, DPU restarted, Pods registered afresh, and the post-restart point had zero failures or pending work | [restart log](data/broker-lifecycle-20260904T062325Z-b4-dpu-restart/dpu-restart.log), [post-restart point](data/broker-lifecycle-20260904T062325Z-b4-dpu-restart/point.log) |
| B5 — isolation | PARTIAL; UID/capabilities/seccomp/PID namespace/network namespace/private root/device isolation passed; CPU/OOM pressure arm was not configured | [isolation](data/broker-lifecycle-20260904T062526Z-b5-isolation/isolation.env), [result](data/broker-lifecycle-20260904T062526Z-b5-isolation/result.env) |

For B1 and B2, recovery is accepted only after the new broker tuple remains
unchanged for five seconds. Both scenarios also retain before/after worker
summaries, strict-quiescence output, background-load output, and the node-agent
log that names the bounded broker quiescence wait. The collateral Pod and its
broker remained unchanged.

B6 (an unauthenticated raw Comch peer held until registration timeout) was not
run because no raw-peer command was configured. B5 pressure and B6 remain open
hardware/injection exercises; they are not reported as passes here.
