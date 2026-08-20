# A Complete Service Mesh on the DPU

This is the single design document for the cluster milestone. It absorbs the
former `PLAN.md`. Sections marked **[BUILT]** describe what exists in the tree
and are stated with the function that implements them; sections marked
**[DESIGN]** are not implemented, and every stage below names the files it
edits, the data it moves and the gate that decides whether it is done.

S1–S12 are built. What is not is the DPU-to-DPU transport itself, which this
document places *Out of scope* and which S7 and S8 name explicitly as the thing
they leave for it: the peer channel's rules, bounds, handles and custody exist
and are tested against a transport seam, and the bytes that cross a node
boundary are what the seam still has nothing behind.

`design/CORE.md`, `design/CONTROL.md` and `design/L7.md` describe the built
single-node system in depth; measurements live in `bench/report/`.

**Assumed available:** a DPU-to-DPU RDMA transport. The data path then has one
shape on both sides of a node boundary:

```text
intra-node   host TX mmap → DPA → staging → SG-DMA         → host RX mmap
inter-node   host TX mmap → DPA → staging → RDMA → staging → host RX mmap
```

Only the middle hop differs. Arrival custody, lanes, credits and reverse
publication are unchanged — with one qualification this document is explicit
about: the node-local custody story is two stories, one per data-path mode, and
the boundary extends each differently (*Two custody domains*).

## The claim

A service mesh provides traffic control, zero-trust security and observability.
The claim is that a DPU can provide all three for the Pods of its node, and that
moving the proxy onto the DPU makes two of them *better* than a sidecar can,
at the cost of one structural concession.

## Threat model

One DPU in the fleet is compromised. Every other component behaves.

This is the decision the rest of the document rests on, and it is the reason
several mechanisms below exist at all. Without it a destination could simply
believe what a peer DPU asserts, and the design would be considerably smaller.

| Party | Assumed | Consequence |
|---|---|---|
| workload Pod | hostile | states nothing about itself; the assertion names it |
| host kernel | honest | it is the source of attestation evidence |
| node agent | honest | it holds the only key that can assert for its node |
| **one DPU** | **hostile** | it must not be able to speak for another node's Pods |
| other DPUs | honest | — |
| controller | honest | it holds the only key that can bind a Pod to a node |

