# DPUmesh Plan

The design documents in [`design/`](design/) state what is built and
[`bench/report/REPORT.md`](bench/report/REPORT.md) states what it measures. This
file is the open work list and the record of what the campaigns established, in
four parts.

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

**Findings is not work.** It is the third part, and it holds what the campaigns
established about the mesh, the instrument and the harness: the rules that now
hold, and how each one is seen. Every later item is measured against them.

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

Four items are open: a Go surface, the transport under the cross-node seam,
node density, and the kernel road around the mesh.

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
  side that `dmesh_peer_accept` completes, and `px_peer_configure` called to
  bind it. What the layer above requires of it is ordered reliable delivery
  within a handle and a mutually authenticated key agreement whose peer static
  key can be read back. Nothing above the seam can be exercised on hardware
  until this binds: the peer table is initialised with no transport, so a remote
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

A node serves the smaller of two caps, and they bind on different hardware and
do not cost the same to move. Nodes routinely run more Pods than the 32 that
answers today, so this is a real limit.

| Constant | Value | Where | What it bounds |
|---|---:|---|---|
| `MAX_DPA_RINGS` | 8 | `include/dpumesh/dmesh_common.h` | forward rings one execution unit can hold |
| execution units (N) | 32 | device query at start-up | EUs the BlueField reports |
| `DPUMESH_RINGS_PER_POD_DEFAULT` (K) | 2 | `include/dpumesh/dmesh_common.h` | EUs one Pod spans |
| `MAX_PODS` | 32 | `include/dpumesh/dmesh_common.h` | registration slots the ARM holds |
| `POD_ID_SPACE` | 128 | `include/dpumesh/dmesh_common.h` | Service-id keyed tables |

N is read from the device and `DPA_THREADS_DEFAULT = 8` is only the fallback
when that query is unavailable, so N comes from the deploy log
(`dpa_threads='32'`) and never from the constant. At the default K, `8 × 32 / 2 = 128` ring slots
back 32 registration slots, so `MAX_PODS` is what binds. The bench deployment
runs `K = 8`, which leaves exactly 32 ring slots for those 32 Pods: density and
per-Pod throughput are the same dial, and the harness has it turned to
throughput.

**The ring array is not the constraint.** `MAX_DPA_RINGS` sizes five arrays in
`struct dpa_thread_arg`, one copy per EU in device memory, at 68 bytes per ring
slot over a fixed 68-byte remainder. Doubling it to 16 takes one EU's thread
argument from 612 B to 1,156 B and the whole device-side cost from 19.1 KiB to
36.1 KiB across 32 EUs.

Two other things are.

- **The per-poll scan is linear in the rings an EU holds.** `run_dma_manager`
  walks `num_rings` on every pass and reads each ring's control block out of
  host memory, so an idle slot costs nothing and occupancy costs everything: at
  `K = 2`, 127 Pods put roughly eight rings on every EU, which is eight
  control-block reads per poll instead of the two a lightly loaded node does.
  That figure cannot be measured off this deployment; it needs a node carrying
  that many Pods.
- **The DPA kernel is device code with its own toolchain.**
  `doca/device/dpa_kernel.c` is compiled by `dpacc` on the BlueField, not by the
  host build. Changing `MAX_DPA_RINGS` touches no wire format — `dpa_ring_info`
  stays 48 bytes, `comch_add_ring_msg` 56, `comch_msg` 60 — but both sides must
  be recompiled against the same header and redeployed together, or the ARM
  writes `rings[12]` into an EU that allocated eight.

Ordered by cost:

- [ ] **`MAX_PODS = 127`.** A host constant, no DPA change, no wire-format
  change: pod ids travel as `int8_t` in `comch_dma_comp_msg` and `struct
  px_unit` with `-1` reserved, and `_Static_assert(MAX_PODS <= 127)` holds the
  line. `POD_ID_SPACE` is 128 and moves with it. At the default `K` this
  hardware's 128 ring slots already back it, so this is the whole move up to 127
  Pods.
