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

Three items are open: a Go surface, two-node validation of the cross-node seam,
and the device-plugin reduction of Pod privilege — the kernel road around the
mesh is closed in both directions and F8 carries the receipts. Node density is
settled at the wire ceiling; F7 keeps its two deeper levers, both unscheduled.

## Now — close the two-node receipt first (2026-08-26)

The software path through the peer seam is complete: TLS 1.3 authentication,
the TCP and RDMA carriers, DPU worker integration, operator-owned node records,
and the controller/agent address binding all build and pass `make test`. A
single-node rapids4 deployment is healthy at Kubernetes endpoint
`147.46.78.169:6443`; native, preload, gRPC and HTTP/1 point tests all pass.

The remaining work is ordered by the first gate that can fail:

1. **Make the rapids4 management address persistent before another reboot.**
   Kubernetes already uses `147.46.78.169`, but the installer cloud-init source
   still records the former management subnet. This is rig maintenance, not a
   DPUmesh code item, but another address rollback would stop every later gate.
2. **Choose and gain access to node B.** Public-key SSH currently fails for both
   `jet1.snu.ac.kr` and `stream10.snu.ac.kr`; the physical target must be named
   before a deploy script can safely encode it.
3. **Install or identify BlueField B and connect the fabric.** rapids4 DPU `p0`
   is still `NO-CARRIER`, has no address, and has no `10.77.0.0/24` RoCE GID.
   Cable the two DPU uplinks, assign persistent peer addresses, then require
   `ethtool`, `show_gids`, ping, bidirectional `rping` and `ib_send_bw` to pass.
4. **Provision node B and join it to Kubernetes.** Match the supported
   BFB/DOCA and container runtime, apply the node prerequisites, join through
   `147.46.78.169:6443`, label it `dpumesh.io/dpu=true`, and install the images,
   keys, identity files and host paths.
5. **Deploy one operator node file to both agents and the controller.** Generate
   one row on each node with `bench/dpumesh_controller.sh node-record`, combine
   the rows, and deploy both DPUs with `DPUMESH_PEER_TRANSPORT=rdma`, their own
   bind address, the same base port and the same `A/K` geometry.
6. **Attach receipts, in order:** handshake, bidirectional data, Pod churn,
   channel loss/recovery, remote policy, then performance. Stop at the first
   failed hardware, identity, or lifecycle gate instead of weakening it.

Do not schedule F4, F7, F8's device-plugin reduction, or the Cost items ahead
of this sequence. None of them can produce the missing cross-node claim.

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

## F6 The cross-node path: validate the transport under the seam

The cluster scope is a layer split. `doca/peer_channel.c` owns everything above
`struct dmesh_peer_transport` — handle namespaces, bounded parsing, the
node-name-to-key binding check, custody across the boundary, refusal accounting
— and `doca/dpu_proxy.c` carries the hooks that bind it to the datapath.
`tests/peer_channel_test.c` drives that layer end to end through a recording
transport. Below the seam a mutually authenticated TLS 1.3 session runs over a
byte carrier, and two carriers implement that inner seam: TCP for CI and
bring-up, RDMA for the mesh. Both halves are built; what is missing is a second
node to run them between.

- [ ] **Bring up a second DPU node.** Follow the ordered management, fabric,
  Kubernetes and receipt gates above. Until then the cross-node path is
  unexecuted. The TCP carrier runs in the host peer-wire test, while its RDMA
  arm skips without a configured local RDMA address; neither carrier has
  connected two DPU nodes or carried a remote application stream. A previous
  hardware bring-up proved only that a per-worker listener could bind and idle.
- [ ] Exercise the remote arm of every campaign that proves only the local one:
  policy verdicts at a remote destination, endpoint selection across the
  boundary, and peer-channel lifetime under Pod churn.
- [ ] Widen the cross-node pin. `px_peer_pin_admits` refuses a stream's second
  remote destination and counts it; per-request fan-out across nodes needs a pin
  per destination, and that function is the one place that decides.
- [ ] Publish node-to-node confidentiality, authentication and custody with the
  status attached: implemented, and not yet demonstrated between two nodes.

## F7 Node density

