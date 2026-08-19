# DPUmesh + Linkerd plan

This is the working design document for the remaining work. It is meant to be
sufficient on its own: every task names the file it edits, the data it moves and
the gate that decides whether it is done. Completed implementation is described
in `design/CONTROL.md`, `design/L7.md` and
`linkerd/CONTRACT.md`; measurements live in `bench/report/`.

The remaining work splits into two lines that do not block each other. **Line A**
finishes the milestone this architecture already implements. **Line B** is what a
mesh would additionally have to do; it is scoped out of the current claim rather
than half-built.

## Deployed architecture

```text
HOST / Kubernetes                                      DPU

+---------------+  Unix socket  +----------------+     +----------------+
| Service Pod   |-------------->| trusted node   |     | Comch control  |
| app + client  | nonce + SVC id| agent DaemonSet|     | thread         |
| thread        |<--------------| peer/cgroup +  |     | nonce/MAC/key  |
+-------+-------+ signed grant  | K8s API reads  |     | replay/binding |
        |                      +--------+-------+     +--------+-------+
        |                               |                      ^
        |                               v                      |
        |                      +----------------+              |
        |                      | Kubernetes API |              |
        | grant + REGISTER     +----------------+              |
        +------------------------------------------------------+
                                                               |
                                                trusted flow   v
                                               +----------------+
 +----------------+    +-------------+          | ARM data       |
 | stock Linkerd  |<---| gateway DS  |<---------| worker         |
 | Identity       |    | TLS pass-thru|  mTLS   | Tokio thread   |
 | Policy         |    +-------------+          | per-session    |
 | Destination    |                             | Linkerd task   |
 +----------------+                             +--------+-------+
                                                         |
                                       validated session |
                                                         |
 +----------------+                                      |
 | registered     |<-------------------------------------+
 | backend Pod    |          PCIe / DPUmesh channel
 +----------------+

Authoritative feeds, each installed by atomic rename at a monotonic generation:
  identity renewal agent  ---> token/trust-root files on the DPU
  Service registry        ---> Service target + ready endpoint feed (adapter)
  node agent + pusher     ---> node membership feed (Comch control thread)
```

The same picture, drawn rather than sketched, is
[`design/figures/control_plane.png`](design/figures/control_plane.png).

The production path has no DPUmesh mock control-plane fallback. The remaining
`mock-identity`, `mock-policy` and `mock-destination` sources belong only to the
upstream `linkerd-app-integration` test crate and are not linked or deployed.

## Where the pieces live

| Concern | Files |
|---|---|
| Grant format, signing, verification, keyring | `doca/workload_grant.{c,h}` |
| Registration, revocation scan, teardown | `doca/comch_server.{c,h}` |
| Membership feed parse/adopt | `doca/pod_membership.{c,h}` |
| DPU-side state | `doca/object.h` (`struct objects`, `struct pod_state`) |
| Control-thread loop | `doca/dpu_worker.c` (`dpu_drain_iteration`) |
| L7 admission, drain and fail-closed | `doca/dpu_proxy.c` (`px_parse_l7`, `px_l7_open_conn`) |
| Feed signature envelope | `doca/workload_grant.c` (`dmesh_feed_verify`) |
| Host registration client | `src/dmesh_core.c` (`init_control_path`), `src/dmesh_attest.{c,h}` |
| Adapter sessions, target feed | `linkerd/rust/src/lib.rs` |
| Backend registry, metrics, IO | `linkerd/port/linkerd2-proxy/linkerd/doca/src/*.rs` |
| Per-session outbound stack | `linkerd/port/linkerd2-proxy/linkerd/app/src/lib.rs` |
| DMesh connector | `linkerd/port/linkerd2-proxy/linkerd/app/outbound/src/tcp/connect.rs` |
| C↔adapter ABI | `linkerd/include/dmesh_l7.h`, `linkerd/shim/l7_null.c` |
| Node agent | `bench/workload_attest_agent.py`, `bench/k8s/workload-agent.yaml` |
| Keyring, agent deploy, membership push | `bench/workload_attest.sh` |
| Service target feed | `bench/linkerd_service_registry.sh` |
| Identity material | `bench/linkerd_identity.sh` |
| Control-plane gateway | `bench/linkerd_cp_gateway.sh`, `bench/linkerd_cp_relay.py` |
| Orchestration | `bench/bench.sh` |