- [ ] `MAX_DPA_RINGS = 16` — 17 KiB more device memory, a `dpacc` rebuild, a
  paired redeploy, and a per-poll scan that doubles at full occupancy. Needed
  only for a device with fewer EUs than this one, or for `K` above 2.
- [ ] Above 127 those wire fields widen. That is a host-and-DPU ABI change of
  the kind the reverse ring's `struct dmesh_tx_ack_entry` is static-asserted
  against, because a count the two ends disagree on is silently lossy rather
  than a failure. Not worth scheduling until a deployment needs it.

Lowering `K` buys ring slots this hardware does not need, and what it costs is
per-Pod parallelism: a Pod spanning one EU has its forward traffic served by one
EU's DMA budget. Which way that goes depends on whether the node's Pods are
individually hot or collectively many. No number is published as supported until
a node has been run at it; today's supportable claim is 32.

## F8 The kernel road around the mesh

The meshed path is fail-closed: an injected Pod's shim refuses a connect it
cannot route through the DPU — only a destination the DPU itself answers as
not-meshed proceeds over kernel TCP — and the admission webhook refuses a Pod
creation it cannot patch or decide (`bench/suite/inject.sh` I5 is that arm).
What stays open is the road that never enters the mesh: a Pod without the
annotation reaches a protected Service's backend over plain kernel TCP, and
nothing on the node stops it.

- [ ] Block kernel-TCP ingress to protected backends at the host. The CNI
  (flannel) enforces no NetworkPolicy, so the block is iptables-level
  configuration owned by the node agent; the arm that closes it is an
  unannotated Pod refused where an annotated one serves.
- [ ] Replace `privileged: true` in the webhook patch with the device access
  the transport actually opens. The container device cgroup blocks the char
  device without it, so this is an RDMA device-plugin deployment, not a
  manifest edit.
- [ ] Exercise the shim's refusal on hardware: with the DPU down, a meshed
  Pod's connect must fail rather than leave the mesh. No campaign arm drives
  this today and `preload_api_contract_test` mocks the channel as available,
  so the refusal is held by construction alone until an arm exists.

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

## D2 gRPC tail regime above the reported capacity

Above roughly 12.5 K rps the p99 changes regime while the p50 stays flat, with a
periodic stall near 15 ms. Eight causes have been excluded. This is what sets
the gRPC capacity that gets published, so it bounds a number already in print
rather than a feature not yet built.

- [ ] **This item carries no receipt in the tree.** Find the campaign data it
  came from and attach it here, or re-run and replace the figures, before it is
  cited anywhere else.
- [ ] Then name the ninth cause, or close it.

One neighbouring item is not on this list, because it has its arm. Clearing
`dma_ready` on one Pod must not clear it on Pods that have nothing to do with it,
and both clear sites are per Pod; `surfaces` `S13`/`S14` drives exactly that — a
failing endpoint inside the Service under test, traffic through it until the
breaker ejects it, then the Deployment deleted while the campaign continues — and
the reductions in [`bench/report/data/f-spin-20260822/`](bench/report/data/f-spin-20260822/)
repeat it against a freshly deployed DPU. The healthy endpoint keeps serving
across the withdrawal in every one of them, and the native and opaque arms are
untouched.

---

# Findings

What the campaigns established. Each entry is a rule that now holds and how it
is seen. The environment is what `bench/bench.sh deploy` leaves behind: one
node, `K=8`, 32 execution units, namespace `test-bench`.

## The mesh

**A datapath pass that answers on state never reaches its wait.** The persistent
driver waits on a `select!` of three notification fds, a maintenance deadline
and `poll_internal`. The fds need the driver to be running, so an internal poll
that answers from a *state* rather than an event completes that select
synchronously on every pass, and the driver never runs. What stops then is not
one port but every listener registered on that driver — the worker's inbound,
outbound and admin listeners alike — so a worker at 99.9% CPU with the cluster
idle serves nothing that lands on it.

