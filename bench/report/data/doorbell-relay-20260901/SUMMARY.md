# Doorbell-relay broker + gated drain result (2026-09-01)

Architecture under test (supersedes minimal-pe-broker-20260901):

- The broker keeps the DOCA PE but is a pure doorbell relay: one pod-global
  eventfd tick per REV_DOORBELL batch. No reverse-entry peek, no port routing,
  no per-EQ eventfds, no host-dispatch page.
- The workload runs the legacy drain thread (no DOCA calls): it interprets the
  reverse rings, performs in-process eq_notify EQ selection, and owns
  arm_epoch, publishing it only before sleeping.
- Polled regime: while completions keep arriving the drain thread re-checks the
  rings on an exponentially backed-off sleep (`DPUMESH_DRAIN_NAP_US` = min,
  default 10, 0 = pure event-driven; `DPUMESH_DRAIN_NAP_CAP_US` = cap, default
  100; work resets to min, each empty check doubles, past the cap it arms the
  doorbell and blocks) without publishing arm_epoch, so the DPU sends no
  doorbells and the broker sleeps through the busy period.
- The DPU still sees only pod-global rings, arm_epoch, REV_DOORBELL; no EQ
  identity. Public application API unchanged. Broker IPC version 2 -> 3
  (fd list is now K + L + TX + RX + 1 doorbell eventfd; the Python agent's
  `BROKER_IPC_VERSION` must match).

## Why this design: measured attribution

Per-pod split of host CPU (app container vs broker cgroup, conc32):

| build | app | broker | total | broker wakes/s |
|---|---:|---:|---:|---:|
| legacy (HEAD, in-process PE) | 0.274 | — | 0.274 | — |
| dispatch broker (previous) | 0.224 | 0.178 | 0.402 | 11.9K |
| relay, nap=0 | 0.252 | 0.177 | 0.428 | 12.4K |
| relay, nap=25 (default) | 0.230 | 0.0004 | 0.231 | 8 |
| relay, nap=50 | 0.168 | 0.0004 | 0.169 | 8 |
| relay, nap=100 | 0.153 | 0.0004 | 0.153 | 8 |

perf on the loaded broker (nap=0) shows no DOCA or peek hotspot: the ~7 us
per-wake cost is kernel wake-chain overhead (PSI/cgroup accounting, scheduler
enqueue, syscall entry, epoll). Wake count, not per-wake work, is the only
broker lever; removing the peek pass alone changed nothing.

## conc32 CPU run (official, REPS=4)

```sh
OUT_DIR=bench/report/data/doorbell-relay-20260901 \
REPS=4 ARMS=dpumesh bash bench/suite/api_l7_cost.sh
```

| metric | legacy | dispatch broker | this build (mean n=4) |
|---|---:|---:|---:|
| throughput | 96.9K | 91.6K | 92.3K |
| p50 | 296.5 us | 311.8 us | 316 us |
| p99 | — | 522 us | 572 us |
| Host CPU | 0.274 | 0.433 | **0.2217** |
| Host CPU/request | 2.82 us | 4.73 us | **2.40 us** |
| DPU ARM CPU | — | 1.015 | **0.751** |
| ARM CPU/request | — | 11.08 us | **8.13 us** |

All four runs `fail=0`. ARM CPU also drops ~26%: the DPU no longer sends
doorbell messages in the polled regime.

## Nap knob curve (conc32 closed loop, attribution runs)

| nap | host total | p50 | throughput |
|---:|---:|---:|---:|
| 0 (pure event-driven) | 0.428 | 317 | 89.4K |
| 25 | 0.231 | 316 | 94.1K |
| 50 | 0.169 | 416 | 72.6K |
| 100 | 0.153 | 462 | 67.8K |

nap=25 dominates nap=0 on every axis at this operating point: the 25 us ring
re-check beats the doorbell->broker->eventfd->drain wake chain's own latency.
Larger naps buy CPU with latency, as expected of the coalescing knob.

## Drain sharding + in-line assist (same day, follow-up)

The threads=8 investigation changed the diagnosis twice; all three additions
are kept because each is load-bearing:

- **Per-stripe drain shards.** `drain_rev_rings_span` + a per-stripe claim
  lock; shards spawn one per registered EQ, capped by `DPUMESH_DRAIN_SHARDS`
  (default L) *and by the cores the process may run on* (`sched_getaffinity`
  at EQ-create time — the bench pins pods after start). Safe because the DPU
  routes every reverse producer for a port to stripe `port % L`
  (`px_rev_owner`), so per-port state keeps a single producer. Shared
  structures made multi-producer-safe: EQ ready list (MPSC, tail CAS +
  zero-sentinel publish), accept queue producer (Vyukov CAS).