A compromised DPU keeps whatever it already has: its own Pods' memory, their
traffic, and its own reported metrics. Those are irreducible — a sidecar and a
`ztunnel` lose exactly the same on a compromised node (a compromised `ztunnel`
node holds every resident workload's certificate). What the design denies it is
reach beyond its node.

```text
                        sidecar   ztunnel   DPUmesh          DPUmesh
   one node compromised                     (peer believed)  (this design)
   ─────────────────────────────────────────────────────────────────────
   its own Pods           lost      lost      lost             lost    ← irreducible
   other nodes' Pods      safe      safe      LOST             safe
   other nodes' traffic    n/a      safe      LOST if the      safe
                                              key is shared           (pairwise keys)
```

Three requirements follow, and they are marked ⟨T⟩ where they appear below:

- identity claims about another node's Pods are refused;
- inter-DPU keys are pairwise, so one compromised DPU decrypts only its own
  conversations;
- a peer consumes bounded resources at a destination **and a stalled peer holds
  bounded resources at a source**.

One consequence deserves stating before any mechanism: **anything cluster-scoped
that a DPU verifies must be verified with a key the DPU cannot use to sign.**
The node-scoped feeds may stay symmetric — a DPU that forges its own membership
feed only harms its own node, which the model already concedes — but a
cluster-scoped generation verified with a shared symmetric key would hand one
compromised DPU the power to forge topology for the whole cluster. The
generation is therefore Ed25519-signed and the DPU holds public keys only.

## Where each part of a mesh can live

Applying the placement test — a function may move away from the workload when
its inputs travel with the traffic and its state can be configured remotely; it
must stay when it needs a secret that cannot travel or evidence that exists only
where the workload runs:

| Mesh function | On the DPU? | Why |
|---|---|---|
| routing, load balancing, splitting, retries | yes | inputs arrive with the bytes; tables come from the control plane |
| authorization | yes | inputs arrive with the bytes; rules come from the control plane |
| encryption | yes, and better | the wire starts at the DPU, and the key never enters host memory |
| observability | yes, and better | every byte crosses the DPU, and the workload cannot alter what is recorded |
| **authentication** | **no** | the evidence is a fact of the host kernel |

One row is negative. Everything else the DPU can own, and two rows it owns
better than a proxy inside the Pod.

## The concession: the DPU is a different machine

```text
host   Linux 5.15.0-186-generic   x86_64      ← the Pod runs here
DPU    Linux 5.15.0-1074-bluefield aarch64    ← its own CPU, memory and kernel
                    ↕ PCIe
```

The DPU is not a layer beneath the host. It is a separate computer across PCIe.
It can read host memory the host explicitly exported, and nothing else. It
cannot read the host kernel's process table, cgroups or namespaces, and DOCA
Comch carries no peer credential the way `SO_PEERCRED` does.

Identity evidence is a fact the kernel knows, not a byte a process hands over.
Anything a process hands over is something it chose, which makes it a claim. A
DPU therefore cannot, by itself, answer who is on the other end of a channel —
not as an implementation gap but as a property of where it sits.

A mesh proxy sits somewhere on one axis, and both of this document's themes are
consequences of where:

```text
   near the workload  ◀─────────────────────────────────▶  far from the workload
   identity: free                                          identity: established
   isolation: weak                                         isolation: strong

   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │   Linkerd    │    │Istio ambient │    │  Canal Mesh  │    │   DPUmesh    │
   │Istio sidecar │    │   ztunnel    │    │   on-node    │    │              │
   └──────────────┘    └──────────────┘    └──────────────┘    └──────────────┘
    in the Pod          host process        host process,       a different
                        per node            per node, plus a    machine,
                                            remote gateway      across PCIe

    sees the Pod:       sees the Pod:       sees the Pod:       sees the Pod:
    it IS the Pod       yes, same kernel    yes, same kernel    no
```

Every shared-proxy mesh keeps a host-resident component for this reason:

| System | Proxy | Sees the Pod | Because |
|---|---|---|---|
| Linkerd, Istio sidecar | in the Pod | is the Pod | kubelet mounts the Pod's own token |
| Istio ambient `ztunnel` | host process, per node | yes | same kernel; the CNI agent tells it |
| Canal Mesh on-node proxy | host process, per node | yes | same kernel |
| **DPUmesh** | **DPU, per node** | **no** | different machine |

So a DPU mesh is necessarily a split design with a host-resident attester. The
node agent is that component. It is not deployment tooling.

## The prize: the proxy is outside the tenant's reach

Canal Mesh's motivating problem is intrusion — the sidecar shares the Pod, so a
fault or a compromise in one reaches the other, and the resources it consumes
were bought for the app. Ambient reduces this by moving to a host process.

The DPU removes it. The proxy runs on a CPU the workload cannot schedule on,
in memory it cannot address, under a kernel it cannot call. Two consequences
that no host-resident proxy has:

- **Keys never enter host memory.** A sidecar holds its Pod's private key in the
  Pod's address space. Here the material for the inter-node path lives only on
  the DPU.
- **Observability is not falsifiable by the workload.** Counters and traces are
  taken on a machine the workload cannot write to.

The concession above is the price of these.

---

# What is built today [BUILT]

## Deployed architecture

```text
HOST / Kubernetes                                      DPU

+---------------+  Unix socket  +----------------+     +----------------+
| Service Pod   |-------------->| trusted node   |     | Comch control  |
| app + client  | nonce + SVC id| agent DaemonSet|     | thread         |
| thread        |<--------------| peer/cgroup +  |     | nonce/sig/key  |
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
 | stock Linkerd  |<---| node agent  |<---------| worker         |
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
[`figures/control_plane.png`](figures/control_plane.png).

The production path has no DPUmesh mock control-plane fallback. The remaining
`mock-identity`, `mock-policy` and `mock-destination` sources belong only to the
upstream `linkerd-app-integration` test crate (the proxy is a git submodule
fork at `linkerd/port/linkerd2-proxy`, branch `dpumesh-integrate`) and are not
linked or deployed.

## Where the pieces live

| Concern | Files |
|---|---|
| Assertion format, verification, keyring, feed envelope | `doca/workload_grant.{c,h}` |
| Registration, revocation scan, teardown | `doca/comch_server.{c,h}` |
| Membership feed parse/adopt | `doca/pod_membership.{c,h}` |
| DPU-side state | `doca/object.h` (`struct objects`, `struct pod_state`) |
| Control-thread loop | `doca/dpu_worker.c` (`dpu_drain_iteration`) |
| L7 admission, drain and fail-closed | `doca/dpu_proxy.c` (`px_parse_l7`, `px_l7_open_conn`) |
| Host registration client | `src/dmesh_core.c` (`init_control_path`), `src/dmesh_attest.{c,h}` |
| Peer resolution (DPU-answered, cached one generation interval) | `src/dmesh_resolve.c`, `RESOLVE` handler in `doca/comch_server.c` |
| Adapter sessions, target feed | `linkerd/rust/src/lib.rs` |
| Backend registry, metrics, IO | `linkerd/port/linkerd2-proxy/linkerd/doca/src/*.rs` |
| Per-session outbound stack | `linkerd/port/linkerd2-proxy/linkerd/app/src/lib.rs` |
| DMesh connector | `linkerd/port/linkerd2-proxy/linkerd/app/outbound/src/tcp/connect.rs` |
| C↔adapter ABI | `linkerd/include/dmesh_l7.h`, `linkerd/shim/l7_null.c` |
| Node agent | `bench/workload_attest_agent.py`, `bench/k8s/workload-agent.yaml` |
| Keyring, agent deploy, membership push | `bench/workload_attest.sh` |
| Service target feed | `bench/linkerd_service_registry.sh` |
| Identity material | `bench/linkerd_identity.sh` |
| Control-plane relay (absorbed into the node agent) | `bench/linkerd_cp_relay.py`, `bench/workload_attest_agent.py` |
| Orchestration | `bench/bench.sh` |
| Topology generation adopt, interning, node-key lookup | `doca/topology.{c,h}` |
| Controller (watcher/publisher) | `controller/dpumesh_controller.py` |
| Controller keys, deploy, topology push | `bench/dpumesh_controller.sh`, `bench/k8s/controller.yaml` |
| Peer channel (S7; the transport behind its seam is out of scope) | `doca/peer_channel.{c,h}` |

Switches: `DPUMESH_NODE_NAME`, `DPUMESH_REGISTRATION_KEY_DIR`,
`DPUMESH_FEED_KEY_DIR`, `DPUMESH_TOPOLOGY_FILE`, `DPUMESH_CONTROLLER_KEY_DIR`,
`DPUMESH_ATTEST_SOCKET`, `DPUMESH_MEMBERSHIP_FILE`, `DPUMESH_ADMISSION_FILE`,
`DPUMESH_L7_SERVICE_TARGETS_FILE`, `DPUMESH_L7_FAIL_CLOSED`,
`DPUMESH_L7_LINKERD_WORKER`.

Counters: `dmesh_control_events_total{kind,reason}`,
`dmesh_sessions_declined_total{reason}`,
`dmesh_backend_target_mismatches_total`, `dmesh_session_stack_*`,
`dmesh_sessions_{opened,closed,active}`. (The `dmesh_` prefix and `_total`
suffix are synthesized by the metrics registry; grep for `control_events`, not
the full name.)

## Trust boundary and scope of the current claim

Who is trusted for what, in the deployed configuration:

| Party | Trusted for | Not trusted for |
|---|---|---|
| Service Pod | relaying its own grant and nonce | any claim about its identity, Service, node |
| Node agent (root DaemonSet) | deriving claims from peer credentials and the Kubernetes API; signing assertions and membership | nothing it does not read from an authoritative object |
| DPU verifier | keyring, nonce binding, replay, Service match, membership | — |
| Feed publishers | content of the feeds they sign | a generation the keyring cannot verify |
| Gateway DaemonSet | carrying bytes | minting or terminating mesh identity |

The embedded proxy feeds DMesh traffic into Linkerd's **outbound** stack only.
Precision matters here because a reader of the code will find an inbound stack:
`linkerd-app` builds, binds and serves an inbound listener in every
configuration (`app/src/lib.rs` binds `LINKERD2_PROXY_INBOUND_LISTEN_ADDR`,
deployed as `127.0.0.1:0` with default policy `all-unauthenticated`), and the
adapter calls `config.disable_inbound_policy_discovery()` so that listener
opens no policy watches. No DMesh byte ever enters it — sessions are served by
`dmesh::serve` into the outbound stack. A stock sidecar's inbound half
terminates mTLS, reads the client certificate identity and enforces
`AuthorizationPolicy`; here the destination is a node-local registered Pod
reached by DMA, so no inbound proxy exists in that byte path, and every
outbound stack authenticates to the control plane with the shared
`dpumesh-dpu` certificate.

What that supports asserting:

> DPUmesh receives per-Pod outbound policy from a stock Linkerd control plane and
> enforces it on the DPU, and it admits registrations and Service membership
> cryptographically, with generation-safe revocation.

What it does not support asserting: source-identity-based inbound authorization,
cross-node protection, or a per-Service mix of protected and unprotected
traffic. Those are stages S7–S12.

## What works now

- A Pod cannot supply its Pod UID, namespace, labels, ServiceAccount, node or
  Linkerd workload. `bench/workload_attest_agent.py` derives them from
  `SO_PEERCRED`, the peer cgroup and authoritative Kubernetes objects, re-checks
  the process start time on both sides of the cgroup read (a recycled pid is
  refused), and bounds concurrent requests with a semaphore. Its RBAC is a
  namespaced `Role` and its Pod reads carry
  `fieldSelector=spec.nodeName=<its node>`; the deploy script asserts the
  *absence* of cluster-wide read (`workload_attest.sh` runs
  `kubectl auth can-i` both ways).
- `dmesh_assert_verify_v2` checks canonical form (including a dotted-quad
  `pod_ip`), version, node binding, expiry, connection nonce and the Ed25519
  signature, in that order; the key is the node agent's *public* key, selected
  by the signed key id from an overlap keyring of at most
  `DMESH_REGISTRATION_MAX_KEYS` (4) — the DPU holds no key that can sign an
  assertion, and a DPU with no `DPUMESH_NODE_NAME` refuses to start.
  `dmesh_registration_consume_grant` rejects replay over a 4096-entry ring
  (evicting — the 300 s lifetime and the per-connection nonce are the real
  bound); `pods_register` refuses a Service *name* other than the asserted one,
  refuses a consumed assertion, and interns the compact id from the held
  generation itself — a Service no generation defines fails closed. The signed
  Pod UID, namespace, ServiceAccount and Pod IP are retained on `pod_state`.
- Deleting a Pod or changing its labels withdraws its `(Pod UID, Service)` pair
  from the node membership generation, and `server_progress_membership` closes
  that exact registration through `pod_begin_cleanup`. Withdrawal takes two
  consecutive generations (`DMESH_MEMBERSHIP_ABSENCES_TO_REVOKE`); a missing,
  malformed, oversized, unsigned or rolled-back generation revokes nothing.
- The embedded stock proxy certifies a dedicated `dpumesh-dpu` identity and
  connects to real Linkerd Identity, Policy and Destination services through
  the node agent's host-network relay (TCP pass-through; TLS end-to-end, never
  terminated on the relay).
- Service targets and ready endpoints arrive in a publisher-monotonic versioned
  feed that a session adopts only when its inode/mtime/length stamp changed. A
  selected target that the snapshot places under a **different** Service is
  `TargetMismatch`; an address the snapshot does not place at all is treated as
  the session's own synthetic address and accepted (`Backends::take_session` —
  membership is enforced against cross-Service rewrite, not positively; S11
  tightens this). Required registration selects `DPUMESH_L7_FAIL_CLOSED=1`, so
  a declined protected session is refused rather than continued as TCP/L4.
- Every authoritative feed carries a `signature=<key-id>,<hex>` envelope signed
  by the feed keyring (`DPUMESH_FEED_KEY_DIR`), a directory disjoint from the
  registration keyring in both names and bytes (`workload_attest.sh prepare`
  asserts it), so no feed publisher holds a key that can mint identity; both
  consumers parse only the signed prefix. An unsigned, forged, appended-to or
  unknown-key generation is refused exactly like a malformed one, so it never
  revokes or admits anything. The two keyrings rotate independently
  (`workload_attest.sh feed-rotate-stage|feed-activate|feed-prune`).
- Every session's policy watch is keyed by the workload granted with that Pod's
  registration (`flow.workload`), not by the process-wide
  `LINKERD2_PROXY_POLICY_WORKLOAD`, which is only the fallback for empty values.
- Names on the outside, handles on the inside: no registry file exists. The
  native API and the POSIX facade resolve names and ClusterIPs through the DPU
  (`RESOLVE`/`RESOLVE_ACK`, answered from the held generation, cached one
  generation interval), leaving the mesh is an explicit logged decision, and
  every compact id on the node-local wire is the DPU's own interning.
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

Both gaps earlier revisions listed here are closed: `px_poison` counts
(`stat_poison`, surfaced beside the peer refusal counters — S7), and the host
user units and their `rsync`/`ssh` hop are gone (S6).

---

# Two custody domains [BUILT — stated precisely because S7 depends on it]

"Custody" means: who is holding a byte's capacity, and what event lets the
sender reuse the slot the byte occupied. The single-node tree has **two**
custody semantics, not one, and the boundary design must extend each on its own
terms. Conflating them was the largest error in the previous revision of this
document.

## L4 / opaque connections: custody is end-to-end

An arrival (one coalesced run of DPA completions, at most
`PX_ARRIVAL_COALESCE_MAX` = 64 KiB, the same contiguous run the SG-DMA engine
sources from) is created holding `bytes + 1` custody. The release chain:

```text
px_build_range      pieces claim staging  (claimed_round += len)
DOCA DMA success    batch → PX_BATCH_DONE      ← dst really is the destination
px_lane_retire      DONE batches → emit list      Pod's host RX mmap
px_engine_emit      px_piece_release per piece  (custody: SG op has read them)
px_custody_sub      unfreed hits 0 → queue on rev owner's ack_releases
px_drain_ack_releases  px_rev_append_ack(port, first_seq, count)
host tx_reclaim_ack    the sender's TX window slots return
```

`dmesh_tx_ack_entry { port, seq, seq_count }` names the consecutive run one
staging extent held — one acknowledgement per extent, not per transport unit.

So on this path the sender's capacity genuinely returns only after the bytes
landed in the destination Pod's RX mapping. Three deliberate exceptions, kept
because the alternative is a leak:

- an **errored** batch still releases custody (`px_lane_drop_dead`: "custody
  still released") — capacity returns for bytes that never landed, and the
  affected connection is poisoned;
- a **dropped** range (poison, window drop, conn delete) releases immediately
  via `px_advance` with no DMA;
- a zero-byte (FIN/notify-only) batch never DMAs and completes at submit.

## L7 connections: custody is hop-by-hop, three bounded stages

An L7-produced unit carries **no arrival custody**: `px_unit_attach_chunk`
sets `piece->arr = NULL`, so `px_piece_release` is a no-op for every L7 egress
byte. The stages, each with its own bound and its own release event:

| Hop | Bound | Released when |
|---|---|---|
| Pod TX ring → DPU staging | sender's TX window + `PX_L7_CUSTODY_MAX` (256 KiB per conn: bytes handed to the L7 layer and not yet released) | the Linkerd stack **consumed the staging segments** — the adapter calls `dmesh_l7_release` when `rx_has_data()` goes false, i.e. after `DmeshIo::poll_read` copied them out; the reference shim states it outright: *"Custody is returned for what was copied out of staging, not for what was forwarded."* |
| inside the session | Tokio channel and stack buffers | stack progress |
| egress arena → destination RX | arena chunk pool | the DMA batch carrying the chunk completes — an arena piece's chunk "returns with the unit" at `px_engine_emit` |

The output-mode A/B (`DMESH_L7_TX_RESERVE`, reservation vs copy) selects how
bytes *enter* the arena; it does not move any release point. There is no L7
mode in which the Pod's TX credit is coupled to destination DMA.

The flow-control property survives, but as composition, not as one credit loop:
a slow destination exhausts arena capacity → `l7_emit` returns no space → the
session's write stalls → the stack stops reading → staging fills to
`PX_L7_CUSTODY_MAX` → `dmesh_l7_release` stops → TX credit stops → the Pod
stalls. Every hop is bounded, so the composition is lossless backpressure; what
is lost relative to L4 is the *identity* of the loop — the Pod's credit no
longer names delivered bytes.

## What the boundary must therefore do

- **L4 streams:** the release in `px_engine_emit` is the point that moves. For
  a remote destination, a piece is retired not when the local RDMA post
  completes but when `STREAM_ACK` reports the extent landed in the destination
  Pod's RX mapping. `px_piece_release` gains a deferred list per handle (S7).
- **L7 streams:** the *first* hop is already decoupled and stays as it is. The
  hop that crosses the wire is the third: an arena chunk whose destination is
  remote is retired on `STREAM_ACK`, not on local RDMA completion. The Pod-side
  semantics are unchanged from intra-node L7.
- Both retirements are per (handle, extent run), which is why `STREAM_ACK`
  names `seq_first, seq_count` — the encoding `dmesh_tx_ack_entry` already
  uses, for the same reason.

## The pool caveat ⟨T⟩

Staging is per source Pod (`pod_state.local_mmap` / `dma_buffer`, 64 MiB,
mirroring the host TX ring 1:1 — the DPA writes `staging offset == host TX
offset`), so a stalled destination fills the *stalling Pod's* staging and no
other Pod's. But two shared structures sit beside it:

- the host-side TX **block pool** is per process (128 × 512 KiB at defaults):
  a Pod's stalled QP can starve that same Pod's other QPs of blocks;
- the DPU-side **proxy node pools** (arrivals, pieces, units, and the L7 egress
  chunk arena) are global with one lock. The arrival/piece/unit pools are sized
  for every Pod's full staging simultaneously, so stalls alone cannot exhaust
  them; the **chunk arena is not** — L7 chunks held un-ACKed by one stalled
  remote peer would starve every other L7 conn's egress.

Hence the source-side bound `PEER_TX_INFLIGHT_MAX` in *The peer channel*: the
claim "a stalled peer stalls only the Pods writing to it" is made true by
bounding what one peer's un-ACKed bytes may hold, not assumed.

---

# Components [BUILT]

```text
  ┌───────────────────── Kubernetes cluster ──────────────────────┐
  │  ┌──────────────────┐  ┌───────────────────┐  ┌────────────┐  │
  │  │ linkerd-identity │  │linkerd-destination│  │  dpumesh-  │  │
  │  │      (Pod)       │  │       (Pod)       │  │ controller │  │
  │  └──────────────────┘  └───────────────────┘  │   (Pod)    │  │
  │                                                └─────┬──────┘  │
  │                        kube-apiserver ◀──── watch ───┘         │
  └─────────────────────────────────┬─────────────────────────────┘
                                   │  signed generation
                                   │  node credentials
             ┌─────────────────────┴─────────────────────┐
             ▼                                           ▼
  ┌──────── NODE A ─────────┐                 ┌──────── NODE B ─────────┐
  │ HOST  (x86, Linux)      │                 │ HOST                    │
  │   ┌──────┐   ┌──────┐   │                 │   ┌──────┐              │
  │   │Pod P │   │ Pod  │   │                 │   │Pod Q │              │
  │   └───┬──┘   └───┬──┘   │                 │   └───┬──┘              │
  │       │ AF_UNIX  │      │                 │       │                 │
  │   ┌───▼──────────▼────┐ │                 │   ┌───▼───────────────┐ │
  │   │ node agent        │ │                 │   │ node agent        │ │
  │   │  SO_PEERCRED      │ │                 │   │                   │ │
  │   │  cgroup           │ │                 │   │                   │ │
  │   │  node private key │ │                 │   │                   │ │
  │   └─────────┬─────────┘ │                 │   └─────────┬─────────┘ │
  │ ═══ PCIe ═══╪═════════  │                 │ ═══ PCIe ═══╪═════════  │
  │   ┌─────────▼─────────┐ │                 │   ┌─────────▼─────────┐ │
  │   │ DPU (ARM, Linux)  │ │                 │   │ DPU               │ │
  │   │  public keys only │ │                 │   │                   │ │
  │   │  node credential  │ │                 │   │                   │ │
  │   └─────────┬─────────┘ │                 │   └─────────┬─────────┘ │
  └─────────────┼───────────┘                 └─────────────┼───────────┘
                └── RDMA · mutually authenticated · pairwise keys ──┘
                      one channel per node pair, long-lived
```

Three scopes, one component each.

| Scope | Component | Holds | Can create identity |
|---|---|---|---|
| cluster | `dpumesh-controller` | issuing key; topology | yes |
| node, host | node agent | that node's private key | for its own node only |
| node, DPU | `dpumesh_dpu` | public keys; node credential | no |

## `dpumesh-controller`

Publishes one signed, versioned generation carrying every cluster-wide fact a
DPU needs, and issues the credential each DPU uses to authenticate to its peers.
It lives in a new top-level `controller/` (Python first, like the agent; the
language is not load-bearing). It performs no attestation. It has no host-local
evidence and never asks for it.

### The generation, exactly

One UTF-8 text document. Line grammar (no spaces around separators; one record
per line; `\n` line endings; comment lines start with `#` and are permitted
only *before* `version=`):

```text
version=<u64 decimal, strictly increasing across publications>
node=<node-name>,<rdma-ip>:<rdma-port>,<agent-key-id>,<agent-ed25519-pub-hex64>,<dpu-static-pub-hex64>
pod=<pod-uid>,<node-name>,<namespace>,<service-account>,<pod-ipv4>
service=<namespace>/<name>,<cluster-ipv4>:<port>
endpoint=<namespace>/<name>,<pod-uid>
protected=<namespace>/<name>
signature=<key-id>,<hex128>
```

Field syntax, enforced before anything is adopted:

| Field | Rule |
|---|---|
| `node-name` | DNS subdomain, ≤ 253 |
| `pod-uid` | lowercase RFC 4122 text form, exactly 36 chars |
| `namespace` | DNS label, ≤ 63 |
| `name` (Service) | DNS label, ≤ 63 — **Services are namespace-scoped in Kubernetes, so a Service is never named without its namespace.** A bare name is not a valid identifier anywhere in this design |
| `service-account` | DNS subdomain, ≤ 253 |
| `pod-ipv4`, `cluster-ipv4` | dotted quad; the Pod IP is what makes destination-side `networks` authorization decidable (*Authorization*) |
| `agent/dpu key hex` | exactly 64 hex chars (a 32-byte public key: the agent's Ed25519 signing key; the DPU's static handshake key) |
| `key-id` | `[A-Za-z0-9._-]+`, ≤ 31, no `/`, no leading `.` — as `feed_key_id` enforces today |
| `hex128` | exactly 128 hex chars (64-byte Ed25519 signature) |

Rules:

- `version=` is the first non-comment line; `signature=` is the last line;
  everything between may appear in any order.
- The signature covers every byte from offset 0 through the `\n` that
  introduces the `signature=` line — the same signed-prefix rule
  `dmesh_feed_verify` applies, so appended bytes are unsigned and refuse the
  document. **The primitive differs: the generation is verified with Ed25519
  against `DPUMESH_CONTROLLER_KEY_DIR` (public keys only), never with the HMAC
  keyring.** A new `dmesh_gen_verify` in `doca/workload_grant.c` implements it
  with the same marker-scan, same key-id parsing, same "nothing may follow the
  envelope" check as `dmesh_feed_verify`.
- Bounds, refused rather than truncated: `GEN_POD_MAX` 65536 pods,
  `GEN_NODE_MAX` 1024 nodes, `GEN_SERVICE_MAX` 4096 services,
  `GEN_ENDPOINT_MAX` 65536 endpoints, and a total byte bound
  `TOPOLOGY_MAX_BYTES` = 16 MiB. Enforced at both ends: the publisher refuses
  to publish an over-bound generation — the last good one stands and the
  failure is loud at the source — and the consumer refuses to adopt one.
  (The membership feed's 256 KiB bound is sized for one node; 65536 `pod=`
  lines at ~150 B each are ~10 MB, so the topology consumer gets its own bound
  rather than inheriting the wrong one.)
- An unchanged cluster publishes nothing: a publication whose records equal
  the held generation's is skipped, so a new version always says something new
  and consumers re-adopt only when there is something to adopt.
- An unknown line kind, a duplicate `pod=` for one UID, an `endpoint=` or
  `protected=` naming a Service or Pod UID no other line defines, or any
  syntax violation refuses the whole document: adoption is all-or-nothing into
  a staging table that is swapped only on success, exactly as
  `dmesh_membership_parse` stages today.
- Adoption outcomes are counted as
  `dmesh_control_events_total{kind="topology",reason=...}` with reasons
  `adopted`, `unchanged`, `rollback`, `unsigned`, `bad-key-id`, `bad-sig`,
  `malformed`, `overflow`, `unreadable`.

**Where it runs.** In the cluster, as a Pod, beside `linkerd-identity` and
`linkerd-destination`. It reads the Kubernetes API (as built, a LIST at the
publication cadence rather than a watch) and speaks to node agents over the
cluster network. It never speaks to a DPU: a DPU has no route into the
cluster CIDRs, so its node agent relays, which is the same channel every other
control message already takes.

**Why not on a DPU.** The threat model admits one hostile DPU. A controller on a
DPU would put the key that binds Pods to nodes inside the blast radius, and the
identity check that key underwrites would collapse for the whole cluster. Keeping
it out is not a preference; it is what admitting a hostile DPU costs.

**Why a controller at all.** Each node agent already reads the Kubernetes API and
its DPU already trusts it, so each agent could derive the topology itself and
sign it with its node key. Three things make that worse rather than simpler:

| | Per-agent derivation | One controller |
|---|---|---|
| privilege | every node needs cluster-wide Pod read | one component does |
| consistency | each agent sees its own snapshot | one numbered generation everyone holds |
| API load | one reader per node | one reader |

The agent reads Pods with `fieldSelector=spec.nodeName=<its node>` under a
namespaced `Role` today — the deploy script asserts it cannot list Pods
cluster-wide — and that is the property to keep. It is also why Istio gives
`ztunnel` its configuration through istiod rather than letting it read the API,
and it is the overhead Canal Mesh names when it observes that control-plane
cost grows with cluster size.

**When it is unavailable.** Nothing that is running stops.

```text
   controller down
        │
        ├─ established streams            unaffected
        ├─ new streams, existing Pods     unaffected
        ├─ new streams, intra-node        unaffected (registration is node-local)
        ├─ a Pod that starts afterwards   registers and serves intra-node;
        │                                 no generation places it, so peers
        │                                 refuse it — cross-node only
        ├─ policy lookups the controller  stall for Pods not yet in the held
        │  mediates (S10)                 generation — new-Pod cross-node only
        └─ revocation via the generation  stops (node-local membership
                                          revocation continues — it is the
                                          agent's feed, not the controller's)
```

This is fail-static, and it is the feed contract the tree already implements: a
missing or older generation withdraws nothing and admits nothing. It is not a
new rule.

**Bootstrap.** A DPU must hold the controller's public key before it can verify
anything, so that key cannot arrive in a generation. It is deployment-time
material in `DPUMESH_CONTROLLER_KEY_DIR`, loaded with the checks
`dmesh_grant_load_key` already applies — regular file, `O_NOFOLLOW`, owned by
the process's effective uid, mode 0600/0400, non-zero content, 0700 directory
(root ownership is the deployment convention on top of those checks, not a
check the code makes) — and a DPU that cannot load one refuses to start. This
is the position a trust anchor occupies in every mesh that verifies anything.

**Rotation.** The directory holds an overlap set of at most
`CONTROLLER_KEYS_MAX` (4) public keys and the envelope names which one signed
it, which is exactly how the registration keyring works today
(`DMESH_REGISTRATION_MAX_KEYS` = 4, enforced at load — note the host tooling
does not enforce the cap; the DPU refuses to start over it, which is the
correct side to be strict on). Adding a key is a file drop, retiring one is a
delete, and a generation naming a key id the set does not hold is refused and
counted rather than adopted. Because key selection is filename-driven, the
controller key directory, the feed key directory (S2) and the registration key
directory must be **disjoint directories** — a key file present in the wrong
directory is a signing capability leak.

**Replication.** One replica is the default, because the system is fail-static:
losing the controller pauses generations and stops nothing that is running. Where
generation freshness matters more than that, replicas with leader election sign
in turn. This does not reduce the copies of the signing key — a Kubernetes Secret
is readable by every replica, and election controls only which one uses it.
Reducing the copies needs an external signer or a KMS, which is a deployment
choice rather than a property of this design.

## Node agent

The only component whose location is forced. It resolves the Unix-socket peer to
a Kubernetes Pod through `SO_PEERCRED` and the peer cgroup, re-checking process
start time around the read, and signs a **local assertion** binding that Pod to
the DPU's connection nonce.

Its key is a per-node private key. Its public half is published in the
generation, so the DPU verifies with a key it did not have to be given, and
rotation follows the generation. (Before S4 lands, the public half is a file
the deploy tooling drops on the DPU; after S4 the file path is retired.)

The assertion never leaves the node. It answers one question for one consumer:
which Pod is on the other end of this Comch channel.

After S6 the agent is also the DPU's only control peer: it delivers every feed
and relays the control-plane TCP streams, replacing the user units and
`bench/linkerd_cp_relay.py`.

## DPU

Verifies, routes, enforces and moves bytes. It holds no key that can create an
identity. It is the outbound proxy for its Pods and — this is new — the inbound
enforcement point for them as well.

---

# Identity [BUILT — the cross-node check waits on the transport]

## What has to be true

A destination must be able to establish the source workload's identity without
trusting the source's node, because in a cluster the source is on someone else's
machine.

## Two bindings, two sources

| Binding | Established by | Scope |
|---|---|---|
| Comch channel ↔ Pod UID | node agent, from host kernel evidence | node |
| Pod UID ↔ node, namespace, ServiceAccount, Pod IP | controller, from Kubernetes | cluster |

```text
   binding 1 — node scope                binding 2 — cluster scope
   ══════════════════════                ═════════════════════════
   "this channel is Pod P"               "Pod P is on node A, is ns/sa, has IP x"

   evidence   host kernel                evidence   Kubernetes objects
              SO_PEERCRED                           API watch
              /proc/<pid>/cgroup
              /proc/<pid>/stat

   by         node agent                 by         controller
   read by    its own DPU, only          read by    every DPU
   travels    never leaves the node      travels    the signed generation
              └──────────────────────────────────────────────────────────┐
   The local binding does not have to be verifiable off-node. A destination
   needs binding 2, and binding 2 is already a signed table it holds. ◀────┘
```

Neither component can produce the other's binding, which is why both exist.

## The local assertion [BUILT — S1–S3]

Binding 1 is carried by `struct dmesh_workload_assert_msg`, which replaced the
v1 grant (`dmesh_workload_grant_msg`, 1090 bytes, HMAC-SHA256). It keeps v1's
canonical form — explicit little-endian numerics, NUL-terminated zero-padded
text, a signature over every preceding byte — and differs in exactly five ways:

1. the Service is identified by `(namespace, name)` instead of a number —
   `service_name` replaces `service_id_le`, and the namespace half of the pair
   is the `namespace_name` field v1 already carries, because a selector Service
   only ever selects Pods in its own namespace;
2. the signed `node_name` v1 already carries becomes *checked* against the
   verifying DPU's own node (v1 only syntax-checks it);
3. the Pod's IP is carried (`pod_ip`), signed, so the destination-side
   `networks` authorization match has an authoritative input;
4. the 32-byte HMAC becomes a 64-byte Ed25519 signature the DPU can only
   verify;
5. `issuer` is dropped — the issuer is implied by `(node_name, key_id)`: the
   generation publishes each node's agent keys, so a separate issuer string
   would be a second, unauthenticated spelling of the same fact.

```c
struct dmesh_workload_assert_msg {
    uint8_t  type;                     /* DMESH_MSG_WORKLOAD_ASSERT */
    uint8_t  version;                  /* 2 */
    uint8_t  flags;                    /* zero */
    uint8_t  reserved;                 /* zero */
    uint8_t  issued_at_le[8];
    uint8_t  expires_at_le[8];
    uint8_t  assert_id[16];            /* replay window key */
    uint8_t  nonce[32];                /* the DPU's connection challenge */
    char     key_id[32];               /* selects this node's public key */
    char     node_name[254];           /* checked against the verifier's node */
    char     pod_uid[64];              /* RFC 4122 text, 36 used */
    char     namespace_name[64];       /* also qualifies service_name */
    char     pod_name[254];
    char     service_account[254];
    char     service_name[64];         /* label; qualified by namespace_name;
                                        * empty = no Service (client-only) */
    char     pod_ip[16];               /* dotted IPv4, e.g. "10.244.1.17" */
    uint8_t  sig[64];                  /* Ed25519 over every preceding byte */
};
_Static_assert(sizeof(struct dmesh_workload_assert_msg) == 1134, "ABI drift");
```

The DPU verifies in this order and refuses at the first failure. The key lookup
happens in the caller before the message-body checks, as the v1 caller already
does (`comch_server.c` resolves the key id, then verifies). Each row names the
reason it reports as `dmesh_control_events_total{kind="assert",reason}`.

| # | Check | reason |
|---|---|---|
| 0 | `key_id` names a public key the held generation (or, pre-S4, the installed key file) publishes for **this node's agent** | `bad-key-id` |
| 1 | canonical form: nothing after each NUL, zero padding intact, DNS fields well formed, `pod_ip` a dotted quad, `flags`/`reserved` zero, `assert_id`/`nonce` not all-zero | `noncanonical` |
| 2 | `version == 2` | `bad-version` |
| 3 | `node_name` equals this DPU's own node (`DPUMESH_NODE_NAME`) ⟨T⟩ | `wrong-node` |
| 4 | `issued_at ≤ now + ASSERT_CLOCK_SKEW`, `expires_at > now`, lifetime ≤ `ASSERT_LIFETIME` — the skew grace is one-sided, on `issued_at` only, as v1's check is; expiry gets no grace | `bad-time` |
| 5 | `nonce` equals the challenge this connection issued (constant-time compare) | `bad-nonce` |
| 6 | signature verifies over `[0, offsetof(sig))` | `bad-sig` |
| 7 | `assert_id` not in the consumed ring (`ASSERT_REPLAY_SLOTS` = 4096, evicting; the per-connection nonce is what makes eviction safe) | `replay` |
| 8 | `(namespace_name, service_name)` equals the Service this registration requests | `bad-service` |

Checks 3 and 8 are what a registration cannot talk its way around: the Pod
relays the assertion, and the assertion names both the node and the Service.

Verification happens once per Pod registration — concretely, once per Comch
connection: the challenge nonce is minted once per connection, the assertion is
accepted once, and `pods_register` consumes it; a reconnect re-attests. Nothing
on the data path verifies assertions.

## The check across a node boundary

Each DPU holds a controller-issued node credential. A pair of DPUs authenticates
**once**, and the channel is long-lived and carries every stream between those
two nodes.

```text
  Pod P (node A)      DPU A                      DPU B           Pod Q (node B)
      │                 │                          │                    │
      │ ──── DMA ─────▶ │                          │                    │
      │                 │ OPEN(src=P, dst=Q, port) │                    │
      │                 ├─────────────────────────▶│                    │
      │                 │  on the authenticated pair channel            │
      │                 │                          │                    │
      │                 │        ① peer is node A?      ← settled at channel setup
      │                 │        ② generation places P on A?   ← the identity check
      │                 │        ③ Q registered here?
      │                 │        ④ policy admits P's identity to Q:port?
      │                 │                          │                    │
      │                 │◀──── HANDLE h ───────────┤                    │
      │                 │                          │                    │
      │                 │ ──── RDMA (h, bytes) ───▶│ ──── SG-DMA ─────▶ │