Two guards hold it. `Worker::poll_internal` does not count a FIN the datapath has
already accepted, which is a closed write half's steady state rather than
outstanding work; and the runtime loop drops the internal poll from the wait
after one wake that publishes nothing, until a notification, a drain that
progresses, or the maintenance deadline says something moved. Two conditions of
the same shape stay reachable — a refused `tx_finish`, and queued bytes no arena
chunk can carry — and the guard bounds both to the millisecond maintenance
period instead of a spin. Nothing is dropped: every pass drains before it waits.
`unpublishable_internal_work_does_not_spin` is the regression test — a backend
that reports work on every ask and publishes none of it must still reach its
wait.

The diagnostic signature is worth carrying: a wedged worker's admin port is
*bound and unanswered*, not refused. The socket is bound, the kernel has
completed handshakes nobody accepts, and `/live` — which reads no metric and
takes no lock — times out with everything else.

```sh
bash bench/bench.sh dpucpu                                       # 99.9% with no traffic
ssh "$DPU_HOST" 'curl -sf --max-time 4 127.0.0.1:4195/metrics'   # nothing
ssh "$DPU_HOST" 'ss -ltn'                                        # Recv-Q > 0 on a LISTEN row
```

Readings are in
[`bench/report/data/f-fix3-20260822-020539/worker-spin-diagnosis.txt`](bench/report/data/f-fix3-20260822-020539/worker-spin-diagnosis.txt)
and [`worker-spin.txt`](bench/report/data/f-fix3-20260822-020539/worker-spin.txt).

**A control descriptor is the source port's incarnation fence.** A source port
must not be reused, and an old `(pod, port)` session must not be reachable, until
the DPU has retired everything that names it. Four places hold that:

- the host tracks zero-length FIN and reset controls in its TX FIFO and does not
  reuse a source port until their DPU ACK retires;
- the DPU defers a request FIN's ACK until both Linkerd output halves and all
  upstream state are gone, and follows a reset ACK with removal of the old
  request key — replies use DPU-assigned high ports and keep their immediate
  directional ACK, so only source requests defer;
- each gRPC client connection carries a one-shot lease shared by the public
  `Channel` and its gRPC `Endpoint`, so abandonment and a late successful connect
  both retire the QP through `dmesh_abort_qp` rather than queueing serial
  five-second graceful closes on the reactor;
- stalled DPU lifecycle work is re-armed whenever EOF, peer FIN, backend FIN or
  ACK publication is still under allocation or backpressure, so a teardown that
  runs while the unit pool is dry cannot leave the refused session key behind.

A full campaign on a fresh DPU decides every stage it offers —
[`policy-route-20260824-095824/stages.csv`](bench/report/data/policy-route-20260824-095824/stages.csv),
45 of 45, with `S14` among them — and afterwards the eight workers report 3,798
sessions opened and 3,798 closed with `ACTIVE=0`, `PENDING=0` and `TASKS=0` on
every one. `dmesh_registrations_orphaned_total` is cumulative and counts late
endpoint registrations the generation fence safely aborted — it is not live
residue.

**A fatal signal on this DPU leaves nothing behind.** Nothing under `doca/` or
`src/` calls `exit()`, `_exit()` or `abort()`, and no Rust in the adapter calls
`process::exit`, so a process that is gone was ended from outside. SIGPIPE's
default action terminates without a core file, and this kernel reports no fatal
signals (`kernel/print-fatal-signals` and `debug/exception-trace` are both `0`),
so taking one leaves no core, no kernel line and a log frozen wherever it had
reached. A Rust binary gets `SIG_IGN` for SIGPIPE from the runtime start-up its
own `main` runs; the embedded proxy is a static library linked into a C `main`,
so `doca/dpu_main.c` installs it before anything opens a socket — while the proxy
holds sockets to a control plane whose peers come and go.

