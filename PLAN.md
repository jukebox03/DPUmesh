# DPUmesh Plan

The design documents in [`design/`](design/) state what is built. This file is
the open work list, and it has two halves in a fixed order.

**Function comes before cost.** A deployment can weigh "it is slower here"; it
cannot weigh "it is not supported here". Every item under *Function* is
something another service mesh does and this one refuses, and refusing is what
ends an adoption conversation — being cheaper afterwards does not reopen it. No
cost item is scheduled ahead of a function item, and a cost item that would make
a function item harder to reach is deferred rather than merged.

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
traffic that flows without one is not a routing result either. Each item below
names the arm that closes it.

---

# Function

Two of the first three items are one layer thick, and it is worth stating why
before they are read as architecture. The datapath already carries what they
need: `dmesh_l7_tx_commit(worker, conn, backend_pod, len)` takes an arbitrary
destination on every commit, `px_upstream_resolve` allocates a return mapping
per `(source Pod, source port, destination Pod)`, and `px_conn_admitted`
re-evaluates inbound policy whenever the destination changes. One client
connection fanning out across several backends is a shape the C side is already
built for. What refuses it is the adapter: a session publishes exactly one
`DmeshIo`, and `Backends::take_session` hands it out once.

## F1 Per-request backend selection

A protocol-aware Service balances per session, not per request. The balancer
holds every ready endpoint, but only the one whose channel the session owns ever
becomes ready; a second dial is refused and counted in
`dmesh_backend_take_errors_total`. Measured against two backends: one client
channel sent 25,104 requests to a single Pod and 0 to the other, four channels
split 90,801 / 29,974. Spread therefore comes from how many channels a client
opens, which is exactly the property Linkerd's HTTP/2 balancing exists to
remove.

- [ ] Replace `Session.backend: Side` with one `Side` per selected backend Pod,
  each carrying its own `BackendRoute`. `pump_side` already publishes per handle
  with a per-handle route, so the publication path is unchanged.
- [ ] Mint a `DmeshIo` per `(session, endpoint)` in `Backends::take_session`
  instead of moving the session's single one out. `AlreadyTaken` stops being the
  answer to a second endpoint and goes back to meaning what it says.
- [ ] Demultiplex replies by backend. A reply connection is keyed by
  `session_key(flow.peer_pod, flow.dst_port)` — the *client* Pod — so several
  backends replying to one client collide on one key today; `flow.src_pod` on
  the reply names which backend sent it.
- [ ] Make FIN pairing, staging custody and session retirement per backend
  `Side`. **This is the risk in the item.** The drain-set crash and the
  claim-then-abandon defects were both in this bookkeeping; write the lifecycle
  tests before the fan-out, not after.
- [ ] Accept on: two backends, one client channel, requests split across both;
  `fail=0`; `dmesh_backend_take_errors_total` flat. Arm: extend `L4` in
  `bench/suite/policy_route.sh`, whose one- against four-channel contrast is the
  current negative result.
- [ ] Report the per-request cost of the extra dispatch. Paying for it is the
  point of the item; hiding it is not.

## F2 Routing across Services

An `HTTPRoute` may reorder, filter or reject requests inside its parent Service
but may not send them to another one: `take_session` refuses a target the signed
generation places elsewhere, and the request fails rather than being dialled
over TCP (81 `dmesh_backend_target_mismatches_total` in the R3 arm). Weighted
`backendRefs` — the ordinary shape of a canary — are therefore unavailable.

- [ ] Relax the `TargetMismatch` guard to admit a target the endpoint resolver
  answers `Live` or `Remote` for, whatever Service the generation places it in.
  The guard that matters is liveness and node placement, not Service identity.
- [ ] Grade the callee by the callee. `px_conn_admitted` reads
  `px_inbound_strict(objs, c->pub.dst_service)` — the Service the *client*
  asked for. Once a route may cross Services that is the wrong subject; it must
  be the destination Pod's own `service_id`, and the mixed-callee rule with it.
- [ ] Carry the resolved destination Service into per-unit accounting so a
  redirected stream is attributed where it went, not where it was addressed.
- [ ] Confirm the security argument holds by test, not by reading: the
  destination's inbound policy is evaluated for the Pod that actually receives
  the bytes. Arm: R3 inverted — a route into another Service must succeed, and
  a `Server` on that other Service must still refuse an unauthorized caller.
- [ ] Accept on: weighted `backendRefs` across two Services splitting traffic in
  the declared ratio, with `fail=0`.

## F3 Automatic injection

There is no injection. A meshed Pod carries, by hand, `privileged: true`, a
`/dev/infiniband` hostPath, a hostPath mount for `libdpumesh.so.5`, a
`/run/dpumesh` mount, `DPUMESH_PCI_ADDR`, `DPUMESH_SERVICE`, and
`config.linkerd.io/skip-inbound-ports` on the data port. Linkerd needs one
namespace annotation. This is the most visible gap and the one with no research
risk in it.

