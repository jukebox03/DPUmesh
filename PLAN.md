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

Two items are open: two-node validation of the cross-node seam, and a Go
surface. F7 keeps one density lever, unscheduled.

## F6 The cross-node path — close the two-node receipt first

The cluster scope is a layer split. `doca/peer_channel.c` owns everything above
`struct dmesh_peer_transport` — handle namespaces, bounded parsing, the
node-name-to-key binding check, custody across the boundary, refusal accounting
— and `doca/dpu_proxy.c` carries the hooks that bind it to the datapath.
`tests/peer_channel_test.c` drives that layer end to end through a recording
transport. Below the seam a mutually authenticated TLS 1.3 session runs over a
byte carrier, and two carriers implement that inner seam: TCP for CI and
bring-up, RDMA for the mesh. Operator-owned node records and the
controller/agent address binding are in. All of it builds and passes
`make test`; the TCP carrier runs in the host peer-wire test and the RDMA arm
skips without a configured local RDMA address. Neither carrier has connected
two DPU nodes or carried a remote application stream — a previous hardware
bring-up proved only that a per-worker listener could bind and idle.

What is missing is the second node, ordered by the first gate that can fail:

1. **Make the rapids4 management address persistent before another reboot.**
   Kubernetes already uses `147.46.78.169`; the installer cloud-init source
   still records the former subnet. Rig maintenance, but a rollback stops
   every later gate.
2. **Choose and gain access to node B.** Public-key SSH fails for both
   `jet1.snu.ac.kr` and `stream10.snu.ac.kr`.
3. **Install or identify BlueField B and connect the fabric.** rapids4 DPU `p0`
   is `NO-CARRIER` with no address and no `10.77.0.0/24` RoCE GID. Cable the
   two uplinks, assign persistent peer addresses, then require `ethtool`,
   `show_gids`, ping, bidirectional `rping` and `ib_send_bw` to pass.
4. **Provision node B and join it.** Matching BFB/DOCA and container runtime,
   the node prerequisites, join through `147.46.78.169:6443`, the
   `dpumesh.io/dpu=true` label, images, keys, identity files and host paths.
5. **Deploy one operator node file to both agents and the controller.** One
   row per node from `bench/dpumesh_controller.sh node-record`, combined, and
   both DPUs started with `DPUMESH_PEER_TRANSPORT=rdma`, their own bind
   address, the same base port and the same `A/K` geometry.
6. **Attach receipts, in order:** handshake, bidirectional data, Pod churn,
   channel loss/recovery, remote policy, then performance. Stop at the first
   failed hardware, identity or lifecycle gate instead of weakening it.

- [ ] Bring up the second DPU node through those gates.
- [ ] Exercise the remote arm of every campaign that proves only the local one:
  policy verdicts at a remote destination, endpoint selection across the
  boundary, and peer-channel lifetime under Pod churn.
- [ ] Prove the encryption contract between two DPUs: a payload marker is not
  observable in plaintext under the carrier; on the TCP carrier a ciphertext
  bit-flip, truncation or replay ends as a channel fault with no byte delivered
  to the destination application; a topology key mismatch, an unset peer
  transport and a failed handshake are each a remote refusal, never a plaintext
  fallback; a node-key rotation resets the live channel and resumes only under
  the new key. Fix the traffic-secret refresh policy of the long-lived pair
  channel here (TLS KeyUpdate or a bounded reconnect) and test its boundary.
- [ ] Widen the cross-node pin. `px_peer_pin_admits` refuses a stream's second
  remote destination and counts it; per-request fan-out across nodes needs a pin
  per destination, and that function is the one place that decides.
- [ ] Publish node-to-node confidentiality, authentication and custody with the
  status attached: implemented, and not yet demonstrated between two nodes.