```

Stream setup carries the source Pod UID, the destination Pod UID and the
destination port. The destination DPU then checks:

1. the peer is authenticated as node *A* — established at channel setup;
2. the held generation places the source Pod UID on node *A*;
3. the destination Pod UID is registered here;
4. policy admits the source identity to that Pod and port.

```c
/* On the destination DPU, for every inter-node STREAM_OPEN.
 * `gen` is the generation this DPU holds; `peer` is the authenticated far end
 * of the channel the message arrived on. */
static int accept_stream(const struct peer *peer, const struct stream_open *o)
{
    if (o->incarnation != peer->incarnation)
        return REFUSE_STALE_INCARNATION;

    if (!peer_admit(peer))                         /* streams/rate ⟨T⟩; staging is
                                                    * charged per DATA arrival */
        return REFUSE_PEER_LIMIT;

    const struct gen_pod *src = gen_find_pod(gen, o->src_pod_uid);
    if (src == NULL || strcmp(src->node, peer->node) != 0)
        return REFUSE_NOT_ON_PEER;                 /* ② the identity check ⟨T⟩ */

    struct pod_state *dst = pod_by_uid(o->dst_pod_uid);
    if (dst == NULL || !pod_data_ready(dst))
        return REFUSE_NO_DESTINATION;              /* ③ */

    /* ④ is asked once per stream at its first byte, in the same admission
     * path an intra-node stream takes (px_conn_admitted → l7_inbound_verdict,
     * S9): src supplies namespace, service_account AND pod_ip from the signed
     * generation — never a peer-supplied value — and a refused verdict
     * poisons the stream before a byte reaches the Pod. */
    return handle_alloc(peer, dst, o->dst_port);   /* the reply carries it */
}
```

**Step ② is the identity check ⟨T⟩, and it is a lookup in a signed table.** No
asymmetric operation happens on the connection path. Every input to step ④ —
namespace, ServiceAccount and source address — comes from the generation too.

Node *A* can claim any Pod the cluster says is on node *A* — that is, its own
Pods, whose memory it already holds. That is irreducible and equally true of
`ztunnel` and of a sidecar. It cannot claim a Pod the generation places
elsewhere.

Without step ② a compromised DPU inherits every identity in the cluster.

```text
   node A   public front end only. Internet-facing. The easiest to compromise.
   node C   a Pod whose ServiceAccount is admin-sa
   node B   the payments service.  Its policy: "admin-sa only"

   believing the peer                     checking the generation
   ──────────────────────                 ───────────────────────
   A: "I am admin-sa"  ──▶ B              A: "I am admin-sa"  ──▶ B
   B: admitted                    ✓       B: the generation says
                                             admin-sa is on node C
                                          B: refused              ✗

   compromise the least-privileged        A may claim only Pods the
   node, obtain the most-privileged       generation places on A —
   identity in the cluster                whose memory it already holds