Switches: `DPUMESH_REGISTRATION_KEY_DIR`, `DPUMESH_REGISTRATION_ISSUER`,
`DPUMESH_ATTEST_SOCKET`, `DPUMESH_MEMBERSHIP_FILE`, `DPUMESH_ADMISSION_FILE`,
`DPUMESH_L7_SERVICE_TARGETS_FILE`, `DPUMESH_L7_FAIL_CLOSED`,
`DPUMESH_L7_LINKERD_WORKER`.

Counters: `dmesh_control_events_total{kind,reason}`,
`dmesh_sessions_declined_total{reason}`,
`dmesh_backend_target_mismatches_total`, `dmesh_session_stack_*`,
`dmesh_sessions_{opened,closed,active}`.

## Trust boundary and scope of the claim

Who is trusted for what, in the deployed configuration:

| Party | Trusted for | Not trusted for |
|---|---|---|
| Service Pod | relaying its own grant and nonce | any claim about its identity, Service, node |
| Node agent (root DaemonSet) | deriving claims from peer credentials and the Kubernetes API; signing grants and membership | nothing it does not read from an authoritative object |
| DPU verifier | keyring, nonce binding, replay, Service match, membership | — |
| Feed publishers | content of the feeds they sign | a generation the keyring cannot verify |
| Gateway DaemonSet | carrying bytes | minting or terminating mesh identity |

The embedded proxy runs Linkerd's **outbound** half only. A stock sidecar also
runs an inbound half, which terminates mTLS, reads the client certificate
identity and enforces `AuthorizationPolicy`. Here the destination is a
node-local registered Pod reached by DMA, so no inbound proxy exists in that byte
path, and every outbound stack authenticates to the control plane with the
shared `dpumesh-dpu` certificate.

What that supports asserting:

> DPUmesh receives per-Pod outbound policy from a stock Linkerd control plane and
> enforces it on the DPU, and it admits registrations and Service membership
> cryptographically, with generation-safe revocation.

What it does not support asserting: source-identity-based inbound authorization,
cross-node protection, or a per-Service mix of protected and unprotected traffic.
Those are Line B.

## What works now

- A Pod cannot supply its Pod UID, namespace, labels, ServiceAccount, node or
  Linkerd workload. `bench/workload_attest_agent.py` derives them from
  `SO_PEERCRED`, the peer cgroup and authoritative Kubernetes objects, rejects a
  pid recycled during attestation, and bounds concurrent requests.
- `dmesh_grant_verify_v1` checks canonical form, issuer, expiry, Service range,
  connection nonce and HMAC; the key is selected by the signed key id from an
  overlap keyring; `dmesh_registration_consume_grant` rejects replay;
  `pods_register` refuses a Service other than the granted one and refuses a
  consumed grant. The signed Pod UID is retained on `pod_state`.
- Deleting a Pod or changing its labels withdraws its `(Pod UID, Service)` pair
  from the node membership generation, and `server_progress_membership` closes
  that exact registration through `pod_begin_cleanup`. Withdrawal takes two
  consecutive generations; a missing, malformed, oversized or rolled-back
  generation revokes nothing.
- The embedded stock proxy certifies a dedicated `dpumesh-dpu` identity and
  connects to real Linkerd Identity, Policy and Destination services through the
  host-network gateway.
- Service targets and ready endpoints arrive in a publisher-monotonic versioned
  feed that a session adopts only when its inode/mtime/length stamp changed. A
  selected target outside the snapshot is `TargetMismatch`; required
  registration selects `DPUMESH_L7_FAIL_CLOSED=1`, so a declined protected
  session is refused rather than continued as TCP/L4.
- Every authoritative feed carries a `signature=<key-id>,<hex>` envelope signed
  by the registration keyring, and both consumers parse only the signed prefix.
  An unsigned, forged, appended-to or unknown-key generation is refused exactly
  like a malformed one, so it never revokes or admits anything. Rotating the
  keyring rotates feed signing.
