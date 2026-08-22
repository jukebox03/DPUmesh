# DPUmesh Plan

The design documents in [`design/`](design/) state what is built. This file is
the open work list, and it has three parts in a fixed order.

**Function comes before cost.** A deployment can weigh "it is slower here"; it
cannot weigh "it is not supported here". Every item under *Function* is
something another service mesh does and this one refuses, and refusing is what
ends an adoption conversation — being cheaper afterwards does not reopen it. No
cost item is scheduled ahead of a function item, and a cost item that would make
a function item harder to reach is deferred rather than merged.

**A defect outranks both.** *Defects* holds failures found in behaviour that is
already published, and fixes written for a failure that has never been run
against it. Something that falls over after it was claimed costs more than
something that was never offered, and a fix nobody has watched work is an open
defect wearing a patch. Where an item there has no receipt in this tree, the
item says so, and attaching the receipt is the first task in it.

**Measurement discipline (binding).** A capacity is quoted with the instrument
that produced it. `bench/suite/analyze_saturation.py` votes a `knees.csv`
`highest_clean_rps` out of an open-loop grid; a closed-loop rate grid reaches
further on the same data. The two are not interchangeable and a figure must not
present one under the other's caption. A result is accepted only from repeated
runs with the frozen topology, placement and 2.5 GHz clock.

**Function is proved the same way.** A feature is not delivered because traffic
flowed. `bench/suite/policy_route.sh` judges every arm twice — by what the
client completed and by what the DPU's own counters say it decided — because
traffic that stops without a matching verdict is not a policy result, and
traffic that flows without one is not a routing result either. A stage whose
client returned no reply at all is recorded as `nodata` and fails: a missing
measurement is the one thing that must never be read as a refusal, because it
would let a stopped instrument pass every arm that expects traffic to stop. Each
item below names the arm that closes it.

---

# Function

Three items are open. Four that stood here — per-request backend selection,
routing across Services, automatic injection and the protocol-aware connection
failure — are built or repaired, measured and stated in [`design/`](design/),
so they are no longer work. The two fixture questions are closed with the policy
controller's own answer in
[`bench/report/data/findings-20260822.md`](bench/report/data/findings-20260822.md),
and the protocol-aware failure is closed by two consecutive 20/20 campaigns in
[`bench/report/data/d4-f5-closure-20260822.md`](bench/report/data/d4-f5-closure-20260822.md).

What is left is a Go surface, the transport under the cross-node seam, and node
density.

## F4 Workloads `LD_PRELOAD` cannot reach

The preload shim gives an application real kernel file descriptors, so `epoll`,
`poll` and `select` work unchanged — but it interposes through
`dlsym(RTLD_NEXT, …)`, so it cannot attach to a static link or to a runtime that
issues syscalls without libc. That is every Go program, and a large part of the
Kubernetes ecosystem is Go.

- [ ] Decide the surface: a Go package presenting `net.Conn` and `net.Listener`
  over the native API is a source change for the application, which is honest;
  syscall interposition is not available for this class of binary and should not
  be promised.
- [ ] Whichever surface is chosen, it registers the process the same way and
  under the same signed grant. No adapter gets its own admission path.

## F6 The cross-node path: the transport under the seam

The cluster scope is a layer split. `doca/peer_channel.c` owns everything above
`struct dmesh_peer_transport` — handle namespaces, bounded parsing, the
node-name-to-key binding check, custody across the boundary, refusal accounting
— and `doca/dpu_proxy.c` carries the hooks that bind it to the datapath.
`tests/peer_channel_test.c` drives that layer end to end through a recording
transport. The remaining work is the half below the seam.

- [ ] **Implement the RDMA transport.** Five callbacks — `connect` with a
  prologue bound into the handshake, `peer_key` returning the peer's
  authenticated static public key, `send`, `recv`, `close` — plus the accept
  side that `dmesh_peer_adopt` completes, and `px_peer_configure` called to bind
  it. What the layer above requires of it is ordered reliable delivery within a
  handle and a mutually authenticated key agreement whose peer static key can be
  read back. Nothing above the seam can be exercised on hardware until this
  binds: the peer table is initialised with no transport, so a remote
  destination is refused at the first branch of `px_peer_stream_ready`.
- [ ] Bring up a second DPU node and re-run the deploy against both.
- [ ] Exercise the remote arm of every campaign that proves only the local one:
  policy verdicts at a remote destination, endpoint selection across the
  boundary, and peer-channel lifetime under Pod churn.
- [ ] Widen the cross-node pin. `px_peer_pin_admits` refuses a stream's second
  remote destination and counts it; per-request fan-out across nodes needs a pin
  per destination, and that function is the one place that decides.
- [ ] Until a transport binds, node-to-node confidentiality, authentication and
  custody are properties the design assigns to it. Publish them as what the
  design provides, with the status attached — not as what a deployment does.

## F7 Node density