```

This is the same escalation Istio's CA refuses when it rejects a `ztunnel`
requesting a certificate for an identity that is not running on its node.

## The three ways to bound it, and why this one

The bound can come from state or from computation.

| | Destination holds | Per-stream cost | One compromised DPU obtains |
|---|---|---|---|
| believe the peer | nothing | none | every identity in the cluster |
| peer forwards a controller-signed assertion | a cache | one verification per source Pod | that node's Pods |
| **hold the signed topology** | the cluster's Pod table | a lookup | that node's Pods |

The second and third are equivalent in what they deny. They differ only in
whether the destination spends memory or cycles.

A DPU is memory-rich and cycle-poor relative to what it is protecting: a
connection costs 73 ARM core-µs to build and tear down, and an asymmetric
verification on these cores is the same order of magnitude, while a `pod=` line
is under two hundred bytes — ten thousand meshed Pods are a couple of
megabytes. On this hardware, state is the cheap side of that trade.

If a cluster ever grows a Pod table a DPU should not hold, the second row is the
migration: it keeps the bound and moves the cost back to computation.

## Why this matters on a DPU

A DPU connection costs 73 ARM core-µs to build and tear down. An asymmetric
verification on these cores is the same order of magnitude, so a per-connection
signature would be a tax comparable to the transport it is protecting.

The design avoids it by using facts that are already signed and already held:
the topology generation is verified once when adopted, and the DPU pair is
authenticated once when the channel opens. Both costs amortize over every stream
between two nodes.

HBONE amortizes too — a `ztunnel` pair reuses a workload-pair connection across
its streams — so the honest comparison is of amortization *units*: ambient pays
an asymmetric handshake per (source workload, destination) connection and again
whenever one goes cold; this design pays one per node pair, and a new workload
pair on a warm channel costs a table lookup. The difference shows exactly when
workload pairs are many or short-lived relative to node pairs.

## Intra-node

The same check, degenerate. The source and destination are on this DPU, so
step ② is satisfied by the live registration rather than by the generation, and
steps ③ and ④ are unchanged. The registration must therefore retain everything
step ④ consumes: `pod_state` today keeps only `workload` and `pod_uid` of the
signed claims (namespace and pod name survive only inside the rendered
workload JSON; `service_account` and `node_name` are dropped at registration),
so S3 adds `namespace_name`, `service_account` and `pod_ip` to `pod_state`.
One admission path serves both cases.

## Freshness — the composite window

Four clocks compose, and the design states their sum rather than letting a
reader discover it:

| Window | Size | Applies to |
|---|---|---|
| generation publication | `GENERATION_INTERVAL` (5 s) + adoption lag | how stale a *placement* can be, both directions |
| local membership withdrawal | 2 × generation cadence (`DMESH_MEMBERSHIP_ABSENCES_TO_REVOKE` = 2, at the 1 s `server_progress_membership` cadence against the agent's publication cadence) | how long a deleted Pod's *local registration* survives |
| assertion lifetime | `ASSERT_LIFETIME` (300 s) | how long a relayed assertion stays usable; bounded by the per-connection nonce, which a new connection refreshes |
| clock skew | `ASSERT_CLOCK_SKEW` (30 s), one-sided on `issued_at` | agent↔DPU clock spread |

For a *hostile* node the operative bound is the first row: a compromised node
can claim a Pod that left it until every destination adopts the generation that
moved it. An honest node stops originating the moment the local registration
ends — the deleted Pod's Comch connection drops and the sweep closes its
streams — so rows two through four never extend an honest node's claim
cross-node. A recreated Pod carries a new UID, so no window lets an old
identity inherit a new placement. Shortening `GENERATION_INTERVAL` narrows the
hostile window; an explicit revocation message would close it exactly, at the
cost of one more mechanism (open question M1).

---

# The peer channel [BUILT except the transport]

Everything in this section has a counterpart inside a node. The Comch connection
between a host process and its DPU already solves the same problems, and the
mechanisms that survived hardware validation there are the ones extended here.

## Custody and flow control across the boundary

*Two custody domains* above states the intra-node facts; this section states
only what the boundary changes.

**L4 streams keep the end-to-end loop, stretched by one round trip.** The
release that `px_engine_emit` performs on local batch completion is deferred:
pieces whose destination is remote are parked in a per-peer un-ACKed slot pool
(`DMESH_PEER_TX_SLOTS`), and `STREAM_ACK { incarnation, handle, seq_first,
seq_count }` retires the named run — only then does `px_custody_sub` run and
the sender's `TX_ACK` follow. A destination sends `STREAM_ACK` when the bytes have landed in
the destination Pod's host RX mapping (its own `px_engine_emit`), staging up to
`STREAM_ACK_BATCH` (64) acknowledgements before flushing, exactly as the
reverse ring batches today.

**L7 streams keep their hop-by-hop composition; the wire hop joins it.** The
Pod-side release (`dmesh_l7_release` on stack consumption) is untouched. An
egress arena chunk whose destination is remote is retired on `STREAM_ACK`
instead of on local completion. The composition stays lossless backpressure:
remote destination stalls → un-ACKed chunks accumulate to the per-peer bound →
that peer's sessions stop emitting → their stacks stop reading → staging fills
→ the Pods writing to that peer stall.

```text
   Pod P ──DMA──▶ staging A ──RDMA──▶ staging B ──SG-DMA──▶ Pod Q
                                                              │
        TX_ACK to P ◀─── STREAM_ACK ◀───────────────────── landed
        (L4: staging piece · L7: arena chunk retired by the ACK)
```

Errored transfers follow the intra-node rule: a fault releases custody, poisons
the streams it carried, and is counted — never silently retried per-stream (see
*Lifetime*).

**The window arithmetic, honestly.** A QP's window is
`TX_BLOCKS_PER_CONN × TX_BLOCK_SIZE` = 8 × 512 KiB = 4 MiB at the defaults —
with two qualifications. First, both constants are seeds: the live values are
clamped to the TX mapping's actual geometry, and blocks come from a shared
128-block per-process pool, so at most 16 QPs can hold a full window
simultaneously; the window is a ceiling, not a reservation. Second, the credit
round trip is fabric RTT plus destination SG-DMA completion plus `STREAM_ACK`
batching. Against that: 100 Gb/s × 10 µs is a 125 KB bandwidth-delay product —
the window covers it ~33×; at 400 Gb/s × 20 µs the product is 1 MB and the
margin is 4×. One stream fills the pipe in either case, but the margin is one
to one-and-a-half orders of magnitude, not two, and it thins as fabrics get
faster. The window is what makes end-to-end credit affordable; a smaller one
would have forced the weaker semantics.

**The transport carries no flow control of its own.** Neither does the DPA or the
SG-DMA engine inside a node — forward-ring credits, arrival custody and lane
credits do that work, and they continue to. A slow destination fills staging at
the source, stops returning capacity, and stalls the Pods writing to it, which
is the behaviour a slow local destination already produces.

## What a peer may consume, on both sides ⟨T⟩

A peer DPU is authenticated, not trusted. Everything it sends is input: stream
opens, lengths, handles, and the rate of all of them.

At the **destination**, a peer is admitted against bounds of its own —
`PEER_STREAMS_MAX` concurrent streams, `PEER_STAGING_MAX` staging bytes,
`PEER_OPEN_RATE` opens per second — and refused beyond them rather than letting
one node's traffic displace another's.

At the **source**, a *stalled* peer is bounded too: `PEER_TX_INFLIGHT_MAX`
caps the un-ACKed bytes (L4 pieces plus L7 arena chunks) one peer may hold.
Beyond it, streams to that peer stall via the existing `px_stall` path. This is
what makes "a stalled peer stalls only the Pods writing to it" a property
rather than a hope: without it, un-ACKed L7 chunks of one dead peer would
drain the shared egress arena that every other L7 connection allocates from
(*The pool caveat*).

This is the same reasoning the data path already applies to a Pod. Forward-ring
credits, arrival custody and lane admission exist because a local Pod is not
trusted to be well behaved either. The peer channel is one more producer and
gets the same treatment.

## What the transport must provide

The RDMA layer is out of scope, but the design leans on it, and these are the
properties it leans on. A transport that does not offer them breaks invariants
this document assumes.

| Property | Why |
|---|---|
| ordered delivery within one handle | `design/CORE.md` preserves per-connection order end to end |
| reliable delivery, or a fault that is visible | custody cannot release on a silent loss |
| completion reported to the source | `STREAM_ACK` is what returns the sender's capacity |
| reordering across handles permitted | independent streams must not head-of-line each other |
| a fault surfaces as channel loss | per-stream recovery is not attempted; see Lifetime |

The unit that crosses is a **staging extent** — the same contiguous run the
SG-DMA engine sources from, up to `PX_ARRIVAL_COALESCE_MAX` (64 KiB). Nothing
is re-segmented at the boundary. (The parser's contiguous *view* can span
adjacent extents; the built pieces never exceed the bound.)

## Authentication and pairwise keys ⟨T⟩

The node credential is one static keypair per DPU, generated on the DPU at
first boot into a 0400 file that never leaves it. The public half is reported
through the node agent to the controller, which publishes it in that node's
`node=` line of the generation. A DPU therefore authenticates a peer with a
key it obtained from the generation, not from the peer.

The handshake is not invented here. The channel runs an existing mutually
authenticated key-agreement protocol — Noise IK, or TLS 1.3 with raw public
keys — with exactly one rule added on top of the stock protocol:

> the peer's static public key must equal the one the held generation binds to
> the peer's claimed node name. A name the generation does not bind, or a key
> that differs, refuses the channel ⟨T⟩.

The channel incarnation is bound into the handshake (the Noise prologue, or
the TLS exporter context), so a completed handshake authenticates the
incarnation its handles will carry. The session keys the protocol derives
protect every control message and data byte on the channel.

Keys are pairwise ⟨T⟩: a fleet-shared key would let one compromised DPU read
every conversation in the cluster rather than its own. No per-workload key
exists on this wire; per-workload separation is *Out of scope* and the trade is
recorded there.

## Lifetime

A channel opens on the first stream that needs the node at its far end. Opening
to every node in advance would be quadratic, and a DPU's channel count should
follow the peers it actually speaks to rather than the size of the cluster. A
bound on that count with idle eviction keeps it there.

The cost of this is that the first stream to a node pays for authentication and
key agreement — the cost the rest of the design works to amortize. It is in the
measurement table below for that reason.

A channel carries an **incarnation**, and every handle and in-flight operation is
stamped with it. A reconnection advances it, so a completion belonging to the
previous incarnation is refused rather than applied to the current one. This is
what `dma_generation` does for a Pod slot, for the same reason: an asynchronous
completion outlives the state that named it. (Note the built precedent's exact
scope: the generation pair `px_batch.pod_generation` ↔ `pod->dma_generation`
is matched on the error, retry and rearm paths; the success path checks
`pod_data_ready`. The channel incarnation is matched on **every** path.)

```text
                    open() on the first stream needing this node
                                     │
   CLOSED ──────────────────────────▶│
     ▲                               ▼
     │                        AUTHENTICATING ── incarnation += 1 on entry
     │                          │        │
     │                     fail │        │ node credentials verified both ways,
     │                          │        │ pairwise key agreed ⟨T⟩
     │                          ▼        ▼
     │◀───────────────────────  OPEN
     │
     │   idle > CHANNEL_IDLE (only with no streams and nothing in flight),
     │   the generation drops or re-keys the peer (`dmesh_peer_table_rebind`,
     │   run on every adoption), or the transport faults.
     │   Teardown is synchronous: the reset poisons every stream and releases
     │   every pinned extent before it returns, so there is no draining state.
```

Control messages on the channel, all of them setup or teardown; the data path
carries only a handle and bytes.

| Message | Direction | Fields |
|---|---|---|
| `STREAM_OPEN` | source → destination | `incarnation`, `src_pod_uid[64]`, `dst_pod_uid[64]`, `dst_port`, `src_generation` |
| `STREAM_OPEN_ACK` | destination → source | `incarnation`, `handle`, `status` |
| `STREAM_FIN` | either | `incarnation`, `handle` |
| `STREAM_ACK` | destination → source | `incarnation`, `handle`, `seq_first`, `seq_count` |
| `POD_GONE` | source → destination | `incarnation`, `pod_uid[64]` |

`STREAM_ACK` names a run of consecutive sequences rather than one, which is the
encoding `dmesh_tx_ack_entry` already uses on the reverse ring, for the same
reason: one acknowledgement per released extent rather than one per transport
unit.

`src_generation` is not used for the identity check. It lets a destination
notice it is behind and adopt sooner, which shortens case (a) below.

**A stream is full-duplex and has one handle.** A `dmesh_qp_t` is one
full-duplex byte stream, and the handle names it in both directions. This is what
a `dpu_upstream` entry already does within a node: one identifier, with
`is_reply` distinguishing which way a segment is going.

**Each side owns the stream on its own worker, by its own rule.** On the source,
the Pod's port decides — `dmesh_worker_for_port(port, A)` is the shared rule in
`include/dpumesh/dmesh_topology.h` (today its caller is the DPU's
`owner_worker`; the host applies the sibling rule `dmesh_forward_ring`). On the
destination, the DPU that allocates the handle encodes its own worker in it, as
`dpu_upstream_create` encodes the residue class `port % A`. The two are
independent because the two DPUs have independent worker sets.

To keep a completion on its owner rather than handing it across workers, the
channel is one queue pair **per (node pair, destination worker)**, while
authentication and the pairwise key are established once **per node pair**. The
handshake amortizes over every worker and every stream; the queue pairs only
carry bytes. (`CHANNEL_MAX` × `PEER_QP_PER_NODE` = 256 × A queue pairs at the
bound — 2048 at A = 8 — which is the number to check against RDMA resource
limits, not 256.)

**Losing a channel ends the streams on it.** Each DPU tells its own Pods by the
path it already uses when a peer Pod disappears — `px_conn_peer_disconnected`
delivers EOF to the survivor and removes the connection (deferred under pool
pressure; silent when the survivor is itself gone). Streams are not migrated
across a reconnection. Canal Mesh spends its Issue #3 on exactly this problem,
because a consolidated proxy must preserve session state while scaling; a
transport whose contract is that a connection can fail and the application
retries does not have to.

## Handles

The destination DPU allocates the handle, because it owns that namespace. The
space is per peer, so nothing cluster-wide has to allocate it — which is the
naming rule applied again.

```c
/* One per (peer, handle). Self-contained by value, as px_unit is, so it
 * survives the teardown of anything that named it. */