A node serves the smaller of two caps, and they bind on different hardware and
do not cost the same to move. `MAX_PODS` sits at the wire ceiling of 127 — pod
ids travel as `int8_t` with `-1` reserved, and `_Static_assert(MAX_PODS <= 127)`
holds the line — so ring supply is what binds, and K sets it at deploy time. A
`K = 2` deployment has been run at 48 Pods of one Service
(`bench/report/data/scale-20260825-172249/stages.csv`, 7 of 7): all Ready in
26 s, zero `table full` refusals, 1.41 M requests at fail=0, and a drain in
steps of eight with zero DPU error lines.

| Constant | Value | Where | What it bounds |
|---|---:|---|---|
| `MAX_DPA_RINGS` | 16 | `include/dpumesh/dmesh_common.h` | forward rings one execution unit can hold |
| execution units (N) | 32 | device query at start-up | EUs the BlueField reports |
| `DPUMESH_RINGS_PER_POD_DEFAULT` (K) | 2 | `include/dpumesh/dmesh_common.h` | EUs one Pod spans |
| `MAX_PODS` | 127 | `include/dpumesh/dmesh_common.h` | registration slots the ARM holds |
| `POD_ID_SPACE` | 128 | `include/dpumesh/dmesh_common.h` | Service-id keyed tables |

N is read from the device and `DPA_THREADS_DEFAULT = 8` is only the fallback
when that query is unavailable, so N comes from the deploy log
(`dpa_threads='32'`) and never from the constant. At the default K,
`16 × 32 / 2 = 256` ring slots exceed the 127 registration slots, so the wire
id now binds first. At the benchmark's `K = 8`, the structural ring supply is
64 Pods, but only the previously exercised 32-Pod topology is a supported
claim. Density and per-Pod throughput remain the same dial — K also caps the
ARM data workers at A ≤ K — and the harness has it turned to throughput.

**The ring array is not the measured constraint.** `MAX_DPA_RINGS` sizes five
arrays in `struct dpa_thread_arg`, one copy per EU in device memory, at 68 bytes
per ring slot over a fixed 68-byte remainder. Its expansion from 8 to 16 took
one EU's thread argument from 612 B to 1,156 B and the whole device-side cost
from 19.1 KiB to 36.1 KiB across 32 EUs.

Two other things are.

- **The per-poll scan is linear in the rings an EU holds.** `run_dma_manager`
  walks `num_rings` on every pass and reads each ring's control block out of
  host memory, so an idle slot costs nothing and occupancy costs everything: at
  `K = 2`, 127 Pods put roughly eight rings on every EU, which is eight
  control-block reads per poll instead of the two a lightly loaded node does.
  That 48-Pod run put three rings on an EU; the eight a full 127 puts there is
  still unmeasured.
- **The DPA kernel is device code with its own toolchain.**
  `doca/device/dpa_kernel.c` is compiled by `dpacc` on the BlueField, not by the
  host build. Changing `MAX_DPA_RINGS` touches no wire format — `dpa_ring_info`
  stays 48 bytes, `comch_add_ring_msg` 56, `comch_msg` 60 — but both sides must
  be recompiled against the same header and redeployed together, or the ARM
  writes `rings[12]` into an EU that allocated eight.

Ordered by cost:

- [x] `MAX_DPA_RINGS = 16` — the paired DPA/DPU rebuild and A=12 hardware
  deployment passed. It costs 17 KiB more device memory; the per-poll scan only
  grows when those ring slots are actually occupied.
- [ ] Above 127 those wire fields widen. That is a host-and-DPU ABI change of
  the kind the reverse ring's `struct dmesh_tx_ack_entry` is static-asserted
  against, because a count the two ends disagree on is silently lossy rather
  than a failure. Not worth scheduling until a deployment needs it.

Lowering `K` buys ring slots this hardware does not need, and what it costs is
per-Pod parallelism: a Pod spanning one EU has its forward traffic served by one
EU's DMA budget. Which way that goes depends on whether the node's Pods are
individually hot or collectively many. No number is published as supported until
a node has been run at it; today's supportable claims are 32 at `K = 8` and 48
at `K = 2`. Near the 127 ceiling ARM DRAM binds first: each live Pod holds
64 MB of DPU staging that is not returned below the node's high-watermark,
about 8 GB at 127.

