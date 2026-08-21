# DPUmesh Control Plane

The control plane answers one question at two scopes: how the DPU learns who is
calling and where the call may go. Inside a node, a host-resident agent binds a
Comch connection to a Kubernetes Pod and the DPU admits the registration.
Across the cluster, a controller publishes a signed generation of every fact a
DPU needs about Pods it cannot see, and a DPU authenticates its peers against
it. This document is both halves and the boundary between them.

The data plane those admissions govern is [`DATA.md`](DATA.md); the
application's own contract is [`API.md`](API.md). The lower RDMA implementation
supplies `struct dmesh_peer_transport`; this tree owns and instantiates every
authenticated stream, routing, custody and lifetime rule above that seam. The
data path has one shape on both sides of a node boundary, and only the middle
hop differs.

```text
   intra-node   host TX mmap → DPA → staging → SG-DMA         → host RX mmap
   inter-node   host TX mmap → DPA → staging → RDMA → staging → host RX mmap
```

Arrival custody, lanes, credits and reverse publication are unchanged, with one
qualification stated where the peer channel needs it: node-local custody is two
stories, one per data-path mode, and the boundary extends each differently.

## Terms

| Term | Meaning |
|---|---|
| node agent | a root-owned DaemonSet on the host that reads Kubernetes objects and signs claims |
| assertion | the node-agent-signed statement of a Pod's identity and authorized Service that registration must present |
| feed | a file the DPU reads for authoritative data: node membership, Service targets, or the cluster topology |
| generation | one version of a feed, installed whole by atomic rename and numbered monotonically |
| topology generation | the cluster-scoped generation: one signed snapshot of every cluster-wide fact a DPU needs |
| controller | the cluster-scoped publisher, `controller/dpumesh_controller.py`; holds the issuing key |
| workload | the Linkerd identifier of the calling Pod, `{"ns":…,"pod":…}`, built from signed claims |
| gateway | the node agent's host-network TCP pass-through leg, which carries the DPU's control-plane connections without reading them |
| admission switch | a file that stops new protected sessions while established ones continue |
| node credential | one static keypair per DPU, generated on the DPU, used to authenticate to peers |
| peer channel | the authenticated, long-lived connection between one pair of DPUs |
| incarnation | the generation number of a peer channel; every handle and in-flight operation carries it |
| handle | one full-duplex cross-node stream, allocated by the destination DPU |
| protection class | whether the generation grades a Service protected or unprotected |

## Threat model

One DPU in the fleet is compromised. Every other component behaves.

| Party | Assumed | Consequence |
|---|---|---|
| workload Pod | hostile | states nothing about itself; the assertion names it |
| host kernel | honest | it is the source of attestation evidence |
| node agent | honest | it holds the only key that can assert for its node |
| **one DPU** | **hostile** | it must not be able to speak for another node's Pods |
| controller | honest | it holds the only key that can bind a Pod to a node |

A compromised DPU keeps what it already has: its own Pods' memory, their traffic
and its own reported metrics. That is irreducible — a sidecar and a `ztunnel`
lose the same on a compromised node. What the design denies it is reach beyond
its node:

```text
                        sidecar   ztunnel   DPUmesh          DPUmesh
   one node compromised                     (peer believed)  (this design)
   ─────────────────────────────────────────────────────────────────────
   its own Pods           lost      lost      lost             lost    ← irreducible
   other nodes' Pods      safe      safe      LOST             safe
   other nodes' traffic    n/a      safe      LOST if the      safe
                                              key is shared           (pairwise keys)
```

Three requirements follow, and they are marked ⟨T⟩ where they appear:

- identity claims about another node's Pods are refused;
- inter-DPU keys are pairwise, so one compromised DPU decrypts only its own
  conversations;
- a peer consumes bounded resources at a destination **and a stalled peer holds
  bounded resources at a source**.

One consequence governs the choice of primitive: anything cluster-scoped that a
DPU verifies is verified with a key the DPU cannot use to sign. Node-scoped
feeds stay symmetric — a DPU that forges its own membership harms only its own
node, which the model already concedes — but the generation is Ed25519-signed
and the DPU holds public keys only.

## Three scopes, one component each

| Scope | Component | Holds | Can create identity |
|---|---|---|---|
| cluster | `dpumesh-controller` | issuing key; the topology | yes |
| node, host | node agent | that node's private key | for its own node only |
| node, DPU | `dpumesh_dpu` | public keys; its node credential | no |

The DPU is a different machine across PCIe: it can read host memory the host
exported and nothing else, and DOCA Comch carries no peer credential the way
`SO_PEERCRED` does. Identity evidence is a fact the kernel knows, not a byte a
process hands over, so a DPU cannot by itself answer who is on the other end of
a channel. That is why the node agent is a component of the design and not
deployment tooling, and it is the same reason every shared-proxy mesh keeps a
host-resident half. What the DPU gains for it: the proxy runs on a CPU the
workload cannot schedule on, so keys never enter host memory and counters are
not falsifiable by the workload.

Where a mesh proxy sits on one axis decides both of those consequences:

```text
   near the workload  ◀─────────────────────────────────▶  far from the workload
   identity: free                                          identity: established
   isolation: weak                                         isolation: strong

   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │   Linkerd    │    │Istio ambient │    │  on-node     │    │   DPUmesh    │
   │Istio sidecar │    │   ztunnel    │    │  host proxy  │    │              │
   └──────────────┘    └──────────────┘    └──────────────┘    └──────────────┘
    in the Pod          host process        host process,       a different
                        per node            per node            machine, PCIe

    sees the Pod:       sees the Pod:       sees the Pod:       sees the Pod:
    it IS the Pod       yes, same kernel    yes, same kernel    no
```

The rightmost column is the only one that has to split, and the node agent is
that split. It is also the only one whose proxy the tenant cannot reach.

```text
  ┌───────────────── Kubernetes cluster ──────────────────┐
  │  linkerd-identity   linkerd-destination   dpumesh-    │
  │                     / policy              controller  │
  └──────────────────────────┬────────────────────────────┘
                             │  signed generation
                             │  node credentials published in node=
         ┌───────────────────┴───────────────────┐
         ▼                                       ▼
  ┌──── NODE A ─────────┐               ┌──── NODE B ─────────┐
  │ HOST (x86)          │               │ HOST                │
  │  ┌─────┐   ┌─────┐  │               │  ┌─────┐            │
  │  │Pod P│   │ Pod │  │               │  │Pod Q│            │
  │  └──┬──┘   └──┬──┘  │               │  └──┬──┘            │
  │     │ AF_UNIX │     │               │     │               │
  │  ┌──▼─────────▼───┐ │               │  ┌──▼─────────────┐ │
  │  │ node agent     │ │               │  │ node agent     │ │
  │  │  SO_PEERCRED   │ │               │  │                │ │
  │  │  cgroup        │ │               │  │                │ │
  │  │  node priv key │ │               │  │                │ │
  │  └────────┬───────┘ │               │  └────────┬───────┘ │
  │ ══ PCIe ══╪════════ │               │ ══ PCIe ══╪════════ │
  │  ┌────────▼───────┐ │               │  ┌────────▼───────┐ │
  │  │ DPU (ARM)      │ │               │  │ DPU            │ │
  │  │  public keys   │ │               │  │                │ │
  │  │  node credent. │ │               │  │                │ │
  │  └────────┬───────┘ │               │  └────────┬───────┘ │
  └───────────┼─────────┘               └───────────┼─────────┘
              └── RDMA · mutually authenticated ────┘
                  pairwise keys · one channel per node pair
```

