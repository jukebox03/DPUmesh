# DPUmesh + Linkerd plan from the current checkpoint

This plan starts at the control-plane and trusted-registration checkpoint of
2026-08-17. Completed implementation history is kept in
`design/LINKERD_CONTROL.md`, `design/L7.md`, `linkerd/CONTRACT.md` and the
reports under `bench/report/`; it is not repeated as a TODO list here.

## Current session architecture

### Before this control-plane session

```text
HOST / Kubernetes                         DPU

+----------------------+                 +-----------------------+
| Service Pod          |  self-reported  | Comch control thread  |
|  app thread          |  workload +---->|  Pod registration     |
|  DPUmesh client      |  Service        |  no trusted binding   |
+----------------------+                 +-----------+-----------+
                                                    |
                                                    v
                                        +-----------------------+
                                        | ARM data worker       |
                                        |  Linkerd session task |
                                        |  embedded proxy       |
                                        +-----------+-----------+
                                                    |
                                         static/mock control data
                                                    |
                                                    v
                                        +-----------------------+
                                        | control-plane fixture |
                                        +-----------------------+
```

The Pod could influence the workload attributed to its session. Control-plane
reachability, identity renewal and Service targets were deployment fixtures,
and there was no authoritative same-Service endpoint check.

### Current implementation

```text
HOST / Kubernetes                                      DPU

+---------------+  Unix socket  +----------------+     +----------------+
| Service Pod   |-------------->| trusted node   |     | Comch control  |
| app + client  | nonce + SVC id| agent thread   |     | thread         |
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
                                         validated DMesh |
                                                         |
 +----------------+                                      |
 | registered     |<-------------------------------------+
 | backend Pod    |          PCIe/DMesh channel
 +----------------+

Supporting agents:
  identity renewal user service  ---> atomic token/trust-root files on DPU
  Service registry user service  ---> versioned Service/endpoint feed on DPU
```

The production path has no DPUmesh mock control-plane fallback. The remaining
`mock-identity`, `mock-policy` and `mock-destination` sources belong only to the
upstream `linkerd-app-integration` test crate and are not linked or deployed.

## What works now

- A Pod cannot supply its Pod UID, namespace, labels, ServiceAccount, node or
  Linkerd workload. The node agent derives them from peer credentials, cgroup
  and authoritative Kubernetes objects.
- Required mode verifies a connection nonce, issuer/key-id, expiry, Service,
  HMAC and consumed grant id before registration.
- The DPU has an overlap keyring and rejects replay. The active hardware state
  contains only `node-hmac-v2` after the tested rotation and prune.
- The embedded stock proxy certifies a dedicated `dpumesh-dpu` identity and
  connects to real Linkerd Identity, Policy and Destination services.
- Service targets and ready endpoints arrive in a monotonically versioned
  feed. Rollback, withdrawal and a selected target outside the same-Service
  snapshot fail new sessions without TCP/L4 fallback.
- Session close is generation-safe and leaves opened equal to closed with zero
  active sessions, pending registrations, live tasks and orphaned endpoints.

## What does not work yet

### Production requirements

1. **Per-source Linkerd inbound authorization**
   - `OutboundPolicies` supplies protocol and route policy, not inbound
     `AuthorizationPolicy` allow/deny decisions.
   - Every outbound stack currently uses the shared `dpumesh-dpu` certificate.
     A destination therefore cannot cryptographically identify the originating
     Pod from that certificate.
   - Required result: a per-workload identity lifecycle and an inbound
     enforcement point, without allowing the Pod to choose its own identity.

2. **Production ownership of identity and registry agents**
   - Token renewal and Service feed publication currently run as Host user
     systemd services and transfer files to the DPU over SSH.
   - Required result: node-scoped supervised agents/controllers, least-privilege
     credentials, authenticated feed transport, HA/retry state and boot order
     independent of an operator login session.

3. **Automated trust-root/key/CSR replacement**
   - Token-only rotation is automatic; trust roots, private key and CSR still
     need an operator-controlled restart.
   - Required result: stop protected admission, drain sessions, atomically
     install material, restart workers, certify, then release readiness.

4. **Revocation of an already registered workload**
   - Pod UID and Service selector are authoritative at grant time, but deleting
     the Pod or changing its selector does not immediately revoke an existing
     live registration.
   - Required result: watch authoritative Pod/Service generations and close the
     exact DPU registration/session when membership is withdrawn.

5. **Complete operational observability**
   - Some rejection data exists only in DPU logs or aggregate counters.
   - Required result: exported counters by grant rejection, feed rejection,
     target mismatch, revocation and control-service failure reason, plus token
     age, certificate expiry and consecutive renewal alerts.

### Capability-dependent work

1. **Exact Linkerd endpoint weighting and failover**
   - Current behavior verifies that a selected endpoint belongs to the same
     Service, then DPUmesh chooses a registered backend Pod.
   - Linkerd weights, TrafficSplit-style behavior and exact endpoint failover
     require a generation-safe mapping from endpoint Pod UID/IP to DPU Pod id.