## F8 The kernel road around the mesh

Both directions of the fail-closed claim now carry an arm, both in
`bench/suite/inject.sh`. Outbound, the shim: an injected Pod refuses a
connect it cannot route through the DPU — only a destination the DPU itself
answers as not-meshed proceeds over kernel TCP — and stage I6 drives that
refusal on hardware by killing the DPU under live meshed Pods. A warm Pod's
connect dies at the resolve deadline, a Pod born into the outage dies at
channel bring-up, both without a container restart; the unmeshed pair moves
420 K requests through the same node in the same window; and the recycled
pair serves again against the relaunched process. Two runs, twelve of twelve
I6 judgments
([`inject-dpudown-20260825-214159/`](bench/report/data/inject-dpudown-20260825-214159/),
[`inject-kernelroad-20260825-220157/`](bench/report/data/inject-kernelroad-20260825-220157/)).

Inbound, the node: flannel enforces no NetworkPolicy, so the workload agent
owns an iptables chain (`IngressGuard`, `DPUMESH-PROTECT`) that rejects a
kernel-TCP SYN to every (address, `DPUMESH_PORT`) pair the injection label
marks. It hangs off the FORWARD hook — the only hook Pod-to-Pod traffic
traverses, which is what exempts kubelet probes and the host-side harness
without an exemption rule — and is rebuilt atomically on the membership
cadence from the same Pod listing that grants and revokes membership. Stage
I7 is the arm: one unannotated Pod's bare connect refused at the mesh-served
port and connecting at the unmeshed one, with a recycled Pod's fresh address
re-covered within one interval (same receipt). The admission webhook refuses
a Pod creation it cannot patch or decide (I5).

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
argument rests on the peer channel's TLS 1.3 session, so it carries F6's status
with it until that session has run between two nodes.

---

# Defects

None are open. The last — the gRPC p99 regime change above ~12.5 K rps —
closed 2026-08-25: a five-repeat re-measurement on the post-D1 build found no
stall shape at any rate
([`grpc-tail-20260825-175740/`](bench/report/data/grpc-tail-20260825-175740/));
the Findings entry under *The mesh* carries the shape.

One item that looks like it belongs here is not on this list, because it has
its arm. Clearing
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

**The gRPC tail above capacity is queueing, not stalls.** The p99 regime change
above ~12.5 K rps — a jump to 8–15 ms under a flat p50, sporadic in one run out
of three to twelve — was measured through the synchronous benchmark server that
D1 replaced, and its campaign data never reached the tree. On the current build
the shape is absent: eight channels at 8–24 K rps, five repeats per rate, give
a p50 and p99 that rise together toward the knee, no sustained run with a p99
jump under a flat p50, and a residual tail only at the p999 level — sporadic
9–22 ms outliers below 16 K rps, about 0.1 % of requests
([`grpc-tail-20260825-175740/`](bench/report/data/grpc-tail-20260825-175740/)).

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

## gRPC evaluation gate — baseline, diagnose and optimize the broker build (2026-09-01)

Commit `36d095d` moved DOCA ownership into a per-Pod broker and changed Host
completion progress after the last retained gRPC capacity campaign. The
2026-08-25 gRPC results remain receipts for their build, but they are not the
performance baseline of the current path. The current broker build is now
baselined in
[`grpc-broker-baseline-20260901/`](bench/report/data/grpc-broker-baseline-20260901/);
the older numbers must not be substituted for it.

The evaluation keeps three costs separate:

1. **C++ adapter:** chttp2 ↔ `DmeshEndpoint` ↔ `DmeshReactor`, including
   callback, ownership, backpressure and QP lifecycle.
2. **Host transport:** `libdpumesh`, the shared rings and the per-Pod broker.
   Host CPU is the recursive Pod cgroup, so application and broker are charged
   exactly once.
3. **Mesh L7:** the DPU ARM worker's embedded Linkerd HTTP/2 path. This is where
   per-request routing and policy run, and it is the dominant term in the old
   gRPC readings.

The order is binding for this campaign:

- [x] Restate the current process, thread and byte ownership in
  [`design/GRPC.md`](design/GRPC.md), including the fact that the broker is not
  a payload hop.