Per-hop encryption is a boundary, not a gap. A DMA-session endpoint answers
`ConditionalClientTls::None(Disabled)` in both the TCP and HTTP stacks, and
discovery offers no identity for it, because the backend is deliberately
advertised as unmeshed: terminating TLS at the destination would need the
destination DPU to run the second byte-stream proxy this design removes. The
node-local hop is plaintext held inside registered DMA mappings the workload
cannot address; the node-to-node half rests on the peer channel's TLS 1.3
session and carries this item's status with it.

F4, F7 and every *Cost* item stay behind this sequence: none of them produces
the missing cross-node claim. The 2026-09-01 gRPC campaign is a single-node
evaluation and does not close it.

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

## F7 Node density

A node serves the smaller of two caps. `MAX_PODS` sits at the wire ceiling of
127 — pod ids travel as `int8_t` with `-1` reserved, and
`_Static_assert(MAX_PODS <= 127)` holds the line — so ring supply is what
binds, and K sets it at deploy time.

| Constant | Value | Where | What it bounds |
|---|---:|---|---|
| `MAX_DPA_RINGS` | 16 | `include/dpumesh/dmesh_common.h` | forward rings one execution unit can hold |
| execution units (N) | 32 | device query at start-up | EUs the BlueField reports |
| `DPUMESH_RINGS_PER_POD_DEFAULT` (K) | 2 | `include/dpumesh/dmesh_common.h` | EUs one Pod spans |
| `MAX_PODS` | 127 | `include/dpumesh/dmesh_common.h` | registration slots the ARM holds |
| `POD_ID_SPACE` | 128 | `include/dpumesh/dmesh_common.h` | Service-id keyed tables |

N is read from the device — `DPA_THREADS_DEFAULT = 8` is only the fallback when
the query is unavailable — so it comes from the deploy log (`dpa_threads='32'`)
and never from the constant. At the default K, `16 × 32 / 2 = 256` ring slots
exceed the 127 registration slots, so the wire id binds first. Supported claims
are the ones a node has been run at: 32 Pods at the benchmark's `K = 8`, and 48
Pods of one Service at `K = 2`
([`scale-20260825-172249/stages.csv`](bench/report/data/scale-20260825-172249/stages.csv),
7 of 7: all Ready in 26 s, zero `table full` refusals, 1.41 M requests at
fail=0, a drain in steps of eight with zero DPU error lines). Density and
per-Pod throughput are the same dial — K also caps the ARM data workers at
A ≤ K — and the harness has it turned to throughput.

Two things bind before the ring array does. The per-poll scan is linear in the
rings an EU holds: `run_dma_manager` walks `num_rings` on every pass and reads
each ring's control block out of host memory, so at `K = 2` a full 127 puts
about eight control-block reads on every EU per poll where the 48-Pod run put
three — still unmeasured. And the DPA kernel is device code with its own
toolchain: `doca/device/dpa_kernel.c` is compiled by `dpacc` on the BlueField,
so a `MAX_DPA_RINGS` change touches no wire format (`dpa_ring_info` stays 48
bytes, `comch_add_ring_msg` 56, `comch_msg` 60) but both sides must be rebuilt
against the same header and redeployed together, or the ARM writes `rings[12]`
into an EU that allocated eight. Near the ceiling ARM DRAM binds first: each
live Pod holds 64 MB of DPU staging that is not returned below the node's
high-watermark, about 8 GB at 127.

- [ ] Above 127 the wire fields widen. That is a host-and-DPU ABI change of the
  kind the reverse ring's `struct dmesh_tx_ack_entry` is static-asserted
  against, because a count the two ends disagree on is silently lossy rather
  than a failure. Not worth scheduling until a deployment needs it.

---

# Defects

Two are open.

## D3 The broker's failure paths are built and unwatched