- `bench/bench.sh admission open|drain` stops admitting protected sessions
  without cutting the ones in flight, and `bench/bench.sh rotate-identity`
  drains, waits for the DPU to observe the switch and for
  `dmesh_sessions_active` to reach zero, installs the material, restarts,
  waits for `/ready`, restores Pod placement and reopens admission.
- Grant, membership, revocation and admission outcomes and session declines are
  exported by reason at the proxy admin metrics surface.
- Fail-closed covers every mode a Service can be assigned to. `px_parse_l7`
  refuses a declined open, and `px_l7_decide` refuses a connection the layer
  returned no verdict for, counting it as
  `dmesh_control_events_total{kind="admission",reason="no-verdict"}`. The
  embedded consumer declines every decision-mode question, so
  `resolve_l7_fail_closed` refuses to deploy `DPUMESH_L7_DECISION_SVC` under
  `L7_BACKEND=linkerd` rather than start a Service that could only refuse.
- Session close is generation-safe and leaves opened equal to closed with zero
  active sessions, pending registrations, live tasks and orphaned endpoints.

---

# Line A — finish the current milestone

## A1 Production ownership of the control agents

### Problem

The identity, Service registry and membership publishers are Host user systemd
units that move files to the DPU over SSH. The feeds themselves are now signed
and verified, so the transport carries no authority — but a deployment still
cannot depend on an operator login session.

### Steps

- [ ] Replace the user units with node-scoped supervised controllers holding
  least-privilege credentials. The membership publisher already runs in-cluster
  as the node agent; only its delivery hop is a user unit.
- [ ] Give each feed retry/HA state and a boot order independent of an operator
  session.
- [ ] Keep the DPU consumers unchanged: they already refuse a generation that is
  missing, malformed, oversized, unsigned or older than the one they hold.

### Gates

- [ ] A node reboot restores every feed with no operator action.
- [ ] Losing one publisher stops updates without revoking membership or
  withdrawing targets.

## A2 Exact Linkerd endpoint semantics, when required

### Problem

`Backends::take_session` verifies that Linkerd's selected address is in the
session's authoritative Service snapshot, then DPUmesh chooses a registered
backend Pod. Membership is enforced; selection is not honored. Linkerd weights,
TrafficSplit-style behavior and endpoint failover therefore do not apply inside
a Service.

### Design

Carry an endpoint-to-registration mapping in the same feed, and resolve it at
`take_session` time.

Feed extension (`bench/linkerd_service_registry.sh`):

```text
endpoint=<service-id>,<ip:port>,<pod-uid>
```

Adapter (`linkerd/rust/src/lib.rs`): keep `HashMap<SocketAddr, PodUid>` per
Service alongside `service_endpoints`. On session open, publish the map with the
session. On `take_session(selected)`, resolve `selected → pod_uid`, then ask
DPUmesh for the live pod id of that UID through a new FFI
`dmesh_l7_pod_for_uid(worker_id, const char *pod_uid) -> int32_t`, backed by a
scan of `objs->pods[]` in `doca/dpu_proxy.c` that matches `pod_uid` and
`registered`. A UID with no live registration is a distinct decline reason, not
a fallback.

Generation safety: the resolution must fail if the registration's
`membership_generation` is older than the feed generation that named the
endpoint. A recreated Pod carries a new UID, so it cannot inherit a mapping; the
generation check covers the reverse case where the feed is ahead of the DPU.

### What already exists

- The feed already carries `endpoint=<service-id>,<ip:port>` and the adapter
  already parses it into `service_endpoints`
  (`parse_versioned_service_targets`), so only the third field and its
  resolution are missing.
- `pod_state` already retains the grant's signed `pod_uid`
  (`doca/object.h`), so `dmesh_l7_pod_for_uid` has the field it must match on
  and needs no new claim retention.
- `Backends::place_targets` / `service_of_target` already give `take_session`
  the address→Service placement; the UID map is a second value on the same key.

### Steps

- [ ] Extend the feed writer and `parse_versioned_service_targets` with the
  third field, rejecting an `endpoint=` line that carries two fields once the
  writer emits three — a partially upgraded publisher must not silently lose
  the mapping.
