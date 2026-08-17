# What a DMesh session costs (2026-08-17)

Every DMesh frontend connection builds its own complete outbound stack: the
per-target closure in `Config::build` calls `outbound.mk`, so a session
constructs its own discovery, protocol, endpoint and reconnect caches. That is
what makes concurrent sessions to one service independent, and it is paid per
connection rather than per request.

A steady closed-loop point hides it, because it opens its connections once and
reuses them. This note moves the connection axis on purpose, in two directions,
and runs each against an `L7_BACKEND=null` control that carries the identical
workload through the DPUmesh datapath with no Linkerd layer at all. The control
is what separates what a Linkerd session costs from what a DPUmesh connection
costs.

## Method

`bench/suite/l7_session_cost.sh`. Stack: `DPUMESH_DPA_THREADS=32`,
`DPUMESH_RINGS_PER_POD=8`, `DPUMESH_ARM_WORKERS=4`,
`DPUMESH_L7_LINKERD_WORKER=all`, opaque services 11, 13 and 14, the reservation
output path, pods pinned to cores 18/19/24/25 at a fixed 2.5 GHz. 1,024-byte
request, 8-byte reply, three repetitions per point.

ARM CPU is the DPU process's tick total across all its threads over the requests
the measured window completed, so it includes setup and warmup and is not a
capacity figure. Every point completed with `fail=0`, `drops=0`, `reorder=0` and
`worker_fail=0`.

## Building one: the Linkerd stack is the whole cost

Concurrency 8 on one client thread, twenty measured seconds. The reconnect
period is how many completions a client serves before it reconnects, so a
shorter period is more sessions per second at the same offered work.

| reconnect period | L7 sessions/s | L7 Mrps | L7 us/req | L7 p99 | L4 sessions/s | L4 Mrps | L4 us/req | L4 p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| never | 0.0 | 0.010523 | 51.9 | 1200 | 0.0 | 0.010304 | 38.1 | 1221 |
| 4000 | 2.5 | 0.010293 | 53.2 | 1284 | 2.5 | 0.010080 | 38.3 | 1248 |
| 1000 | 10.0 | 0.010069 | 55.9 | 1398 | 10.1 | 0.010132 | 38.4 | 1249 |
| 250 | 36.1 | 0.009271 | 63.9 | 2716 | 40.1 | 0.010152 | 38.7 | 1256 |
| 60 | 109.7 | 0.007338 | 92.5 | 2766 | 167.5 | 0.010204 | 39.3 | 1248 |

Least squares over each set, with DPU cores as ARM us/req times Mrps:

```
L7:  cores = 0.548 + 1.20e-3 x sessions/s     ->  1,200 ARM core-us per session
L4:  cores = 0.389 + 0.073e-3 x sessions/s    ->     73 ARM core-us per session
```

**A DPUmesh connection costs 73 microseconds to build and tear down. Putting a
Linkerd session on it costs 1,200 — sixteen times more.** The per-session
outbound stack is essentially the entire figure.

The tables diverge in the same way. Under churn the L4 datapath does not move:
at 167 sessions per second its throughput and p99 are the same as at zero. The
L7 path at 110 sessions per second has lost 30% of its throughput and more than
doubled its p99. It reached only 110 because its own collapse left fewer
completions to reconnect on.

## Carrying them: mostly not the Linkerd layer

Total outstanding requests held at 128 (`conc` is per thread, so `conc x threads`
is the offered window), fifteen measured seconds. Sessions move; offered work
does not.

| live sessions | L4 us/req | L7 us/req | L7 / L4 | L4 Mrps | L7 Mrps | Mrps delta |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2.298 | 3.843 | 1.67x | 0.193208 | 0.162243 | -16.0% |
| 2 | 3.207 | 6.057 | 1.89x | 0.189746 | 0.175622 | -7.4% |
| 4 | 5.348 | 8.746 | 1.64x | 0.179180 | 0.176261 | -1.6% |
| 8 | 8.315 | 15.206 | 1.83x | 0.161581 | 0.159318 | -1.4% |

This is also the price of the L7 layer as a whole: 1.6x to 1.9x the ARM CPU of
the same workload through the same datapath without it. The throughput gap
closes as sessions rise — 16% at one session, 1.4% at eight — but the CPU ratio
does not. A closed loop at a fixed outstanding window is latency-bound, so with
more sessions the work spreads over four workers and the layer pays its cost in
cores that were otherwise idle rather than in delivered requests. The cost did
not go away; it stopped being visible in the throughput column.

**This curve is not the Linkerd layer.** The L4 control has no sessions, no
outbound stack and no `Worker::poll_internal`, and it still costs 262% more per
request at eight connections than at one. The L7 layer multiplies it by a
roughly constant 1.6x to 1.9x without bending its shape. Whatever makes a live
connection expensive is in the DPUmesh datapath; the Linkerd stack pays a
proportional surcharge on it.

Three candidates are ruled out:

- **Backend fan-out.** Restricting the service to one backend pod left the L7
  numbers unchanged: 3.84 against 3.81 at one session, 6.06 against 6.15 at two.
- **Worker activation.** A per-thread breakdown at one and eight sessions
  (`dmesh-main` 9.9% → 34.6%, `dmesh-w0..w3` 46.1/2.5/2.1/2.1% → ~53% each,
  process 63.4% → 248.2%) shows an idle worker draws 2.1%, so the three idle
  ones account for at most 6 of the 185 points gained.
- **`Worker::poll_internal` walking every session.** It exists only in the L7
  build, and the L4 build shows the same curve.

## What this settles

- The L7 layer costs 1.6x to 1.9x the ARM CPU per request of the same datapath
  without it, and 16 times as much to open a connection. That is the price of
  terminating the stream and applying discovery, policy and load balancing; it
  is now a number rather than an assumption.
- The per-session Linkerd outbound stack is expensive and precisely located:
  1,127 of the 1,200 microseconds a session costs. This is the price of the P2.2
  trade — one complete outbound stack per session, which is what makes
  concurrent same-service sessions independent. It is the right correctness
  answer and the most expensive thing in the L7 path.
- Reducing it is worth far more than the per-request output copy for anything
  that reconnects. The Linkerd share of a session is 1,127 us; the copy
  `bench/report/REPORT_L7_TX_AB.md` measured is 0.265 us per request. They break
  even at about 4,300 requests per connection.
- The live-session scaling problem is a DPUmesh datapath question, not a Linkerd
  one, and should be pursued there. Optimizing the L7 layer would cut a 1.7x
  multiplier on a cost that is not its own.
- Any capacity claim for this L7 path must state its connection lifetime. A
  benchmark that opens connections once and holds them reports an operating
  point a churning workload never sees.

## What this does not settle

- Which part of the outbound stack the 1,127 microseconds is in — discovery and
  policy cache construction, the endpoint and reconnect layers, or the balancer.
  That split decides what can be shared without giving up session isolation.
- Why a live DPUmesh connection costs what it does. The L4 control establishes
  that it does, not where.