The per-Pod broker (`36d095d`) carries the lifecycle
[`design/CONTROL.md`](design/CONTROL.md) §2-1.9 describes. A dead broker takes
its registered mappings with it, so the workload's recovery unit is the
process: the library's control thread raises SIGTERM on `TRANSPORT_DOWN`
(`src/core/dmesh_core.c`) and Kubernetes restarts the container into a fresh
HELLO. The agent serializes HELLOs per Pod UID, backs a failed launch off,
records each broker in a root-private state file and re-adopts recorded
brokers when it restarts. The DPU disconnects a Comch slot that has not
registered within 30 s (`doca/comch_server.c`, `registration-timeout`). The
broker's confinement — private mount, cgroup and network namespaces, the
Pod-slice child cgroup, `pivot_root` into an empty tmpfs, uid drop, empty
capability set, `no_new_privs` and a seccomp filter — is in
`src/core/dmesh_core.c` behind the broker's registration.

None of it has an arm in `bench/suite/` or a receipt in `bench/report/data/`.
The only observation is incidental: after a broker is killed, the first
re-HELLO lands in a 5 s backoff because the agent sweeps dead brokers lazily —
a restart loop absorbs it, a one-shot client must retry. The DPU's side of a
dead mapping owner is recorded on the pre-broker build — a SIGKILL under c32
load left two `px_poison` entries, no wedge, and re-registration within
seconds ([`broker-design-20260828/SUMMARY.md`](bench/report/data/broker-design-20260828/SUMMARY.md)
§A.3) — but that was the workload dying, not a broker.

- [ ] Add the arm, under load (`point dpumesh 1024 8 32 60 5 1` in the
  background), with the judgment `inject.sh` uses: broker SIGKILL — the Pod
  restarts and re-registers, a later point at fail=0, an unrelated pair
  untouched; workload SIGKILL — the broker exits and the DPU reaches
  quiescence; node-agent rollout — running traffic untouched, broker PID and
  start time unchanged, re-adoption logged, new registrations resume; DPU
  restart — every workload restarts through `TRANSPORT_DOWN`, no duplicate
  broker per Pod UID.
- [ ] Verify the confinement from outside on a sacrificial Pod: `/proc/1` is
  the broker itself, no host PID visible, a network namespace different from
  the agent's, no host cgroup RW mount in `mountinfo`, a dedicated uid/gid,
  `CapEff` 0, exec refused. Drive a CPU throttle and an OOM and show each is
  charged to the served Pod alone.
- [ ] Hold an unauthenticated Comch client past 30 s on hardware: the timeout
  counter, the disconnect callback and the slot's reuse.

## D4 Worker progress regresses with deployment age, and sporadically on fresh deploys

An 11-hour 64-byte probe regressed, and fresh deployments also produced a
worker-5 stall at 92k that dropped 73,587 schedules plus bad 98/99k
observations, so age alone does not explain the variance. The historical 80k
aged point fell to ratio 0.9878 and 904 ms p99, while one full deploy restored
its three-run median to ratio 1.0000 and 4.973 ms p99
([`grpc-professor-20260902/`](bench/report/data/grpc-professor-20260902/ANALYSIS.md)).

- [ ] Run repeated fresh-deploy probes as well as 6/12/24-hour soaks and
  correlate worker progress, session generations, allocator state, queue depth
  and DPU counters. Until it is explained, no single campaign's upper envelope
  is an operating limit and no single-run maximum is a reproducible capacity.

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
`unpublishable_internal_work_does_not_spin` is the regression test.

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

**Ejecting a failing endpoint under traffic leaves the healthy one serving.**
`S13`/`S14` eject a failing endpoint and withdraw its Deployment under traffic
while the healthy endpoint keeps serving — the `dma_ready` collateral arm — and
the current build passes both
([`policy-route-20260902-212705/stages.csv`](bench/report/data/policy-route-20260902-212705/stages.csv)).

**A fatal signal on this DPU leaves nothing behind.** Nothing under `doca/`
calls `exit()`, `_exit()` or `abort()` (the workload library's `_exit(75)` in
`src/core/dmesh_core.c` follows its own SIGTERM on `TRANSPORT_DOWN`), and no
Rust in the adapter calls `process::exit`, so a DPU process that is gone was
ended from outside. SIGPIPE's
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