- [x] Establish the unmodified release baseline: `make test-hostfree` and all
  four CTest targets (endpoint, real chttp2 channel, reactor/runtime and native
  symbol linkage) pass on `36d095d`.
- [x] Run all four targets under Clang 14 Debug ASAN+UBSAN. The sanitizer found
  a test-only lifetime violation: public gRPC Channel destruction is deferred,
  but four lifecycle fixtures supplied a stack-backed `UnownedExecutor`. Those
  fixtures now exercise the production owned executor and 4/4 pass with leak
  detection disabled because LeakSanitizer cannot run under this ptrace policy.
- [x] Run the current hardware correctness gates before retaining cost:
  gRPC-scope deploy smoke, `grpcshutdown`, the gRPC policy/routing surfaces,
  zero Pod restarts and quiescent DPU session/task/mapping counters.
- [x] Freeze placement, `N/K/A/L`, reactor count and 2.5 GHz clock, then take a
  repeated **before-change** baseline. The open-loop grid supplies
  `highest_clean_rps`; the closed-loop concurrency/payload grid describes the
  latency/throughput shape and must not be published as that capacity.
- [x] Record p50/p99/p999, achieved/offered, failures/drops, total client and
  server Pod CPU (application + broker), DPU ARM CPU and per-worker balance.
  A point with a restart, a missing result, retained-credit loss or an
  exhausted EQ budget is not clean.
- [x] Separate the three meanings of batching. The adapter was forcing
  `dmesh_flush` at every chttp2 logical Write boundary and defeating the native
  bounded coalescer. Retain the same-build removal only after release,
  ASAN+UBSAN, real-DPU lifecycle/policy gates and repeated payload A/B pass.
- [x] Widen Host placement from 6+6 to 9+9 CPUs and measure process/cgroup CPU,
  rather than inferring saturation from affinity. The unchanged binary gained
  only 3.2--4.0% at 64 B/1 KiB while the server Pod consumed 1.35--1.86 cores;
  Host core count was not the first limit.
- [x] Instrument DPU drain state and SG-DMA grouping, sample quiescent and loaded
  workers, and profile the loaded call tree. Workers fall to 0--1% with no
  sessions and the loaded profile is flat (no symbol above 3.5% self). That
  rules out a traffic-independent spin, not a worker that stays runnable while
  a request is open; the in-flight CPU finding below is the open question.
- [x] Restore the allocator used by the stock Linkerd executable. The embedded
  staticlib was the final Rust artifact in a C executable and therefore never
  inherited Linkerd's `#[global_allocator]`; it used glibc despite the upstream
  proxy selecting jemalloc. A same-binary `LD_PRELOAD` experiment first proved
  the cause, then a Rust-global jemalloc build retained a 36.7% closed-loop
  improvement without changing C allocation semantics.
- [x] Match the stock proxy's release profile by enabling LTO in the embedded
  workspace. A same-allocator repeated A/B adds 8.4% at 64 B and 15.2% at
  1 KiB. The final low-overhead profile sustains 49,999 request/s with zero
  errors or drops while collecting 3,009 samples with none lost.
- [ ] Use O1 only on the connection-churn arm: it attributes the remaining
  0.4–0.5 ms session cost but cannot explain steady requests on an already-live
  HTTP/2 channel.
- [ ] Use O2 as a same-build A/B after the baseline. The measured
  reservation-versus-copy improvement was only 0.265 ARM µs/request; that
  proves the first copy lever is small beside the old 400–500 ARM µs/request
  gRPC total, not that removing the remaining queue is worthless. Accept the
  larger change only with copy-byte/arena evidence and no p99 or correctness
  regression.
- [x] Treat O3 as a separate topology campaign. Fixed ownership tables and
  scratch layout were expanded together, and A=4/6/8/12 with one persistent
  channel per worker measured 40k/70k/80k/130k clean RPC/s. A=16 still needs a
  distinct main/control CPU before it can be supported.
- [ ] Leave O6 out of this campaign: topology delta publication is control-plane
  churn work and cannot change steady gRPC request cost. O5 is the later fair
  ARM-versus-x86 full-stack comparison, not a prerequisite for establishing the
  current DPUmesh number.

This campaign is an explicit single-node evaluation and does not close F6 or
change the product priority of obtaining the two-node receipt.