- [ ] Add `dmesh_l7_pod_for_uid` to `linkerd/include/dmesh_l7.h`,
  `linkerd/shim/l7_null.c` and `doca/dpu_proxy.c`.
- [ ] Resolve in `Backends::take_session`; add
  `dmesh_sessions_declined_total{reason="endpoint-unresolved"}` and
  `{reason="endpoint-stale"}`.
- [ ] Apply add/update/delete as one generation.
- [ ] Reject unresolved, stale and cross-Service selections with no
  round-robin/TCP fallback.

### Gates

- [ ] Rolling update: no request reaches a Pod whose UID left the generation.
- [ ] Zero ready endpoints: sessions are refused, not round-robined.
- [ ] Weighted distribution matches Linkerd's own within the tolerance the
  balancer guarantees.
- [ ] Control-plane reconnect does not resurrect a stale mapping.

## A3 Measured optimization

### What is known

L7 capacity is bounded by per-session cost, not by the transport. The total is
measured: `bench/suite/l7_session_cost.sh` moves the reconnect rate against an
`L7_BACKEND=null` control on the identical workload, and least squares over both
sets puts a DPUmesh connection at **73 ARM core-µs** and the Linkerd session on
top of it at **1,200** — so 1,127 µs is the Linkerd share. The receipts are in
[`REPORT_L7.md`](bench/report/REPORT_L7.md) and
`bench/report/data/l7-session-cost-20260817`.

The synchronous half of that is instrumented in `linkerd/app/src/lib.rs` and
reported by `SessionMetrics::observe_stack_build`. Over 9,565 opens and closes
across four workers:

| Phase | Per session | Share of the 1,200 |
|---|---:|---:|
| `configure` (clone the outbound template, set `dmesh_session`) | 5.9 µs | 0.5% |
| `layers` (`build_policies` + `outbound.mk`) | 107.8 µs | 9.3% |
| `service` (`NewService::new_service`) | 34.7 µs | 3.0% |
| synchronous total | **148.5 µs** | **12.9%** |

So the synchronous `outbound.mk` boundary is about one eighth of the slope, not
the four fifths the earlier estimate implied. The remaining seven eighths is
lazy discovery and policy work, task execution and teardown, and the surrounding
DPUmesh lifecycle. **Locating it requires instrumenting those asynchronous
boundaries; do not assume the whole figure is inside the synchronous call, and do
not conflate template caching with watch sharing — they attack different parts.**

### A3.1 Reduce per-session stack construction

- [x] Re-measure the total against the frozen baseline and record it in
  `bench/report/`. Done: 1,200 ARM core-µs per session against a 73 µs L4
  control, published in `REPORT_L7.md`.
- [ ] Instrument the untimed remainder: policy discovery, destination/profile
  discovery, reconnect layers, endpoint construction and balancer construction.
  Extend `SessionMetrics` rather than adding a second surface. This is the whole
  of A3.1's remaining unknown: 1,051 of the 1,200 µs are unattributed.
- [ ] Cache only immutable templates. `SessionToken`, backend channel, workload,
  target generation, cancellation and metrics must stay session-local — the
  connector binds to `dmesh_session`, so a shared service would take another
  session's channel.
- [ ] Share watches only by authoritative `(workload, target, generation)`.
  Before sharing, add tests for: an update arriving while two sessions share a
  watch; a withdrawal invalidating a shared watch; and the last consumer
  releasing it.
- [ ] Accept only a repeated hardware improvement with no p99 or correctness
  regression.

### A3.2 Direct `AsyncWrite` reservation

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

### A3.3 Conditional worker-local state

- [ ] Re-profile after A3.1/A3.2; continue only if endpoint locking becomes
  material. Nothing counts lock contention today: `Backends` holds one
  `parking_lot::Mutex` with no instrumentation, and the profile in `REPORT_L7.md`
  puts the AArch64 parking-lot fast path at 1.3–1.7% with no pool symbol above
  it. Add a counter before drawing a conclusion from that.
