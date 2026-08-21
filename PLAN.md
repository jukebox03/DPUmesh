# DPUmesh Optimization Plan

The design documents in [`design/`](design/) state what is built. This file is
the open work list, and it holds one line of work only: reducing cost. Nothing
here changes admission, custody or any security property, which is what makes
the items independently schedulable — and what makes it a measurement error to
run one of them across a build that also changes correctness behavior.

**Measurement discipline (binding).** A capacity is quoted with the instrument
that produced it. `bench/suite/analyze_saturation.py` votes a `knees.csv`
`highest_clean_rps` out of an open-loop grid; a closed-loop rate grid reaches
further on the same data. The two are not interchangeable and a figure must not
present one under the other's caption. An optimization result is accepted only
from repeated runs with the frozen topology, placement and 2.5 GHz clock.

## What is known

L7 capacity is bounded by per-session cost, not by the transport.
`bench/suite/l7_session_cost.sh` moves the reconnect rate and reads the ARM cost
out of the slope. The live receipt is
[`bench/report/data/l7-shared-ab-20260821.md`](bench/report/data/l7-shared-ab-20260821.md),
which puts one session at **3.9 ms** of ARM time without per-workload stack
sharing and **0.4–0.5 ms** with it. An earlier campaign split that total into a
DPUmesh connection (**73 ARM core-µs**) and the Linkerd session above it
(**1,200**) by differencing against a null-L7 control; neither that control nor
that campaign's data is in the tree any more, so the split is history rather
than a receipt.

The synchronous half of that is instrumented in `linkerd/app/src/lib.rs` and
reported by `SessionMetrics::observe_stack_build`. Over 9,565 opens and closes
across four workers:

| Phase | Per session |
|---|---:|
| `configure` (clone the outbound template, set `dmesh_session`) | 5.9 µs |
| `layers` (`build_policies` + `outbound.mk`) | 107.8 µs |
| `service` (`NewService::new_service`) | 34.7 µs |
| synchronous total | **148.5 µs** — about one eighth of the 1,200 |

So the synchronous `outbound.mk` boundary is about one eighth of the slope, not
the four fifths the earlier estimate implied. The remaining seven eighths is
lazy discovery and policy work, task execution and teardown, and the surrounding
DPUmesh lifecycle. **Locating it requires instrumenting those asynchronous
boundaries; do not assume the whole figure is inside the synchronous call, and do
not conflate template caching with watch sharing — they attack different parts.**

## O1 Reduce per-session stack construction

- [x] Re-measure the total against the frozen baseline and record it in
  `bench/report/`. Done: 1,200 ARM core-µs per session against a 73 µs L4
  control. That campaign's raw data has been retired.
- [ ] Instrument the untimed remainder: policy discovery, destination/profile
  discovery, reconnect layers, endpoint construction and balancer construction.
  Extend `SessionMetrics` rather than adding a second surface. This is the whole
  of O1's remaining unknown: 1,051 of the 1,200 µs are unattributed.
- [ ] Cache only immutable templates. `SessionToken`, backend channel, workload,
  target generation, cancellation and metrics must stay session-local — the
  connector binds to `dmesh_session`, so a shared service would take another
  session's channel (`two_same_service_sessions_take_their_own_channels` in
  `outbound/src/tcp/connect.rs` is the regression test for exactly this).
- [ ] Share watches only by authoritative `(workload, target, generation)`.
  Before sharing, add tests for: an update arriving while two sessions share a
  watch; a withdrawal invalidating a shared watch; and the last consumer
  releasing it.
- [ ] Accept only a repeated hardware improvement with no p99 or correctness
  regression.

## O2 Direct `AsyncWrite` reservation

The reservation-versus-copy A/B is measured and published: the reservation path
costs 0.265 ARM µs/request less at concurrency 128, with the three-run ranges
disjoint at 32 and 128. That fixed the default and the priority — 1,127 µs of
session against 0.265 µs of copy break even at about 4,300 requests per
connection — but it did not remove the intermediate queue, which is what this
item is for.

- [ ] Inject a worker-local egress reservation interface while keeping all
  `dmesh_l7_*` FFI in the adapter.
- [ ] Write from Linkerd directly into the DPUmesh arena without the
  intermediate tx queue, preserving partial-write and output ordering.
- [ ] Define capacity wakeup, cancellation, shutdown and task-drop semantics.
- [ ] Compare copy bytes, ARM CPU/request, arena stalls, publication rate and
  p50/p99 against the reservation baseline. **Re-baseline first.** The published
  `l7-tx-ab-20260817` arm predates the `DmeshIo` tx cursor, which removed the
  queue-tail move `consume_tx` performed on every publication; the absolute
  µs/request in that dataset is therefore no longer the arm to subtract from.

## O3 Conditional worker-local state

- [ ] Re-profile after O1/O2; continue only if endpoint locking becomes
  material. Nothing counts lock contention today: `Backends` holds one
  `parking_lot::Mutex` with no instrumentation, and a retired profile put the
  AArch64 parking-lot fast path at 1.3–1.7% with no pool symbol above it. Add a
  counter before drawing a conclusion from that.
- [ ] If justified, prototype only the DPUmesh specialization on Tokio `LocalSet`
  with `Rc<RefCell<_>>`; do not add unsafe `Send`/`Sync` claims or modify stock
  TCP Linkerd behavior.

## O4 Optional shared backend transport

- [ ] Attempt only if session-stack work still shows a material need.
- [ ] Define a separately versioned backend-transport id, independent lifetime,
  stream response routing, flow control and cancellation. Do not overload the
  current client connection handle.

## O5 Equivalent ARM/x86 study

- [ ] Build an equivalent x86 full-stack harness with the same Linkerd revision,
  control plane, compiler, flags, request sizes and concurrency.
- [ ] Compare cycles/request, instructions, cache misses, migrations, context
  switches, latency, throughput, affinity and clock frequency.
- [ ] Separate `DmeshIo`, Tokio, opaque stack, protocol detection,
  policy/identity and full-stack slices before proposing a runtime change.
