# DPUmesh Naming & Identity — Whitepaper (design)

> **Status: Phase 0 IMPLEMENTED (host C-side) and HW-validated via `bench.sh`; Phases 1–4
> remain PROPOSED.** Phases 1–3 build the naming control plane; Phase 4 carries Kubernetes
> endpoint readiness into DPU routing. See [§8](#8-implementation-phasing-this-document--code).
> Phase 0 replaced the as-built `dmesh_create_channel(int service_id)` / `dmesh_create_qp(cq, int)`
> ([API.md](API.md) §3), the `DMESH_PRELOAD_SVC/_LISTEN/_MAP/_REGISTRY` env tables ([API.md](API.md) §7),
> and the `DPUMESH_SERVICE_ID` override ([API.md](API.md) §9) with the name-only public API and
> `src/dmesh_resolve.c` — obsoleting those three sections. The `dpumesh-controller` and admission
> webhook that feed the registry from k8s in production are Phases 1–2.

**DPUmesh invents no names.** The **k8s Service** is the namespace; its DNS name is the only
identifier a user ever types. The wire's `service_id` (int8, `[0,127]`) is an **intern handle**
for that name — allocated by a controller, consumed by the transport, and **absent from app
code, from public headers, and from YAML.**

Today the user picks that integer by hand in **three** unrelated places: `dmesh_create_channel(7)`
in source, `DMESH_PRELOAD_SVC=15` in a pod spec, and `DPUMESH_SERVICE_ID=7` in the env — which
silently **overrides the caller's argument** inside the core (`src/dmesh_core.c`). Nothing
arbitrates any of them, and the failure is not a loud one: the DPU derives a service's backends by
scanning `pods[]` for `service_id == svc` (`collect_live_hosts`, `doca/dpu_worker.c`), so two
*different* services that both pick `7` are not a collision that one of them wins — **both become
backends of service 7**, and the LB round-robins across the union. Because a connection with no
codec is pinned to its first backend (`px_resolve_backend`, `doca/dpu_proxy.c`), a conn that lands on the wrong application
**stays on it for its whole life.**
The integer is a **wire address wearing a config's clothes**, and this design's whole content is
moving its provenance from the user to a control plane, changing **nothing else**.

> That third path is also the good news. An "identity from outside the app" hook **already exists
> at exactly the right layer** — `dmesh_core.c`, below both façades. This design does not add a
> mechanism; it narrows that one to take a *name* and fills it from a *controller* instead of a
> human.

## The contract

| | |
|---|---|
| **User writes** | ordinary k8s: a Deployment + a Service. Zero DPUmesh-specific fields. |
| **User runs, once per namespace** | `kubectl label namespace <ns> dpumesh-injection=enabled` |
| **User names a peer by** | its **k8s Service name** — the same string `getaddrinfo()` already takes |
| **User never sees** | `service_id`, `pod_id`, ClusterIP→service tables, registration |

---

## 1. What the user does

### 1a. LD_PRELOAD — zero lines of code, zero lines of YAML

```yaml
# Ordinary k8s. Not one line was added for DPUmesh.
apiVersion: apps/v1
kind: Deployment
metadata: { name: checkoutservice }
spec:
  template:
    metadata: { labels: { app: checkoutservice } }
    spec:
      containers: [{ name: server, image: myco/checkout:1.0 }]
---
apiVersion: v1
kind: Service
metadata: { name: checkoutservice }
spec:
  selector: { app: checkoutservice }
  ports: [{ port: 5050, targetPort: 5050 }]
```
```sh
kubectl label namespace default dpumesh-injection=enabled    # once per namespace. That is all.
```

Identical to `istio-injection=enabled`, deliberately: an app that runs under an Envoy/Linkerd
sidecar must run on DPUmesh with **no code and no deploy change** — only the transport differs.

### 1b. Native API — the same YAML, the same label, two lines

```c
dmesh_channel_t *ch = dmesh_create_channel();                 // identity: injected, not declared
dmesh_qp_t      *qp = dmesh_create_qp(cq, "checkoutservice"); // the k8s Service name, verbatim
```

`"checkoutservice"` is **the same string** the preloaded app hands `getaddrinfo()`. The two
surfaces name the same thing the same way; the only difference between them is linking vs
preloading.

---

## 2. What DPUmesh does — three pieces

| Piece | Is | Does |
|---|---|---|
| **`dpumesh-controller`** | an ordinary k8s controller. **No CRD, no new object kind.** | watches Services → interns each name to an int8 → publishes the table as a **ConfigMap** |
| **admission webhook** | the standard mutating webhook, gated on the namespace label | per pod: injects `LD_PRELOAD` + `libdpumesh.so` + `/dev/infiniband` + the registry volume, and **computes the pod's identity** from its labels |
| **`src/dmesh_resolve.c`** | one table. No DOCA, no thread, no hot path. | answers four questions for **both** façades; `inotify`-reloads the file |

**The registry** — a ConfigMap mounted at `/etc/dpumesh/registry`. ConfigMap volumes update
in place, so a live control-plane feed needs **no xDS server and no new protocol**:
```
# ClusterIP:port      name              svc
10.96.0.15:5050       checkoutservice   7
10.96.0.22:7070       paymentservice    9
```

**Identity** — the webhook matches the pod's labels against the namespace's Service selectors and
injects the result as env: `DPUMESH_SERVICE=checkoutservice`, `DPUMESH_PORT=5050`. Unset =
pure client (`DMESH_SVC_NONE`). This **replaces** `DPUMESH_SERVICE_ID` (`src/dmesh_core.c`,
[API.md](API.md) §9), which is deleted — an int override surviving next to a name would restore
the two-sources-of-identity defect this document exists to remove.

> **Stated rather than hidden: the mechanism is still env.** The defect in today's
> `DMESH_PRELOAD_SVC=15` was never the env var — it was that a **human typed the integer**. A
> webhook deriving `DPUMESH_SERVICE` from a k8s label is what Istio does, and it is the fix. The
> provenance changes; the transport does not.

**A pod resolves its own identity through the same table it uses for peers** —
`resolve_name(getenv("DPUMESH_SERVICE"))` → its int8 → `dpumesh_init(ctx, svc, cfg)`. The core's
init signature is unchanged; one table serves both directions.

---

## 3. The two paths converge

```
  LD_PRELOAD app (unmodified)                        Native API app
  ═══════════════════════════                        ══════════════
  getaddrinfo("checkoutservice")                     dmesh_create_qp(cq, "checkoutservice")
        │   CoreDNS — unchanged, not a fork                │
        ▼                                                  │
  connect(10.96.0.15:5050)                                 │
        │   shim interposes libc                           │
        ▼                                                  ▼
   resolve_addr(10.96.0.15, 5050)                  resolve_name("checkoutservice")
        └──────────────────┬───────────────────────────────┘
                           ▼
              src/dmesh_resolve.c  ── THE SAME TABLE ──  /etc/dpumesh/registry
                           ▼
                        svc = 7                    ← the integer is born here
                           ▼
                 dmesh_qp_open(cq, 7)              ← and appears nowhere above this line
                           ▼
              wire:  dst = (svc 7, BLANK, BLANK)   ← unchanged (API.md §6)
                           ▼
              DPU:   pods[] where service_id==7 && live  →  RR pick (per message if the
                     service runs a codec; else once, then the conn is pinned)
```

Two keys, one table: the shim resolves by `ClusterIP:port` (it must decide "is this meshed?"
**before** creating anything — an unlisted address falls through to kernel TCP, exactly as
`route_lookup` returns `-1` today); the native API resolves by name. Both land on the same row.

```
  k8s (nothing DPUmesh-specific)          dpumesh-controller
  ══════════════════════════════          ══════════════════
  Service checkoutservice ──────watch────▶ intern "checkoutservice" → 7
    ClusterIP 10.96.0.15:5050                  │
  Namespace dpumesh-injection=enabled          ├──▶ ConfigMap  (the registry, live)
                                               └──▶ webhook    (per pod: LD_PRELOAD, volumes,
                                                                DPUMESH_SERVICE, DPUMESH_PORT)
                                                        │
                            ┌───────────────────────────┘
                            ▼  inside the pod
                  src/dmesh_resolve.c ── identity() · listen_port() · resolve_name() · resolve_addr()
                       ▲                              ▲
              dmesh_core.c                     dmesh_preload.c
               REGISTER(svc)                   connect() / listen()
               create_qp(name) → qp_open(int)   → qp_open(int)
                       ▲
              <dpumesh/dmesh.h> ── the app (names only)
```

---

## 4. Where the integer lives

**Invariant: `service_id` never appears in a public header.**

```c
/* <dpumesh/dmesh.h> — public. Names only. */
dmesh_channel_t *dmesh_create_channel(void);
dmesh_qp_t      *dmesh_create_qp(dmesh_cq_t *cq, const char *service_name);

/* src/dmesh_core.h — internal. Integers only. Shared by the shim and the wrapper above. */
dmesh_qp_t *dmesh_qp_open(dmesh_cq_t *cq, int svc);
int  dmesh_config_load(const char *path);      /* NULL → /etc/dpumesh/registry; idempotent */
int  dmesh_config_listen_port(void);           /* -1 = not a server */
int  dmesh_resolve_name(const char *name);     /* -1 + ENOENT */
int  dmesh_resolve_addr(uint32_t ip, uint16_t port);   /* -1 = not meshed → kernel TCP */
```

Public `dmesh_create_qp(cq, name)` is a two-line wrapper: `resolve_name` → `dmesh_qp_open`. It
stays in **`dmesh_core.c`**, where the public lifecycle already lives (`dmesh_create_qp` is
defined there today, and `src/dmesh_core.h` states the split explicitly) — **not** in
`dmesh_api.c`, which is by its own contract the verbs data path only.

**The shim is not a client of the public API** ([API.md](API.md) §7) — it sits on the core and
calls `dmesh_qp_open` with its integer directly. So a name-only public surface costs the shim
nothing, and its lookup cost is unchanged (today's `route_lookup` is already an integer-keyed
scan).

**Rejected: a pre-resolved handle** (`dmesh_svc_t s = dmesh_resolve(ch, name)` then
`create_qp(cq, s)`). It mimics `rdma_resolve_addr` → `rdma_create_qp`, but that split exists
because rdma_cm's resolve is a genuinely **asynchronous network operation** (ARP + an SM path
query) with its own completion event. Here resolution is a lookup in a local file-backed table —
the reason does not transfer. Worse, the handle **reintroduces the defect being removed**: an
app-cached integer wire address that goes stale when the controller recycles an id, silently
addressing whatever now owns it. Resolving at point of use, inside the transport, is the whole
point. If per-`create_qp` lookup ever measures (it is a control-path op against a ~3 µs
reconnect), a pre-resolve fast path can be **added** later; a public type cannot be removed.

