# DPUmesh Naming & Identity — Whitepaper (design)

> **Status: PROPOSED — none of this is implemented.** The as-built state is
> `dmesh_create_channel(int service_id)` / `dmesh_create_qp(cq, int)` ([API.md](API.md) §3), the
> `DMESH_PRELOAD_SVC/_LISTEN/_MAP/_REGISTRY` env tables ([API.md](API.md) §7), and the
> `DPUMESH_SERVICE_ID` override ([API.md](API.md) §9). This document is the design that replaces
> all three. Implementing it obsoletes those three sections.

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
backends of service 7**, and the LB round-robins across the union. Because a connection is sticky
by default (`px_resolve_backend`, `doca/dpu_proxy.c`), a conn that lands on the wrong application
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
              DPU:   pods[] where service_id==7 && live  →  sticky pin, else RR pick
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

## 6. Before / after

| | Before (as-built) | After |
|---|---|---|
| native identity | `dmesh_create_channel(7)` | `dmesh_create_channel()` |
| core identity override | `DPUMESH_SERVICE_ID=7` — an int, silently beats the caller | `DPUMESH_SERVICE=checkoutservice` — a name, webhook-written |
| native peer | `dmesh_create_qp(cq, 3)` | `dmesh_create_qp(cq, "checkoutservice")` |
| shim identity | `DMESH_PRELOAD_SVC=15` — typed | webhook, derived from k8s labels |
| shim listen port | `DMESH_PRELOAD_LISTEN=9095` — typed | the Service's `targetPort` |
| shim route table | `DMESH_PRELOAD_MAP` + `_REGISTRY` — typed | ConfigMap, controller-fed, live |
| `service_id` | app code, YAML, public header | **core only** |
| user's deploy | 3 env vars + hand-mounted volumes | one namespace label |
| DPU / wire ABI | — | **unchanged** |

```yaml
# BEFORE — bench/k8s/pods.yaml:280. A human chose "15".
env:
- { name: PRELOAD_SVC, value: "15" }
- { name: ECHO_PORT,   value: "9095" }
# + preload_runner.c writes /tmp/dpumesh_registry at runtime and re-exports both as DMESH_PRELOAD_*

# AFTER
metadata: { labels: { app: checkoutservice } }     # ...that is the whole diff.
```

**Deleted from `dmesh_preload.c`:** `struct route_ent`, `g_route[]`, `g_route_n`, `g_svc`,
`g_listen_port`, `route_add`, `route_lookup`, `route_load_registry`, `PRELOAD_MAX_MAP`, the four
`DMESH_PRELOAD_*` vars (`_DEBUG` survives), and the ACTIVATION block of its header comment.
**From `dmesh_core.c`:** the `DPUMESH_SERVICE_ID` int override. Net ≈ −50 lines in the shim,
**zero added.** Untouched: fd realization, the dispatcher, `pin_route`, the
`getpeername` truth (`e->paddr`), the lost-wakeup discipline. The change is confined to where
config comes from.

---

## 7. Out of scope, and the gaps this leaves

- **Readiness is not wired, and this is the largest remaining gap — it is live, not future.**
  Round-robin LB over a service's backends already ships (`lb_pick`, `doca/dpu_worker.c`), and the
  backend set is derived on demand from `pods[]` gated on **`registered + dma_ready`**
  (`doca/object.h`) — that is *transport* liveness, not k8s **Ready**. A pod whose channel is up
  but whose `readinessProbe` has not passed is therefore **already in the rotation**, and since a
  conn is sticky by default (`px_resolve_backend`), one that lands there **stays** — stickiness
  turns a transient warm-up into a permanent misroute for that connection. Envoy would not route
  to it at all. Naming does not fix this; it is a separate design, and it is the real remaining
  blocker to "deploy it on k8s and it just works."
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