A node serves the smaller of `MAX_PODS` and `MAX_DPA_RINGS × N / K`. The sizing
is in [`bench/report/data/node-density-sizing.md`](bench/report/data/node-density-sizing.md):
this BlueField reports 32 execution units, so at the default two rings per Pod
the ring array offers 128 slots and `MAX_PODS = 32` is what binds.

- [ ] Raise `MAX_PODS` to 127. It is a host constant, the ring array already
  backs it at the default `K`, and pod ids stay inside the signed one-byte wire
  space. Past 127 those fields widen, which is a host-and-DPU wire-ABI change.

## Not a gap: per-hop encryption

There is no endpoint mTLS, and this is a boundary rather than an omission. A
DMA-session endpoint answers `ConditionalClientTls::None(Disabled)` in both the
TCP and HTTP stacks, and discovery offers no identity for these endpoints
because the backend is deliberately advertised as unmeshed. Terminating TLS at
the destination would require the destination DPU to run a second byte-stream
proxy, which is the arrangement this design exists to remove.

What remains plaintext is the node-local hop, held inside registered DMA
mappings the workload cannot address. That is a real difference from a sidecar
mesh and should be published as the trade it is. The node-to-node half of the
argument rests on the peer channel's transport, so it carries F6's status with
it until that transport binds.

---

# Defects

Three of the six items that stood here are closed and gone: the gRPC client that
stopped answering after a run of failures, the DPU that exited with no cause,
and the refused-session leak. The first two are written up in
[`bench/report/data/findings-20260822.md`](bench/report/data/findings-20260822.md);
the refused-session repair and its real-DPU receipt are in
[`bench/report/data/d4-f5-closure-20260822.md`](bench/report/data/d4-f5-closure-20260822.md).
What remains is two failures nothing has diagnosed and one fix nothing has
watched work.

## D1 gRPC overload crash

`echo_grpc` takes a SIGSEGV under sustained overload — one channel near 64 K
rps, two channels near 100 K — faulting inside libc on what reads as a corrupted
function pointer. Both gRPC sweeps are built around it rather than without it:
`bench/suite/grpc_closed_sweep.sh` and `grpc_conns_sweep.sh` watch the container
restart count between points, re-resolve the endpoints, and append the point to
`crashes.csv`. That is scaffolding around a defect, and scaffolding is not a
fix.

- [ ] Reproduce under ASAN. Until it reproduces there is nothing to repair, and
  the sweeps go on measuring across a process that died.
- [ ] While it stands, no gRPC capacity may be quoted from a sweep whose
  `crashes.csv` is non-empty without saying that it is.

## D2 gRPC tail regime above the reported capacity

Above roughly 12.5 K rps the p99 changes regime while the p50 stays flat, with a
periodic stall near 15 ms. Eight causes have been excluded. This is what sets
the gRPC capacity that gets published, so it bounds a number already in print
rather than a feature not yet built.

- [ ] **This item carries no receipt in the tree.** Find the campaign data it
  came from and attach it here, or re-run and replace the figures, before it is
  cited anywhere else.
- [ ] Then name the ninth cause, or close it.

## D3 A fix nothing has watched work

A fix written for a failure and never run against that failure is not yet a fix.
One is left here; the shared-DMA-context item that stood beside it has its arm
now — see below.

- [ ] DMA ring behaviour under 40 K-rps overload. The abandoned-ticket latch is
  gone, the overload was still failing when it was last driven, and two
  candidate fixes — batching, and descriptor admission — were reverted as
  regressions. Same treatment as D2: attach the receipt or re-run it.

**Shared DMA context collateral — armed.** Killing one backend under load used
to clear `dma_ready` on Pods that had nothing to do with it; both clear sites
were made per Pod and nothing had since killed a backend under load to watch the
others keep running. `surfaces` `S13`/`S14` now does exactly that — a failing
endpoint inside the Service under test, traffic through it until the breaker
ejects it, then the Deployment deleted while the campaign continues — and the
reductions in
[`f-spin-20260822/`](bench/report/data/f-spin-20260822/) repeat it against a
freshly deployed DPU. The healthy endpoint keeps serving across the withdrawal
in every one of them, and the native and opaque arms are untouched. The
withdrawal was not the cause of the now-closed protocol-aware failure: it was
tested six ways, none reproduced it, and the closure receipt records the actual
connection-lifetime cause.

---

# Cost

Nothing in this part changes admission, custody or any security property, which
is what makes these items independently schedulable — and what makes it a
measurement error to run one across a build that also changes correctness
behavior.

## What is known

Per-session cost, not the transport, bounds L7 capacity, and per-workload stack
sharing removed most of it. The receipt is
[`bench/report/data/l7-shared-ab-20260821.md`](bench/report/data/l7-shared-ab-20260821.md):
one session costs **3.9 ms** of ARM time without sharing and **0.4–0.5 ms**
with it, the closed loop completes 4.4× more sessions under heavy churn, and
the 30–40 ms p99 spikes of per-session stack building disappear. The steady
per-request point is unchanged, which is the expected result: the data path does
not know the stacks are shared.