struct peer_handle {
    uint32_t incarnation;      /* the channel incarnation that issued it */
    int32_t  dst_pod_idx;      /* destination slot; validated against pod generation */
    uint32_t dst_pod_generation;
    uint16_t dst_port;
    uint16_t up_port;          /* the intra-node upstream this stream feeds */
    char     src_pod_uid[64];  /* the key POD_GONE and peer loss sweep on */
};
```

Two generations guard it: the **channel** incarnation rejects a handle from a
previous connection to the same peer, and the **pod** generation rejects one
whose destination slot has been re-tenanted. This is the pair `px_batch` already
carries — `pod_generation` stamped at submit, matched in the completion — for
the same reason.

`dpu_upstream_create` and `dpu_upstream_free` already do this within a node, and
their reclamation paths extend one for one. Note the built precedent's real
shape: paths (b) and (c) below are *one* sweep today — both `POD_UNREGISTER`
and a Comch disconnect set `cleanup_pending` and the same per-worker sweep
matches the Pod as client or backend. The table distinguishes them because the
cross-node column does.

| Event | Within a node | Across nodes |
|---|---|---|
| normal close | FIN fan-out (`px_try_fin`), then the upstream is freed | stream FIN; the destination releases the handle |
| source Pod gone | a worker sweeps the upstream table | **the source DPU says so on the channel**; the destination releases that Pod's handles |
| peer gone | the Pod's Comch connection drops | the destination releases every handle whose source is that node |

The middle row is the one new mechanism. Within a node a sweep suffices because
the DPU sees the Pod leave; across nodes a destination cannot see it, so the
source has to say. The shape is not new either: `px_try_fin` fans a client's FIN
out to every upstream it opened, which is the same "the departing side tells the
remaining side" message. If the source crashes before sending `POD_GONE`, the
handles survive until the channel faults or idles out — bounded by
`PEER_STREAMS_MAX`, which is why that bound exists per peer rather than
globally.

A policy change does not disturb an established handle. The gate stated for an
`AuthorizationPolicy` cycle (S9) is that admission changes within one watch
update and nothing is admitted while the rule denies — it is a statement about
new streams. This matches what a sidecar mesh does and is stated here so that
the handle's lifetime is not read as a policy lifetime.

## A peer whose claims are refused

A refusal has three causes and two of them are ordinary:

```text
   (a) the destination's generation is behind
       Pod P moved C → A; the destination has not adopted that generation yet.
       The source claims P honestly and the destination refuses honestly.

   (b) the source's generation is behind
       Pod P left A; the source has not noticed yet.  Both are honest.

   (c) the source is hostile
       It claims an identity it never held.