- [ ] If justified, prototype only the DPUmesh specialization on Tokio `LocalSet`
  with `Rc<RefCell<_>>`; do not add unsafe `Send`/`Sync` claims or modify stock
  TCP Linkerd behavior.

### A3.4 Optional shared backend transport

- [ ] Attempt only if session-stack work still shows a material need.
- [ ] Define a separately versioned backend-transport id, independent lifetime,
  stream response routing, flow control and cancellation. Do not overload the
  current client connection handle.

## A4 Equivalent ARM/x86 study

- [ ] Build an equivalent x86 full-stack harness with the same Linkerd revision,
  control plane, compiler, flags, request sizes and concurrency.
- [ ] Compare cycles/request, instructions, cache misses, migrations, context
  switches, latency, throughput, affinity and clock frequency.
- [ ] Separate `DmeshIo`, Tokio, opaque stack, protocol detection,
  policy/identity and full-stack slices before proposing a runtime change.

---

# Line B — mesh completeness beyond this milestone

## B1 Per-source inbound authorization

### What Linkerd does

Every meshed Pod's proxy holds its own leaf certificate whose SAN is
`<serviceaccount>.<namespace>.serviceaccount.identity.<trust-domain>`. It obtains
it by presenting the projected ServiceAccount token kubelet mounted into that
Pod with the Linkerd identity audience, and refreshes at 70% of the certificate
lifetime. The destination's inbound proxy reads the client certificate identity
and evaluates `AuthorizationPolicy` against it.

The inbound API, as the vendored proxy uses it in
`linkerd/app/inbound/src/policy/api.rs`:

```text
InboundServerPolicies.WatchPort(PortSpec { port: u32, workload: String })
  -> stream of ServerPolicy
```

`ServerPolicy::protocol` carries `tcp_authorizations: Arc<[Authorization]>` and
HTTP routes with their own authorizations. Each
`Authorization { networks, authentication, meta }`, where

```rust
enum Authentication {
    Unauthenticated,
    TlsUnauthenticated,
    TlsAuthenticated { identities: BTreeSet<String>, suffixes: Vec<Suffix> },
}
```

**`workload` is a plain string the client supplies**, exactly like the outbound
`LINKERD2_PROXY_POLICY_WORKLOAD`. So a client that knows the destination's
workload can ask for its inbound policy; whether the policy controller serves
that to a caller which is not that workload is the open question in Q1 below.

### The consequence for this architecture

A certificate exists to let an enforcer authenticate a party it did not
authenticate itself. On the node-local path the DPU is both: the node agent
already signed the source's namespace and ServiceAccount into the grant, which
is strictly more than a certificate would prove. What is missing is an
enforcement point, not an identity.

That makes three designs possible, in increasing cost:

| Design | Needs | Breaks / costs |
|---|---|---|
| **D1 — DPU-side evaluation.** The DPU watches the destination workload's inbound policy and evaluates `Authorization` against the grant-derived source identity string | Q1 answered yes; a defensible `networks` match for a DMA source | No certificates, no key material, no new lifecycle. Diverges from Linkerd in that the *source's* proxy enforces the *destination's* rule |
| **D2 — per-workload certificate, DPU-terminated.** The DPU holds a leaf per registered workload and presents it on connections it originates | Token provenance (below); N keys and renewal loops; binding each certificate to the live registration | Real client identity on the wire; needed anyway for B2 |
| **D3 — full inbound proxy on the DPU.** An inbound stack terminates for each destination Pod | D2, plus an inbound byte path that today is a DMA write into Pod memory | Closest to stock Linkerd; largest change to the datapath and to the cost model the reports measure |

D1 is the only one that is node-local-only. D2 is the one B2 also needs. The
decision therefore depends on whether cross-node protection is in scope: if it
is, do D2 once and let node-local reuse it.

### Token provenance, if D2 or D3

A certificate for a workload requires proof of that workload's ServiceAccount,
and the DPU is not inside the Pod:

| Path | Cost |
|---|---|
| The Pod relays its own projected token over the registration channel | The Pod supplies identity material. The token is verifiable rather than asserted, but Pods must first be given an `identity.l5d.io`-audience projected volume, and the DPU must bind the token's subject to the grant's ServiceAccount before using it |
| A node-scoped credential mints tokens for other ServiceAccounts (`TokenRequest` on their behalf) | Impersonation authority on the node agent; its compromise forges cluster identity |
| Linkerd Identity gains delegated issuance | Not upstream; a fork to maintain |

Whichever is chosen, add: per-workload key ownership (never exposed to the Pod),
N renewal loops at 70% lifetime, revocation on deregistration reusing the
membership generation, and binding each certificate to the exact live
registration through the retained Pod UID.

### Steps

- [ ] Answer Q1 (below). It selects D1 or forces D2.
- [ ] Write the identity architecture document jointly with B2: certificate
  subject, key ownership, token provenance, source-to-session binding, renewal,
  revocation, and the exact Linkerd API calls involved.
- [ ] If D1: build the source identity string
  `<service_account>.<namespace>.serviceaccount.identity.<trust-domain>` from the
  grant. Both fields are signed in `struct dmesh_workload_grant_msg`, but only
  `workload` and `pod_uid` are retained on `pod_state` today, so the first
  datapath change is to retain `service_account` and `namespace_name` beside them
  in `doca/comch_server.c` and carry the identity string into
  `struct dmesh_l7_flow`. Then define the `networks` match for a source that has
  no routable address in the destination's view (Q2).
- [ ] Only then edit the datapath. Do not simulate the decision with a local
  mock and do not report an outbound API feature as inbound authorization.

### Gates

- [ ] An `AuthorizationPolicy` allow→deny→allow cycle on a live Service changes
  admission within one watch update, with no session admitted during `deny`.
- [ ] A Pod whose ServiceAccount does not match any authorization is refused,
  and the refusal is counted by reason.
- [ ] The refusal happens without any Pod-supplied identity input.

## B2 Node-to-node mTLS

### Problem

The protected path is node-local: what separates one Pod from another is the
DPU's per-Pod mapping, not a wire an attacker can reach. A Service that spans
nodes has no such argument — the bytes cross a real network.

### Design questions to settle

- **What carries the traffic between DPUs?** The node-local path is a DMA
  channel with no wire format. A cross-node path needs one: TCP+TLS between DPU
  proxies is the smallest step; an RDMA path would need its own protection.
- **Where does mTLS terminate?** On the originating DPU and the destination DPU,
  which makes each DPU an inbound termination point — that is D3's inbound stack
  arriving through a different door.
- **What identity is presented?** The shared `dpumesh-dpu` certificate proves
  only the node. Per-workload identity (B1/D2) is what makes the destination
  able to authorize.
- **How does it interact with A3?** With cross-node Services, Linkerd's endpoint
  selection can name a Pod on another node, which DPUmesh cannot serve from its
  local registration set. A2's resolution must then distinguish "not local" from
  "not live".

### Steps

- [ ] Choose the cross-node transport and write its frame/stream contract.
- [ ] Define termination points and the identity each presents, jointly with B1.
- [ ] Define the per-Service transport mode: which Services may span nodes and
  what happens when an endpoint is remote.
- [ ] Extend A2 resolution with a remote outcome distinct from unresolved.

### Gates

- [ ] A cross-node request is refused unless mTLS is established with an
  identity the destination authorizes.
- [ ] A node-local request is unaffected in path and cost.

## B3 Mixed protected and unprotected Services

### Problem

Every registration is attested, so every Service is protected to the same
degree. A real deployment grades protection per Service, and that choice must
not be inferable from Pod input — otherwise a Pod opts itself out.

### Design

A protected-Service policy delivered like the other feeds, signed under A1:

```text
version=<u64>
protected=<service-id>
signature=<key-id>,<hex>
```

Consumed by `doca/comch_server.c` at registration time: a Service in the set
carries the strict policy below, and a Service outside it carries the relaxed
one. Every registration stays grant-verified either way; what the feed grades is
the interaction rules, not whether a Pod is attested.

Interaction rules to define:

- A protected Service calling an unprotected one: the callee cannot be
  authenticated, so either the call is refused or the policy explicitly permits
  it. Default to refusal, name the counter.