`dmesh_qp_t.dst_service` (`include/dpumesh/dmesh.h`) stays readable for diagnostics. Reading it is
harmless; with a name-only constructor, round-tripping it back into `create_qp` **will not
compile.**

---

## 5. Decisions, stated rather than hidden

**Who interns the name → the controller, not the DPU.** The tempting symmetry is to have the DPU
intern names at REGISTER and return `service_id` alongside `pod_id` (which it already assigns —
today `pod_id` is DPU-owned while `service_id`, the field beside it, is caller-declared). It was
**rejected**: a client needs the id of a **peer** it never registers, so the DPU would still have
to distribute the table — the same ConfigMap, **plus** a wire-ABI change to REGISTER and string
handling on the DPU. It does not remove the distribution problem; it adds cost on top of it.
Controller-interned costs **zero DPU changes and zero wire-ABI bits**: REGISTER still carries an
int8, and the DPU still derives a service's backends by scanning `pods[]` for `service_id == svc`,
untouched. Keeping the data plane still is what "a transport-layer swap" means.

**Id scope → cluster-global, for now.** The int8 space could be scoped per node (raising the cap
from 128 services per *cluster* to 128 per *node*, which also matches `MAX_PODS` being a per-node
cap). Deferred: with one DPU, a live pod cap of 32, and benchmark meshes of ~10 services, 128 does
not bind — and switching to node scope later is a **controller-internal change, invisible to the
user, the API, and the wire.** Do not buy complexity for a constraint that is not binding.