The batching/Host diagnosis is retained in
[`grpc-batching-20260901/`](bench/report/data/grpc-batching-20260901/FINAL.md),
and the subsequent L7 profile, allocator/LTO A/B and final capacity receipt are
in
[`grpc-l7-perf-20260901/`](bench/report/data/grpc-l7-perf-20260901/FINAL.md).
The earlier native-coalescing build's 64-byte open-loop bracket was 45k clean /
48k bad. The first repetition after jemalloc and LTO reached 64 B 100k clean,
1 KiB 75k clean and 8 KiB 25k clean. A later predeclared fine-knee repetition
changes the operating claim: 64 B is cross-deployment-confirmed through 90k
with a mixed 92k point and independent bad 98/99k points; 1 KiB is 75k clean /
75.25k mixed; 8 KiB is 29.75k clean / 30k mixed. Near-knee p99 is deliberately
not called healthy: it reaches 318 ms at the accepted 29.75k 8 KiB point. At
total concurrency 1,024, independent closed-loop
medians are 106.8k, 89.1k and 34.3k request/s. Closed-loop plateaus must not be
relabelled as open-loop capacities. The full professor-facing receipt, including
the standard-Linkerd-sidecar comparison, is in
[`grpc-professor-20260902/`](bench/report/data/grpc-professor-20260902/ANALYSIS.md)
(`FINAL.md` beside it is the short professor-facing summary).

A follow-up concurrency sweep covers total concurrency 8 through 8,192, three
repetitions each. The 64 B median rises 12.5k→118.9k request/s (9.5x) through
concurrency 2,048, then falls to 115.8k/102.1k at 4,096/8,192. The 1 KiB median
rises 12.2k→88.7k (7.3x) through 1,024, then falls to 85.8k/81.0k/77.4k.
Every retained run has zero failure/drop/restart. At the apparent 8.02-process-
core points, the new split counter records 7.98--7.99 data-worker cores and
0.03--0.04 non-worker cores: the A=8 geometry limits that arm's data workers,
not the whole process cpuset. This fixes the 64 B plateau at total concurrency 2,048 and
the 1 KiB plateau at 1,024 while retaining the large rising region. The 8 KiB
high-concurrency sequence produced RPC failures and backend-channel reuse
warnings after overload cancellation; it is retained as rejected stress data,
not averaged into the plateau. The harness must gate the next run on DPU
session/task/backend quiescence before that arm is repeated.

The original 64 B open-loop campaign has clean 92.5k/97.5k/100k points and a
102.5k first bad point; isolated 110k/115k observations then plateau and decline.
The independent knee repetition does not reproduce that upper envelope: 90k is
3/3 clean, 92k is mixed after worker 5 stalls and drops 73,587 schedules, and
fresh 98/99k observations are bad. These are displayed as individual hollow
points instead of being averaged into one monotonic line. This is now a
deployment/worker-progress stability issue, not merely a wider confidence
interval around 100k.

At equal total concurrency 256, removing the forced physical flush changes the
three-run medians from 29.3k→58.2k (64 B), 27.9k→51.0k (1 KiB), and
19.9k→22.3k (8 KiB). The remaining shape fits approximately
`100.9 ARM us/request + 31.4 ns/frame-byte` on that pre-allocator build: fixed
Linkerd/HTTP/2 per-request work still dominates small messages. DPU SG-DMA
multi-unit tasks are rare, but 64-byte Linkerd output already carries 4.36 RPCs
per DMA batch, so delaying the SG engine merely to increase its unit count is
not justified by the profile. The 64/1 KiB/8 KiB open-loop knees remain clearly
payload-dependent, while the dense 8 KiB curve now shows tail latency rising
smoothly through 29.75k before the mixed 30k boundary. This still shows that
fixed work dominates small frames. O2 remains the open direct-reservation lever
for the residual copy and lock cost; O3 is the distinct capacity-scaling lever
because the A=8 workers, rather than Host CPU, define that arm's knee.