The synchronous half of a stack build is instrumented in
`linkerd/app/src/lib.rs` and reported by `SessionMetrics::observe_stack_build`.
The figures below are that instrument's cumulative nanosecond counters in
[`bench/report/data/api-l7-20260821/proof_protocol_aware_worker0_metrics.txt`](bench/report/data/api-l7-20260821/proof_protocol_aware_worker0_metrics.txt),
over the 14 builds the same snapshot counts:

| Phase | Per session |
|---|---:|
| `configure` (clone the outbound template, set `dmesh_session`) | 8.7 µs |
| `layers` (`build_policies` + `outbound.mk`) | 107.7 µs |
| `service` (`NewService::new_service`) | 32.0 µs |
| synchronous total | **148.4 µs** |

The remainder is lazy discovery and policy work, task execution and teardown,
and the surrounding DPUmesh lifecycle. **Locating it requires instrumenting
those asynchronous boundaries; do not assume the whole figure sits inside the
synchronous call.**

## O1 Attribute what is left of a session

- [x] Cache immutable templates and share per-workload stacks. Shipped and
  measured; `dmesh_session_stack_cache_hits_total` climbs while
  `dmesh_session_stack_builds_total` stays flat.
- [ ] Instrument the untimed remainder of the **current** 0.4–0.5 ms: policy
  discovery, destination and profile discovery, reconnect layers, endpoint
  construction and balancer construction. Extend `SessionMetrics` rather than
  adding a second surface.
- [ ] Keep session-local what must be: `SessionToken`, backend channel,
  workload, target generation, cancellation and metrics. The connector binds to
  `dmesh_session`, so a shared service would take another session's channel —
  `two_same_service_sessions_take_their_own_channels` in
  `outbound/src/tcp/connect.rs` is the regression test for exactly that.
- [ ] Accept only a repeated hardware improvement with no p99 or correctness
  regression.

## O2 Direct `AsyncWrite` reservation

The reservation-versus-copy A/B is measured: the reservation path costs 0.265
ARM µs/request less at concurrency 128, with the three-run ranges disjoint at 32
and 128. That fixed the default, but it did not remove the intermediate queue,
which is what this item is for.

- [ ] Inject a worker-local egress reservation interface while keeping all
  `dmesh_l7_*` FFI in the adapter.
- [ ] Write from Linkerd directly into the DPUmesh arena without the
  intermediate tx queue, preserving partial-write and output ordering.
- [ ] Define capacity wakeup, cancellation, shutdown and task-drop semantics.
- [ ] Compare copy bytes, ARM CPU/request, arena stalls, publication rate and
  p50/p99 against the reservation baseline. **Re-baseline first.** The published
  `l7-tx-ab-20260817` arm predates the `DmeshIo` tx cursor, which removed the
  queue-tail move `consume_tx` performed on every publication, so its absolute
  µs/request is no longer the arm to subtract from.

## O3 Conditional worker-local state

Per-request backend selection put more through this registry: a session takes
one channel per endpoint instead of one for its life, and every worker pass asks
whether an endpoint was minted. That question is answered by an atomic rather
than the registry lock, so an idle pass does not take it — but the take path
itself is busier than the profile that last looked at it.

- [ ] Re-profile; continue only if endpoint locking becomes material. Nothing
  counts lock contention today — `Backends` holds one `parking_lot::Mutex` with
  no instrumentation — so add a counter before drawing a conclusion.
- [ ] If justified, prototype only the DPUmesh specialization on Tokio
  `LocalSet` with `Rc<RefCell<_>>`; do not add unsafe `Send`/`Sync` claims or
  modify stock TCP Linkerd behavior.

## O4 What per-request selection costs the data path

Backend selection per request adds work to paths that run per segment and per
unit, and none of it has been priced. `l7_conn_segment` names the direction a
segment belongs to instead of assuming it; `px_conn_admitted` scans a
four-entry verdict cache instead of comparing one destination; `struct px_conn`
carries that cache. The verdict cache is a net removal for a session that
alternates backends — it replaces a policy re-entry per unit with four
comparisons — and a net addition for one that does not.

The deploy smoke gate shows p50 unchanged at concurrency 1 across the change,
which bounds nothing: it is one operating point on a closed loop.

- [ ] Price it with `bench/report/CORE.md`'s core-attribution campaign, run
  against a build that changes nothing else. Report ARM µs/request for a
  single-backend opaque stream, where the additions are pure cost, and for a
  protocol-aware stream alternating backends, where the cache is the saving.

## O5 Equivalent ARM/x86 study

This is a study, not an optimization: it answers what the ARM costs relative to
an x86 host running the same proxy, which is a question the paper needs and no
deployment is blocked on.

- [ ] Build an equivalent x86 full-stack harness with the same Linkerd revision,
  control plane, compiler, flags, request sizes and concurrency.
- [ ] Compare cycles/request, instructions, cache misses, migrations, context
  switches, latency, throughput, affinity and clock frequency.
- [ ] Separate `DmeshIo`, Tokio, opaque stack, protocol detection,
  policy/identity and full-stack slices before proposing a runtime change.