```

Distinguishing them from a single refusal is not possible. Carrying the
generation version in the stream open tells a destination which side is behind,
but both directions have an ordinary explanation, so the version does not
separate (c) — it is worth carrying for a different reason, because a
destination that learns it is behind can adopt sooner and shorten case (a).

Tearing the channel down is therefore wrong: ordinary generation skew would
become an outage. The question is better asked as what a refusal *consumes*. A
refused stream costs a lookup and a reply, so what needs bounding is the rate.

**A destination bounds and reports; the controller evicts.** Per-peer limits cap
what any peer can hold, and a per-peer rate limit on stream opens caps what a
flood of refusals can cost. A peer that is refused often becomes slow, not
disconnected, so the honest Pods on that node keep their streams and can still
open new ones. Reporting is built: `px_poison` counts (`stat_poison`), and the
per-worker refusal counters are surfaced through `l7_control_event` (as
`dmesh_control_events_total{kind="peer",reason=...}`) where the adapter runs
and in the worker stat line always.

Removing a node is a cluster-wide decision and belongs to the component that
already holds cluster-wide authority. When the controller drops a node from the
generation, every DPU refuses it at once and consistently, rather than each
destination reaching its own verdict. The division is the one the tree already
uses for a misbehaving Pod: `px_poison` ends the connection, and the membership
generation is what actually removes the Pod.

---

# Scope of the control-plane credential ⟨T⟩ [BUILT]

The DPU authenticates to Linkerd's destination and policy services with one
credential per node and supplies the workload it is asking about as a string —
`PortSpec.workload` and the outbound API's `source_workload` are plain,
unauthenticated strings. A compromised DPU can therefore read the outbound
policy of every workload in the cluster, which is reach beyond its node and so
is excluded by the threat model.

The credential a DPU presents to the control plane is scoped to the Pods it
serves. Where the upstream API cannot express that, the controller mediates the
lookup: the DPU asks it, and it answers only for Pods the generation places on
that node. This keeps the enforcement in the same component that already binds
Pods to nodes, rather than adding a second authority. The availability cost is
stated in the controller's fail-static table: a mediated lookup for a Pod not
yet in the held generation stalls while the controller is down — new-Pod,
cross-node only.

---

# Naming [BUILT]

`service_id` is a 7-bit-range identifier invented by DPUmesh (`[0,127]` — the
bound is `POD_ID_SPACE` range checks plus `int8_t` wire fields, not a mask).
It was written by hand in a registry file, baked into Pod images, into the
node agent's ConfigMap and into the DPU's numeric environment. To be precise
about what was and was not signed then, because the security claim depended on
it:

- the *number* is signed twice — the grant carries `service_id_le` under the
  agent's HMAC, and the membership generation signs `(pod_uid, service_id)`
  pairs — and `pods_register` refuses a registration whose number differs from
  the granted one;
- the *name↔number mapping* is signed nowhere. It is a hand-edited file whose
  copies live in Pod images, and the host library resolves against its own copy
  before it ever speaks to the DPU (`src/dmesh_resolve.c` — a runtime-loaded
  table, not a compiled-in one; deleting the file leaves an empty table).

The mapping was the only unsigned authority in the control plane, and its
failure mode was the worst available: a missing registry file warned once and
resolved nothing; a malformed line was skipped silently; an *unmatched*
address returned "not meshed" and the POSIX facade silently fell back to
kernel TCP — traffic left the mesh with no counter and no refusal. A wrong
number in one image dialed another Service's backend set.

The design does not fix this. It removes it: no registry file exists anywhere.

Names on the outside, handles on the inside, each allocated by whoever owns it.
This is already what the public API does: `dmesh_create_qp(eq, service_name)`
takes a name and `DPUMESH_SERVICE` is a name. The number is an internal detail
that leaked outward because the host library resolves it from a file before it
ever speaks to the DPU.

| Value | Scope | Assigned by | Where it travels |
|---|---|---|---|
| Service name `namespace/name` | cluster | Kubernetes | control plane |
| Pod UID | cluster | Kubernetes | control plane, stream setup |
| node name, RDMA address | cluster | Kubernetes, controller | generation |
| interned service id | node | that node's DPU | node-local wire |
| `pod_id` | node | that node's DPU | node-local wire |
| stream handle | node pair | the destination DPU | inter-DPU wire |

**Service identifiers are namespace-qualified everywhere.** A Kubernetes
Service is namespace-scoped; a bare name is ambiguous by construction. The
generation writes `namespace/name`; the assertion carries the pair
`(namespace_name, service_name)`; the public API accepts `"name"` (resolved in
the calling Pod's own namespace, which the registration already knows) or
`"name.namespace"` for a cross-namespace peer — the DNS convention applications
already use.

**Resolution lives on the DPU.** The host asks; the DPU answers from the held
generation; the answer carries the DPU-interned id, which is a handle in the
table above, not a name. The Comch control messages:

```c
struct dmesh_resolve_msg {          /* host → DPU */
    uint8_t  type;                  /* DMESH_MSG_RESOLVE */
    uint8_t  version;               /* 1 */
    uint8_t  by_name;               /* 0: by ClusterIP, 1: by name */
    uint8_t  reserved;
    uint32_t ipv4_be;               /* by_name == 0 */
    uint16_t port_be;
    uint16_t reserved2;
    char     name[128];             /* by_name == 1: "name" or "name.namespace" */
};
struct dmesh_resolve_ack_msg {      /* DPU → host */
    uint8_t  type;                  /* DMESH_MSG_RESOLVE_ACK */
    uint8_t  status;                /* 0 meshed, 1 not-meshed, 2 no-generation */
    int16_t  interned_svc;          /* valid when status == 0 */
    uint32_t reserved;
    uint64_t generation_le;         /* the version this answer came from */
    char     namespace_name[64];
    char     service_name[64];
};
```

The host caches answers per address for `GENERATION_INTERVAL` and re-resolves
after that or on any connection error — the cache is then never staler than
the generation's own freshness bound, so no second invalidation rule exists.
The generation names every cluster Service, meshed or not (the apiserver
included), so a by-address answer is `meshed` only when the Service can
actually be served on the mesh — a live registered backend on this node.
Widening that to the generation's endpoint set waits on the transport: until
bytes can cross, a remote-only Service must be refused as `peer.transport`,
not absorbed into a stream nothing can carry. A by-name lookup is an
explicit request for the mesh and answers from the generation alone.
`status = 1` makes leaving the mesh an **explicit, logged** decision of the
facade instead of a silent one; `status = 2` (no generation held) resolves
nothing — a facade `connect()` falls back to kernel TCP with the same log, and
a *registration* fails closed, because serving an identity requires the
generation that defines it.

**What the host calls a remote peer.** `dmesh_rev_done_entry.src_pod_id` is an
`int8_t` in a structure that is exactly 16 bytes, and `dmesh_qp_t.remote_pod`
(an `int16_t`) reports it to the application. A Pod on another node has no such
identifier, because the space is node-local by construction. It does not need
one. The DPU already ignores what a host names as the destination of a reply
and routes it from conntrack — `px_unit_prepare` warns and overrides when the
two disagree. `remote_pod` is therefore advisory, and a remote peer is reported
as `DMESH_POD_REMOTE` (`-2`, beside `DMESH_POD_BLANK` `-1`). No wire structure
changes and no identity is lost, because a pod id was never workload identity:
it is a transport identifier for one node's slot table, and `design/API.md`
says so explicitly after this change.

`service_id` numbers left every file a Pod image carries: the registry file
and the resolver's file-loading path in `src/dmesh_resolve.c` are deleted, the
`DPUMESH_L7_*_SVC` mode lists and the adapter's target feed take
`namespace/name` entries (the adapter resolves them through
`dmesh_l7_svc_for_name`), the membership feed pairs `(Pod UID, Service name)`,
and the wire's `int8_t` service fields carry the DPU-interned id, which the
host treats as opaque and receives at assignment. Nothing needs a cluster-wide
compact namespace, so nothing has to allocate one.

---

# Reach [BUILT except the remote data path]

`collect_live_hosts` returns Pods registered on this DPU — it walks
`objs->pods[]` and nothing else — so a Service with no local replica is
unroutable and its streams are poisoned although the Service is healthy
elsewhere (`px_unit_prepare` returns the same terminal `-1` for "no live host"
as for "host not ready", and `px_ship_range` poisons on it). This is invisible
on one node, where the local set and the cluster set are the same, and it is
the common case on many.

The backend set comes from the generation: the `endpoint=` lines of the held
generation, joined with `pod=` for placement, split into a local set (this
node) and a remote set (any other). Local endpoints are preferred because they
are a DMA away; remote endpoints are reached over the peer channel. Having no
local replica costs latency, never the connection.

Locality is a preference, which is what Canal Mesh does across availability
zones and what Kubernetes topology-aware routing does across them. Neither
treats it as a constraint.

---

# Authorization [BUILT]

Nothing anywhere decides whether a caller may reach a callee today. Registration
authorizes what a Pod may *serve*, and Linkerd's outbound API carries routing
and protocol configuration, not an allow/deny verdict.

The destination DPU becomes the inbound enforcement point, the role a sidecar's
inbound half plays. It holds the verified source identity from the check above
and the destination Pod's inbound policy.

The stock implementation is already in the tree — `linkerd/app/inbound` in the
proxy submodule, `InboundServerPolicies.WatchPort` as its policy source — and
the build called `disable_inbound_policy_discovery()` for everything; discovery
is now on for the Pods the DPU serves. The call survives with a narrower job:
this runtime's own inbound and admin listeners are ephemeral and belong to no
Pod, so discovering policy for their ports would only ask the controller about
servers it has never heard of. They keep their configured default; the
per-destination-Pod stores are built from the discovering configuration that
call now preserves. Three facts about the
stock evaluation shape the design, and each is a constraint, not a choice:

**1. Networks are matched first, and an empty match denies.** The evaluation is

```rust
fn is_authorized(authz, client_addr, tls) -> bool {
    if !authz.networks.iter().any(|n| n.contains(&client_addr.ip())) {
        return false;
    }
    is_tls_authorized(tls, authz)
}
```

so a verdict is **never** decided from the peer identity alone. The adapter
today fabricates source addresses from a private `10.97.0.0/24` mapping of pod
ids — an address no realistic `networks` clause names. The design's answer is
the `pod_ip` field: the generation (cross-node) and the assertion (intra-node)
both carry the Pod's real cluster IP, signed, and the DPU presents
`client_addr = pod_ip:src_port` to the evaluation. `AuthorizationPolicy`
networks clauses written against cluster CIDRs then evaluate unchanged, with an
authoritative input. The synthetic mapping is deleted.

**2. The identity input is a TLS *state*, not a string.** `is_tls_authorized`
matches `Authentication::TlsAuthenticated` only through
`ConditionalServerTls::Some(ServerTls::Established { client_id, .. })`. The
adapter therefore constructs that state — `Established` with
`client_id = Some(ClientId(<sa>.<ns>.serviceaccount.identity.<trust-domain>))`
and no negotiated protocol — from the verified source identity, instead of from
a handshake. This is the same substitution `DmeshIo` makes for the byte stream.
The evaluation functions are private to the inbound crate, so the fork exposes
one function (this is the submodule fork's job — no upstream patch):

```rust
/// linkerd/app/inbound/src/policy.rs
pub fn connection_verdict(
    policy: &ServerPolicy,
    client: Remote<ClientAddr>,
    tls: &tls::ConditionalServerTls,
) -> bool
```

**3. "Connection-level" is per protocol variant, and HTTP variants have no
connection-level list.** `Protocol::Detect` carries `tcp_authorizations`;
`Tls` and `Opaque` carry their own authorization lists; `Http1`, `Http2` and
`Grpc` carry only per-route authorizations. The rule, per variant:

| `ServerPolicy::protocol` | Connection verdict |
|---|---|
| `Detect { tcp_authorizations, .. }` | any authorization admits `(client, identity)` |
| `Tls(a)`, `Opaque(a)` | any authorization in `a` admits |
| `Http1(routes)`, `Http2(routes)`, `Grpc(routes)` | any authorization of **any route** admits — the union. Route-level *differences* are not enforced (that would need the second parser this split exists to avoid); the connection is refused only when no route could ever admit this client. This over-admits exactly as `ztunnel`'s L4 enforcement does, and for the same reason; a per-route verdict needs a waypoint there and is *Out of scope* here |

## Which side runs what

A session that crosses two nodes must not build two proxies. The measured cost of
one Linkerd session is 1,127 ARM core-µs, and doubling it would make a cross-node
L7 stream half the capacity of a local one — a design choice becoming a
performance claim.

The split follows from what each side decides:

| | Source DPU | Destination DPU |
|---|---|---|
| discovery, balancing, retries, protocol | full outbound stack, per session | — |
| authorization | — | policy evaluation |
| cost scaling | per session | **per destination Pod and port** |

The destination needs a verdict, not a proxy. Its inputs are the source
identity and address, which the generation supplies, and the destination Pod's
policy, which is a watch on `InboundServerPolicies.WatchPort` — **one watch per
Pod and port, shared by every stream that arrives at it**. Mechanically:
`Inbound::build_policies(workload, ...)` binds one watch set to one workload
string, so the adapter calls it once per registered destination Pod (not once
per process, as a sidecar does) and caches the resulting `GetPolicy` on the
registration; streams share the watch and pay only `connection_verdict`. The
per-stream cost is an evaluation, not a session build, so a cross-node L7
stream costs one outbound session plus a lookup.

On the relationship to the policy controller: the DPU asks for the inbound
policy of Pods it is the proxy for, which is the relationship the API is built
around — but that is an argument about intent, not a measurement. Whether the
controller *also* serves a workload's policy to a caller that is not its proxy
is exactly what S10's scoping exists to make irrelevant, and S9 carries an
explicit gate to observe the controller's actual behaviour before enforcement
is enabled.

---

# Encryption [DESIGN — it is the transport's]

Node-local traffic is plaintext because it crosses PCIe and DPU memory and never
a wire. That argument ends at the node boundary. Canal Mesh states the rule the
inter-node path inherits: data is encrypted before it leaves the node.

Encryption applies to the DPU-pair channel, not to each stream, so its
negotiation cost amortizes exactly as the authentication does. The identity the
handshake presents is the node credential. Ambient separates workloads on the
wire by giving each source workload its own mTLS connection under its own
certificate; here one channel carries every workload between two nodes, and
per-workload separation is enforced by the per-Pod mapping at both ends instead
of by the wire. That is a weaker wire property, chosen deliberately: under this
document's threat model a compromised node holds all its resident workloads'
keys in either design, so the wire distinction buys nothing against the
adversary being defended against. The trade is recorded in *Out of scope*.

**Keys are pairwise ⟨T⟩.** A key shared by the fleet would let one compromised
DPU read every conversation in the cluster rather than its own, which is the
reach the threat model denies everywhere else.

The key material lives only on the DPU. This is the first mesh property in this
document that a sidecar cannot match at any cost, because a sidecar's key is in
the Pod.

---

# Observability [BUILT intra-node; the cross-node half waits on the transport]

Every byte crosses the DPU, so the workload cannot alter what is recorded —
counters and traces are taken on a machine it cannot write to. What is
recordable differs by locality, and the design states both halves rather than
the better one:

- **Intra-node**, the L7 layer parses the stream in the same place the bytes
  move, so the sidecar metric set — both the client-side and the server-side
  view — is available from one machine, with no in-Pod component. Unlike Canal
  Mesh, which downgrades its on-node proxy to L4 and keeps L7 observability on
  the remote gateway, both halves are in the same place here.
- **Cross-node**, the source DPU runs the only parser (the outbound session),
  so the source emits the full outbound family; the destination emits the
  inbound family **at connection level** — bytes, streams, verdicts, identities
  — but no server-side HTTP metrics, because it deliberately has no parser.
  This is the same line `ztunnel` accepts, and it is the observability face of
  the *Out of scope* decision against a second L7 stack.

The double-counting rule is the one a sidecar mesh already uses: the source
emits the outbound family and the destination emits the inbound family. A
stream recorded by both is the client-side and the server-side view of the same
request.

---

# What this costs

The design is only worth the concession if the identity it adds is cheap
relative to the transport it protects. That is a measurement, not a claim, and
these are the numbers that decide it.

| Quantity | Why it decides something | Reference already measured |
|---|---|---|
| identity check per stream | should be a lookup; if it is not, the design fails | DPUmesh connection = 73 ARM core-µs |
| DPU-pair channel setup | amortized, but bounds a cold cluster | Linkerd session = 1,127 µs |
| inbound authorization per stream | the new enforcement point | — |
| encryption per byte on the RDMA path | ARM cores are the known bottleneck | L4 ceiling, `../bench/report/REPORT.md` |
| registration with the split attester | one round trip per Pod, not per stream | — |

The comparison that gives the design its meaning is against a mesh that pays
asymmetric crypto per *workload-pair* connection. Canal Mesh built a remote key
server because fewer than 10% of their VM models have crypto acceleration; a
DPU has it on board, which is the reason the same problem does not need the
same answer here.

**Measurement discipline (carried over, binding):** a capacity is quoted with
the instrument that produced it. The L4 and gRPC reports quote `knees.csv`'s
twice-voted `highest_clean_rps`; the linkerd report quotes the single-run rate
grid, which reaches further on the same data. The two are not interchangeable
and a figure must not present one under the other's caption. Optimization
results are accepted only from repeated runs with the frozen topology,
placement and 2.5 GHz clock.

---

# The optimization line [BUILT baseline, DESIGN work]

This line is independent of the stages: it changes cost, never admission, and
must not be measured across a run that also changes correctness behavior.

## What is known

L7 capacity is bounded by per-session cost, not by the transport. The total is
measured: `bench/suite/l7_session_cost.sh` moves the reconnect rate against an
`L7_BACKEND=null` control on the identical workload, and least squares over both
sets puts a DPUmesh connection at **73 ARM core-µs** and the Linkerd session on
top of it at **1,200** — so 1,127 µs is the Linkerd share. The receipts are in
[`REPORT_L7.md`](../bench/report/REPORT_L7.md) and
`bench/report/data/l7-session-cost-20260817` (the L4 control fits live in
`l4-churn-control-20260817` and `l4-session-control-20260817`).

The synchronous half of that is instrumented in `linkerd/app/src/lib.rs` and
reported by `SessionMetrics::observe_stack_build`. Over 9,565 opens and closes
across four workers:

| Phase | Per session |
|---|---:|
| `configure` (clone the outbound template, set `dmesh_session`) | 5.9 µs |
| `layers` (`build_policies` + `outbound.mk`) | 107.8 µs |
| `service` (`NewService::new_service`) | 34.7 µs |
| synchronous total | **148.5 µs** — about one eighth of the 1,200 |

So the synchronous `outbound.mk` boundary is about one eighth of the slope, not
the four fifths the earlier estimate implied. The remaining seven eighths is
lazy discovery and policy work, task execution and teardown, and the surrounding
DPUmesh lifecycle. **Locating it requires instrumenting those asynchronous
boundaries; do not assume the whole figure is inside the synchronous call, and do
not conflate template caching with watch sharing — they attack different parts.**

## O1 Reduce per-session stack construction

- [x] Re-measure the total against the frozen baseline and record it in
  `bench/report/`. Done: 1,200 ARM core-µs per session against a 73 µs L4
  control, published in `REPORT_L7.md`.
- [ ] Instrument the untimed remainder: policy discovery, destination/profile
  discovery, reconnect layers, endpoint construction and balancer construction.
  Extend `SessionMetrics` rather than adding a second surface. This is the whole
  of O1's remaining unknown: 1,051 of the 1,200 µs are unattributed.
- [ ] Cache only immutable templates. `SessionToken`, backend channel, workload,
  target generation, cancellation and metrics must stay session-local — the
  connector binds to `dmesh_session`, so a shared service would take another
  session's channel (`two_same_service_sessions_take_their_own_channels` in
  `outbound/src/tcp/connect.rs` is the regression test for exactly this).
- [ ] Share watches only by authoritative `(workload, target, generation)`.
  Before sharing, add tests for: an update arriving while two sessions share a
  watch; a withdrawal invalidating a shared watch; and the last consumer
  releasing it.
- [ ] Accept only a repeated hardware improvement with no p99 or correctness
  regression.

## O2 Direct `AsyncWrite` reservation

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

## O3 Conditional worker-local state

- [ ] Re-profile after O1/O2; continue only if endpoint locking becomes
  material. Nothing counts lock contention today: `Backends` holds one
  `parking_lot::Mutex` with no instrumentation, and the profile in `REPORT_L7.md`
  puts the AArch64 parking-lot fast path at 1.3–1.7% with no pool symbol above
  it. Add a counter before drawing a conclusion from that.
- [ ] If justified, prototype only the DPUmesh specialization on Tokio `LocalSet`
  with `Rc<RefCell<_>>`; do not add unsafe `Send`/`Sync` claims or modify stock
  TCP Linkerd behavior.

## O4 Optional shared backend transport

- [ ] Attempt only if session-stack work still shows a material need.
- [ ] Define a separately versioned backend-transport id, independent lifetime,
  stream response routing, flow control and cancellation. Do not overload the
  current client connection handle.

## O5 Equivalent ARM/x86 study

- [ ] Build an equivalent x86 full-stack harness with the same Linkerd revision,
  control plane, compiler, flags, request sizes and concurrency.
- [ ] Compare cycles/request, instructions, cache misses, migrations, context
  switches, latency, throughput, affinity and clock frequency.
- [ ] Separate `DmeshIo`, Tokio, opaque stack, protocol detection,
  policy/identity and full-stack slices before proposing a runtime change.

---

# Parameters

Every value here is a proposal with a reason, not a measurement. The ones marked
∎ change a security property and the rest change only cost.

| Name | Proposed | What it decides |
|---|---|---|
| `GENERATION_INTERVAL` | 5 s | ∎ how long a hostile node can claim a Pod that left it |
| `ASSERT_LIFETIME` | 300 s | ∎ how long a relayed assertion stays usable — as today (`DMESH_GRANT_MAX_LIFETIME_SEC`) |
| `ASSERT_CLOCK_SKEW` | 30 s | ∎ tolerated agent→DPU clock spread, one-sided on `issued_at` — as today |
| `ASSERT_REPLAY_SLOTS` | 4096 | ∎ consumed assertion ids retained (evicting ring; the nonce is the hard bound) — as today |
| `CHANNEL_IDLE` | 60 s | when an idle peer channel is evicted |
| `CHANNEL_MAX` | 256 | peer channels one DPU keeps open, LRU beyond it |
| `PEER_STREAMS_MAX` | 4096 | ⟨T⟩ concurrent streams one peer may hold at a destination |
| `PEER_STAGING_MAX` | 16 MiB | ⟨T⟩ staging bytes one peer may hold at a destination |
| `PEER_OPEN_RATE` | 1000/s | ⟨T⟩ stream opens accepted from one peer |
| `PEER_TX_INFLIGHT_MAX` | 16 MiB | ⟨T⟩ un-ACKed bytes (L4 pieces + L7 chunks) one peer may pin at a **source** |
| `GEN_POD_MAX` | 65536 | meshed Pods a generation may name, refused rather than truncated |
| `GEN_NODE_MAX` | 1024 | nodes a generation may name |
| `GEN_SERVICE_MAX` | 4096 | Services a generation may name |
| `GEN_ENDPOINT_MAX` | 65536 | endpoints a generation may name |
| `TOPOLOGY_MAX_BYTES` | 16 MiB | generation byte bound (the 256 KiB membership bound is one node's) |
| `CONTROLLER_KEYS_MAX` | 4 | controller public keys held for rotation overlap |
| `PEER_QP_PER_NODE` | A | queue pairs per node pair — one per destination worker (× `CHANNEL_MAX` = the real QP bound) |
| `STREAM_ACK_BATCH` | 64 | delivery acknowledgements staged before one is sent |

`GENERATION_INTERVAL` and `CHANNEL_IDLE` are the two that interact: a channel
evicted and reopened between two generations pays setup twice, which is M2.

---

# Stages

Each stage names the files it edits, the data it moves and the gate that decides
whether it is done. Each is independently deployable. The order is chosen so
that every stage's verification inputs exist before the stage needs them; the
one transitional mechanism (dual-carry registration in S3–S4) is called out
where it lives.

## S1 — Bind the assertion to its node [BUILT]

The v1 grant already carries a signed `node_name` (filled by the agent from
`spec.nodeName`, canonical-checked, MAC-covered). What is missing is only the
comparison.

*Edits:*
- `doca/object.h`: `char node_name[254];` on `struct objects`.
- `doca/dpu_main.c`: read `DPUMESH_NODE_NAME`; empty or unset refuses startup,
  same as a missing keyring.
- `doca/workload_grant.h/.c`: `dmesh_grant_verify_v1` gains
  `const char *expected_node`; the check sits after canonical form and before
  the time check; new result `DMESH_GRANT_WRONG_NODE`, reported as reason
  `wrong-node`.
- `doca/comch_server.c`: pass `objs->node_name`; map the new result to its
  reason string.
- `bench/bench.sh`: export `DPUMESH_NODE_NAME` to `dpumesh_dpu` (the value the
  agent already receives via the downward API).
- `tests/workload_grant_test.c`: a grant minted for `worker-2` verified on
  `worker-1` fails `WRONG_NODE`; the matching case still passes.

*Gate:* an assertion minted for another node is refused and counted as
`wrong-node`; a matching one still registers; one node is otherwise unaffected.

## S2 — Separate the signing roles [BUILT]

Feed signing and assertion signing share one keyring today —
`pod_membership.c` and the Service-target verifier both call
`dmesh_feed_verify` with `DPUMESH_REGISTRATION_KEY_DIR`, and the header states
the coupling as intent. Splitting the roles is prerequisite to S3 taking the
signing capability off the DPU.

*Edits:*
- `doca/workload_grant.c`: `dmesh_feed_verify` takes its directory from
  `DPUMESH_FEED_KEY_DIR`; `dmesh_grant_verify` keeps
  `DPUMESH_REGISTRATION_KEY_DIR`. Key selection is filename-driven
  (`<dir>/<key-id>.key`), so the two directories must be **disjoint sets of
  files** — the deploy tooling enforces it, and the gate checks it.
- `bench/workload_attest.sh`, `bench/linkerd_service_registry.sh`: generate and
  install a distinct feed key; publishers sign feeds with it.
- `doca/pod_membership.c`: configuration error message names the new variable.

*Gate:* a feed signed with the assertion key is refused, and the reverse; the
Service-target publisher no longer reads a key that can mint identity; no key
file appears in both directories.

## S3 — Asymmetric assertions [BUILT]

*Edits:*
- `doca/comch_common.h`: `struct dmesh_workload_assert_msg` exactly as
  specified in *Identity* (1134 bytes, static-asserted), new message type
  `DMESH_MSG_WORKLOAD_ASSERT`, version 2.
- `doca/workload_grant.{c,h}`: verify Ed25519 (OpenSSL `EVP_DigestVerify` with
  `EVP_PKEY_ED25519` — libcrypto is already linked for HMAC) instead of HMAC;
  check order and reasons as tabled; per-node public key loaded from a file the
  deploy tooling installs (retired in S4 when the generation supplies it).
- `bench/workload_attest_agent.py`: sign with the node's Ed25519 private key
  (python `cryptography`); fill `pod_ip` from `status.podIP`; keep `MAX_TTL`.
- `doca/object.h` / `doca/comch_server.c`: retain `namespace_name[64]`,
  `service_account[254]` and `pod_ip[16]` on `pod_state` beside `workload` and
  `pod_uid` — S9 and intra-node step ④ consume them.
- `src/dmesh_attest.{c,h}`, `src/dmesh_core.c`: relay the v2 message; the
  registration request carries **both** the Service name and, transitionally,
  the legacy number — the name is authoritative for identity (check 8), the
  number keeps the data-plane tables running until S5 interns ids on the DPU. A
  name/number disagreement is refused once S4's generation can adjudicate it.
- `tests/workload_grant_test.c`: forged signature, wrong node, expired
  lifetime, replayed id, bad `pod_ip`, and a v1 message presented to the v2
  verifier.

*Gate:* the DPU holds no key that can sign an assertion; the listed tests pass;
a registration round trip is measured against the 73 µs connection cost.

## S4 — Topology generation [BUILT]

*Edits:*
- `controller/`: the reader and publisher; one Kubernetes reader; emits the
  generation exactly as specified in *Components*; signs with Ed25519.
- `doca/topology.{c,h}`: `dmesh_topology_adopt(objs, path)` with the adoption
  contract `doca/pod_membership.c` already implements — strictly newer,
  completely parsed, signature-verified, size-bounded, staged-then-swapped —
  and `dmesh_gen_verify` (Ed25519 over the signed prefix, keys from
  `DPUMESH_CONTROLLER_KEY_DIR`). In-memory tables: pods by UID (hash),
  services by `namespace/name`, endpoints per service, nodes and their keys.
  Interning: the DPU assigns each Service named by the generation a free id in
  `[0,127]`, stable across adoptions while the Service persists.
- `doca/dpu_worker.c`: poll `DPUMESH_TOPOLOGY_FILE` on the control thread at
  the membership cadence.
- From this stage the assertion's `key_id` resolves through the generation's
  `node=` entry for this node; the installed keyring remains only the bring-up
  fallback for a DPU that holds no generation yet.
- A `protected=` line naming a Service no `service=` line defines refuses the
  document, by the same rule as a dangling `endpoint=`: a dangling protection
  is a typo that would fail open at enforcement time.
- `tests/topology_gen_test.c`: unsigned, oversized, malformed, rolled-back,
  unknown-key, bad-signature, duplicate-pod, dangling-endpoint and
  dangling-protected, bound overflow — each changes nothing and is counted;
  adoption, lookups, node-key resolution and interning stability.
  (`tests/topology_test.c` was already taken by the worker-topology math.)

Nothing consumes the tables for routing yet.

*Gate:* an unsigned, oversized, malformed or rolled-back generation changes
nothing; adoption is counted; bounds are enforced rather than truncated; the
DPU verifies a generation while holding no key that could have signed it.

## S5 — Names outside, handles inside [BUILT]

*Edits:*
- Delete `bench/k8s/registry` and every `COPY` of it in
  `bench/docker/*.Dockerfile`, `bench/validators/*.Dockerfile`,
  `integrations/grpc/bench/docker/*.Dockerfile`; delete the file-loading path
  in `src/dmesh_resolve.c`.
- `doca/comch_common.h`, `doca/comch_server.c`: the `RESOLVE`/`RESOLVE_ACK`
  messages as specified in *Naming*, answered from the topology tables.
- `src/dmesh_core.c`, `src/dmesh_preload.c`: `dmesh_create_qp` and the facade's
  `connect()` resolve through the DPU with the caching rule in *Naming*;
  leaving the mesh becomes an explicit logged decision (`status = 1`); the
  registration path drops the legacy number (ends the S3 dual-carry).
- `doca/dpu_proxy.c`: `DPUMESH_L7_SVC` / `DPUMESH_L7_OPAQUE_SVC` /
  `DPUMESH_L7_DECISION_SVC` accept `namespace/name` lists; `px_l7_resolve_modes`
  resolves names through the interning table, re-run on each adoption.
- `include/dpumesh/dmesh_common.h`: `#define DMESH_POD_REMOTE (-2)`.
- `design/API.md`: a pod id is a node-local transport identifier, not workload
  identity.

*Gate:* no Service number appears in any file a Pod image carries; a Service
renumbered in Kubernetes needs no redeploy; ClusterIP resolution still works for
the POSIX facade; an unmeshed destination is logged, not silent.

## S6 — One channel [BUILT]

The node agent is the DPU's only control peer. The three host *user* systemd
units (`dpumesh-membership`, `dpumesh-linkerd-service-registry`,
`dpumesh-linkerd-identity-agent`) and every `rsync`/`ssh` hop they used are
gone.

*Edits:*
- A delivery hop with these properties, and no more: unattended (survives a
  node reboot with no operator session), least-privilege (it can install the
  four feed files — `membership`, `service-targets`, `identity-bundle`,
  `topology` — and nothing else: no login shell, no sudo), atomic
  (a temporary beside the target, then rename — the contract every consumer
  already assumes)
  and size-bounded at the door. It needs no authentication authority of its
  own: every feed is signed and every consumer refuses what it cannot verify.
  Two stock implementations qualify — an ssh key restricted to a forced
  command (`command="dpumesh-install-feed"` in `authorized_keys`, no pty, no
  forwarding), or a minimal receive daemon on the management interface.
  Either satisfies the gate; what the gate refuses is today's arrangement, a
  user unit piping a sudo password over an operator's login path.
- The node agent gains the delivery loop (retry with backoff,
  resend-on-reconnect) and absorbs `bench/linkerd_cp_relay.py` — the
  gateway DaemonSet and the agent DaemonSet become one Pod with two listeners.
- The Service-registry publisher derives its adapter-format feed from the held
  generation instead of its own `kubectl` reads — one source of truth.
- Remove the user units and every `ssh`/`rsync` hop from
  `bench/workload_attest.sh`, `bench/linkerd_service_registry.sh`,
  `bench/linkerd_identity.sh`.

*Built:* the delivery hop is the receive daemon
(`bench/dpumesh_feed_receiver.py`), an unprivileged account under a system unit
installed once by `bench/workload_attest.sh install-hop`. It can write the four
feeds and nothing else — plus one read, the DPU static public key S7's node
report travels through — bounds each payload at the door, and installs by
rename. Its framing offers the document's digest first and transfers only what
the far end does not hold, so a resend is decided against the receiver's actual
state rather than the sender's memory of it — which is what makes a restart at
either end converge. The delivery loop and the feed derivations live in
`bench/feed_delivery.py`; the agent process (`bench/workload_attest_agent.py`)
runs them, fetches the generation from the controller's cluster-network
listener, derives the Service-target feed from that generation, mints the
identity bundle's short-lived half itself, and absorbs
`bench/linkerd_cp_relay.py` as a second listener in one Pod.
`tests/feed_delivery_test.py` covers the whitelist, the bounds, the atomic
install, the bundle framing, the resend and the derivation.

*Gate:* a node reboot restores every feed with no operator session; no login
shell, password or sudo appears in the control path; losing one publisher
stops its updates without revoking membership or withdrawing targets.

## S7 — Node credentials and peer channels ⟨T⟩ [BUILT except the transport]

*Edits:*
- `dpumesh_dpu` generates its static peer-channel keypair at first boot
  (0400 file); the agent reports the public half to the controller; the
  controller publishes it in the node's `node=` line.
- `doca/peer_channel.{c,h}`: the state machine, the handshake from
  *Authentication and pairwise keys*, the five control messages, the handle
  table (`struct peer_handle`), per-peer destination bounds
  (`PEER_STREAMS_MAX`, `PEER_STAGING_MAX`, `PEER_OPEN_RATE`) and the per-peer
  source bound (`PEER_TX_INFLIGHT_MAX`). One QP per (node pair, destination
  worker); handshake once per node pair.
- `doca/dpu_proxy.c`: the deferred-release lists — L4 pieces and L7 arena
  chunks whose destination is remote retire on `STREAM_ACK`, per *Two custody
  domains*; `POD_GONE` emitted where the local sweep runs today; per-worker
  refusal and poison counters (`stat_poison`, `stat_peer_refused`), surfaced
  via `l7_control_event` as `dmesh_control_events_total{kind="peer",reason=…}`
  where the adapter runs and in the worker stat line always.

*Built:* `doca/peer_channel.{c,h}` carries the state machine and its
incarnations, the binding rule the stock handshake cannot know (the peer's
static key must equal the one the held generation binds to the node name it
claims — re-checked on every adoption by `dmesh_peer_table_rebind`, which
resets a channel the generation dropped or re-keyed), the five control
messages and their bounded parsing, the handle table with both generations,
the destination bounds (`PEER_STREAMS_MAX`, `PEER_STAGING_MAX`,
`PEER_OPEN_RATE` as a token bucket), the source bound
(`PEER_TX_INFLIGHT_MAX`) and the un-ACKed slot pool `STREAM_ACK` retires.
Staging is charged per `DATA` arrival; a delivery the node holds (an
asynchronous landing) keeps its charge and defers its acknowledgement until
`dmesh_peer_delivered` reports the bytes in the destination Pod's mapping,
which is what makes `PEER_STAGING_MAX` a real bound. The node credential is
generated on the DPU at first boot into a 0400 file
(`dmesh_peer_node_key_load`); the agent reports its public half and the
controller publishes it in the node's `node=` line. `doca/dpu_proxy.c` carries
the hooks the table binds to — the deferred release (`px_peer_release`, L4
piece and L7 arena chunk by their own custody), the `POD_GONE` emission at the
same sweep the local case uses, and the refusal and poison counters, surfaced
as `dmesh_control_events_total{kind="peer",reason=…}` and in the worker stat
line — but nothing instantiates the table in production until the transport
lands, because no channel can open without one; the module is driven
end-to-end by `tests/peer_channel_test.c` through the recording transport,
covering every gate clause below that does not need bytes on a wire.

*Not built — and stated rather than implied:* the transport itself. The RDMA
layer is *Out of scope* by this document's own boundary, so the byte-carrying
half of a cross-node stream has no implementation here: `struct
dmesh_peer_transport` is the seam it plugs into, and the tests drive the module
through a recording transport rather than a fabric. What the transport still
owes is exactly the property table in *What the transport must provide*.

*Gate:* two DPUs authenticate and exchange a full-duplex stream; a channel to a
node the generation does not bind is refused; a reconnection invalidates every
handle from the previous incarnation; `POD_GONE` and peer loss both release
handles; a peer cannot exceed its destination bounds and a stalled peer cannot
pin more than `PEER_TX_INFLIGHT_MAX` at a source while other peers' streams
keep flowing; an L4 sender's capacity returns only after the bytes reach the
destination Pod's mapping; an L7 arena chunk is reusable only after its
`STREAM_ACK`; every refusal is counted by reason. Every clause but the first
is met against the recording transport; the first needs a fabric.

## S8 — Reach [BUILT except the remote data path]

*Edits:*
- `doca/dpu_worker.c` / `doca/dpu_proxy.c`: `collect_live_hosts` is replaced by
  an endpoint set derived from the generation, split local and remote;
  `px_resolve_backend` prefers local and falls to remote; `px_unit_prepare`
  stops treating an empty local set as terminal (today the same `-1` poisons
  both "no live host" and "host not ready" — the remote case becomes a third,
  routable outcome).
- `src/dmesh_core.c`: `dmesh_qp_t.remote_pod` reports `DMESH_POD_REMOTE` for a
  peer on another node; replies to a remote peer route from conntrack exactly
  as `px_unit_prepare` already overrides host-supplied destinations.

*Built:* the remote half of a backend set comes from the generation
(`dmesh_topology_remote_endpoints`, `collect_remote_hosts`), split against this
node's own name. The local half stays registration-derived, which is the one
deviation from the sentence above and is deliberate: a Pod that registered
between a generation's snapshot and its publication is absent from that one
generation without having stopped serving, and the controller's fail-static
table already promises it keeps serving intra-node. `px_resolve_backend`
prefers local and falls to `PX_DST_REMOTE`, which is the third outcome
`px_unit_prepare` now distinguishes from "host not ready" — a Service healthy
elsewhere is reported and counted as remote-only rather than poisoned as a
Service with no replicas. `DMESH_POD_REMOTE` is what a host reads for a peer on
another node, and the reply routes from conntrack as it already does.

*Not built:* carrying the bytes, which is S7's transport. Until one exists a
remote-only Service is counted `peer.transport` and refused, which is the
honest state and not the old silent poison.

*Gate:* a Service with no local replica is served, not poisoned; a Service with
a local replica still takes the DMA path; `fail=0 reorder=0` across a node
boundary; a reply to a remote peer routes from conntrack with no host-supplied
destination.

## S9 — Inbound authorization [BUILT]

*Edits:*
- Fork (`linkerd/port/linkerd2-proxy`): expose `connection_verdict` as
  specified in *Authorization*; no other stock behavior changes.
- `linkerd/rust/src/lib.rs`: remove `disable_inbound_policy_discovery()`; per
  registered destination Pod, one `build_policies(workload)` + one
  `WatchPort` per port, cached on the registration, dropped with it; delete
  the synthetic `pod_addr` mapping — the client address presented to the
  verdict is the signed `pod_ip:src_port`.
- `linkerd/include/dmesh_l7.h`: `struct dmesh_l7_flow` gains
  `char source_identity[254];` (offset 410; struct grows 410 → 664) built as
  `<service-account>.<namespace>.serviceaccount.identity.<trust-domain>` from
  the retained registration (intra-node) or the generation (cross-node), and
  `src_ip` carries the real Pod IP. The Rust mirror's `offset_of!` assertions
  and `design/L7.md` record the layout; `linkerd/shim/l7_null.c` follows.
- `doca/dpu_proxy.c`: the destination-side verdict is asked before a stream is
  admitted to a registered Pod, intra-node and cross-node through the same
  path; refusals counted by reason.
- **Observation gate before enforcement:** call `WatchPort` for a test Pod's
  workload from the DPU through the existing gateway and record whether the
  policy controller serves, denies, or serves empty to a caller authenticated
  as `dpumesh-dpu`. The result does not change this design (S10 scopes the
  credential regardless); it is recorded because enforcement must not be
  enabled on an unobserved dependency.

*Built:* the fork exposes `connection_verdict` and `AllowPolicy::admits`;
`disable_inbound_policy_discovery()` is gone and one
`build_policies(workload)` store is bound per registered destination Pod
(`App::dmesh_inbound_policies`), cached on the registration and dropped with it
through `l7_inbound_forget`. The synthetic `10.97.0.0/24` mapping is deleted:
`struct dmesh_l7_flow` carries the signed `src_ip` and a
`source_identity[254]` at offset 410 (410 → 664), and the DPU builds both from
the retained registration. `l7_inbound_verdict` is asked once per stream,
before its first byte reaches a registered Pod, on the same path intra-node and
cross-node. Refusals are counted as
`dmesh_control_events_total{kind="inbound",reason=…}` and in the worker stat
line.

*Observation gate — observed, 2026-08-19:* `bench/bench.sh policy-observe`
records what the policy controller serves to a caller authenticated as
`dpumesh-dpu`. It serves. A destination Pod's stream was admitted by the stock
evaluation against its watched policy —
`dmesh_control_events_total{kind="inbound",reason="admitted"}` and the worker
line `inbound admitted=N denied=0 no-policy=0` both moved, one per session, and
the streams carried. What the controller answers `NotFound: unknown server` to
is a port no Pod serves, which is what the proxy's own ephemeral inbound and
admin listeners are; those keep their configured default policy and are not
discovered, while the Pods the DPU is the enforcement point for are.

The verdict counters are what separate the three cases the gate asks about, and
they are distinct on purpose: `admitted` and `denied` are decisions taken
against a policy that was served, `no-policy` is a dependency that did not
answer, and its reasons name which — `out-of-scope` (S10 refused the question),
`no-subject`, `no-port`. Nothing is refused silently and nothing is admitted
without saying so.

*Gate:* an `AuthorizationPolicy` allow→deny→allow cycle on a live Service
changes admission within one watch update, with no session admitted during
`deny`; a `networks` clause written against the cluster CIDR admits and refuses
by the source Pod's real IP; an HTTP-typed policy admits per the union rule and
refuses a client no route admits; established streams are undisturbed;
refusals are counted by reason; no Pod-supplied identity input exists anywhere
in the path.

## S10 — Scope the control-plane credential ⟨T⟩ [BUILT]

A DPU asks about Pods the generation places on its node. Where the upstream API
cannot express the restriction, the controller mediates the lookup.

*Built:* the controller serves `GET /workload-scope?pod_uid=…` and answers only
for Pods the generation places on the asking node. What binds the question to
its asker is not anything the DPU says: a DPU has no route into the cluster
CIDRs, so the request travels its own agent's relay and the controller resolves
the node from the source address against the addresses Kubernetes records for
it. `doca/control_scope.{c,h}` asks once per registration on the control thread
and again on every adoption; a data worker reads the answer
(`pod_state.scope_state`) and asks about nothing the controller has not
allowed, counted as `inbound.out-of-scope`. An unanswered question withdraws
nothing, which is the same fail-static rule every feed has.

*Gate:* a request for a workload the generation places elsewhere is refused.

## S11 — Exact Linkerd endpoint semantics [BUILT]

`Backends::take_session` refused only a cross-Service placement; Linkerd
weights, TrafficSplit behavior and endpoint failover do not apply inside a
Service because DPUmesh chooses the backend Pod itself (`BACKEND_ANY` on the
data path).

*Design:* resolve Linkerd's selected endpoint address to a Pod UID through the
generation (`pod=` carries the IP; the address's IP identifies the Pod — ports
do not participate in identity), then to the live registration:

```c
/* linkerd/include/dmesh_l7.h, linkerd/shim/l7_null.c, doca/dpu_proxy.c */
int32_t dmesh_l7_pod_for_uid(uint32_t worker_id, const char *pod_uid);
/* ≥0 live pod id · -1 no live registration · -2 placed on another node
 * · -3 the held generation no longer names that UID */
