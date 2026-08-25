# F8 shim refusal arm — first hardware run of inject.sh I6, 2026-08-25

## Question

The shim refuses a connect it cannot route through the DPU, but no campaign
arm had ever driven that refusal on hardware: `preload_api_contract_test`
mocks the channel as available, so the property was held by construction
alone. This run adds stage I6 to `bench/suite/inject.sh` — kill the DPU under
live meshed Pods, demand refusal, restore, demand service — and executes it.

## Build under test

- Deploy: 2026-08-25 `bench.sh deploy` (default), the same deploy the
  `grpc-tail-20260825-175740` campaign ran on; DPU data-path PID 3252634,
  killed by the stage and relaunched as PID 3509240 from the deploy's own
  launcher (`/tmp/start_dpu_bench.sh`).
- Fixtures: `bench/k8s/injected.yaml` — meshed pair `inject-bench` →
  `inject-echo` (9101), unmeshed pair `plain-bench` → `plain-echo` (9102),
  webhook `dpumesh-inject`.

## Result — 14 of 15 PASS; every I6 judgment held

| stage | subject | expected | observed |
|---|---|---|---|
| I6a | meshed serve before the kill | serve | serve (rcnt=28,105 fail=0) |
| I6b | dpumesh_dpu after the kill | absent | absent |
| I6c | warm meshed connect while the DPU is down | refuse | refuse (rcnt=0 fail=1, 1 shim refusal line, 0 container restarts) |
| I6d | meshed Pod born in the outage | refuse | refuse (rcnt=0 fail=1, 1 shim refusal line) |
| I6e | unmeshed pair while the DPU is down | serve | serve (rcnt=416,954 fail=0) |
| I6f | meshed serve after restore | serve | serve (rcnt=30,160 fail=0, admitted+=1) |

The two refusal shapes are the two coded paths: the warm Pod's resolve round
trip dies against the dead process (2 s deadline in `dpumesh_resolve`,
`EHOSTUNREACH`), the newborn Pod cannot bring a channel up at all
(`ENETUNREACH`). Both apps stayed up — zero container restarts through the
outage — and the unmeshed pair moved 417 K requests through the same node in
the same window, which is what separates enforcement from a broken node.

I0–I4 all PASS (values in `stages.csv`). Stages I1–I5 are unchanged from the
`inject-20260825-170137` campaign; this run repeats them in passing.

## The one FAIL, recorded

I5b (Pod creation once the webhook answers again) observed `refuse`: the
probe's mutate call hit the restored webhook and ran out its 30 s deadline
(`raw.log`, `context deadline exceeded`). The same stage passed at
`inject-20260825-170137` a few hours earlier, so this is sporadic —
single-occurrence, not judged, to be watched on the next campaign run.

## Notes for reading the raw files

- `stages.csv` rows I6c/I6e: the subject strings of this first run contained
  commas, which shifts those two rows' columns. The subjects were reworded
  in `inject.sh` after this run; the CSV is kept as produced.
- The restore's "no startup banner within 90 s" note is a too-short wait —
  the relaunched DPU takes longer than 90 s to provision but came up fully
  (I6f). The wait was widened to 240 s after this run.
- The kill severs every meshed Pod on the node, not only the fixtures; the
  standing bench Pods are recycled by the full redeploy that follows a
  campaign, per the usual post-outage rule.