**The kernel road around the mesh is closed in both directions.** Outbound, the
shim refuses a connect it cannot route through the DPU; only a destination the
DPU itself answers as not-meshed proceeds over kernel TCP. Stage I6 of
`bench/suite/inject.sh` kills the DPU under live meshed Pods: a warm Pod's
connect dies at the resolve deadline, a Pod born into the outage dies at channel
bring-up, both without a container restart; the unmeshed pair moves 420 K
requests through the same node in the same window; and the recycled pair serves
again against the relaunched process. Inbound, flannel enforces no
NetworkPolicy, so the workload agent owns an iptables chain (`IngressGuard`,
`DPUMESH-PROTECT`) that rejects a kernel-TCP SYN to every (address,
`DPUMESH_PORT`) pair the injection label marks. It hangs off the FORWARD hook —
the only hook Pod-to-Pod traffic traverses, which is what exempts kubelet probes
and the host-side harness without an exemption rule — and is rebuilt atomically
on the membership cadence from the same Pod listing that grants and revokes
membership. Stage I7 is the arm: one unannotated Pod refused at the mesh-served
port and connecting at the unmeshed one, with a recycled Pod's fresh address
re-covered within one interval. The admission webhook refuses a Pod creation it
cannot patch or decide (I5). Two runs, twelve of twelve judgments
([`inject-dpudown-20260825-214159/`](bench/report/data/inject-dpudown-20260825-214159/),
[`inject-kernelroad-20260825-220157/`](bench/report/data/inject-kernelroad-20260825-220157/)).

**The broker is a relay, and its wake count is the only lever.** A broker wake
costs ~7 µs whatever it does — the profile shows PSI accounting, scheduler
enqueue, epoll and syscall entry and no DOCA symbol, and removing the
reverse-entry peek changed nothing. So the broker forwards one pod-global
eventfd tick per `REV_DOORBELL` batch and the workload's drain thread owns ring
interpretation, EQ selection and `arm_epoch`, publishing it only before it
sleeps. While completions keep arriving it re-checks the rings on an
exponential backoff (`DPUMESH_DRAIN_NAP_US`, min 10 µs, doubling per empty
check up to `DPUMESH_DRAIN_NAP_CAP_US` 100 µs, then arm and block), so the DPU
sends no doorbell and the broker sleeps through the busy period; polling turns
itself off when live EQs outnumber the affinity's CPUs. Against the in-process
PE it replaced: conc32 host 2.47–2.55 µs/request at p50 259–261 µs and
102–104 K rps versus 2.82 µs at 296 µs and 97 K, conc1 p50 136 µs, ARM −26 %
because the polled regime sends no doorbell Comch messages
([`doorbell-relay-20260901/SUMMARY.md`](bench/report/data/doorbell-relay-20260901/SUMMARY.md)).
The per-Pod choice rests on a shared relay's burst serialization — p50 wake
delay +12/+38/+57 µs at 8/32/64 simultaneous wakes through one shared relay
against a flat 13–15 µs per Pod — and the memfd handoff is neutral over 13
ABA runs ([`broker-design-20260828/SUMMARY.md`](bench/report/data/broker-design-20260828/SUMMARY.md)
§A.5, §A.2). Two traps: `DMESH_IPC_VERSION` and the agent's `BROKER_IPC_VERSION` move
together or the agent refuses every HELLO and no broker starts; and the
broker's argv is assembled by the agent's `systemd-run` command, so a flag
changes in both places at once.

**The ARM worker's per-request fixed cost is loop passes, not a hot function.**
Two E5 builds are in the tree: draining the Rust side before the C engine and
reading the wake eventfd only when a tick was posted took syscalls/RPC 47→32
for worker CPU −2–6 % at ≤1k RPS/worker; two edits in the vendored `h2`
0.4.15 — a `pending_open` stream given no send capacity, so HEADERS and DATA
left in two polls, and a closed stream's last handle waking the connection for
nothing — took h2 client connection polls/RPC 4.0→2.0 for −2–6 % below 1k
RPS/worker and −5.8 % closed-loop, nothing at the knee, 90k 3/3 clean
([`e5-h2-20260902/`](bench/report/data/e5-h2-20260902/SUMMARY.md)).

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