- **In-line assist**: `dpumesh_drain_assist` at the top of `dmesh_poll_eq` —
  an awake EQ thread interprets published entries itself (stripe trylocks),
  removing the drain->EQ handoff under load; `assist_progress` keeps drain
  shards in the polled regime so the doorbell path stays closed.
- **threads=8 conc32 resolution.** The 717K -> 620K regression was NOT drain
  serialization: the echo server is single-EQ (only client threads scale), the
  deploy default pin is fair = 1 core per pod, and on one core sharding/assist
  move work between threads without changing it. The regression is the nap
  delay landing directly in RTT at an oversubscribed closed-loop point
  (256 outstanding / RTT). With `DPUMESH_DRAIN_NAP_US=0` this build reaches
  **758K rps, p50 313 us** — better than the dispatch broker's 717K/330 —
  while single-EQ conc32 keeps 0.216 core / p50 317 at the default nap=25.

## Exponential backoff replaces the fixed nap (final policy)

The fixed 25 us cadence was an operating-point assumption (it matched the
~84 us conc32 burst gap). The classic poll & sleep policy — reset to a short
sleep on work, double on empty, park on the doorbell past a cap — removes
that assumption and measured better on every axis but the oversubscribed one:

| point (fair pin) | dispatch broker | fixed nap=25 | **backoff 10..100 (default)** | nap=0 |
|---|---:|---:|---:|---:|
| conc32 host core | 0.433 | 0.216–0.231 | 0.253 | ~0.43 |
| conc32 host us/req | 4.73 | 2.48 | **2.47** | ~4.8 |
| conc32 p50 | 311.8 | 316–317 | **261** | 317 |
| conc32 rps | 91.6K | 87–94K | **102.4K** | 89.4K |
| conc1 p50 | 144.5 | 166 | **136** (p99 211) | ~144 |
| threads=8 conc32 | 717K / 330 | 615K / 390 | 661K / 380 | **758K / 313** |

The conc32 total core is a little above the fixed nap's because the process
runs 9% faster; per-request cost is identical. conc1 beats even the dispatch
broker: the reply lands inside the early 10–40 us sleeps, quicker than the
doorbell wake chain. The `DPUMESH_DRAIN_NAP_BUDGET` knob is gone.

## Oversubscription auto-off (final piece)

With more live EQ consumers than allowed cores, every polling delay lands
directly in RTT and the precise doorbell wake wins, so the drain thread now
disables the polled regime whenever `n_live_eqs > CPU_COUNT(affinity)`
(re-read on a 100 ms period because the bench pins pods after start; the
count is live registrations, not the high-water `n_eqs`, so one threads=8 run
cannot poison later single-EQ runs). Also fixed on the way: an assisting EQ
now holds the drain shard at cap cadence instead of min, so a saturated
in-line consumer is not preempted at the work rate.

Final numbers, default knobs, fair pin, fail=0:

| point | dispatch broker | final build |
|---|---:|---:|
| conc32 host core / us-per-req | 0.433 / 4.73 | 0.253–0.265 / 2.47–2.55 |
| conc32 p50 / rps | 311.8 / 91.6K | **259–261 / 102–104K** |
| conc1 p50 | 144.5 | **136** |
| threads=8 conc32 | 717K / 330 | **733K / 324** (auto-off active on the client) |

Every point now beats the dispatch broker; `DPUMESH_DRAIN_NAP_US=0` remains
the manual escape to pure event-driven (758K at threads=8 with both sides
forced off — the residual ~3% is the still-polled single-EQ server).

## Known costs of this build

- `make test`, `make test-hostfree` pass; full deploy exit 0 with all
  native/preload/gRPC embedded-Linkerd smoke gates green.

## Pitfall log

- Bumping `DMESH_IPC_VERSION` requires the same bump in
  `bench/workload_attest_agent.py` (`BROKER_IPC_VERSION`) or the agent rejects
  every HELLO ("invalid broker HELLO framing") and no broker ever starts.
- The broker's argv is assembled by the agent's systemd-run command, so a CLI
  flag change must land in both at once. The cleanup pass removed the unused
  direct-fd launch path and the `--systemd-child` flag; the only launch form is
  now `--launch-sock/--launch-token/--agent-sock`.