**Bench needs no k8s and no API escape hatch.** `DPUMESH_CONFIG=<path>` points the resolver at a
hand-written file, so the harness plays the controller — which is exactly what
`bench/validators/preload_runner.c` already does when it writes `/tmp/dpumesh_registry`. One
mechanism, exercised by tests and production alike; no programmatic identity override is added.

---

## 6. Remaining gaps and scope boundaries

- **Readiness is not wired, and this is the largest remaining gap until Phase 4 (§8).**
  Round-robin LB over a service's backends already ships (`lb_pick`, `doca/dpu_worker.c`), and the
  backend set is derived on demand from `pods[]` gated on **`registered + dma_ready`**
  (`doca/object.h`) — that is *transport* liveness, not k8s **Ready**. A pod whose channel is up
  but whose `readinessProbe` has not passed is therefore **already in the rotation**, and on a
  service with no codec a conn that lands there **stays pinned to it** — turning a transient
  warm-up into a permanent misroute for that connection. Envoy would not route
  to it at all. Naming alone does not fix this; Phase 4 supplies the separate endpoint-readiness
  signal and keeps unready endpoints out of new backend selections.
- **A pod backing several Services is refused.** Legal in k8s, but a channel advertises one
  `service_id` and the shim already permits one listener per process ([API.md](API.md) §7). The
  webhook must **fail admission loudly** when >1 selector matches — never silently pick one.