## The gRPC path on the broker build

Commit `36d095d` moved DOCA ownership into the per-Pod broker and changed Host
completion progress, so the 2026-08-25 gRPC results are receipts for their
build and not the baseline of the current path. The baseline is
[`grpc-broker-baseline-20260901/`](bench/report/data/grpc-broker-baseline-20260901/);
the batching and Host diagnosis is
[`grpc-batching-20260901/`](bench/report/data/grpc-batching-20260901/FINAL.md);
the L7 profile, allocator and LTO A/B and the capacity receipt are
[`grpc-l7-perf-20260901/`](bench/report/data/grpc-l7-perf-20260901/FINAL.md);
the independent professor-facing repetition, with the sidecar comparison, is
[`grpc-professor-20260902/`](bench/report/data/grpc-professor-20260902/ANALYSIS.md)
(`FINAL.md` beside it is the short summary). The evaluation keeps three costs
separate and every reading names which one it is:

1. **C++ adapter:** chttp2 ↔ `DmeshEndpoint` ↔ `DmeshReactor`, including
   callback, ownership, backpressure and QP lifecycle.
2. **Host transport:** `libdpumesh`, the shared rings and the per-Pod broker.
   Host CPU is the recursive Pod cgroup, so application and broker are charged
   exactly once.
3. **Mesh L7:** the DPU ARM worker's embedded Linkerd HTTP/2 path, where
   per-request routing and policy run. It is the dominant term.

What the campaign established, in the order it was found:

- **The adapter was defeating the transport's coalescer.** It forced
  `dmesh_flush` at every chttp2 logical write boundary. Removing it, at equal
  total concurrency 256, moved three-run medians from 29.3k→58.2k (64 B),
  27.9k→51.0k (1 KiB) and 19.9k→22.3k (8 KiB) request/s.
- **The embedded proxy ran on glibc.** The staticlib is the final Rust artifact
  in a C executable and never inherited Linkerd's `#[global_allocator]`; a
  Rust-global jemalloc build retained +36.7 % closed-loop, and LTO on the
  embedded workspace a further +8.4 % at 64 B and +15.2 % at 1 KiB.
- **Host core count is not the first limit.** Widening placement from 6+6 to
  9+9 CPUs gained 3.2–4.0 % while the server Pod used 1.35–1.86 cores. The knee
  is the ARM data workers: at the apparent 8.02-process-core points the split
  counter reads 7.98–7.99 data-worker cores and 0.03–0.04 non-worker, so the
  `A=8` geometry limits that arm, not the process cpuset. O3 is the lever.
- **Fixed per-request work dominates small frames.** The pre-allocator build
  fits ≈ `100.9 ARM µs/request + 31.4 ns/frame-byte`. SG-DMA multi-unit tasks
  are rare, but 64-byte Linkerd output already carries 4.36 RPCs per DMA batch,
  so delaying the SG engine to raise its unit count is not justified. Workers
  fall to 0–1 % with no sessions and the loaded profile is flat (no symbol
  above 3.5 % self); what remains is the per-event fixed cost E5 targets.
- **Capacity carries two definitions.** Open loop (`highest_clean_rps`): 64 B
  is confirmed through 90k across deployments, with a mixed 92k point and bad
  98/99k points; 1 KiB 75k clean / 75.25k mixed; 8 KiB 29.75k clean / 30k
  mixed, and near-knee p99 is not called healthy — 318 ms at the accepted 8 KiB
  point. Closed loop at total concurrency 1,024: medians 106.8k, 89.1k and
  34.3k request/s, which must not be relabelled as open-loop capacity. The
  concurrency sweep 8 → 8,192 plateaus 64 B at 2,048 (118.9k) and 1 KiB at
  1,024 (88.7k) with zero failure, drop or restart on every retained run; the
  8 KiB high-concurrency sequence produced RPC failures and backend-channel
  reuse warnings after overload cancellation and is retained as rejected
  stress data, not averaged into the plateau.