With `print-fatal-signals=0` a fatal signal of any kind leaves `dmesg` silent, so
the absence of a kernel message is a fact about this DPU rather than evidence.
What still rules out the signals that do dump — SIGSEGV, SIGABRT, SIGBUS — is a
missing core with the limit unlimited, the pattern a plain file and disk free.
`stop_dpu` snapshots `screen -ls`, the kernel log, the core pattern and the log
tail into `$OUT` when it finds the process already absent; every command in that
capture is guarded and the function answers `0` whatever happens, because it runs
on the way to a redeploy and nothing it collects is a reason to stop one.
Evidence: [`dpu-exit-sigpipe.txt`](bench/report/data/f-spin-20260822/dpu-exit-sigpipe.txt)
and [`dpu-exit2-evidence.txt`](bench/report/data/f-spin-20260822/dpu-exit2-evidence.txt).

## The instrument

**A benchmark client must not outlive its own run.** `bench_grpc`'s control
server accepts one connection at a time and runs the benchmark on the accept
loop's own thread, so a run that never ends is a client that answers nothing for
every stage after it. Four rules keep a run bounded: a call joins the worker's
live set before it reaches gRPC, so a cancellation asked for in between is held
on the context and applied when the call starts; issuance closes under the same
lock the shutdown sweep takes, so no call outlives the sweep uncancelled; the
sweep waits out the calls already admitted before shutting the completion queue
down; and a run bounded by its own duration plus its channels' connect budget
reports the fault and exits rather than holding the control port, which lets the
Deployment bring back a client that answers.

This is closed by construction rather than reproduced on demand: repeated runs
each failing 120,000–140,000 requests do not wedge a client built without those
rules either. The campaign therefore runs the mass-failure stages and the ones
after them without restarting the client between them, which makes the
`surfaces` arm itself the test.

## The harness

These are the reading rules the campaigns hold. Each one exists because its
absence puts something false in the record or stops the run that is making it.

| Rule | Why |
|---|---|
| A reply with no `rcnt=` field is `nodata` and fails | a client that answers nothing otherwise looks exactly like a client whose requests were all refused, and every stage expecting a refusal passes |
| An unreadable counter answers `NA`, and `ctl_delta` propagates it | defaulting to `0` makes the next delta negative, and a process-global counter cannot run backwards |
| A counter with no sample defaults on the *value*, not on a line | a metric name with no sample yields no line, and a substitution with no line writes nothing, so the stage evaluates `$(( - ))` — invisible until a freshly restarted DPU has none of the family |
| An empty metric grep is tolerated | under `set -o pipefail` and `set -e`, reading per-Pod counts for a Service that has served nothing ends the campaign mid-stage |
| A timeout is read from the route's own counter | the client's latency includes a DMA round trip on both sides that a route timeout does not cover |
| A breaker's failing endpoint is inside the balancer under test | every endpoint failing leaves it nowhere to eject to, and so does a failing backend a route already chose by weight |
| A gRPC retry condition is written on a `GRPCRoute` | on an `HTTPRoute` the annotation is dropped and the limit beside it still builds a retry policy with no condition, which can never fire |
| An inbound authorization is written in the kind its `Server` carries | a gRPC `Server` carries gRPC routes, so an `HTTPRoute` parented to it never appears in the port's policy and the port keeps its deny-by-default |
| Every balancing row has a verdict | a row that only records `rcnt`, `fail` and distribution never increments either campaign total, so missing DPU evidence and a stopped client both disappear from the summary |
| Opaque balancing reads client distribution and active-worker endpoint gauges; protocol-aware balancing reads client completion, DPU request attribution and take errors | traffic alone cannot distinguish a fixed backend from a working balancer, and one worker's maximum endpoint count must not hide a different positive count on another worker |
| Free text is written with the CSV separator replaced and every row is schema-checked | fixture labels and `dist` fields contain commas, which shift those rows out from under the 13-column header; a malformed row now fails the campaign even if every behavioural verdict passed |
| Evidence capture never fails the run it informs | `screen -ls` answers non-zero when there is no session, which under `set -e` ends the deploy the capture runs inside |
| A measurement after anything recreated a Pod re-pins first | core pinning is per PID, so a validator or stage that restarts a workload leaves it unpinned; the arm then reads the scheduler rather than its subject |
| A stage waits for Ready, never for Running | a container that exits and restarts keeps its Pod in phase `Running`, so the traffic stage gets a server that never started and its refusal reads as a mesh verdict |
| A Pod's ring geometry is read from the running DPU | the host refuses a channel whose `K` is below the DPU's landing stripes, so a harness that defaults `DPUMESH_RINGS_PER_POD` instead of reading the startup banner crash-loops every Pod the webhook admits |