- **ConfigMap propagation is ~60 s** (kubelet sync). This bounds only *"a new Service becomes
  callable"* — comparable to DNS TTL. Endpoint churn does not ride this path: the DPU derives the
  backend set live from its own `pods[]` registrations.
- **mTLS / SPIFFE** is untouched here. Identity in this document is a **routing** name, not a
  security principal.
- **Multi-DPU.** A backend set is derived from one DPU's `pods[]`; a future DPU→DPU hop must
  translate ids at the boundary or move to a cluster-global space — the point at which the §5
  scoping decision gets revisited.

---

## 7. Implementation phasing (this document → code)

**The whole of the public API change is Phase 0; Phases 1–4 add none.** This is the design's
central property, not an accident of scheduling: `dmesh_create_channel(void)` has no argument to
carry identity, so its only source is the resolver — which means the signature change, the
resolver, and the deleted `DPUMESH_SERVICE_ID` override are one indivisible commit. Phases 1–3
only change **who writes the registry file and the env**; the code that consumes them
(`src/dmesh_resolve.c`, the shim, the wrapper) is frozen once Phase 0 lands. *Provenance moves;
the transport does not.* Phase 4 is separate: it adds internal registration/control messages and
DPU routing state, but still adds nothing to the application API. Thus the riskiest change — a
public-header break across every caller — is completed and `bench.sh`-proven before any k8s
scaffolding or readiness wiring exists.

| Phase | Runtime | Adds to the API? | State |
|---|---|---|---|
| **0 — host resolver** | this repo (C) | **the entire change** | **DONE, HW-validated** |
| **1 — controller** | k8s (Go), new | no — writes the ConfigMap | proposed |
| **2 — webhook** | k8s (Go), new | no — injects env + volumes | proposed |
| **3 — live reload** | this repo (C) | no — swaps the table under readers | proposed |
| **4 — endpoint readiness** | k8s node agent + host/DPU control path | no — internal control ABI only | proposed |