```

*Edits:*
- Adapter: keep `HashMap<IpAddr, PodUid>` per Service beside
  `service_endpoints`, built from the generation; on
  `take_session(selected)`, resolve `selected.ip() → pod_uid →
  dmesh_l7_pod_for_uid`; each negative outcome is a distinct decline —
  `endpoint-unresolved`, `endpoint-remote` (routable after S8, refused before),
  `endpoint-stale` — never a round-robin or TCP fallback.
- Tighten `take_session`: once endpoints are authoritative, an address the
  snapshot does not place is no longer assumed session-own; session-own
  addresses are registered explicitly at publish time and everything else must
  resolve.
- Generation safety: the resolution fails (`-3`) when the held generation no
  longer names the UID; a recreated Pod carries a new UID, so it cannot
  inherit a mapping.
- Apply add/update/delete as one generation.

*Built:* the derived Service-target feed carries the Pod UID beside each
endpoint address (`endpoint=<ns>/<name>,<ip>:<port>,<pod-uid>`), so the adapter
holds the address→UID map the generation defines rather than one of its own.
`dmesh_l7_pod_for_uid` answers from the held generation and the live
registrations, and `Backends::take_session` consults it through an
`EndpointResolver` the adapter installs with each generation: an address the
snapshot does not place is no longer assumed session-own, and every other
address must resolve. The three negative outcomes are distinct declines —
`EndpointUnresolved`, `EndpointRemote`, `EndpointStale` — and the connector
already refuses to dial TCP on any of them, so neither a round robin nor a
fallback can carry a protected stream somewhere its policy never named. Ports
do not participate in identity: the address's IP is what names the Pod.

*Gate:* rolling update — no request reaches a Pod whose UID left the
generation; zero ready endpoints — sessions are refused, not round-robined;
weighted distribution matches Linkerd's own within the balancer's tolerance;
control-plane reconnect does not resurrect a stale mapping.

## S12 — Mixed protected and unprotected Services [BUILT]

Every registration is attested, so every Service is protected to the same
degree. A real deployment grades protection per Service, and that choice must
not be inferable from Pod input — otherwise a Pod opts itself out.

*Design:* the `protected=<namespace>/<name>` lines of the generation. A Service
in the set carries the strict policy; a Service outside it carries the relaxed
one. Every registration stays assertion-verified either way; what the feed
grades is the interaction rules, not whether a Pod is attested.

- A protected Service calling an unprotected one: the callee cannot be
  authenticated, so the call is refused unless the destination's policy
  explicitly permits it; counted as `mixed-callee-unprotected`.
- An unprotected Service calling a protected one: admission is decided by the
  protected side, which is S9's enforcement point.

*Edits:* consume the set in `pods_register` and in `px_parse_l7`'s fail-closed
decision, replacing the process-wide `DPUMESH_L7_FAIL_CLOSED`; distinct
counters per rule.

*Built:* the generation's grading is cached per interned Service
(`px_protection_refresh`, run by the control thread on adoption), so a data
worker's decision is a byte read. `px_parse_l7`'s two fail-closed decisions ask
the Service rather than the process; `DPUMESH_L7_FAIL_CLOSED` survives only as
the default for a Service no generation grades, which is the deployment that
has no controller. `pods_register` records the class it registered under.
`dmesh_inbound_admits` in `doca/dpu_proxy.h` states the three rules as one
decision over its inputs and `tests/l4_pin_policy_test.c` pins each: a served
verdict decides alone, an ungraded Service carries the stream (so enabling
enforcement cannot refuse traffic no policy ever named), and a protected caller
reaching an unprotected callee is refused and counted
`mixed-callee-unprotected` unless the callee's own policy admitted it.

*Gate:* moving a Service from unprotected to protected takes effect at the next
generation, without restart, and rejects the next unverified registration; no
Pod input changes which side of the boundary its Service is on.

## Order

```text
S1 node check ── S2 key roles ── S3 asymmetric assertions ──┐
                                                            │
                              S4 topology generation ───────┤
                                                            │
        S5 naming ── S6 one channel                         │
                                                            │
        S7 peer channels ── S8 reach ── S9 authorization ── S10 scope
                                             │
                                 S11 endpoints    S12 mixed mode