The two route-kind rules are also why `linkerd diagnostics policy` is the
arbiter for a fixture question: in both, the client cannot tell an enforced
policy from a policy that was never built, and the controller's own answer can.
`conditions: {}` under a retry limit, or a `Grpc` port carrying one default
route and no authorizations, is the whole diagnosis.

The balancing false-pass is closed by
[`policy-route-lb-20260824-judge-v3/stages.csv`](bench/report/data/policy-route-lb-20260824-judge-v3/stages.csv):
21 of 21 rows carry `PASS`, every row has exactly 13 columns, L5 returns from two
replicas to one with one ready endpoint and one serving Pod, and backend take
errors stay flat. The earlier `policy-route-20260824-095824` LB rows have no
verdict and are not receipts; its 45 policy-through-surface verdicts remain
valid.

## Deployment

**Wait for the object before waiting for its condition.** `kubectl wait` fails
at once on no matching resources rather than waiting, so scaling a Deployment to
one and immediately waiting on a still-empty label set reports `failed to start`
for a Pod that starts normally seconds later. The race is invisible until
something puts latency in front of Pod creation, which admission does to every
Pod in the namespace.

Scaling a shared-label Deployment is a different case: waiting for every Pod
with that label to be deleted can never finish when the desired replica count is
one, and waiting for `Ready` on the label may accept the old Pod before the new
replica exists. Scale arms wait on the Deployment's exact `readyReplicas` value.

---

# Cost

Nothing in this part changes admission, custody or any security property, which
is what makes these items independently schedulable — and what makes it a
measurement error to run one across a build that also changes correctness
behavior.

## What is known

Per-session cost, not the transport, bounds L7 capacity, and per-workload stack
sharing removed most of it: one session costs 3.9 ms of ARM time unshared and
0.4–0.5 ms shared, the closed loop completes 4.4× more sessions under heavy
churn, and the 30–40 ms p99 spikes of per-session stack building disappear. The
steady per-request point is unchanged, which is the expected result: the data
path does not know the stacks are shared. The receipt is
[`bench/report/REPORT.md`](bench/report/REPORT.md), *What a session costs*.

The synchronous half of a stack build is instrumented in
`linkerd/app/src/lib.rs` and reported by `SessionMetrics::observe_stack_build`:
8.7 µs to clone the outbound template and set `dmesh_session`, 107.7 µs of
layers (`build_policies` + `outbound.mk`), 32.0 µs of `NewService::new_service`,
**148.4 µs** in total. The remainder is lazy discovery and policy work, task
execution and teardown, and the surrounding DPUmesh lifecycle. **Locating it
requires instrumenting those asynchronous boundaries; do not assume the whole
figure sits inside the synchronous call.**

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
  µs/request is not the arm to subtract from.

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

- [ ] Price it with the core-attribution arms of `bench/suite/api_l7_cost.sh`,
  run against a build that changes nothing else. Report ARM µs/request for a
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