### Phase 0 — the host-side resolver *(implemented)*

- **`src/dmesh_resolve.c`** — one file-backed table, both façades, load-once (so post-load reads
  need no lock — live reload is deliberately deferred to Phase 3). It answers `identity()` /
  `listen_port()` / `resolve_name()` / `resolve_addr()`; the integer `service_id` is born here and
  appears in no public header. Registry format grew a column: `IP:port name svc`.
- **Public API is name-only.** `dmesh_create_channel(void)` (identity from `$DPUMESH_SERVICE` via
  the table), `dmesh_create_qp(cq, const char *name)`. The integer entry `dmesh_qp_open(cq, int)`
  is internal (`src/dmesh_core.h`), shared by the shim and the two-line public wrapper. The
  `DPUMESH_SERVICE_ID` override is **deleted** — an int identity beside a name is the
  two-sources defect this removes.
- **Shim rewired** to the resolver: `DMESH_PRELOAD_{SVC,LISTEN,MAP,REGISTRY}` deleted (`_DEBUG`
  kept), `connect()`→`resolve_addr`, `listen()`→`config_listen_port` (`$DPUMESH_PORT`),
  identity→`$DPUMESH_SERVICE`.
- **Bench dogfoods the names.** Every validator addresses by k8s-Service name and takes its
  identity from `$DPUMESH_SERVICE`, resolved through a hand-authored registry baked at the
  resolver's default `/etc/dpumesh/registry` (`bench/k8s/registry`) — the bench harness *is* the
  controller (§5). This exercises `resolve_name` + `config_identity` on real hardware, not just
  the transport. Receipts: [bench/RESULT.md](../bench/RESULT.md).

### Phase 1 — `dpumesh-controller` *(proposed)*

Watch Services → intern each name to an int8 → publish the `IP:port name svc` registry as a
ConfigMap. **The binding correctness constraint is restart-stable id allocation:** the name→int8
map must survive a controller restart (persist it in the ConfigMap itself or a Service
annotation), or a recycled id silently re-addresses whatever now owns it — precisely the stale-
address hazard §4 rejects the pre-resolved handle to avoid. Refuse the 129th service loudly
rather than wrap.

### Phase 2 — admission webhook *(proposed)*

Gated on the namespace label, per pod: inject `LD_PRELOAD` + `libdpumesh.so` + `/dev/infiniband`
+ the registry volume, and derive `$DPUMESH_SERVICE` / `$DPUMESH_PORT` from the pod's labels vs
the namespace's Service selectors; **fail admission loudly on >1 selector match** (§7). **The gap
to close is the self-identity bootstrap order:** a pod resolves its *own* name through the same
table, so it must not start before the controller has interned that name into the mounted
ConfigMap — the webhook and controller must coordinate (block admission until interned, or the
pod retries `resolve_name` until its own row appears). ConfigMap propagation (~60 s) bounds this.

### Phase 3 — live reload *(proposed)*

Replace Phase 0's load-once with an `inotify` snapshot swap so a controller-pushed ConfigMap
update takes effect without a pod restart. This is the point — and the *only* point — at which
the table becomes mutable under concurrent readers, so it needs an atomic pointer swap of an
immutable snapshot (double-buffer / RCU), not an in-place edit. Phase 0 is load-once expressly to
keep this concurrency out of the correctness-critical provenance change.

### Phase 4 — Kubernetes endpoint readiness gating *(proposed)*

