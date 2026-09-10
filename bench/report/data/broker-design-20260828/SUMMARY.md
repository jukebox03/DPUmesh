> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Per-Pod broker design receipts

Appendices A, C and D moved here from `CONTROL_REPORT.md`, an uncommitted design
review. Every measurement was taken on the rapids4 rig (single node,
`N/K/A=32/8/8`) with the library-owned build that predates the broker. The raw
material (`campaign.log`, `e1e2.log`, `repeat_campaign.log`, `e3_wake.c`,
`e3b_fanout.c`) lived only in session scratch and is gone — these tables are the
only record.

How to read it: E1–E3b are the experiments of §A, V1–V6 the feasibility questions
they answered, G1–G7 the acceptance gates for the broker implementation (the
unexecuted ones are `PLAN.md` D3), M# the implementation stages, and A.1–A.5 the
section numbers of §A. The lifecycle design in §D is the proposal as reviewed and
differs from the implementation in one place: when the broker dies, the library
does not re-HELLO from the same process but raises SIGTERM on `TRANSPORT_DOWN` so
the container restarts (`src/core/dmesh_core.c`; `design/CONTROL.md` §2-1.9). The
full design rationale is `design/CONTROL.md` §2-1.9 and §5.4, and the final broker
measurement is
[`doorbell-relay-20260901/SUMMARY.md`](../doorbell-relay-20260901/SUMMARY.md).

---
## A. Measurement detail (all on the same rig)

### A.1 REV_DOORBELL frequency

Of the eleven comch wire enums, only REV_DOORBELL repeats at runtime: the eight
setup and teardown messages occur once per Pod lifetime, and RESOLVE once per
name per 5 s cache miss. Measured with a temporary counter in
`dpu_flush_host_doorbells` (since removed), six pairs of 5 s log lines inside the
load window per run:

| Run | RPC/s | doorbells/s | doorbell/RPC |
|---|---|---|---|
| idle (71 s plus three gaps) | 0 | ~0.2 | — |
| conc=1 (a/b repeat) | 7,318 / 6,886 | 28,940 / 27,081 | 3.96 / 3.93 |
| conc=4 | 15,627 | 31,306 | 2.00 |
| conc=32 | 100,530 | 27,781 | 0.28 |

The doorbells/s column is a **node total**, and this rig has two active Pods (the
bench client and the echo server), so it saturates at roughly 14–16K/s per Pod —
which means it is bound to the PE thread's wake cycle, not to traffic.

Most of that load is not new but a **relocation of what the per-Pod PE thread
already cost** (measured earlier: the PE thread was 72% of a 1-core Pod's
syscalls). What is genuinely additional in the new structure is **one eventfd
write and one thread wake per doorbell** (§3.3 cost ②); at the frequencies above
that is roughly 14–16K/s per active Pod, and V5 detected no extra wake latency
from the process boundary itself.

Note: the DPU's `-l 40` is DOCA WARNING, so stat lines must be emitted at WARN.

### A.2 E1 — memfd registration and DMA, plus a repeat campaign

An env gate (`DPUMESH_MEMFD_BUFFERS=1`, since removed) replaced one call site,
`alloc_buffer_and_set_mmap`, which made every host-exported allocation a memfd.
Single run: c32 97.9K rps at p50 294 µs, c1 6.8K rps at 131 µs, fail=0. Repeated
ABA (one binary, env flip plus rollout plus fair re-pinning; the arm was
identified from ReplicaSet history):

| Arm | n | rps | p50 |
|---|---|---|---|
| stock A+B | 8 | 97.7K ± 1.2K | 291.9 µs |
| memfd (first run after the flip excluded) | 4 | 98.2K ± 1.7K | 291.8 µs |
| stockA against stockB (same config) | 4/4 | 1.0% block drift | — |

No significant difference. The mechanism agrees: anon THP is madvise (never
called) and shmem THP is never, so both arms use 4K pages and the post-
registration runtime path is identical. Two methodological traps: the first run
after a flip is warm-up and was discarded, and `kubectl logs deploy/` during a
rolling update can read the old Pod (the arm was identified from RS history
instead). A re-check on 2026-08-31 still found `DPUMESH_MEMFD_BUFFERS=1` on bench
RS revisions 168/170 and echo RS revisions 152/154 but not on the active RS, so
the evidence for the ABA arm switch was preserved.

### A.3 E2 — sudden death of the process holding the registration

The echo server holding the memfd registration was SIGKILLed at t ≈ 25 s of a 60 s
c32 load. On the DPU: two `px_poison` events and buffer drops of 72 B and 3,504 B,
and nothing else — no DMA fault storm and no wedge. The client terminated cleanly
with fail=1, the Pod restarted and re-registered within seconds (fail=0
immediately after), and the preload Pod pair was unaffected. This experiment
proves only the blast radius of one registration-holding process dying. Surviving
an agent rollout, automatic broker re-registration, and a DPU restart are judged
separately by G5 of the new structure. "All Pods die at once" for a shared broker
is not a test target once per-Pod brokers are adopted.