One object crosses both scopes, and the node-scope half below leans on it from
its first paragraph, so it is worth naming here. **The topology generation** is
a single signed, versioned text document the controller publishes, carrying
every node, Pod placement, Service, endpoint and protection grade in the
cluster. A DPU adopts it whole or not at all, and verifies it with a key it
holds no signing half of. It answers what nothing node-local can know: which
Service a name refers to, which node a Pod is on, and whether a Service is
graded protected. *The generation*, under **Cluster scope**, is its grammar and
its bounds.

---

# Node scope

## Resolution

Names live outside, handles inside: the host asks, the DPU answers from the
held topology generation, and the answer carries the DPU-interned compact id —
a node-local transport identifier, never workload identity. No registry file
exists anywhere.

```text
Host Pod                                     DPU
  │──── RESOLVE(name | ClusterIP:port) ─────▶│  answered from the adopted,
  │◀─── RESOLVE_ACK(status, interned id) ────│  Ed25519-verified generation
```

The native API resolves `"name"` (in the Pod's own namespace) or
`"name.namespace"` in `dmesh_create_qp`. The preload facade resolves the IPv4
destination passed to `connect`; a ClusterIP answers `meshed` only when its
Service has a live registered backend here (the generation names unmeshed
Services too), `status = not-meshed` makes leaving the mesh an explicit,
logged kernel-TCP decision, and `status = no-generation` falls back the same
way while a registration fails closed. Answers are cached per
key for one generation interval (`src/dmesh_resolve.c`) and re-resolved after
that or on a connection error, so the cache is never staler than the
generation itself.

## Trusted registration

`$DPUMESH_SERVICE` names the Service provided by the process. An unset or
unknown value creates a client-only channel.

```text
Host Pod                 trusted node agent                    DPU
  │                               │                              │
  │◀──────────────────────── REG_CHALLENGE(nonce) ───────────────│
  │── nonce + requested service ─▶│                              │
  │                               │── SO_PEERCRED/cgroup ─┐      │
  │                               │◀─ K8s Pod/Service ────┘      │
  │◀─ signed immutable claims ────│                              │
  │───────────────────────── WORKLOAD_ASSERT ───────────────────▶│
  │───────────────────── POD_REGISTER(service name) ────────────▶│
  │◀──────────────────────── POD_ASSIGNED(pod_id, stripes) ──────│
  │───────────────────────── MMAP_EXPORT × regions ─────────────▶│
  │◀──────────────────────── POD_INIT_RESULT(READY) ─────────────│
```

The DPU creates a fresh 32-byte nonce for each Comch connection. The root-owned
node agent identifies the Unix-socket peer with `SO_PEERCRED`, re-checks the
peer's process start time
around the cgroup read so a recycled pid cannot be attested as another Pod,
resolves the cgroup to the authoritative Kubernetes Pod, verifies that the Pod
labels select the requested Service, and returns a canonical Ed25519-signed
assertion. It carries key id, issue and expiry, assertion id, nonce, node, Pod
UID, namespace, Pod name, ServiceAccount, Service name and Pod IP; the issuer
is implied by `(node, key id)`. The application relays the assertion; it cannot
change a claim.

The DPU selects the node's *public* key by the signed key id — it holds no key
that can sign an assertion — refuses an assertion naming another node, verifies
and consumes it once, rejects a registration whose Service name differs from
the asserted one (the compact id is the DPU's own interning of the topology
generation, returned in `POD_ASSIGNED`; a Service no generation defines fails
closed), constructs the Linkerd workload JSON from the signed namespace and
Pod, and keeps the signed Pod UID, namespace, ServiceAccount and Pod IP with
the registration. An assertion cannot move to another connection or survive
reconnect or DPU restart, because the nonce is different.

A Pod cannot state a workload or a Service of its own: the assertion is the
only thing that names either, and a registration without one is refused. The
Pod enters backend selection after `POD_INIT_RESULT(READY)`.

Unregister, revocation or Comch disconnect removes the Pod from selection. Each
ARM worker first closes the connection and L7 state it owns. After every producer
has joined that barrier, the workers that own destination lanes drain them and
run one further progress pass, so DOCA has released the buffer references its
completion callbacks hold. Only then does the control thread destroy imported
mappings and return `POD_QUIESCED`.

Revocation begins this while the Pod's Comch connection is still live, so the
first gate — every EU acknowledging `RING_DEL` — runs against a Pod that is
still mapped. An acknowledgement the DPU channel has no posted receive for is
held as a fence and retried whenever that EU releases its execution unit, and
the control thread resends `RING_DEL` to unacknowledged EUs every 10 ms. A
quiescence that has not passed every gate within five seconds is reported with
the gate holding it, and reported again every five seconds it remains there;
until it passes, the slot and its imported mappings are held.

## The assertion, checked

`struct dmesh_workload_assert_msg` is 1134 bytes and *Trusted registration*
above states what it carries. This is the order the DPU checks it in, and the
reason each failure reports as
`dmesh_control_events_total{kind="assert",reason}`. The key lookup happens in
the caller, before the message-body checks.

| # | Check | reason |
|---|---|---|
| 0 | `key_id` names a public key the held generation publishes for **this node's agent** | `bad-key-id` |
| 1 | canonical form: nothing after each NUL, padding intact, DNS fields well formed, `pod_ip` a dotted quad, `flags`/`reserved` zero, `assert_id`/`nonce` not all-zero | `noncanonical` |
| 2 | `version == 2` | `bad-version` |
| 3 | `node_name` equals this DPU's own node (`DPUMESH_NODE_NAME`) ⟨T⟩ | `wrong-node` |
| 4 | `issued_at ≤ now + ASSERT_CLOCK_SKEW`, `expires_at > now`, lifetime ≤ `ASSERT_LIFETIME`; the skew grace is one-sided, on `issued_at` only | `bad-time` |
| 5 | `nonce` equals the challenge this connection issued (constant-time compare) | `bad-nonce` |
| 6 | signature verifies over `[0, offsetof(sig))` | `bad-sig` |
| 7 | `assert_id` not in the consumed ring (`ASSERT_REPLAY_SLOTS`, evicting; the per-connection nonce is what makes eviction safe) | `replay` |
| 8 | `(namespace_name, service_name)` equals the Service this registration requests | `bad-service` |

Checks 3 and 8 are what a registration cannot talk its way around: the Pod
relays the assertion, and the assertion names both the node and the Service.
Verification happens once per Comch connection; nothing on the data path
verifies assertions, and a reconnect re-attests because the nonce is different.

## Authoritative feeds

Three versioned feeds carry authority to the DPU, and they share one contract.
Identity material is not one of them — it is root-only files installed
atomically, and what authenticates it is the certificate the control plane
issues against them.

| Feed | Published by | Scope | Signed with | Carries |
|---|---|---|---|---|
| node membership | node agent | node | feed keyring, HMAC-SHA256 | the `(Pod UID, Service)` pairs this node may hold |
| Service targets | the registry publisher | node | feed keyring, HMAC-SHA256 | each Service's ClusterIP and ready endpoints, with the Pod UID of each |
| topology generation | controller | cluster | `DPUMESH_CONTROLLER_KEY_DIR`, Ed25519 | every cluster-wide fact a DPU needs — *The generation* below |

The two node-scoped feeds may stay symmetric: a DPU that forges its own node's
feed harms only its own node, which the threat model already concedes. The
cluster-scoped one may not, and *Three scopes* states why.

The target snapshot places every address it names — session key, ClusterIP and
ready endpoints — in its Service, and a session refuses to dial an address the
held generation places in another one. The session's own addresses are the ones
the snapshot registers as such; every other selected address must resolve
through the Pod UID the snapshot pairs with it, so a Linkerd endpoint the
generation no longer names is declined rather than assumed to be the session's.

Every generation of every feed is installed by atomic rename and ends with a
signature envelope naming the key that signed it. For the two node-scoped
feeds that is

```text
signature=<key-id>,<hex HMAC-SHA256 over every preceding byte>
```

and the key comes from the root-only feed keyring (`DPUMESH_FEED_KEY_DIR`),
which is a disjoint set of files from the registration keyring, so a feed
publisher holds no key that can mint identity and the two roles rotate
independently. The topology generation uses the same envelope shape with an
Ed25519 signature and the controller keyring, which *The generation* specifies.
In all three, only the signed prefix is parsed: bytes appended after the
envelope are refused rather than ignored, and a key id naming a file outside
its keyring directory is rejected.

A generation is adopted only if it is strictly newer than the one held and
completely parsed. A missing, malformed, oversized, unsigned or rolled-back
generation changes nothing — it never revokes membership, withdraws a target or
admits an unverified registration. Publishers derive each generation from the
last one they wrote rather than from the wall clock alone, so a clock step
backwards cannot present a rollback, and concurrent publishers are serialized so
the newest generation is the one installed last.

Consumers skip re-reading an unchanged generation, but that is an optimization
and never a decision. The DPU filesystem reuses the freed inode across a rename
and stamps coarse timestamps, so a generation is trusted to be unchanged only
once its inode, modification time and length all match *and* it has been
installed longer than that granularity.

## Membership and revocation

The node agent publishes the `(Pod UID, Service)` pairs this node may hold, one
per line:

```text
member=<pod-uid>,<service-name>
member=<pod-uid>,-              ← the Pod is on this node with no Service
```

Names, not node-local numbers, cross this feed; the compact id is the DPU's own
interning of the topology generation. The same label rule decides an assertion
and a membership entry, so deleting a Pod or changing its labels withdraws its
pair. Every live Pod also contributes the bare form, which is what a Pod
registering without Service membership holds.

The Comch control thread adopts each newer generation and closes the exact
registration whose pair has left it, through the same teardown a disconnect uses.
Withdrawal takes two consecutive generations: a generation whose snapshot
predates a registration omits it without meaning it, so one absence is not
authority to tear a Pod down.

---

# Cluster scope

## The controller

The controller publishes the generation and mediates the workload lookups the
upstream control-plane API cannot scope. It performs no attestation: it has no
host-local evidence and never asks for it. It runs in the cluster as a Pod,
reads the Kubernetes API, and never speaks to a DPU — a DPU has no route into
the cluster CIDRs, so its node agent relays, which is the channel every other
control message already takes.

It exists rather than letting each node agent derive the topology itself,
because that would need cluster-wide Pod read on every node, a different
snapshot per agent, and one API reader per node. The agent reads Pods with
`fieldSelector=spec.nodeName=<its node>` under a namespaced `Role`, and
`workload_attest.sh` asserts it cannot list Pods cluster-wide.

**Bootstrap.** A DPU must hold the controller's public key before it can verify
anything, so that key cannot arrive in a generation. It is deployment-time
material in `DPUMESH_CONTROLLER_KEY_DIR`, loaded with the checks
`dmesh_grant_load_key` applies — regular file, `O_NOFOLLOW`, owned by the
effective uid, mode 0600/0400, non-zero, 0700 directory — and a DPU that cannot
load one refuses to start.

**Rotation.** The directory holds an overlap set of at most `CONTROLLER_KEYS_MAX`
public keys and the envelope names which one signed. Adding a key is a file
drop, retiring one is a delete, and a generation naming a key id the set does
not hold is refused and counted. Because key selection is filename-driven, the
controller key directory, the feed key directory and the registration key
directory are disjoint: a key file in the wrong directory is a signing
capability leak.

**Replication.** One replica by default, because the system is fail-static.
Replicas with leader election sign in turn where generation freshness matters
more; this does not reduce the copies of the signing key, since a Kubernetes
Secret is readable by every replica.

**When it is unavailable, nothing that is running stops.**

```text
   controller down
        ├─ established streams            unaffected
        ├─ new streams, existing Pods     unaffected
        ├─ new streams, intra-node        unaffected (registration is node-local)
        ├─ a Pod that starts afterwards   registers and serves intra-node; no
        │                                 generation places it, so peers refuse
        │                                 it — cross-node only
        ├─ mediated policy lookups        stall for Pods not yet in the held
        │                                 generation — new-Pod cross-node only
        └─ revocation via the generation  stops (node-local membership
                                          revocation continues — it is the
                                          agent's feed, not the controller's)
```

### The generation

One UTF-8 text document, one record per line, `\n` endings, no spaces around
separators. Comments start with `#` and are permitted only before `version=`.

```text
version=<u64 decimal, strictly increasing across publications>
node=<node-name>,<rdma-ip>:<rdma-port>,<agent-key-id>,<agent-pub-hex64>,<dpu-static-pub-hex64>
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
| `name` (Service) | DNS label, ≤ 63 — a Service is never named without its namespace |
| `service-account` | DNS subdomain, ≤ 253 |
| `pod-ipv4`, `cluster-ipv4` | dotted quad; the Pod IP is what makes destination-side `networks` authorization decidable |
| `agent/dpu key hex` | exactly 64 hex chars — the agent's Ed25519 signing key, the DPU's static handshake key |
| `key-id` | `[A-Za-z0-9._-]+`, ≤ 31, no `/`, no leading `.` |
| `hex128` | exactly 128 hex chars (64-byte Ed25519 signature) |

- `version=` is the first non-comment line, `signature=` is the last; everything
  between may appear in any order.
- The signature covers every byte from offset 0 through the `\n` that introduces
  the `signature=` line, so appended bytes are unsigned and refuse the document.
  `dmesh_gen_verify` applies the same marker scan and key-id parsing as
  `dmesh_feed_verify`, with Ed25519 against `DPUMESH_CONTROLLER_KEY_DIR` in
  place of the HMAC keyring.
- `GEN_POD_MAX`, `GEN_NODE_MAX`, `GEN_SERVICE_MAX`, `GEN_ENDPOINT_MAX` and
  `TOPOLOGY_MAX_BYTES` are refused rather than truncated, at both ends: the
  publisher refuses to publish an over-bound generation, so the last good one
  stands and the failure is loud at the source, and the consumer refuses to
  adopt one.
- An unchanged cluster publishes nothing, so a new version always says something
  new and consumers re-adopt only when there is something to adopt.
- An unknown line kind, a duplicate `pod=` for one UID, an `endpoint=` or
  `protected=` naming a Service or Pod UID no other line defines, or any syntax
  violation refuses the whole document. Adoption is all-or-nothing into a
  staging table swapped only on success.
- Outcomes are counted as `dmesh_control_events_total{kind="topology",reason}`
  with reasons `adopted`, `unchanged`, `rollback`, `unsigned`, `bad-key-id`,
  `bad-sig`, `malformed`, `overflow`, `unreadable`.

## Identity across a node boundary

A destination must be able to establish the source workload's identity without
trusting the source's node. Two bindings do that — the first is the local
assertion the sections above specify — and neither component can produce the
other's:

| Binding | Established by | Evidence | Scope |
|---|---|---|---|
| Comch channel ↔ Pod UID | node agent | `SO_PEERCRED`, peer cgroup, `/proc/<pid>/stat` | node; never leaves it |
| Pod UID ↔ node, namespace, ServiceAccount, Pod IP | controller | Kubernetes objects | cluster; travels in the generation |

```text
   binding 1 — node scope                binding 2 — cluster scope
   ══════════════════════                ═════════════════════════
   "this channel is Pod P"               "Pod P is on node A, is ns/sa, has IP x"

   evidence   host kernel                evidence   Kubernetes objects
              SO_PEERCRED                           API reads
              /proc/<pid>/cgroup
              /proc/<pid>/stat

   by         node agent                 by         controller
   read by    its own DPU, only          read by    every DPU
   travels    never leaves the node      travels    the signed generation
              └──────────────────────────────────────────────────────────┐
   The local binding does not have to be verifiable off-node. A destination
   needs binding 2, and binding 2 is already a signed table it holds. ◀────┘
```

### The check

A pair of DPUs authenticates once, and the channel is long-lived and carries
every stream between those two nodes. Stream setup carries the source Pod UID,
the destination Pod UID and the destination port.

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
        return REFUSE_NOT_ON_PEER;                 /* the identity check ⟨T⟩ */

    struct pod_state *dst = pod_by_uid(o->dst_pod_uid);
    if (dst == NULL || !pod_data_ready(dst))
        return REFUSE_NO_DESTINATION;

    /* The inbound verdict is asked once per stream at its first byte, on the
     * same path an intra-node stream takes (px_conn_admitted →
     * l7_inbound_verdict). Its inputs — namespace, service_account and pod_ip —
     * come from the signed generation, never from a peer-supplied value, and a
     * refused verdict poisons the stream before a byte reaches the Pod. */
    return handle_alloc(peer, dst, o->dst_port);   /* the reply carries it */
}
```

**The identity check is a lookup in a signed table ⟨T⟩.** No asymmetric
operation happens on the connection path: the generation is verified once when
adopted and the DPU pair is authenticated once when the channel opens, and both
amortize over every stream between two nodes. A node may claim any Pod the
cluster says is on it — its own Pods, whose memory it already holds — and
nothing else. Without that check, compromising the least-privileged node yields
the most-privileged identity in the cluster.

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

The bound can come from state or from computation, and the choice is a
hardware fact rather than a preference:

| | Destination holds | Per-stream cost | One compromised DPU obtains |
|---|---|---|---|
| believe the peer | nothing | none | every identity in the cluster |
| peer forwards a controller-signed assertion | a cache | one verification per source Pod | that node's Pods |
| **hold the signed topology** | the cluster's Pod table | a lookup | that node's Pods |

The second and third deny the same thing and differ only in whether the
destination spends memory or cycles. A DPU is memory-rich and cycle-poor
relative to what it protects — a connection costs 73 ARM core-µs to build and
tear down, an asymmetric verification on these cores is the same order of
magnitude, and a `pod=` line is under two hundred bytes, so ten thousand meshed
Pods are a couple of megabytes. If a cluster ever grows a Pod table a DPU should
not hold, the forwarded assertion is the migration: it keeps the bound and moves
the cost back to computation.

**Intra-node** the same check is degenerate: source and destination are on this
DPU, so the placement step is satisfied by the live registration rather than by
the generation. The registration therefore retains everything the verdict
consumes — `namespace_name`, `service_account` and `pod_ip` on `pod_state` — and
one admission path serves both cases.

### Freshness

Four windows compose:

| Window | Size | Applies to |
|---|---|---|
| generation publication | `GENERATION_INTERVAL` + adoption lag | how stale a *placement* can be, both directions |
| local membership withdrawal | 2 × generation cadence (`DMESH_MEMBERSHIP_ABSENCES_TO_REVOKE`) | how long a deleted Pod's *local registration* survives |
| assertion lifetime | `ASSERT_LIFETIME` | how long a relayed assertion stays usable; the per-connection nonce is the hard bound |
| clock skew | `ASSERT_CLOCK_SKEW`, one-sided on `issued_at` | agent↔DPU clock spread |

For a hostile node the operative bound is the first row: it can claim a Pod that
left it until every destination adopts the generation that moved it. An honest
node stops originating the moment the local registration ends — the deleted
Pod's Comch connection drops and the sweep closes its streams — so the other
three never extend an honest node's claim cross-node. A recreated Pod carries a
new UID, so no window lets an old identity inherit a new placement.

## The peer channel

Everything here has a counterpart inside a node: the Comch connection between a
host process and its DPU already solves the same problems, and the mechanisms
that survived hardware validation there are the ones extended.
`doca/peer_channel.{c,h}` carries the state machine, disjoint full-duplex handle
namespaces, the five control messages plus DATA, bounded parsing and both
sides' bounds;
`doca/dpu_proxy.c` carries the hooks it binds to. `tests/peer_channel_test.c`
drives the module end to end through a recording transport.

### Custody across the boundary

[`DATA.md`](DATA.md) states the intra-node custody rules — L4 is
end-to-end, L7 is hop-by-hop in three bounded stages. The boundary changes one
release point in each, and nothing else.

```text
 L4, intra-node                            what the boundary changes
 ──────────────────────────────────────    ─────────────────────────
 px_build_range      claim staging
 DOCA DMA success    batch → PX_BATCH_DONE
 px_lane_retire      DONE → emit list
 px_engine_emit      px_piece_release      ◀── deferred: a remote piece is
 px_custody_sub      unfreed hits 0            parked until its STREAM_ACK
 px_drain_ack_releases → px_rev_append_ack
 host tx_reclaim_ack the sender's slots return

 L7, intra-node                            what the boundary changes
 ──────────────────────────────────────    ─────────────────────────
 Pod TX → staging    released when the     ◀── unchanged
                     stack consumed it
 inside the session  stack progress        ◀── unchanged
 arena chunk → RX    released on the DMA   ◀── released on STREAM_ACK
                     batch completing
```

**L4 keeps the end-to-end loop, stretched by one round trip.** The release
`px_engine_emit` performs on local batch completion is deferred: pieces whose
destination is remote park in a per-peer un-ACKed slot pool
(`DMESH_PEER_TX_SLOTS`), and `STREAM_ACK { incarnation, handle, seq_first,
seq_count }` retires the named run — only then does `px_custody_sub` run and the
sender's `TX_ACK` follow. A destination sends `STREAM_ACK` when the bytes have
landed in the destination Pod's host RX mapping, staging up to
`DMESH_STREAM_ACK_BATCH` acknowledgements before flushing, as the reverse ring
batches today.

**L7 keeps its hop-by-hop composition; the wire hop joins it.** The Pod-side
release (`dmesh_l7_release` on stack consumption) is untouched. An egress arena
chunk whose destination is remote retires on `STREAM_ACK` instead of on local
completion.

```text
   Pod P ──DMA──▶ staging A ──RDMA──▶ staging B ──SG-DMA──▶ Pod Q
                                                              │
        TX_ACK to P ◀─── STREAM_ACK ◀───────────────────── landed
        (L4: staging piece · L7: arena chunk retired by the ACK)
```

Errored transfers follow the intra-node rule: a fault releases custody, poisons
the streams it carried, and is counted — never silently retried per stream.

A QP's window is `TX_BLOCKS_PER_CONN × TX_BLOCK_SIZE` at the defaults, with two
qualifications. Both constants are seeds: the live values are clamped to the TX
mapping's geometry and blocks come from a shared per-process pool, so the window
is a ceiling, not a reservation. And the credit round trip is fabric RTT plus
destination SG-DMA completion plus `STREAM_ACK` batching — against a 125 KB
bandwidth-delay product at 100 Gb/s × 10 µs the window covers it ~33×, at
400 Gb/s × 20 µs the product is 1 MB and the margin is 4×. One stream fills the
pipe in either case, but the margin is one to one-and-a-half orders of magnitude
rather than two, and it thins as fabrics get faster. The window is what makes
end-to-end credit affordable; a smaller one would have forced weaker semantics.

The transport carries no flow control of its own, and neither does the DPA or
the SG-DMA engine inside a node. Forward-ring credits, arrival custody and lane
credits do that work and continue to.

### What a peer may consume ⟨T⟩

A peer DPU is authenticated, not trusted. Everything it sends is input: stream
opens, lengths, handles, and the rate of all of them.

At the **destination** a peer is admitted against `DMESH_PEER_STREAMS_MAX`
concurrent streams, `DMESH_PEER_STAGING_MAX` staging bytes,
`DMESH_PEER_TX_SLOTS` pending landing completions and
`DMESH_PEER_OPEN_RATE` opens per second (a token bucket), and is refused beyond
them rather than letting one node's traffic displace another's. Staging is
charged per `DATA` arrival, and a delivery the node holds keeps its charge and
defers its acknowledgement until `dmesh_peer_delivered` reports the bytes in the
destination Pod's mapping — which is what makes the staging bound real.

At the **source** a stalled peer is bounded too: `DMESH_PEER_TX_INFLIGHT_MAX`
caps the un-ACKed bytes (L4 pieces plus L7 arena chunks) one peer may hold, and
beyond it streams to that peer stall via the existing `px_stall` path. Without
it, un-ACKed L7 chunks of one dead peer would drain the shared egress arena that
every other L7 connection allocates from. This is the same reasoning the data
path already applies to a local Pod, which is not trusted to be well behaved
either.

### What the transport must provide

| Property | Why |
|---|---|
| ordered delivery within one handle | [`DATA.md`](DATA.md) preserves per-connection order end to end |
| reliable delivery, or a visible fault | custody cannot release on a silent loss |
| completion reported to the source | `STREAM_ACK` is what returns the sender's capacity |
| reordering across handles permitted | independent streams must not head-of-line each other |
| a fault surfaces as channel loss | per-stream recovery is not attempted |

The unit that crosses is a staging extent — the same contiguous run the SG-DMA
engine sources from, at most `PX_ARRIVAL_COALESCE_MAX`. Nothing is re-segmented
at the boundary.

### Authentication and pairwise keys ⟨T⟩

The node credential is one static keypair per DPU, generated on the DPU at first
boot into a 0400 file that never leaves it (`dmesh_peer_node_key_load`). The
agent reports its public half to the controller, which publishes it in that
node's `node=` line. A DPU therefore authenticates a peer with a key it obtained
from the generation, not from the peer.

The channel runs an existing mutually authenticated key-agreement protocol —
Noise IK, or TLS 1.3 with raw public keys — with one rule added on top:

> the peer's static public key must equal the one the held generation binds to
> the peer's claimed node name. A name the generation does not bind, or a key
> that differs, refuses the channel ⟨T⟩.

`dmesh_peer_table_rebind` re-checks that on every adoption and resets a channel
the generation dropped or re-keyed. The channel incarnation is bound into the
handshake (the Noise prologue, or the TLS exporter context), so a completed
handshake authenticates the incarnation its handles will carry.

Keys are pairwise ⟨T⟩: a fleet-shared key would let one compromised DPU read
every conversation in the cluster rather than its own. No per-workload key
exists on this wire.

### Lifetime

A channel opens on the first stream that needs the node at its far end, because
opening to every node in advance would be quadratic. The cost is that the first
stream to a node pays for authentication and key agreement.

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
     │   the generation drops or re-keys the peer, or the transport faults.
     │   Teardown is synchronous: the reset poisons every stream and releases
     │   every pinned extent before it returns, so there is no draining state.
```

Control messages, all setup or teardown; the data path carries only a handle and
bytes.

| Message | Direction | Fields |
|---|---|---|
| `STREAM_OPEN` | source → destination | `incarnation`, `source_token`, `src_pod_uid[64]`, `src_service_key[128]`, `dst_pod_uid[64]`, `dst_port`, `src_generation` |
| `STREAM_OPEN_ACK` | destination → source | `incarnation`, `source_token`, `handle`, `status` |
| `STREAM_FIN` | either | `incarnation`, `handle` |
| `STREAM_ACK` | receiver → sender | `incarnation`, `handle`, `seq_first`, `seq_count` |
| `POD_GONE` | source → destination | `incarnation`, `pod_uid[64]` |
| `DATA` | either | `incarnation`, `handle`, `seq`, bytes |

`STREAM_ACK` names a run of consecutive sequences, which is the encoding
`dmesh_tx_ack_entry` already uses on the reverse ring, for the same reason: one
acknowledgement per released extent rather than one per transport unit.
`src_generation` is not an input to the identity check — it lets a destination
notice it is behind and adopt sooner.

A stream is full-duplex and has one handle, because a `dmesh_qp_t` is one
full-duplex byte stream. The handle's owner bit gives the two nodes disjoint
wire namespaces even when each independently allocates index 1. Each side owns
its streams on its own worker by its own rule: at
the source the Pod's port decides (`dmesh_worker_for_port`), at the destination
the DPU that allocates the handle encodes its own worker in it. The two are
independent because the two DPUs have independent worker sets. To keep a
completion on its owner, the channel is one queue pair per (node pair,
destination worker), while authentication and the pairwise key are established
once per node pair — so `CHANNEL_MAX × PEER_QP_PER_NODE` is the number to check
against RDMA resource limits, not `CHANNEL_MAX`.

Losing a channel ends the streams on it. Each DPU tells its own Pods by the path
it already uses when a peer Pod disappears — `px_conn_peer_disconnected`
delivers EOF to the survivor and removes the connection. Streams are not
migrated across a reconnection.

### Handles

The destination DPU allocates the handle, because it owns that namespace. The
space is per peer, so nothing cluster-wide has to allocate it.

```c
/* One per (peer, handle). Self-contained by value, as px_unit is, so it
 * survives the teardown of anything that named it. */
struct peer_handle {
    uint32_t incarnation;      /* the channel incarnation that issued it */
    uint32_t wire_handle;      /* owner bit plus allocator-local index */
    int32_t  dst_pod_idx;      /* destination slot; validated against pod generation */
    uint32_t dst_pod_generation;
    uint16_t dst_port;
    uint16_t up_port;          /* the intra-node upstream this stream feeds */
    char     src_pod_uid[64];  /* the key POD_GONE and peer loss sweep on */
    uint32_t staging_bytes;
    uint32_t rx_seq;
    uint8_t  rx_seq_valid, rx_fin, tx_fin;
};
```

Two generations guard it: the channel incarnation rejects a handle from a
previous connection to the same peer, and the pod generation rejects one whose
destination slot has been re-tenanted. That is the pair `px_batch` already
carries, for the same reason — an asynchronous completion outlives the state
that named it. The channel incarnation is matched on **every** path, where the
Pod generation pair is matched on the error, retry and rearm paths and the
success path checks `pod_data_ready`.

| Event | Within a node | Across nodes |
|---|---|---|
| normal close | FIN fan-out (`px_try_fin`), then the upstream is freed | stream FIN; the destination releases the handle |
| source Pod gone | a worker sweeps the upstream table | **the source DPU says so on the channel**; the destination releases that Pod's handles |
| peer gone | the Pod's Comch connection drops | the destination releases every handle whose source is that node |

The middle row is the one new mechanism: within a node a sweep suffices because
the DPU sees the Pod leave, and across nodes a destination cannot, so the source
has to say. If the source crashes before sending `POD_GONE`, the handles survive
until the channel faults or idles out — bounded by `DMESH_PEER_STREAMS_MAX`,
which is why that bound is per peer rather than global.

A policy change does not disturb an established handle. Admission changes within
one watch update and nothing is admitted while a rule denies, which is a
statement about new streams; the handle's lifetime is not a policy lifetime.

### A peer whose claims are refused

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

A single refusal cannot distinguish them, and both skew cases have an honest
explanation, so tearing the channel down would turn ordinary generation skew
into an outage. Carrying `src_generation` in the stream open tells a destination
which side is behind, which shortens case (a) — it does not separate (c). The
question is better asked as what a refusal *consumes*: a lookup and a reply, so
what needs bounding is the rate.

A destination bounds and reports; the controller evicts. Per-peer limits cap
what any peer can hold and the open-rate bucket caps what a flood of refusals
can cost, so a peer that is refused often becomes slow rather than disconnected,
and the honest Pods on that node keep their streams. Removing a node is a
cluster-wide decision and belongs to the component that already holds
cluster-wide authority: when the controller drops a node from the generation,
every DPU refuses it at once and consistently. This is the division the tree
already uses for a misbehaving Pod — `px_poison` ends the connection, and the
membership generation is what actually removes the Pod. Refusals and poison are
counted per worker and surfaced as
`dmesh_control_events_total{kind="peer",reason}` and in the worker stat line.

## Reach

The backend set comes from the generation: the `endpoint=` lines joined with
`pod=` for placement, split into a local set (this node) and a remote set
(`dmesh_topology_remote_endpoints`, `collect_remote_hosts`).
`px_resolve_backend` prefers local, because local endpoints are a DMA away, and
falls to `PX_DST_REMOTE` — the third outcome `px_unit_prepare` distinguishes
from "host not ready", so a Service healthy elsewhere is reported as remote-only
rather than poisoned as a Service with no replicas. Having no local replica
costs latency, never the connection. Locality is a preference, as it is in
Kubernetes topology-aware routing.

The local half stays registration-derived rather than generation-derived. That
is deliberate: a Pod that registered between a generation's snapshot and its
publication is absent from that one generation without having stopped serving,
and the fail-static table already promises it keeps serving intra-node.

## Authorization

[`DATA.md`](DATA.md) owns the inbound verdict and its per-protocol rules. What is
cluster-scoped is where its inputs come from: the destination presents
`client_addr = pod_ip:src_port` and a client identity built as
`<service-account>.<namespace>.serviceaccount.identity.<trust-domain>`, both from
the signed generation cross-node and from the retained registration intra-node.
Neither is ever a peer-supplied or Pod-supplied value. `networks` clauses are
matched first and an empty match denies, so a synthetic source address would
make every realistic policy unevaluable — the signed Pod IP is what makes the
stock evaluation decidable.

A session that crosses two nodes builds one proxy, not two. The destination
needs a verdict, not a proxy:

| | Source DPU | Destination DPU |
|---|---|---|
| discovery, balancing, retries, protocol | full outbound stack, per session | — |
| authorization | — | policy evaluation |
| cost scaling | per session | **per destination Pod and port** |

`Inbound::build_policies(workload, …)` binds one watch set to one workload
string, so the adapter calls it once per registered destination Pod rather than
once per process, and streams share the watch and pay only `connection_verdict`.
A cross-node L7 stream therefore costs one outbound session plus a lookup.
Sidecarless workload templates opt into that index with the Linkerd
control-plane label while marking DMA ports as skipped inbound ports. This
keeps policy discovery enabled without falsely claiming that a proxy listener
exists inside each Pod. The DPU destination context includes its real
Kubernetes `nodeName`, so stock endpoint discovery can apply locality without
an empty-node lookup.

## Scope of the control-plane credential ⟨T⟩

The DPU authenticates to Linkerd's destination and policy services with one
credential per node and names the workload it asks about as a plain string, so
the upstream API cannot express "only the Pods this node serves" — a compromised
DPU could otherwise read every workload's outbound policy, which is reach beyond
its node.

Where the API cannot express the restriction, the controller mediates:
`GET /workload-scope?pod_uid=…` answers only for Pods the generation places on
the asking node. What binds the question to its asker is not anything the DPU
says — the request travels its own agent's relay, and the controller resolves
the node from the source address against the addresses Kubernetes records for
it. `doca/control_scope.{c,h}` asks once per registration on the control thread
and again on every adoption; a data worker reads `pod_state.scope_state` and
asks about nothing the controller has not allowed, counted as
`inbound.out-of-scope`. An unanswered question withdraws nothing, which is the
fail-static rule every feed has.

## Protection classes

The `protected=` lines grade a Service. A Service in the set carries the strict
policy, a Service outside it the relaxed one; every registration stays
assertion-verified either way, so what the generation grades is the interaction
rules, not whether a Pod is attested. The grading cannot be inferred from Pod
input, or a Pod would opt itself out.

`px_protection_refresh` caches the grading per interned Service on adoption, so
a data worker's decision is a byte read, and `px_parse_l7`'s two fail-closed
decisions ask the Service rather than the process. `DPUMESH_L7_FAIL_CLOSED`
survives only as the default for a Service no generation grades, which is the
deployment that has no controller. `dmesh_inbound_admits` states the three rules
as one decision over its inputs:

- a served verdict decides alone;
- an ungraded Service carries the stream, so enabling enforcement cannot refuse
  traffic no policy ever named;
- a protected caller reaching an unprotected callee is refused and counted
  `mixed-callee-unprotected` unless the callee's own policy admitted it.

---

# The Linkerd control plane

The Linkerd static library creates destination, identity and policy clients from
`LINKERD2_PROXY_*` environment variables. Deployment requires the stock Linkerd
control plane, its three management-link gateway addresses, provisioned identity
material and a signed Service target feed. Missing configuration fails
preflight; no mock control-plane path exists. The remaining `mock-identity`,
`mock-policy` and `mock-destination` sources belong to the upstream
`linkerd-app-integration` test crate and are neither linked nor deployed.

![How a Pod is admitted and where the DPU's authority comes from](figures/control_plane.png)

[PDF](figures/control_plane.pdf)

The application can request only a Service name; it cannot assert Pod UID,
namespace, labels, ServiceAccount, node or Linkerd workload. The gateway is
byte-transparent, so it cannot mint or terminate mesh identity.

### Identity

1. A DPU identity agent obtains a projected ServiceAccount token with the
   Linkerd identity audience, the Linkerd trust roots, and a key and CSR whose
   DNS SAN is
   `<service-account>.<namespace>.serviceaccount.identity.<trust-domain>`.
2. The embedded proxy sends `Identity.Certify(token, identity, CSR)` to
   `linkerd-identity` over the configured control connection.
3. The returned leaf and intermediate certificates are installed in the proxy's
   in-memory credential watch. Destination and policy clients use that watch for
   mTLS.
4. The stock certify loop refreshes at 70% of certificate lifetime, bounded by
   the configured minimum and maximum. `TokenSource` reloads the token file on
   every certify request, so token rotation does not restart `dpumesh_dpu`.
5. Startup is not ready until the first certificate is installed. Trust roots,
   the private key and the CSR are read while parsing startup configuration, so
   replacing them is a controlled restart.

Control-service TLS names are distinct from the DPU proxy identity:

| Connection | Default TLS identity |
|---|---|
| Identity | `linkerd-identity.linkerd.serviceaccount.identity.linkerd.cluster.local` |
| Destination | `linkerd-destination.linkerd.serviceaccount.identity.linkerd.cluster.local` |
| Policy | `linkerd-destination.linkerd.serviceaccount.identity.linkerd.cluster.local` |

The DPU does not participate in the Kubernetes Service or Pod CIDRs, and on the
current hardware neither ClusterIPs nor Pod IPs are reachable from it.
`LINKERD_*_ADDR` therefore names a node-local TCP pass-through on the Host/DPU
management link, carried by the node agent. TLS remains end-to-end between the
embedded proxy and the Linkerd service; the gateway neither terminates
identity nor interprets gRPC. The agent runs on the host network and opens its
upstream connections to the Service and Pod CIDRs, so the target Pod's Linkerd
inbound proxy still terminates mTLS.
Kubernetes API `port-forward` is not suitable, because it bypasses that inbound
proxy and reaches the control application as plaintext gRPC.

### Outbound policy

Each client session builds its own outbound stack and policy watch.
`OutboundPolicies.Watch` carries:

```text
source_workload = the exact workload value granted during Pod registration
target          = the real Kubernetes Service ClusterIP:port
```

For current Linkerd installations the workload is the injector-compatible JSON
object, for example `{"ns":"test-bench","pod":"bench-dpumesh-abc"}`. It is
neither the DPUmesh Service name nor the DPU proxy's certificate identity.

The embedded runtime is outbound-only. Its loopback admin and ephemeral inbound
listeners use a fixed local default and open no `GetPort` watches for
nonexistent DPU ports, which does not disable the per-session outbound policy
watches.

An invalid or unroutable policy fails the protected L7 session. A control-plane
disconnect retains only state Linkerd's watches already hold; a new lookup that
cannot obtain policy fails and is never converted to an unobserved TCP dial. A
declined protected session ends rather than being forwarded as plain L4: the
generation's `protected=` set grades which Services that applies to, and
`DPUMESH_L7_FAIL_CLOSED` is the default a Service no generation grades falls
back to. The deployment script pins that default to `1` and refuses to deploy
with any other value.

### Destination

The destination presented to Linkerd is the Service's real ClusterIP and port.
The adapter keeps its synthetic `10.96.0.<interned-id>:9092` address only as an
internal backend key; the target feed names Services by `namespace/name` and
the adapter resolves each to the DPU-interned id.

Destination and profile streams may update policy metadata while a session is
live. DPUmesh remains the authority for the set of node-local registered Pods.
The feed snapshots the Service ClusterIP and ready endpoint IPs, each address
paired with a port from the subset that published it and with the Pod UID the
generation places there. That UID is what makes an endpoint Linkerd selects
checkable against a live local registration; [`DATA.md`](DATA.md) states what
the adapter does with each outcome, and none of them is ever replaced by a TCP
dial. Within the same Service, DPUmesh retains backend selection: the
resolution enforces that the selected endpoint is live and node-local, and
Linkerd's own endpoint weighting inside a Service is not claimed.

### Policy boundary

Linkerd's stock `OutboundPolicies.Get/Watch` response contains protocol and
route configuration. It does not expose the inbound `AuthorizationPolicy`
allow/deny decision, which a sidecar's inbound half enforces against the
authenticated peer identity. There is no Linkerd inbound proxy in the DPUmesh
byte path, so the DPU is that enforcement point instead:

- `InboundServerPolicies.WatchPort` is discovered per registered destination Pod
  and port, and the verdict is the stock evaluation reached through the fork's
  `connection_verdict`. [`DATA.md`](DATA.md) states the rules and
  *Authorization* above states where their inputs come from;
- the inputs are the node-agent-signed Pod IP and ServiceAccount, so the verdict
  never rests on the shared `dpumesh-dpu` certificate, which is proof of the DPU
  and not of the originating Pod;
- `source_workload` remains trustworthy input to stock outbound discovery;
- a DPU asks about a Pod only where the controller's mediated lookup places that
  Pod on its own node, which is what keeps one node's credential from reading
  the whole cluster's policy.

---

# Across both scopes

## Naming and identifier spaces

Names on the outside, handles on the inside, each allocated by whoever owns it.
*Resolution* above owns the mechanism; what is cluster-scoped is which space each
identifier lives in.

| Value | Scope | Assigned by | Where it travels |
|---|---|---|---|
| Service name `namespace/name` | cluster | Kubernetes | control plane |
| Pod UID | cluster | Kubernetes | control plane, stream setup |
| node name, RDMA address | cluster | Kubernetes, controller | generation |
| interned service id | node | that node's DPU | node-local wire |
| `pod_id` | node | that node's DPU | node-local wire |
| stream handle | node pair | the destination DPU | inter-DPU wire |

Service identifiers are namespace-qualified everywhere, because a Kubernetes
Service is namespace-scoped and a bare name is ambiguous by construction. The
public API accepts `"name"` — resolved in the calling Pod's own namespace, which
the registration already knows — or `"name.namespace"`, the DNS convention
applications already use.

A Pod on another node has no `pod_id`, because that space is node-local by
construction, and it does not need one: the DPU already ignores what a host
names as the destination of a reply and routes it from conntrack.
`dmesh_qp_t.remote_pod` is advisory and reports `DMESH_POD_REMOTE` for such a
peer. A pod id was never workload identity; it is a transport identifier for one
node's slot table.

### Wire representations

| Identifier | Representation |
|---|---|
| pod id | nonnegative `int8_t` on the wire |
| service id | nonnegative `int8_t` on the wire |
| port | `uint16_t` |
| sequence | per-connection `uint16_t` |
| internal routing fields | `int32_t` |

`POD_REGISTER` is a fixed 72-byte message carrying the requested Service name
beside the pod id, where `-1` asks the DPU to assign one. The v2 assertion is a 1134-byte canonical message whose numeric
fields are explicit little-endian bytes, whose text is NUL-terminated and
zero-padded, and whose final 64 bytes are an Ed25519 signature over every
preceding byte. Forward and reverse descriptors use fixed-width fields and
compile-time layout assertions. Host and DPU endpoints are little-endian.

## gRPC authority

A gRPC client target is the DPUmesh Service name supplied to each connection
attempt. HTTP/2 `:authority`, TLS SNI and certificate identity remain gRPC
values. `GRPC_ARG_DEFAULT_AUTHORITY` is preserved; otherwise the target is used.

The C++ integration creates a targeted QP for each EventEngine `Connect`. The
server receives QPs through the experimental `PassiveListener` endpoint
injection API. Protobuf messages, stubs and handlers are unchanged.

## Control channels

| Input | State supplied |
|---|---|
| DOCA Comch | pod registration, resolution, mappings, readiness, teardown, doorbells |
| signed feeds | node membership; Service targets and ready endpoints; the topology generation |
| root-only files | the registration, feed and controller keyrings; the DPU's own node credential; Linkerd identity material; the admission switch |

Resolution answers follow the topology generation, so nothing reloads out of
band. Dynamic instances of an existing Service join and leave through Comch
registration.

## Encryption and observability

Node-local traffic is plaintext because it crosses PCIe and DPU memory and never
a wire. Across the boundary, encryption applies to the DPU-pair channel rather
than to each stream, so its negotiation cost amortizes exactly as the
authentication does and the identity it presents is the node credential. One
channel carries every workload between two nodes and per-workload separation is
enforced by the per-Pod mapping at both ends instead of by the wire — a weaker
wire property, chosen because under this threat model a compromised node holds
all its resident workloads' keys in either design. The key material lives only
on the DPU, which is the one mesh property here a sidecar cannot match at any
cost.

Every byte crosses the DPU, so the workload cannot alter what is recorded, and
what is recordable differs by locality:

```text
   intra-node   the L7 layer parses where the bytes move
                → both the client-side and the server-side metric families,
                  from one machine, with no in-Pod component

   cross-node   the source DPU runs the only parser
                → source: the full outbound family
                → destination: the inbound family at connection level only
                  (bytes, streams, verdicts, identities) — no server-side HTTP
                  metrics, because it deliberately has no parser
```

The double-counting rule is a sidecar mesh's: a stream recorded by both is the
client-side and the server-side view of one request.

## Operations

- `bench/linkerd_identity.sh status` reports systemd health, JWT issue and
  expiry timestamps, seconds remaining and consecutive token-renewal errors
  without printing the token.
- Alert before `control_identity_cert_expiration_timestamp_seconds - time()`
  reaches the drain and restart budget. Also alert when the renewal unit is not
  active, `token_seconds_remaining` approaches zero, or
  `control_identity_cert_refreshes_total{result="error"}` increases.
- Trust-root, private-key or CSR replacement runs as `bench/bench.sh
  rotate-identity`: drain protected admission, wait for the DPU to observe the
  switch and for `dmesh_sessions_active` to reach zero under a deadline,
  atomically replace the root-only material, restart, wait for `/ready`, restore
  Pod placement, then reopen admission. A drain that does not reach zero reopens
  admission and aborts rather than cutting sessions. Token-only replacement does
  not restart the proxy.
- `bench/bench.sh admission open|drain` sets the switch on its own. The DPU
  polls the file, so it needs no restart, and an unreadable switch means open: a
  lost file must never stop admission.
- Alert on `dmesh_control_events_total{kind="membership"}` with any reason other
  than `ok`. The consumer refuses to revoke on a feed it cannot trust, so a stuck
  publisher shows up as a stale generation rather than as an outage.
- Registration, membership, revocation and admission outcomes are exported as
  `dmesh_control_events_total{kind,reason}` and refused sessions as
  `dmesh_sessions_declined_total{reason}`. Both are process-global, so every
  worker's admin endpoint reports the same values.
- The node agent DaemonSet — which carries the gateway routes — is rebuilt
  under one image tag, so deployment restarts it explicitly. An apply alone
  leaves the previous binary running behind a successful rollout status.

## Parameters

The ones marked ∎ change a security property; the rest change only cost.

| Name | Value | What it decides |
|---|---|---|
| `GENERATION_INTERVAL` | 5 s | ∎ how long a hostile node can claim a Pod that left it |
| `ASSERT_LIFETIME` | 300 s | ∎ how long a relayed assertion stays usable |
| `ASSERT_CLOCK_SKEW` | 30 s | ∎ tolerated agent→DPU clock spread, one-sided on `issued_at` |
| `ASSERT_REPLAY_SLOTS` | 4096 | ∎ consumed assertion ids retained; the nonce is the hard bound |
| `CHANNEL_IDLE` | 60 s | when an idle peer channel is evicted |
| `CHANNEL_MAX` | 256 | peer channels one DPU keeps open, LRU beyond it |
| `DMESH_PEER_STREAMS_MAX` | 4096 | ⟨T⟩ concurrent streams one peer may hold at a destination |
| `DMESH_PEER_STAGING_MAX` | 16 MiB | ⟨T⟩ staging bytes one peer may hold at a destination |
| `DMESH_PEER_OPEN_RATE` | 1000/s | ⟨T⟩ stream opens accepted from one peer |
| `DMESH_PEER_TX_INFLIGHT_MAX` | 16 MiB | ⟨T⟩ un-ACKed bytes one peer may pin at a **source** |
| `GEN_POD_MAX` | 65536 | meshed Pods a generation may name, refused rather than truncated |
| `GEN_NODE_MAX` | 1024 | nodes a generation may name |
| `GEN_SERVICE_MAX` | 4096 | Services a generation may name |
| `GEN_ENDPOINT_MAX` | 65536 | endpoints a generation may name |
| `TOPOLOGY_MAX_BYTES` | 16 MiB | generation byte bound; the 256 KiB membership bound is one node's |
| `CONTROLLER_KEYS_MAX` | 4 | controller public keys held for rotation overlap |
| `PEER_QP_PER_NODE` | A | queue pairs per node pair — one per destination worker |
| `DMESH_STREAM_ACK_BATCH` | 64 | delivery acknowledgements staged before one is sent |

`GENERATION_INTERVAL` and `CHANNEL_IDLE` interact: a channel evicted and reopened
between two generations pays setup twice.

## Decisions taken

| Question | Decision | Refused alternative |
|---|---|---|
| Is a compromised DPU in the threat model? | yes | believing a peer's claim, which gives one compromised node every identity in the cluster |
| Cluster-scope signing primitive? | Ed25519; the DPU holds public keys only, node-local feeds stay HMAC | one symmetric primitive for both, which lets any DPU forge cluster topology |
| Where does custody release across nodes? | at the destination Pod's memory — L4 pieces and L7 arena chunks both retire on `STREAM_ACK`; the L7 Pod-side release stays on stack consumption | one custody semantics for both modes, which the intra-node tree contradicts; or releasing at the source, which credits bytes nobody received |
| Source address for authorization? | the signed Pod IP, from the assertion and the generation | a synthetic address range, which no realistic `networks` clause admits and nothing signs |
| HTTP-typed server policy at the destination? | connection verdict from the union of route authorizations, over-admission stated | refusing HTTP-typed ports, which breaks stock policies; or a second parser |
| Does a stream have one handle or two? | one, full-duplex | two, which splits a `dmesh_qp_t` the API defines as one stream |
| Which worker owns a cross-node stream? | each side by its own rule; one queue pair per (pair, destination worker) | one queue pair per node pair with a cross-worker handoff on every completion |
| Where does the L7 stack run? | outbound at the source, a policy verdict at the destination | a full stack on both sides, which doubles the measured session cost |
| Who issues node credentials? | the controller; the DPU's static keypair never leaves the DPU | `linkerd-identity`, which is a fork to maintain |
| Peer-channel handshake? | a stock protocol plus one binding rule against the generation | a bespoke handshake |
| Peer refused repeatedly? | bounded and reported; the controller evicts | tearing the channel down, which turns ordinary skew into an outage |
| ClusterIP for the POSIX facade? | answered by the DPU from the generation; leaving the mesh is logged | a file in the Pod image, which fails open silently |

## Current bounds

**Node scope**
- Service names resolve through the controller's topology generation;
- backend membership is node-local, and only node-agent-signed Pod and Service
  membership is admitted;
- service and pod identifiers occupy the signed one-byte wire space;
- deployment requires Linkerd destination, identity and policy services, gateway
  addresses, TLS service names, a signed per-service discovery feed and DPU
  identity material;
- declined L7 sessions are refused fail-closed, and the deployment script does
  not deploy a configuration that would forward them at L4 instead; the worker
  placement and session isolation those sessions run under are
  [`DATA.md`](DATA.md)'s bounds.

**Cluster scope and the node boundary**
- Each ARM worker instantiates a peer table and `px_peer_configure` binds the
  supplied lower RDMA transport. Accepted lower connections enter through
  `px_peer_accept`; the authenticated upper state owns them thereafter.
- Linkerd-selected remote endpoints retain their exact topology Pod UID. The
  source opens that Pod on its node's channel, DATA lands through destination
  SG-DMA, and `STREAM_ACK` releases source custody only after `REV_DONE` was
  published.
- Encryption and mutual key agreement are the lower transport's; topology key
  binding, stream identity and policy admission are this layer's.
- Route-level HTTP authorization at the destination is out of scope: the
  connection verdict needs no parser, and a route-level one needs a second L7
  stack — the cost the source/destination split exists to avoid.
- Per-workload certificates on the wire are out of scope. The topological check
  gives the same blast radius under node compromise; per-workload keys become
  necessary only when peer nodes are not trusted to speak for their own Pods,
  which is a multi-tenant concern.
- Sharing one DPU across tenants, and replacing Linkerd as the source of policy,
  are out of scope.

## Hardware validation

**Node scope.** Initial Identity failure, token rotation and fresh
certification; gateway and control-service loss and recovery; Linkerd control
Pod replacement; registration key overlap and prune; and mock-free traffic with
no fallback, drops or reorder. Target withdrawal refuses protected sessions and
restore recovers them with the L4 fallback counter at zero. Generations stay
monotonic across a clock step backwards and across concurrent publishers, an
unsigned generation is counted and refused, and a membership feed that
disappears revokes nothing. Removing a live Pod's Service label revokes that one
registration and leaves every other registration and its traffic untouched.

**Cluster scope.** Every feed the DPU consumes arrives through the node agent
alone and is adopted; the DPU generates its node credential and the controller
publishes its public half in the generation's `node=` line; the mediated lookup
answers for a Pod the generation places here and refuses one it names nowhere;
registrations record their protection class; and the destination-side verdict
runs once per session against a policy the controller served, with sessions
opened equal to closed and none left active.