Phase 4 makes the DPU's backend-selection set match Kubernetes Service endpoint readiness. The
source of truth is the Service's
[**EndpointSlice conditions**](https://kubernetes.io/docs/concepts/services-networking/endpoint-slices/#conditions),
not whether the DPU transport happens to be connected: `ready=true` admits new traffic;
`ready=false` or `terminating=true` removes the endpoint from new selections. The agent honors the
published `ready` value (including a Service's `publishNotReadyAddresses` policy) rather than
reconstructing Pod Ready independently. This is a control-path update only — no readiness lookup,
file read, or Kubernetes call enters the data hot path.

#### Identity and control path

Readiness must attach to a **pod incarnation**, not the DPU-assigned `pod_id`: a `pods[]` slot and
its small integer id are reused after disconnect. The webhook therefore injects a stable
`DPUMESH_ENDPOINT_UID` from the Kubernetes Pod UID (via the downward API), and the host includes
that UID in its internal REGISTER message. It remains absent from the public C API and application
configuration.

A node-local `dpumesh-agent` watches EndpointSlices for pods scheduled on its node and sends a
full snapshot followed by deltas to the node's DPU management path. Each update is keyed by Pod
UID and carries `{ready, serving, terminating, agent_session, revision}`. The agent aggregates all
EndpointSlices of a Service and de-duplicates repeated endpoints before publishing its snapshot.
The DPU caches an update that arrives before REGISTER and applies it when the matching UID
registers; an old UID can never make a reused `pods[]` slot ready. A fresh random `agent_session`
on agent start, monotonically increasing `revision` within that session, and a full snapshot after
reconnect prevent delayed updates from reviving stale state.

Only the node agent may write this state. Application pods report their UID at registration but do
not report their own readiness, because a process cannot be the authority for the result of its
Kubernetes readiness probe.

#### Two predicates, not one

Do **not** add Kubernetes Ready to the existing `pod_data_ready()` predicate everywhere. The data
path needs two distinct questions:

- `pod_transport_live(p)`: registration, DMA mappings, and communication channel are usable. This
  gates memory access and delivery on an already-established connection.
- `pod_route_ready(p)`: `pod_transport_live(p)` plus the latest EndpointSlice state has
  `ready=true` and `terminating!=true`. (`ready` already represents serving-and-not-terminating in
  the normal Kubernetes case; `serving` is retained separately for drain policy and diagnostics.)
  This gates `lb_pick`, `collect_live_hosts`, L7 host overrides, and every other **new** backend
  selection.

This distinction preserves L4 connection semantics. A Ready→NotReady transition immediately stops
new QPs and new codec-routed messages from selecting the endpoint, but an existing pinned
connection continues on its original backend while the transport remains live. It is drained, not
silently rebound mid-stream. A transport disconnect remains terminal and follows the transport's
normal close/error path. A Terminating endpoint follows the same no-new-selection rule; Kubernetes
grace and application shutdown drain existing pins before the channel disappears.

#### State and failure policy

A newly registered endpoint starts **NotReady** and becomes selectable only after a matching
EndpointSlice snapshot/update arrives (fail closed). On node-agent or DPU reconnect, the agent sends
a complete snapshot before deltas. If the watch is lost, the agent reconnects and reconciles from a
fresh list; the DPU never guesses readiness from `dma_ready`. A bounded stale-state TTL may keep
existing pins draining, but expiry must remove the endpoint from new selection.

The DPU state needed per pod is small and control-plane-owned: Pod UID (or a collision-resistant
fixed-width digest), readiness flags, agent session/revision, and last-update time. Service
membership still comes from the Phase 1 registry plus REGISTER's `service_id`; readiness never
allocates or changes a service id.

#### Required validation

Phase 4 is complete only when the hardware validators demonstrate all of the following:

1. A transport-live but NotReady backend receives no new connections.
2. A NotReady→Ready transition admits traffic without restarting the pod or DPU.
3. Ready→NotReady/Terminating stops new selections while an existing pinned stream drains on
   the same backend.
4. A transport disconnect closes/fails the existing stream rather than rebinding it mid-stream.
5. Pod deletion plus slot/id reuse ignores delayed readiness updates for the old Pod UID.
6. Agent and DPU restart/reconnect converge from a full snapshot before admitting traffic.
7. L4 `lb_pick`, L7 `collect_live_hosts`, and explicit L7 host override all use the same
   `pod_route_ready` eligibility rule.