### A.4 E3 — cross-process wake

Two eventfds in an epoll ping-pong, pinned to separate cores, 200K iterations x 2
rounds: RTT p50 is 17.7–18.0 µs when the writer is a thread of the same process
and 14.7–14.9 µs when it is a different process.

Read that carefully: cross-process came out **faster**. That direction is
unexplained, so it must not be claimed as "the process boundary gains 3 µs"; the
conclusion goes only as far as **"the wake cost of the process boundary is 0
within measurement error"**. That is consistent with the kernel's eventfd wake
path not caring which process the writer is in. (This benchmark measures only
wake latency; the syscall cost of the write itself must be computed separately
from the frequencies in A.1.)

### A.5 E3b — quantifying tail coupling of a shared relay (host-local)

What E3, an uncontended 1:1 test, cannot see: the serialization of a single
shared relay when bursts align. Simulation: a source issues a burst to N Pods on a
130 µs period like the DPU flush (with one unconsumed coalescing entry per Pod,
as real doorbells have), the relay is swapped between N per-Pod relays (today's
structure), one shared relay, and two shared relays (sharded), and the wake
latency distribution is measured. N=8 pinned the waiter cores; 32 and 64 did not
(they are for relative comparison between modes). **p50 is the valid signal** —
the tail (p99 and above) is dominated in every mode by C-state and scheduler noise
of hundreds of microseconds and cannot be judged from this benchmark.

| N (simultaneous wakes) | perpod p50 | shared1 p50 | shared2 p50 |
|---|---|---|---|
| 8 | 15.0 µs | 26.6 µs (+12) | 21.9 µs (+7) |
| 32 | 13.0 µs | 51.4 µs (+38) | 33.9 µs (+21) |
| 64 | 13.2 µs | 70.6 µs (+57) | 52.1 µs (+39) |

**Verdict**: the serialization cost of a shared relay is real, and the excess over
the queueing model, ≈ (N/2R) x S with S ≈ 2 µs per relay event, **agrees in order
of magnitude and trend** — though not exactly (measured minus model: +3–4 µs at
N=8, +5–6 µs at N=32, −7 to +7 µs at N=64). Use the model only to pick a sharding
rule; do not quote it as a prediction. It assumes worst-case alignment (all N Pods
at conc=1 waking together), and total CPU is unchanged.

**This measurement is the basis for adopting per-Pod brokers in §3**: the perpod
arm — which is the shape of a per-Pod broker — is flat at 13–15 µs regardless of
N, so it has no structural tail-coupling problem. Sharding was needed only with a
shared broker, and the rule `R ≥ N/8` comes from putting a budget of "hold the
excess under 8 µs" into the model above; it is not an arbitrary constant. The real
p999 is settled by the prototype (it has to be separated from this host's C-state
noise).

## C. Attack scenarios compared with existing meshes

| Attack | sidecar | ambient (ztunnel) | DPUmesh today | new structure |
|---|---|---|---|---|
| a compromised Pod exfiltrating its own identity key | **possible** (the key is in the Pod) | no (the key is in ztunnel) | no | no — **there is no workload key anywhere on the host** |
| a hostile workload impersonating another Pod on the same node (honest dataplane) | no | no (the kernel identifies the source) | impossible by protocol, but **exposing a privileged device violates the deployment premise** | blocked by the agent's `SO_PEERCRED` plus cgroup and the broker barrier (goal of G5) |
| identity blast from compromising the dataplane or proxy itself | that sidecar's workload | every workload cert on the node | the DPU can act for every Pod on its node | the same — separating the broker does not shrink the DPU blast |
| injecting malicious **data** into a shared dataplane | not applicable (the parser is per-Pod, so it is self-harm) | **every byte passes the shared daemon's parser** — a parser bug is a node DoS, which ambient accepts | every byte passes the shared DPU — a hardened boundary (bounded parsing, px_poison) | the same (DPU) — **the broker never looks at data bytes** |
| attacking the shared **control** surface | istiod (CSR/XDS from a compromised proxy) | the CNI and redirection surface | the DPU comch server (nine checks, slot caps; a gap in the unauthenticated timeout) | plus broker_i IPC — the surface is **fixed-size setup/RESOLVE messages and shape-sealed fds**, with 0 data bytes. Crash and overload blast is per-Pod, but broker code execution crosses the host/device trust boundary, so hardening is mandatory |
| resource-exhaustion DoS | its own cgroup only | connection-table pressure (with its own limits) | **privileged, so effectively unbounded through the raw device — worst** | no device plus agent quotas — **best** |
| shared-daemon crash blast | none (per-Pod) — its one advantage | the whole node dataplane, recovered by restart — an accepted profile | if the DPU dies the whole node is already down | the DPU only — **a broker_i crash is per-Pod** (E2 is the evidence for DPU teardown; automatic broker recovery is judged by G5) |
| remote network attacker | workload proxy mTLS | workload-identity HBONE mTLS | DPU node-pair TLS | the same goal, a different cryptographic principal |
| same-node or PCIe observer | mTLS between proxies, the local leg as-is | ztunnel's workload-identity path, the Pod↔ztunnel local leg as-is | **plaintext**, trusting the mapping and infrastructure | **plaintext**, trusting the mapping and infrastructure |
| host kernel, node or DPU compromise | node-resident keys and traffic lost | node-resident workload keys and traffic lost | node workload traffic and the right to act for it lost | the same |