- [ ] A mutating webhook that applies that patch on a namespace or Pod
  annotation, refusing rather than half-injecting when the node has no DPU.
- [ ] Keep `skip-inbound-ports` in the patch and say why in the webhook's own
  documentation: removing it makes the destination controller advertise the
  endpoint as meshed and every session ends before carrying a byte. It is part
  of the data path, not cosmetic.
- [ ] Accept on: an unmodified Deployment plus one annotation reaching a meshed
  backend, and the same Deployment without the annotation still reaching it
  unmeshed.

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

## F5 Surfaces that exist but were never exercised

These are Linkerd's own features running in a proxy nobody has pointed at them.
Each is a short campaign arm, and each either passes — and stops being an open
question — or becomes an item above.

- [ ] `HTTPRoute` timeouts and retries.
- [ ] Header and method matching, and `GRPCRoute`.
- [ ] Route-level authorization: an `AuthorizationPolicy` whose `targetRef` is
  an `HTTPRoute` rather than a `Server`.
- [ ] HTTP/1.1 through the protocol-aware path. The stack handles it; the bench
  holds no HTTP/1 workload, so nothing has ever driven it.
- [ ] Circuit breaking and failure accrual.

## F6 The cross-node path, on two DPUs

`design/CONTROL.md` spends half its length on the cluster scope: pairwise node
credentials, peer channels, handles, custody across the boundary, and the claim
that one compromised DPU cannot speak for another node's Pods. The deployment is
a single node. That machinery has never run between two DPUs; `peer_channel_test.c`
is a unit test.

- [ ] Bring up a second DPU node and re-run the deploy against both.
- [ ] Exercise the remote arm of every campaign that currently proves only the
  local one: policy verdicts at a remote destination, endpoint selection across
  the boundary, and peer-channel lifetime under Pod churn.
- [ ] Until this runs, the threat-model claim is a design statement and must be
  written as one wherever it is published.

## F7 Node density

`MAX_PODS` is 32 and the live cap the DPU enforces is
`MAX_DPA_RINGS × N / K` — eight rings per execution unit, eight units, two rings
per Pod — so the two meet at 32 on this hardware. Nodes routinely run more Pods
than that.

- [ ] Raising it means fewer rings per Pod or a larger per-unit ring array, and
  the ring array is DPA device code with its own hardware validation. Size the
  change before promising a number.

## Not a gap: per-hop encryption

There is no endpoint mTLS, and this is a boundary rather than an omission. A
DMA-session endpoint answers `ConditionalClientTls::None(Disabled)` in both the
TCP and HTTP stacks, and discovery offers no identity for these endpoints
because the backend is deliberately advertised as unmeshed. Terminating TLS at
the destination would require the destination DPU to run a second byte-stream
proxy, which is the arrangement this design exists to remove.

Node-to-node traffic is already encrypted by the authenticated RDMA peer
channel with pairwise keys. What remains plaintext is the node-local hop, held
inside registered DMA mappings the workload cannot address. That is a real
difference from a sidecar mesh and should be published as one — stated as the
trade it is, with the property it provides instead, and not as a feature that
is coming.

---

# Cost

Nothing in this half changes admission, custody or any security property, which
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
`linkerd/app/src/lib.rs` and reported by `SessionMetrics::observe_stack_build`:

| Phase | Per session |
|---|---:|
| `configure` (clone the outbound template, set `dmesh_session`) | 5.9 µs |
| `layers` (`build_policies` + `outbound.mk`) | 107.8 µs |
| `service` (`NewService::new_service`) | 34.7 µs |
| synchronous total | **148.5 µs** |

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

- [ ] Re-profile after O1 and O2; continue only if endpoint locking becomes
  material. Nothing counts lock contention today — `Backends` holds one
  `parking_lot::Mutex` with no instrumentation — so add a counter before drawing
  a conclusion. Note that F1 makes this registry busier, which is a reason to
  re-profile after F1 rather than before it.
- [ ] If justified, prototype only the DPUmesh specialization on Tokio
  `LocalSet` with `Rc<RefCell<_>>`; do not add unsafe `Send`/`Sync` claims or
  modify stock TCP Linkerd behavior.

## O4 Equivalent ARM/x86 study

This is a study, not an optimization: it answers what the ARM costs relative to
an x86 host running the same proxy, which is a question the paper needs and no
deployment is blocked on.

- [ ] Build an equivalent x86 full-stack harness with the same Linkerd revision,
  control plane, compiler, flags, request sizes and concurrency.
- [ ] Compare cycles/request, instructions, cache misses, migrations, context
  switches, latency, throughput, affinity and clock frequency.
- [ ] Separate `DmeshIo`, Tokio, opaque stack, protocol detection,
  policy/identity and full-stack slices before proposing a runtime change.