- An unprotected Service calling a protected one: admission is decided by the
  protected side, which is B1's enforcement point.

### Steps

- [ ] Define and sign the protected-Service feed.
- [ ] Consume it in `pods_register` and in `px_parse_l7`'s fail-closed decision,
  replacing the process-wide `DPUMESH_L7_FAIL_CLOSED`.
- [ ] Define and implement the two interaction rules with distinct counters.

### Gates

- [ ] Moving a Service from unprotected to protected takes effect at the next
  generation, without restart, and rejects the next unverified registration.
- [ ] No Pod input changes which side of the boundary its Service is on.

---

## Deferred

**`dmesh-doca`'s `own-datapath` feature.** It compiles a second copy of the
DPUmesh C datapath — `linkerd/port/DPUMesh/` plus `src/shim.c`, `driver.rs` and
the `DmeshDoca` owner — so the crate can open DOCA and run its own worker loop.
The product build selects `default-features = false`, its bundled sources do not
match the current tree, and nothing builds the feature. Removing it would also
drop the second `RuntimeBackend` implementation that `linkerd/CONTRACT.md`
documents as the boundary between the two owners.

**Do not delete any of it without asking and receiving explicit confirmation
first.**

## Open questions

These are unverified assumptions. Each names how to answer it.

**Q1 — Does linkerd-policy serve a workload's inbound policy to a caller that is
not that workload?** `WatchPort` takes the workload as a client-supplied string,
and the DPU authenticates with the shared `dpumesh-dpu` certificate. Answer by
calling `InboundServerPolicies.WatchPort` from the DPU with a test Pod's
workload, through the existing gateway, and observing whether the controller
serves, denies, or serves an empty policy. This single result decides D1 vs D2 in
B1.

**Q2 — What does the destination's `Authorization.networks` mean for a DMA
source?** A node-local source has no address in the destination's network view.
Determine whether the synthetic source address the adapter already constructs
(`pod_addr`) can be made to satisfy a realistic `networks` clause, or whether D1
must treat `networks` as unenforceable and say so.

**Q3 — What is the current total session establishment cost? Answered.** On the
current build a DPUmesh connection costs 73 ARM core-µs to build and tear down
and the Linkerd session on it costs 1,200, of which 148.5 is the synchronous
`outbound.mk` boundary. What remains open is not the total but its split: the
1,051 µs outside the synchronous call is unattributed, and that split is what
decides whether anything can be shared without giving up session isolation. It
is A3.1's remaining step, not a question.

## Acceptance invariants

- [ ] One ARM worker owns each connection and all reachable session state.
- [ ] Every accepted staging extent is released exactly once and is never read
  after release.
- [ ] Close and revocation are generation-safe and idempotent from every path.
- [ ] Protected traffic has no unauthenticated admission, silent TCP/L4
  fallback, cross-Service rewrite, drop or reorder.
- [ ] A missing, malformed, unsigned or rolled-back authoritative feed never
  withdraws membership, revokes a registration or admits an unverified one.
- [ ] After each gate, opened equals closed and active/pending/tasks/backend
  entries are zero.
- [ ] Optimization results use repeated runs with the frozen topology, placement
  and 2.5 GHz clock; otherwise they are not accepted.
- [ ] A capacity is quoted with the instrument that produced it. The L4 and gRPC
  reports quote `knees.csv`'s twice-voted `highest_clean_rps`; the linkerd report
  quotes the single-run rate grid, which reaches further on the same data. The
  two are not interchangeable and a figure must not present one under the other's
  caption.

## Order

```text
A1 agent ownership ──┐  deployment engineering; blocks no measurement
                     │
A2 endpoint semantics (only if the product requires exact Linkerd selection)
                     │
A3 optimization ─────┴──> A4 ARM/x86 study

Q1 ──> B1 inbound authorization ──> B2 node-to-node mTLS ──> B3 mixed mode
       (design first; D2 makes B1 and B2 one architecture)
```

A1 is deployment engineering: it changes no measured behavior and must not gate
A3. A3 must not be measured across a run that also changes correctness behavior.
Line B starts with Q1 and a design document, never with a datapath edit.