How to read it: as a shared dataplane the new structure is in the same class as
ambient, with the advantages of no key, key storage off the host, and per-Pod
quotas. But ambient uses a workload identity-pair tunnel while DPUmesh uses a
node-pair tunnel, so the wire scope is not the same. The broker IPC is a new
host/device trust-boundary surface, and the sidecar retains the structural
advantage that compromising one proxy blasts one workload. DPUmesh accepts that
difference as the explicit price of offloading and of a node trust boundary.

## D. Design Q&A — lifecycle and failure edges (per-Pod broker)

This section is the result of sweeping questions of the form "what if broker_i
dies?" across lifecycle x component x failure. **Five design requirements were
found**: ① a persistent Pod↔broker UNIX socket is mandatory, not optional
(eventfd has no HUP semantics and cannot report the peer's death — detection is
socket EOF); ② the current PE thread's role as a "missed wake safety net" must
survive as a lightweight watchdog in the library (only the DOCA role disappears);
③ the broker must be spawned detached from the agent, and an agent restart needs
a re-adoption protocol (otherwise the agent's death regresses into a node-wide
outage); ④ only one control thread may read the broker socket; ⑤ broker EOF and a
DPU/comch down must be different reconnection paths.

| Question | Answer |
|---|---|
| how does a Pod detect and recover from broker_i dying? | the control thread detects socket EOF on the persistent socket immediately → every existing QP takes ECONNRESET (no automatic revival) → old mappings and fds are discarded → HELLO to the agent listener on a **new socket** → READY from a new broker with new memfds. On the DPU side the comch break drives quiescence, and a registration during quiescing is refused with backoff. A broken fd is never re-delivered and a mapping about to be discarded is never memset |
| how does the broker detect the Pod dying? | HUP on the other end of the same socket (plus pidfd). The rule is **exit the broker on HUP** → comch break → DPU teardown. "broker lifetime = Pod lifetime" is enforced by the socket |
| a wedged broker (alive but not relaying)? | there is no socket HUP → the library watchdog self-wakes and counts when the rev ring goes unconsumed and unwoken beyond T (the wake is a hint, the ring is the truth — the existing arm/recheck philosophy) |
| the agent dying? | a broker moved into the target Pod's parent cgroup survives and only new registrations stop (parity with today's fail-static). On agent restart the registry is re-adopted by re-verifying PID/starttime, cgroup and Kubernetes against root-only state |
| Pod restart and a double-broker race? | the old broker is exiting on HUP; the agent keeps at most one live registry entry per Pod UID with deny and backoff; DPU overlap is absorbed by the existing quiescing refusal. A new Pod has a different UID, so name reuse is never confused |
| library-to-broker version skew? | the library is a host mount from the webhook and is updated together with the broker, so the skew window is only "a Pod running with the old library loaded plus a newly respawned broker". The broker re-reads the HELLO the agent `MSG_PEEK`ed and refuses a version mismatch before READY |
| DPU restart? | every broker's comch breaks → TRANSPORT_DOWN → each broker recreates its control path, data path and memfds → a new READY on the same Pod socket. If that fails within a fixed time the Pod closes the socket, which promotes it to the broker-death path; a new broker is not created from the start |
| a Pod leaking its fds? | leaking its own buffer is the same as leaking its own memory, which is possible today too; identity is attached to `SO_PEERCRED`, not to an fd. A Pod writing its own eventfd only produces a spurious self-wake |
| the Pod's CPU and memory limits competing with the broker? | put the broker in the Pod's parent cgroup rather than the container scope, preserving the semantics the PE thread has today (it pays for its own wake latency, and OOM kills its own path); supervision respawn uses backoff to prevent a storm. The real semantics are settled by the G5 sacrificial-Pod gate |
