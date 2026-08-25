# F8 kernel-road closure — inject.sh I0–I7, 2026-08-25

## Question

Two F8 items met their arms in one campaign. First run of I7: the node agent's
ingress guard (`--protect-ingress`) rejects kernel-TCP SYNs to every
(address, `DPUMESH_PORT`) pair the injection label marks, from the FORWARD
hook, on the membership cadence — does an unannotated Pod's kernel road to a
mesh-served port actually close, while everything else stays open? Second run
of I6 (first: `inject-dpudown-20260825-214159/`): does the shim's DPU-down
refusal repeat?

## Build under test

- Deploy: 2026-08-25 `bench.sh deploy` carrying the ingress guard — agent
  image with iptables, DaemonSet with `--protect-ingress`, `NET_ADMIN`, and
  the host's `/run/xtables.lock`.
- Pre-campaign guard state, read through the agent Pod: `DPUMESH-PROTECT`
  holds exactly the two standing mesh-served pairs (`preload-echo:9100`,
  `http1-echo:9103`), rule count stable across consecutive reconcile ticks
  (no duplicate accumulation), FORWARD jump in place.
- The I6 stage kills DPU data-path PID 3526738 (this deploy's) and relaunches
  PID 3532472 from the deploy's launcher.

## Result — 17 of 17 PASS

Every stage of the previous run repeated its verdict, I5b included (its FAIL
in `inject-dpudown-20260825-214159/` did not repeat: sporadic, two passes out
of three same-day runs). The new stages:

| stage | subject | expected | observed |
|---|---|---|---|
| I7a | kernel probe to the unmeshed port | connect | connect |
| I7b | kernel probe to the mesh-served port | refuse | refuse (settled=0 s) |

Both probes are the same bare `/dev/tcp` connect from the same unannotated
Pod (`plain-bench`); only the destination differs. `plain-echo:9102` — an
ordinary kernel listener — connects; `inject-echo:9101` — served over DMA —
is rejected with tcp-reset. `settled=0 s` means the guard already covered the
recycled `inject-echo` at its post-restore address before the probe ran:
re-coverage of a fresh Pod IP inside one membership interval.

I6 second run: warm refuse (1 shim refusal line, 0 restarts), newborn refuse,
unmeshed pair 420 K requests through the outage, meshed serve after restore
(rcnt=26,821, fail=0, admitted+=1).

## What the guard never touched

Meshed traffic itself: I3 and I6f serve through the mesh with the guard's
rules live on the same node, and the deploy's smoke gate passed with the two
standing pairs already protected. Host-sourced traffic (kubelet probes, the
harness's control-port `nc`) rides OUTPUT, not FORWARD, and needed no
exemption rule.

## Notes

- The restore's "no startup banner" note repeated: the relaunched DPU reaches
  its banner ~4 minutes after launch (DPA provisioning), 2 s past the 240 s
  wait this run used. The wait was widened to 360 s after this run; function
  was never affected (I6f judges it).