- **Against a per-Pod Linkerd sidecar** (`edge-26.8.1`, `LINKERD2_PROXY_CORES=1`):
  max RPS 4.3× / 4.6× / 2.2× at 64 B / 1 KiB / 8 KiB with zero failures on
  both arms; at 10k rps p50 is +0.2 ms at 64 B and 1 KiB with p99 below
  Linkerd's, and 8 KiB p50 is 3× (1.55 vs 0.51 ms) at p99 +10 %.

O1 belongs to the connection-churn arm: it attributes the remaining 0.4–0.5 ms
session cost and cannot explain steady requests on a live HTTP/2 channel. O2 is
a same-build A/B against this baseline. O6 is control-plane churn work and
cannot change steady request cost; O5 is the later fair ARM-versus-x86
comparison, not a prerequisite for the current number.

- [ ] **E5 Cut the per-event fixed cost of the ARM worker loop.** Measured on
  one worker with one channel: a 64 B RPC costs 467k instructions, 4.9k cache
  misses and 747 ARM µs at 100 RPS against 173k / 1.7k / 154 µs at the knee;
  it is handled in ~8 drain passes (half of them Idle), 47 syscalls
  (29 `epoll_pwait`) and 1.2 wakes, as two continuous on-CPU runs of ~450 and
  ~320 µs. Not a spin, not DVFS, not session rebuilding, not the Host
  coalescer (`TX_TAIL_DELAY_NS` 50 µs left the 100 RPS p50 at 1.6 ms and
  worsened 10k RPS from 611 to 1,456 µs; reverted). The levers are the pass
  count (two drains around arming plus re-registering five `select` futures
  per pass), the per-pass work (every session and both H2 connections are
  polled on every pass) and the syscalls per pass. Target: 100 RPS ≤ 200 µs/RPC,
  closed-loop single-request p50 < 0.7 ms (now 0.94 ms), no knee regression.
  Receipts: [`grpc-professor-20260902/lowload/`](bench/report/data/grpc-professor-20260902/lowload/).
  Done so far and folded into *Findings*: the Rust-drain-first pass order and
  tick-gated wake read, and the vendored `h2` 0.4.15 patch
  (`linkerd/port/linkerd2-proxy/h2/`, `[patch.crates-io]` in
  `linkerd/rust/Cargo.toml`) that halves h2 connection polls per request
  ([`e5-h2-20260902/`](bench/report/data/e5-h2-20260902/SUMMARY.md)). What
  remains per request is the stack itself (≈ 350 µs probe-inflated of
  ≈ 1.2 ms) and the ~21 runtime passes — a session walk plus
  `doca_pe_progress` each — that seven consecutive `Progressed` results after
  every publication produce. The next levers are those passes and the depth of
  the linkerd service stack, each under the same A/B.
- [ ] Gate the next high-concurrency arm on DPU quiescence. `grpc_closed_sweep.sh`
  does not yet check session, task and backend-channel counters between
  points, so a run started on top of the residue an overload cancellation
  leaves measures that residue; add the check before the 8 KiB sequence at
  total concurrency ≥ 4,096 is repeated.
- [ ] **E3 Repeat the sidecar comparison with matched cores:**
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
rig's several-percent spread — with the split attributed at exactly 50.0/50.0.
The closed-loop alternating arm completes 9 % less at +10 % µs/request, which
is the route hop's latency turned into throughput by a closed loop, not a
per-request cost
([`api-l7-selcost-20260825-222604/`](bench/report/data/api-l7-selcost-20260825-222604/)).

The backend registry lock never contends: 9,280 acquisitions and zero
contended takes across ~550 K requests and 3,081 session builds — three per
session (publish, take, remove), none per request. Endpoint locking is not
material, and the Tokio `LocalSet` + `Rc<RefCell>` specialization that was
conditioned on it stays unwritten
([`backend-lock-20260825/`](bench/report/data/backend-lock-20260825/)).