- [ ] Explain the observed deployment/worker-progress regression before calling
  any single campaign's upper envelope an operating limit. An 11-hour 64-byte
  probe regressed, and fresh deployments also produced a worker-5 stall at 92k
  plus bad 98/99k observations, so age alone no longer explains the variance.
  The historical 80k aged point fell to ratio 0.9878 and 904 ms p99, while one
  full deploy restored its three-run median to ratio 1.0000 and 4.973 ms p99.
  Run repeated fresh-deploy probes as well as 6/12/24-hour soaks and correlate
  worker progress, session generations, allocator state, queue depth and DPU
  counters. Do not publish a single-run maximum as a reproducible capacity.
- [ ] Cut the per-event fixed cost of the ARM worker loop (E5). Measured on
  one worker with one channel: a 64 B RPC costs 467k instructions, 4.9k cache
  misses and 747 ARM µs at 100 RPS against 173k / 1.7k / 154 µs at the knee;
  it is handled in ~8 drain passes (half of them Idle), 47 syscalls
  (29 `epoll_pwait`) and 1.2 wakes, as two continuous on-CPU runs of ~450 and
  ~320 µs. Not a spin, not DVFS, not session rebuilding, not the Host
  coalescer (E2: `TX_TAIL_DELAY_NS` 50 µs left the 100 RPS p50 at 1.6 ms and
  worsened 10k RPS from 611 to 1,456 µs; reverted). The levers are the pass
  count (two drains around arming plus re-registering five `select` futures
  per pass), the per-pass work (every session and both H2 connections are
  polled on every pass) and the syscalls per pass. Target: 100 RPS ≤ 200 µs/RPC,
  closed-loop single-request p50 < 0.7 ms (now 0.94 ms), no knee regression.
  Receipts: [`grpc-professor-20260902/lowload/`](bench/report/data/grpc-professor-20260902/lowload/).
  First build (Rust drain before the C engine drain; wake-eventfd read only
  when a tick was posted) is in the tree: worker CPU −2–6% at ≤1k RPS/worker,
  syscalls/RPC 47→32, latency unchanged, 90k clean, `grpcshutdown` passed.
  Uprobes then showed the ceiling of loop work: per RPC the hyper server
  connection is polled 2.0× and the h2 client connection 4.0×, and those six
  cold polls are ~64% of the cost. The next levers are the h2 client poll count
  (4→2), a bounded post-event spin to avoid the wake chain and cold re-entry,
  and the depth of the linkerd service stack, each under the same A/B.
- [ ] Repeat the sidecar comparison with matched cores (E3):
  `config.linkerd.io/proxy-cpu-limit: "4"` on both Pods, plus a direct-TCP 10k
  point, and report per-proxy-core figures beside the absolute ones. The
  current 4.3× is against `LINKERD2_PROXY_CORES=1`; per configured core the two
  meshes are level at 64 B and Linkerd leads at 8 KiB.

## What is known

Per-session cost, not the transport, bounds L7 capacity, and per-workload stack
sharing removed most of it: one session costs 3.9 ms of ARM time unshared and
0.4–0.5 ms shared, the closed loop completes 4.4× more sessions under heavy
churn, and the 30–40 ms p99 spikes of per-session stack building disappear. The
steady per-request point is unchanged, which is the expected result: the data
path does not know the stacks are shared. The receipt is
[`bench/report/REPORT.md`](bench/report/REPORT.md), *What a session costs*.

Per-request backend selection is priced, and at a matched rate it costs the
data path nothing measurable: 8,000/s gRPC reads 481–487 ARM µs/request with
one backend and 489–496 alternating 50/50 across two Services — inside the
rig's several-percent spread — with the split attributed at exactly 50.0/50.0,
and the single-backend opaque prices reproduce the `1518aae` receipts. The
closed-loop alternating arm completes 9% less at +10% µs/request, which is the
route hop's latency turned into throughput by a closed loop, not a per-request
cost. No pre-selection receipt exists on these arms — `px_conn_admitted` and
`l7_conn_segment` are already in `1518aae` — so the price is bounded by
reproduction and by the same-build A/B, not by an ablation
([`api-l7-selcost-20260825-222604/`](bench/report/data/api-l7-selcost-20260825-222604/)).