O1–O5 (optimization) run beside the stages and must not share a measurement
run with any stage that changes admission behavior.
```

S1–S6 are worth doing whether or not the transport lands. S4 is what makes
S5 and S7–S12 possible. S10 stands alone.

---

# Acceptance invariants

- [ ] One ARM worker owns each connection and all reachable session state.
- [ ] Every accepted staging extent is released exactly once and is never read
  after release — on the L4 path by delivery (or counted error/drop), on the
  L7 path by stack consumption; an arena chunk by delivery.
- [ ] Close and revocation are generation-safe and idempotent from every path.
- [ ] Protected traffic has no unauthenticated admission, silent TCP/L4
  fallback, cross-Service rewrite, drop or reorder.
- [ ] A missing, malformed, unsigned or rolled-back authoritative feed or
  generation never withdraws membership, revokes a registration or admits an
  unverified one.
- [ ] No component below the controller holds a key that can create an identity
  beyond its own scope: the agent signs for its node, the DPU signs nothing.
- [ ] After each gate, opened equals closed and active/pending/tasks/backend
  entries are zero.
- [ ] Every refusal — assertion, admission, peer, poison, endpoint — is counted
  by reason; nothing is refused silently.
- [ ] Optimization results use repeated runs with the frozen topology, placement
  and 2.5 GHz clock; otherwise they are not accepted.
- [ ] A capacity is quoted with the instrument that produced it (`knees.csv`
  twice-voted vs. rate grid — never one under the other's caption).

Observed on hardware, 2026-08-19, with S1–S12 deployed on one node: every feed
the DPU consumes arrived through the node agent alone and was adopted
(`topology.adopted`, `membership.ok`); the DPU generated its node credential
and the controller published its public half in the generation's `node=` line;
the mediated lookup answered 200 for a Pod the generation places here and 404
for one it names nowhere; registrations recorded their protection class; and
the destination-side verdict ran once per session and admitted
(`inbound admitted=5 denied=0 no-policy=0`, sessions opened 5 = closed 5,
active 0). The invariants above stay unchecked because several of them are
about the node boundary, and the boundary has no transport to cross yet.

---

# Out of scope

- The RDMA transport itself: framing, flow control, connection lifecycle.
- Sharing one DPU across tenants.
- Replacing Linkerd as the source of policy.
- Route-level HTTP authorization at the destination. The connection-level
  verdict needs no parser; a route-level one needs a second L7 stack, which is
  the cost the source/destination split exists to avoid. Istio ambient draws the
  same line between `ztunnel` and a waypoint. (Its observability face is stated
  in *Observability*: no server-side HTTP metrics cross-node.)
- Per-workload certificates on the wire. The topological check gives the same
  blast radius under node compromise at a fraction of the cost. It becomes
  necessary only when peer nodes are not trusted to speak for their own Pods,
  which is a multi-tenant concern and not this deployment's.

---

# Open questions

Everything else in this document is decided. What remains cannot be settled by
reasoning, only by running it.

**M1 — How short must `GENERATION_INTERVAL` be?** ∎ It bounds how long a
compromised node can still claim a Pod that has left it (*Freshness*). An
honest node stops originating the moment the registration ends, so the window
applies only to a node that is already hostile. A shorter interval costs
controller and agent work and shortens ordinary skew as well; a revocation
message would close the window exactly and add a mechanism. The interval is the
cheaper answer until a measurement says it is not.

**M2 — Does a lazily opened channel amortize?** The design assumes the first
stream to a node pays for the channel and the rest do not. That holds for
long-lived Pods over a stable set of peers. It does not obviously hold when Pod
lifetimes are short and peers many, where a pair may be evicted between uses and
every stream is a first stream. `CHANNEL_IDLE`, `CHANNEL_MAX` and the Pod
lifetime distribution decide it together.

**M3 — What does the policy controller actually serve to a non-workload
caller?** ∎ *Observed, 2026-08-19: it serves.* `PortSpec.workload` is a plain
string and the DPU authenticates as `dpumesh-dpu`; the controller answered for
the destination Pods this DPU serves, the stock evaluation admitted, and the
streams carried (S9's observation gate). That is one cluster and one version,
so it is a recorded observation rather than a property — which is why S10
scopes the credential regardless, and why the answer changes nothing about the
security claim.

All three are in the measurement table or a stage gate. None blocks
implementation: M1 has a working default, M2 changes a constant, M3 is a
recorded observation inside S9 and is now recorded.

---

# Decisions taken

Recorded because each was a real fork, and because a reader should not have to
reconstruct why the other branch was refused.

| Question | Decision | Refused alternative |
|---|---|---|
| Is a compromised DPU in the threat model? | yes | believing a peer's claim, which gives one compromised node every identity in the cluster |
| Cluster-scope vs node-scope signing primitive? | the generation is Ed25519, DPU holds public keys only; node-local feeds may stay HMAC | one symmetric primitive for both, which lets any one DPU forge cluster topology |
| Where does custody release across nodes? | at the destination Pod's memory — L4 pieces and L7 arena chunks both retire on `STREAM_ACK`; the L7 Pod-side release stays on stack consumption, as intra-node | pretending one custody semantics covers both modes, which the intra-node tree already contradicts; or releasing at the source proxy, which credits bytes nobody received |
| Source address for authorization? | the signed Pod IP, carried in the assertion and the generation | the synthetic `10.97.0.0/24` mapping, which no realistic `networks` clause admits and nothing signs |
| HTTP-typed server policy at the destination? | connection verdict from the union of route authorizations, over-admission stated | refusing HTTP-typed ports (breaks stock policies); or a second parser (the cost this split avoids) |
| Service identity? | namespace-qualified everywhere; the assertion reuses its `namespace_name` as the qualifier | bare names, ambiguous by construction; or one packed field too small for `name.namespace` |
| Does a stream have one handle or two? | one, full-duplex | two, which splits a `dmesh_qp_t` the API defines as one stream |
| Which worker owns a cross-node stream? | each side by its own rule; one queue pair per (pair, destination worker) | one queue pair per node pair with a cross-worker handoff on every completion |
| Where does the L7 stack run? | outbound at the source; a policy verdict at the destination | a full stack on both sides, which doubles the measured session cost |
| Who issues node credentials? | the controller; the DPU's one static keypair never leaves the DPU, its public half travels via the generation | `linkerd-identity`, which is a fork to maintain |
| Peer-channel handshake? | a stock protocol (Noise IK / TLS 1.3 raw public keys) plus one binding rule against the generation | a bespoke handshake, which adds nothing the stock one lacks and everything it can get wrong |
| Signing key in a replicated controller? | one replica by default; election does not reduce key copies | claiming replication protects the key, which a Secret readable by every replica does not |
| Peer that is refused repeatedly? | bounded and reported (per-worker refusal and poison counters); the controller evicts | tearing the channel down, which turns ordinary generation skew into an outage |
| A stalled peer at the source? | `PEER_TX_INFLIGHT_MAX` pins its holdings, `px_stall` on its streams | letting un-ACKed chunks drain the shared arena, which stalls Pods that never spoke to that peer |
| What does a refused source tell its Pod? | `px_poison` and EOF, as within a node | a distinct failure the application would have to learn |
| ClusterIP for the POSIX facade? | answered by the DPU from the generation, cached for one `GENERATION_INTERVAL`; leaving the mesh is logged | a file in the Pod image, which is the fail-open path this design removes — silently, today |
| Registration during the naming transition? | dual-carry (name authoritative for identity, number for tables) from S3 until S5 interns ids | name-only before the DPU can intern, which strands the data plane; or number-only, which defers the identity fix |