The synchronous half of a stack build is instrumented in the proxy fork
(`linkerd/port/linkerd2-proxy/linkerd/app/src/lib.rs`) and reported by
`SessionMetrics::observe_stack_build`:
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
  `two_same_service_sessions_take_their_own_channels` in the fork's
  `linkerd/app/outbound/src/tcp/connect.rs` is the regression test for exactly that.
- [ ] Accept only a repeated hardware improvement with no p99 or correctness
  regression.

## O2 Direct `AsyncWrite` reservation

The reservation-versus-copy A/B is measured: the reservation path costs 0.265
ARM µs/request less at concurrency 128, with the three-run ranges disjoint at 32
and 128. That fixed the default, but it did not remove the intermediate queue,
which is what this item is for. The figure proves the first copy lever is small
beside the per-request L7 total, not that the remaining queue is worthless.

- [ ] Inject a worker-local egress reservation interface while keeping all
  `dmesh_l7_*` FFI in the adapter.
- [ ] Write from Linkerd directly into the DPUmesh arena without the
  intermediate tx queue, preserving partial-write and output ordering.
- [ ] Define capacity wakeup, cancellation, shutdown and task-drop semantics.
- [ ] Compare copy bytes, ARM CPU/request, arena stalls, publication rate and
  p50/p99 against the reservation baseline. **Re-baseline first.** The
  reservation-versus-copy figures above were taken before the `DmeshIo` tx
  cursor and their receipt is not in `bench/report/data/`, so their absolute
  µs/request is not the arm to subtract from. Accept only with copy-byte/arena
  evidence and no p99 or correctness regression.

## O3 Use more than eight ARM data cores

The fixed arrays admit `MAX_ARM_WORKERS=16` and `MAX_DPA_RINGS=16`, expanded
together across the public transport, broker descriptor handoff, DPA ring
tables, DPU completion PEs, reverse lanes, scratch layout and peer-worker port
ranges; reverse landing stripes are derived from A, not configured. The deploy
harness exposes `DPUMESH_THROUGHPUT_WORKERS` (4, 6, 8 or 12), deriving
A=K=W, the largest valid N ≤ 32 and all-worker L7, and the scale runner derives
`threads=channels=workers` from it so session fan-in is held constant; policy
and injection fixtures read effective K/A from the live DPU
(`bench/suite/deployed_geometry.sh`) instead of assuming eight. With the same
9+9 Host placement, A=4/6/8/12 reach 40k/70k/80k/130k clean RPC/s, 3 of 3,
consuming 3.97/4, 5.95/6, 7.79/8 and 11.37/12 worker cores; at 130k the Host
used 5.05/9 and 5.93/9 cores, and the A=12 point has balanced 97.4–98.9 %
worker use, clean ownership and loss counters and zero RPC failure. This is a
measured capacity lever. Independent N/K/A remain for the density deployment
where K > A is intentional.

- [ ] Reserve a distinct CPU for the DPU control/main thread. `dpu_worker.c`
  pins the main thread to CPU index A (`dpu_arm_pin_current("main",
  n_data_workers)`), so with all 16 CPUs allowed `A=16` wraps it onto worker 0;
  a larger worker geometry must not buy throughput by starving control
  progress. A=16 stays outside the supported range until this closes.

## O6 Incremental topology generations

A generation republishes whole: one Pod's churn re-signs the entire document and
every DPU re-fetches, re-verifies and re-parses all of it — O(fleet) work for an
O(1) change. Storage is not the pressure (~200 bytes per Pod against DPU DRAM,
16 MiB publication bound in `DMESH_TOPOLOGY_MAX_BYTES`); the republish
amplification is, and it grows as cluster size times churn rate.

- [ ] Publish a delta generation — records added and removed against a named
  base version — signed with the same Ed25519 key under the same strictly
  increasing version line. A consumer holding the base applies it; one that
  does not, or that fails any check, falls back to fetching the full
  generation. Nothing unsigned is adopted, and a refused or missing delta
  leaves the last adopted generation standing.
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