The backend registry lock never contends. A temporary counter pair in
`Backends::registry()` — removed after the reading — counted 9,280
acquisitions and zero contended takes across ~550 K requests and 3,081 session
builds: three per session (publish, take, remove), none per request, on both
session churn and the alternating-backend load. Endpoint locking is not
material, and the Tokio `LocalSet` + `Rc<RefCell>` specialization that was
conditioned on it stays unwritten
([`backend-lock-20260825/`](bench/report/data/backend-lock-20260825/)).

The synchronous half of a stack build is instrumented in
`linkerd/app/src/lib.rs` and reported by `SessionMetrics::observe_stack_build`:
8.7 µs to clone the outbound template and set `dmesh_session`, 107.7 µs of
layers (`build_policies` + `outbound.mk`), 32.0 µs of `NewService::new_service`,
**148.4 µs** in total. The remainder is lazy discovery and policy work, task
execution and teardown, and the surrounding DPUmesh lifecycle. **Locating it
requires instrumenting those asynchronous boundaries; do not assume the whole
figure sits inside the synchronous call.**

## O1 Attribute what is left of a session

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

## O3 Use more than eight ARM data cores

The fixed arrays now admit `MAX_ARM_WORKERS=16` and `MAX_DPA_RINGS=16`.
Reverse landing stripes are not an independent configuration: they are derived
directly from A. The hot-service deploy harness exposes
`DPUMESH_THROUGHPUT_WORKERS`, deriving A=K=W, the largest valid N≤32 and
all-worker L7. The scale runner derives `threads=channels=workers` from that
same value so session fan-in is held constant. On the expanded binary,
A=4/6/8/12 reached 40k/70k/80k/130k 3/3 clean; at 130k the Host used only
5.05/9 and 5.93/9 cores while DPU workers used 11.37/12. This is a measured
capacity lever. Independent N/K/A remain for the distinct density deployment
where K>A is intentional. Policy/injection fixtures share a deployment-geometry
resolver: they consume canonical W or parse effective K/A from the live DPU,
instead of retaining K=A=8 and inspecting only eight admin endpoints.

- [x] Expand `K/A/L` geometry together across the public transport, broker
  descriptor handoff, DPA ring tables, DPU completion PEs, reverse lanes,
  scratch layout and peer-worker port ranges. A=12 deploy, smoke and traffic
  passed; topology tests cover 12 and 16.
- [ ] Reserve a distinct CPU for the DPU control/main thread. With all 16 CPUs
  allowed, `A=16` would wrap the current `main=A` affinity back onto worker 0;
  a larger worker geometry must not buy throughput by starving control progress.
- [x] Compare 4/6/8/12-worker open-loop knees with the same 9+9 Host placement.
  Their highest repeated clean points consume 3.97/4, 5.95/6, 7.79/8 and
  11.37/12 worker cores. The accepted A=12 point has balanced 97.4–98.9%
  worker use, clean ownership/loss counters and zero RPC failure through 130k.
  A=16 remains outside the supported range until the preceding control-CPU
  item is closed.

## O6 Incremental topology generations

A generation republishes whole: one Pod's churn re-signs the entire document and
every DPU re-fetches, re-verifies and re-parses all of it — O(fleet) work for an
O(1) change. Storage is not the pressure (~200 bytes per Pod against DPU DRAM,
16 MiB publication bound in `TOPOLOGY_MAX_BYTES`); the republish amplification
is, and it grows as cluster size times churn rate. Past the point where even
deltas cannot keep up, the migration is forwarded assertions; this item is the
step before that.

- [ ] Publish a delta generation — records added and removed against a named
  base version — signed with the same Ed25519 key under the same strictly
  increasing version line. A consumer holding the base applies it; one that
  does not, or that fails any check, falls back to fetching the full
  generation. The security property is unchanged: nothing unsigned is adopted,
  and a refused or missing delta leaves the last adopted generation standing.
- [ ] Keep the periodic full generation as anchor and recovery path, so a delta
  chain never becomes required state.
- [ ] The wire grammar and consumer bounds are host+DPU ABI (`doca/topology.c`
  and the feed hop), so both ends change together, with the ABI-Impact note.
- [ ] Accept on measurement: bytes moved and DPU parse time per churn event,
  before and after, at a cluster size where the full republish is the dominant
  term. Below that size this stays unscheduled — the single-digit-node rig
  cannot motivate it.

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