2. **Node-to-node mTLS**
   - The current protected path is node-local. Cross-node/DPU traffic needs a
     separately designed mTLS termination and per-Service transport mode.

3. **Mixed protected and unprotected Services**
   - Required production mode protects every registration. A mixed deployment
     needs an authoritative protected-Service policy and must not infer it from
     Pod input.

## Execution plan

### P0 — finish mandatory production behavior

- [ ] Design the per-workload identity and inbound enforcement architecture.
  Specify certificate subject, key ownership, source-to-session binding,
  renewal, revocation and the exact Linkerd authorization API involved before
  editing the datapath.
- [ ] Replace the Host user services and SSH/password file transfer with
  production node agents/controllers and authenticated, atomic DPU delivery.
- [ ] Implement drain/restart/readiness orchestration for trust-root, key and
  CSR changes. Test both successful rotation and expired/invalid replacement.
- [ ] Add Kubernetes Pod/Service watches and generation-bound revocation of
  existing DPU registrations.
- [ ] Export reason-labelled registration, revocation, feed and control-plane
  metrics through the existing admin metrics surface and define alert rules.
- [ ] Run adversarial hardware gates: forged identity, replay, deleted/recreated
  Pod UID, selector withdrawal, certificate expiry, controller restart and feed
  rollback. Require zero unauthenticated or silently forwarded sessions.

### P1 — exact endpoint semantics, when required

- [ ] Extend the authoritative feed with Service generation and endpoint Pod
  UID/IP, bound to the exact live DPU Pod registration.
- [ ] Apply add/update/delete as one generation; a recreated Pod must never
  inherit an earlier mapping.
- [ ] Translate Linkerd endpoint weights/failover only after every selected
  endpoint resolves to a live same-generation registration.
- [ ] Reject unresolved, stale and cross-Service selections with distinct
  metrics and no round-robin/TCP fallback.
- [ ] Validate rolling updates, zero-ready-endpoint state, weighted distribution
  and control-plane reconnect on hardware.

### P2 — optimization after P0 correctness

#### P2.1 Reduce per-session stack construction

- [ ] Split stack timing into policy discovery, destination/profile discovery,
  reconnect layers, endpoint construction and balancer construction.
- [ ] Cache only immutable templates. Keep `SessionToken`, backend channel,
  workload, target generation, cancellation and metrics session-local.
- [ ] Share watches only by authoritative workload, target and generation after
  update/withdrawal/last-consumer tests exist.
- [ ] Accept only a repeated hardware improvement without p99 or correctness
  regression.

#### P2.2 Direct `AsyncWrite` reservation

- [ ] Inject a worker-local egress reservation interface while keeping all
  `dmesh_l7_*` FFI in the adapter.
- [ ] Write from Linkerd directly into the DPUmesh arena without the
  intermediate tx queue, while preserving partial-write and output ordering.
- [ ] Define capacity wakeup, cancellation, shutdown and task-drop semantics.
- [ ] Compare copy bytes, ARM CPU/request, arena stalls, publication rate and
  p50/p99 against the frozen reservation baseline.

#### P2.3 Conditional worker-local state

- [ ] Re-profile after P2.1/P2.2; continue only if endpoint locking becomes
  material. Existing profiles observed zero contention.
- [ ] If justified, prototype only the DMesh specialization on Tokio `LocalSet`
  with `Rc<RefCell<_>>`; do not add unsafe `Send`/`Sync` claims or modify stock
  TCP Linkerd behavior.

#### P2.4 Optional shared backend transport

- [ ] Attempt only if session-stack work still shows a material need.
- [ ] Define a separately versioned backend-transport id, independent lifetime,
  stream response routing, flow control and cancellation. Do not overload the
  current client connection handle.

### P3 — equivalent ARM/x86 study

- [ ] Build an equivalent x86 full-stack harness with the same Linkerd revision,
  control plane, compiler, flags, request sizes and concurrency.
- [ ] Compare cycles/request, instructions, cache misses, migrations, context
  switches, latency, throughput, affinity and clock frequency.
- [ ] Separate `DmeshIo`, Tokio, opaque stack, protocol detection,
  policy/identity and full-stack slices before proposing a runtime change.

## Acceptance invariants

- [ ] One ARM worker owns each connection and all reachable session state.
- [ ] Every accepted staging extent is released exactly once and is never read
  after release.
- [ ] Close and revocation are generation-safe and idempotent from every path.
- [ ] Protected traffic has no unauthenticated admission, silent TCP/L4
  fallback, cross-Service rewrite, drop or reorder.
- [ ] After each gate, opened equals closed and active/pending/tasks/backend
  entries are zero.
- [ ] Optimization results use repeated runs with the frozen topology,
  placement and 2.5 GHz clock; otherwise they are not accepted.

## Required order

```text
P0 production security/lifecycle
        |
        +--> P1 exact endpoint semantics (if product requirements need it)
        |
        +--> P2 measured optimization
                 |
                 +--> P3 equivalent ARM/x86 study
```

Do not mix P0 correctness changes with P2 performance changes in one hardware
comparison.
