# DPUmesh Control Plane

## Abstract

DPUmesh places the Kubernetes-facing authority in one controller Pod, the
host-local evidence and device allocation in one `dpumeshd` system service,
and packet processing on a BlueField DPU outside Kubernetes. The split follows
the evidence each component can obtain. Kubernetes knows desired and observed
cluster state; the host kernel knows which process owns a connection; the DPU
owns transport state and enforcement. A workload supplies neither credentials
nor identity claims.

The controller reads Pods, Services, and EndpointSlices and publishes bounded,
signed documents. `dpumeshd` identifies a caller with `SO_PEERCRED`, cgroup v2,
and `/proc`, authenticates to the controller with a node certificate, supervises
one broker for the allocated channel, and relays signed bytes to the DPU. The
DPU verifies the controller-issued `WorkloadGrant` before importing workload
memory. This document specifies those authority, registration, distribution,
peer, authorization, lifetime, configuration, and operating contracts.

## Architecture

```text
Kubernetes API ── get/list ──▶ dpumesh-controller Pod
                                     ▲
                                     │ TLS 1.3 + node client certificate
                                     │
kubelet ◀── Device Plugin ──▶ dpumeshd.service ── signed-feed delivery ──▶ BlueField
                                  │                                      Linux
                                  └── broker process per allocated slot ── DOCA Comch
                                       ▲
                                       │ one allocation socket
                                  workload Pod
```

![Implemented control-plane authority and placement](figures/control_plane.png)

[PDF](figures/control_plane.pdf)

Kubernetes runs the controller and workload Pods. `dpumeshd` and its brokers
run under host systemd; the feed receiver and `dpumesh_dpu` run under BlueField
systemd. A workload Pod declares the
transport by requesting and limiting exactly one `dpumesh.io/channel` on one
regular container. Kubelet calls the Device Plugin and mounts that slot's Unix
socket at `/run/dpumesh/channel.sock`. The PodSpec names no host device or host
path, and the workload receives no DPUmesh or Kubernetes credential.

The data plane is specified in [`DATA.md`](DATA.md), the application contract in
[`API.md`](API.md), and the gRPC adapter in [`GRPC.md`](GRPC.md). Across a node
boundary the same stream contract uses the authenticated peer carrier:

```text
intra-node   host TX mmap → DPA → staging → SG-DMA         → host RX mmap
inter-node   host TX mmap → DPA → staging → peer transport → staging → host RX mmap
```

`struct dmesh_peer_transport` is the carrier seam. TCP and RDMA carriers provide
ordered reliable bytes to mutually authenticated TLS 1.3 sessions. The code
above the seam owns stream frames, handles, generation binding, routing,
custody, limits, and failure accounting.

## Reading order

The chapters are ordered by dependency:

```text
1     Authority and distribution   who may state a fact and how it arrives
2-0   Node authentication          which DPU terminates a peer channel
2-1   Pod registration             which workload owns one Comch registration
2-2   Cross-node identity          how local and cluster bindings are joined
3     Authorization                whether an established identity may enter
4     Resources and lifetime       what an authenticated peer may consume
5     Operations and reference     names, configuration, files, and bounds
```

Authority supplies the keys used by node and workload authentication.
Authenticated identities supply authorization inputs. Resource limits remain
mandatory after authentication because an authenticated peer is not trusted.

## Terms

| Term | Meaning |
|---|---|
| controller | `controller/dpumesh_controller.py`, the cluster reader and document issuer |
| `dpumeshd` | the root-owned host service containing the Device Plugin, slot registry, evidence reader, controller client, feed relay, scope tunnel, cgroup manager, and broker supervisor |
| slot | one advertised `dpumesh.io/channel` device and its host Unix socket |
| broker | one host process for one live slot generation; it owns DOCA objects and exported memory |
| `WorkloadGrant` | the fixed-size, controller-signed v3 assertion binding kernel evidence, allocation lifecycle, DPU nonce, and expiry |
| feed | a complete signed document installed atomically on the DPU: topology, node membership, or Service targets |
| generation | the monotonic version of one feed; consumers adopt a complete newer document or retain the held one |
| topology | the cluster-scoped generation containing nodes, Pod placements, Services, endpoints, and protection classes |
| workload | the calling Pod identity built from a verified grant or adopted topology, never from request bytes |
| node credential | a static Ed25519 keypair generated and retained on one DPU for peer TLS |
| node certificate | a TLS client certificate whose single URI SAN is `spiffe://dpumesh.io/node/<node>` |
| peer channel | one worker-local authenticated connection between a DPU pair |
| incarnation | a peer-channel generation; every handle and in-flight operation carries it |
| handle | one full-duplex cross-node stream allocated by the destination DPU |
| admission switch | a root-owned DPU file that stops new protected sessions while established sessions drain |
| protection class | the topology's protected or unprotected grade for a Service |

## Security model

Workload processes are hostile. The Kubernetes API, kubelet, host kernel,
`dpumeshd`, controller, and broker bootstrap are trusted infrastructure. A
single DPU may be compromised while other nodes remain honest.

| Party | Trusted input or privilege | Security consequence |
|---|---|---|
| workload | request bytes only | cannot state its Pod, container, node, Service membership, slot, or grant |
| Kubernetes API | Pods, Services, EndpointSlices | supplies placement, selected container, Service membership, and routing facts |
| kubelet | Device Plugin allocation | mounts exactly one allocated socket into the resource-owning container |
| host kernel | peer PID and cgroup | binds the socket connection to a Pod UID and container ID |
| `dpumeshd` | root, `/proc`, delegated cgroups, DOCA device access, node TLS key | verifies local evidence, owns slot state, and supervises brokers; holds no Kubernetes token or grant-signing key |
| controller | read-only Kubernetes token and signing keys | validates its latest Kubernetes snapshot and issues topology, feeds, and WorkloadGrants |
| broker | trusted bootstrap, confined steady state | owns device, Comch, mappings, and one workload channel |
| one DPU | its node's memory and traffic | cannot authenticate as a different configured node or place foreign Pods on its channel |

A compromised DPU can read or corrupt workloads on its own node. Node-private
peer keys restrict that compromise to peer sessions involving the node. A
remote DPU verifies that every claimed source Pod is placed on the authenticated
peer node by the controller-signed topology. Cluster-scoped statements use
Ed25519 signatures whose private key is absent from every DPU. Node membership
and Service-target feeds use a distinct HMAC key shared only by the controller
and DPU; `dpumeshd` transports signed documents but holds no feed key.

Every untrusted input has a size or cardinality bound before allocation. Peer
stream count, staging, open rate, and source in-flight bytes are independently
bounded. A stalled or malicious authenticated peer therefore cannot turn its
identity into unbounded destination or source retention.

## Authority scopes and keys

| Scope | Component | Creates | Consumes |
|---|---|---|---|
| cluster | controller | topology, membership, Service targets, WorkloadGrants | Kubernetes objects, operator node file |
| node host | `dpumeshd` | kernel evidence, slot generations, broker lifecycle, DPU public-key report | kubelet allocation, node mTLS, signed controller responses |
| node DPU | `dpumesh_dpu` | registration nonce, static peer keypair, enforcement decisions | WorkloadGrant, topology and node feeds |
| workload | application adapter | Service request and stream bytes | one allocated socket and broker-exported mappings |

Key roles are deliberately disjoint:

| Credential | Algorithm | Private/secret holder | Verifier or peer |
|---|---|---|---|
| topology signing key | Ed25519 | controller | every DPU's controller public-key directory |
| per-node grant key | Ed25519 | controller | the named node's DPU; public key also appears in topology |
| feed key | HMAC-SHA256 | controller and each DPU | membership and Service-target consumers |
| DPU node key | Ed25519 | one DPU | remote DPU, bound through topology |
| node client certificate | TLS 1.3, P-256 certificate | one host's `dpumeshd` | controller client CA and URI SAN mapping |
| Linkerd workload key | P-256 | DPU Linkerd runtime | Linkerd identity service |

The topology, grant, and feed key directories contain distinct key material.
Rotation is selected by each directory's root-owned `active` file. A DPU keeps
bounded public-key overlap so a complete newer generation can move consumers to
a new key without accepting an unknown key id.

## Runtime placement and isolation

The implementation spans three kernels and distinct address spaces:

```text
Kubernetes cluster
├─ dpumesh-controller Deployment
│  └─ controller process: uid/gid 65532, read-only root, RuntimeDefault seccomp
└─ workload Pod
   └─ selected application container: uid/gid 65532, no capabilities
      └─ application + selected DPUmesh adapter
         └─ /run/dpumesh/channel.sock  (one Device Plugin allocation mount)

host Linux
└─ dpumeshd.service
   ├─ manager thread and Kubernetes Device Plugin
   ├─ slot listeners and controller/DPU relay threads
   └─ workers/pod<uid>.g<generation> cgroup
      └─ short-lived broker supervisor
         └─ dmesh_broker, PID 1 in private PID/network/cgroup/mount namespaces

BlueField Linux
├─ dpumesh-feed-receiver.service: unprivileged file installer
└─ dpumesh_dpu
   ├─ ARM control thread
   ├─ pinned ARM data workers and configured Linkerd runtimes
   └─ DPA execution contexts
```

The controller's ServiceAccount has cluster-wide `get` and `list` only for
Pods, Services, and EndpointSlices. Its Pod uses a read-only root filesystem,
drops every capability, and satisfies the Restricted security context in the
supplied manifest. It does not mount a host path.

`dpumeshd.service` runs as root because Device Plugin registration, kernel
process evidence, namespace creation, cgroup delegation, and DOCA device setup
are host operations. Its systemd unit bounds capabilities, address families,
devices, CPU, and memory; protects the filesystem; and makes writable only
`/run/dpumesh` and kubelet's Device Plugin directory. It receives a node TLS
key but no ServiceAccount token. CPUs assigned to the service are inside
kubelet's `reservedSystemCPUs`, and `systemReserved.memory` covers the service's
fixed host budget.

The service moves itself into a `manager` cgroup, enables `cpu`, `memory`, and
`pids` controllers on the delegated subtree, and creates one worker leaf named
by Pod UID and slot generation. Each leaf receives `cpu.max`, `memory.high`,
`memory.max`, and `pids.max`. This is infrastructure accounting under
`dpumeshd.service`, not Pod or container accounting. Kubernetes schedules one
extended resource per slot; the host reserve accounts for the process and DMA
memory that the extended resource represents.

A broker supervisor is a direct child of `dpumeshd`. Parent-death signals and a
private barrier close the fork/credential race. The worker enters a new PID
namespace, remounts private procfs, moves itself through an opened cgroup dirfd,
and creates private mount, cgroup, and network namespaces. After DOCA discovery,
Comch setup, and memory registration it pivots into a 4 MiB private tmpfs,
drops to uid/gid 65532, clears capabilities, sets `no_new_privs`, and installs
a seccomp filter denying `execve` and `execveat`. The filter is an execution
denial boundary, not a syscall allowlist. Bootstrap remains node-trusted;
steady state has no host filesystem or network namespace.

The application, broker, DPU ARM process, and DPA contexts have separate virtual
address spaces. The application and broker map the same broker-created sealed
memfds at unrelated virtual addresses. Wire records carry an mmap handle and
checked offset, never a process virtual address. The workload owns no DOCA fd;
its only received descriptors are forward/reverse ring memfds, TX/RX memfds,
and one eventfd.

The Pod contract is explicit:

```yaml
spec:
  automountServiceAccountToken: false
  containers:
  - name: app
    env:
    - { name: DPUMESH_SERVICE, value: echo-dpumesh } # omit for client-only
    - { name: DPUMESH_RINGS_PER_POD, value: "8" }
    resources:
      requests: { dpumesh.io/channel: 1 }
      limits:   { dpumesh.io/channel: 1 }
```

Exactly one regular container may request the resource. The controller rejects
init-container ownership, unequal request/limit values, a mounted projected
ServiceAccount token, a kernel container ID different from the resource owner,
a terminating Pod, a foreign node, or a server Service whose selector does not
match the Pod. Native and gRPC programs link their adapter. A POSIX program sets
`LD_PRELOAD` explicitly. Traffic outside those integration surfaces is outside
the DPUmesh data path; admission is fail-closed once a stream enters DPUmesh.

## Control paths

```text
                 node TLS                         signed documents
Kubernetes ─▶ controller ◀──────── dpumeshd ─────────────────────────▶ DPU
                 │                    │                                 │
                 │ WorkloadGrant      │ SO_PEERCRED + cgroup            │ nonce
                 └────────────────────┴──── broker manager socket ───────┘

kubelet ── Register/ListAndWatch/Allocate ──▶ dpumeshd
workload ── HELLO ──▶ allocated slot socket ──▶ broker ── Comch ──▶ DPU
DPU ── plaintext HTTP ──▶ local tunnel ── node mTLS ──▶ controller scope API
```

The node certificate's single URI SAN selects the exact configured node for all
controller routes except `/healthz`. Source addresses and caller-supplied node
names are not authorization inputs. `dpumeshd` delivers the topology,
membership, and Service-target documents without parsing them. The DPU feed
receiver bounds each payload, compares its SHA-256 digest for idempotent
transfer, writes a temporary file, calls `fsync`, and atomically renames it.
Consumers independently verify the document signature and monotonic generation.

The local scope tunnel is protocol-blind. It wraps the DPU's byte stream in the
same node-authenticated TLS context used by `dpumeshd`; it neither terminates
HTTP semantically nor changes a request. The controller answers a workload
scope query only when its held signed topology places the named Pod on the
certificate's node.

# 1. Authority and distribution

## 1.1 The controller

The controller is the only Kubernetes API reader. It lists Pods, Services, and
EndpointSlices, builds one topology snapshot, derives node membership and
Service targets from the same object snapshots, and signs WorkloadGrants after
validating host-kernel evidence against the latest successfully polled Pod,
Service, and EndpointSlice objects. It has no host-local
visibility and accepts evidence only from a configured node certificate.

The supplied Deployment has one replica. The process has no leader election or
shared publication lock: it replaces a complete document atomically and keeps
serving the last good document when a read, build, or signature operation
fails. Its HTTPS interface is:

| Method and path | Authentication | Result |
|---|---|---|
| `GET /healthz` | TLS only | readiness text |
| `GET /topology.v1` | configured node certificate | held Ed25519 topology |
| `GET /membership.v1` | configured node certificate | HMAC membership for that certificate's node |
| `GET /service-targets.v1` | configured node certificate | HMAC Service map derived from the held topology |
| `GET /workload-scope?pod_uid=…` | configured node certificate | placement only when the held topology assigns the Pod to that node |
| `POST /node` | configured node certificate | reports only that node's DPU public key and configured RDMA address |
| `POST /workload-grant` | configured node certificate | fixed 1545-byte v3 grant or a fail-closed error |

The server negotiates TLS 1.3 and admits at most 32 concurrent request handlers.
TLS handshake and HTTP I/O occur inside that bound with a 10-second connection
deadline; an excess connection is closed before a handler is created. All
routes except `/healthz` require a client certificate issued by the configured
node CA and carrying exactly one URI SAN
`spiffe://dpumesh.io/node/<node>`. A caller uses the API as follows:

```bash
curl --tlsv1.3 --cacert controller-ca.crt --cert node.crt --key node.key \
  https://dpumesh-controller.example:8443/topology.v1

curl --tlsv1.3 --cacert controller-ca.crt --cert node.crt --key node.key \
  'https://dpumesh-controller.example:8443/workload-scope?pod_uid=12345678-1234-1234-1234-123456789abc'
```

`POST /node` accepts `application/json` with the certificate node name, its
operator-configured IPv4 RDMA endpoint, and the 32-byte DPU public key in
lowercase hexadecimal:

```json
{"name":"rapids4","rdma":"192.168.100.2:47900","dpu_public_key":"<64 hex>"}
```

`POST /workload-grant` accepts the evidence and allocation lifecycle known to
`dpumeshd`. `service` is an unqualified Service name or the empty string;
`nonce` is 32 bytes in hexadecimal and `daemon_incarnation` is 16 bytes in
hexadecimal.

```json
{
  "pod_uid": "12345678-1234-1234-1234-123456789abc",
  "container_id": "<64 hex>",
  "service": "echo",
  "nonce": "<64 hex>",
  "slot": 3,
  "generation": 9,
  "daemon_incarnation": "<32 hex>"
}
```

Success returns `application/octet-stream` containing exactly one WorkloadGrant.
Malformed or oversized requests return 400 or 413, a certificate-scope or
authorization failure returns 403, missing held state returns 404 or 503 as
appropriate, and an unknown route returns 404. `/healthz` needs no client
certificate and returns 200 only after a topology is held; before that it
returns 503. The client bounds every response before accepting it.

Every data-bearing route maps the certificate's single URI SAN to the node
before reading caller claims. The controller does not authorize from a source
address. One cluster reader and one signed snapshot prevent per-node API load
and divergent topology views.

## 1.2 The generation

One UTF-8 text document, one record per line, `\n` endings, no spaces around
separators; comments start with `#` and are permitted only before `version=`. A
DPU adopts it whole or not at all, verified with a key it holds no signing half
of. It answers what nothing node-local can know: which Service a name refers to,
which node a Pod is on, and whether a Service is graded protected.

```text
version=<u64 decimal, strictly increasing across publications>
node=<node-name>,<rdma-ip>:<rdma-port>,<grant-key-id>,<grant-pub-hex64>,<dpu-static-pub-hex64>
pod=<pod-uid>,<node-name>,<namespace>,<service-account>,<pod-ipv4>
service=<namespace>/<name>,<cluster-ipv4>:<port>
endpoint=<namespace>/<name>,<pod-uid>
protected=<namespace>/<name>
signature=<key-id>,<hex128>
```

| Line | Answers | Read by |
|---|---|---|
| `node=` | RDMA address, grant public key, DPU static public key | 2-0, 2-1 |
| `pod=` | which node a Pod UID is on; its namespace, ServiceAccount, IP | 2-2, 3 |
| `service=` | ClusterIP and port | resolution, 3 |
| `endpoint=` | ready backends | reach, 3 |
| `protected=` | protection class | 3 |

Field syntax, enforced before anything is adopted:

| Field | Rule |
|---|---|
| `node-name` | DNS subdomain, ≤ 253 |
| `pod-uid` | lowercase RFC 4122 text form, exactly 36 chars |
| `namespace` | DNS label, ≤ 63 |
| `name` (Service) | DNS label, ≤ 63 — never named without its namespace |
| `service-account` | DNS subdomain, ≤ 253 |
| `pod-ipv4`, `cluster-ipv4` | dotted quad; the Pod IP is what makes destination-side `networks` authorization decidable |
| `grant/dpu key hex` | exactly 64 hex chars — the controller's per-node grant public key and the DPU's static handshake key |
| `key-id` | `[A-Za-z0-9._-]+`, ≤ 31, no `/`, no leading `.` |
| `hex128` | exactly 128 hex chars (64-byte Ed25519 signature) |

- `version=` is the first non-comment line, `signature=` the last; everything
  between may appear in any order.
- An unknown line kind, a duplicate `pod=` for one UID, an `endpoint=` or
  `protected=` naming a Service or Pod UID no other line defines, or any syntax
  violation refuses the whole document.
- `DMESH_GEN_POD_MAX`, `DMESH_GEN_NODE_MAX`, `DMESH_GEN_SERVICE_MAX`, `DMESH_GEN_ENDPOINT_MAX` and
  `DMESH_TOPOLOGY_MAX_BYTES` are refused rather than truncated at both ends: the
  publisher refuses to publish an over-bound generation so the last good one
  stands and the failure is loud at the source, and the consumer refuses to
  adopt one.
- An unchanged cluster publishes nothing, so a new version always says something
  new. A rotated signing key republishes even an unchanged cluster: the held
  document must never outlive the key that signed it.
- The next version is `max(now, held + 1)`, and `held` is read back from the
  installed file at startup, so a clock step backwards cannot present a rollback
  and a restart continues the sequence.

## 1.3 The node-scoped feeds

The controller publishes two HMAC feeds in addition to the topology. Both are
derived from the same Pod, Service, and topology snapshots used for grants.

| Feed | Published by | Scope | Signed with | Carries |
|---|---|---|---|---|
| node membership | controller | certificate node | feed keyring, HMAC-SHA256 | grant-eligible `(Pod UID, Service)` pairs for that node |
| Service targets | controller, derived from held topology | cluster view | feed keyring, HMAC-SHA256 | each Service's ClusterIP and ready endpoints, including endpoint Pod UID |
| topology generation | controller | cluster | `DPUMESH_CONTROLLER_KEY_DIR`, Ed25519 | *The generation* above |

The symmetric key still buys everything below a hostile DPU: a non-root host
process, a truncated or corrupted file, a stale or unsigned copy. A feed that
does not verify does not revoke — the direction that matters, since revocation
is the operation that ends a Pod. Membership revocation does not enable at all
without a feed keyring.

The feed keyring is disjoint from the grant keyring, so a leaked key is accepted
for only one wire role and the roles rotate independently. `dpumeshd` holds
neither key; it carries already-signed bytes. Key selection is filename-driven,
so provisioning rejects shared directories and identical key material.

The Service target snapshot is derived from the held generation rather than an
independent Kubernetes read, so the two cannot disagree. It places every address
it names — session key, ClusterIP and ready endpoints — in its Service, and a
session refuses to dial an address the generation places in another one. Every
selected address other than the session's own must resolve through the Pod UID
the snapshot pairs with it, so a Linkerd endpoint the generation no longer names
is declined rather than assumed to be the session's.

## 1.4 The envelope

Every generation of every feed is installed by atomic rename and ends with a
signature envelope naming the key that signed it:

```text
signature=<key-id>,<hex HMAC-SHA256 over every preceding byte>
```

The topology generation uses the same shape with Ed25519 and the controller
keyring; `dmesh_gen_verify` applies the same marker scan and key-id parsing as
`dmesh_feed_verify`. In all three, **only the signed prefix is parsed**: the
verifier returns the prefix length and the parser never sees past it, so
appended bytes are refused rather than ignored. A key id naming a file outside
its keyring directory is rejected.

Key files are 32 raw bytes or 64 hex digits, regular, opened `O_NOFOLLOW`,
owned by the effective uid, mode 0600 or 0400, in a 0700 directory.

## 1.5 Adoption

The DPU polls the topology file once a second. Nothing about that poll is
allowed to become a decision.

```text
   stat        size ≤ TOPOLOGY_MAX_BYTES?
   stamp       inode, mtime and length all match, AND the file has been
               installed longer than the timestamp granularity? → skip the read
   verify      Ed25519 over the signed prefix
   parse       into a staging table; interning is staged with it
   version     strictly older than what is held? → rollback, refused
   swap        release-store the new tables; park the displaced one
```

- **The stamp is an optimization, never a decision.** The DPU filesystem reuses
  the freed inode across a rename and stamps coarse timestamps, so unchanged is
  believed only once inode, mtime and length match *and* the file has settled
  past that granularity.
- **Interning is staged with the tables.** Interning assigns a node-local
  compact id to a Service name, which is a side effect; a document that is
  subsequently refused must not leave it behind. Adoption is all-or-nothing
  including its side effects.
- **Only a fully parsed generation is stamped**, so a rejected document is
  re-read until the publisher installs a newer one.
- The swap is a release store and the displaced generation is parked rather than
  freed: workers read the table locklessly and adoptions are at least a poll
  interval apart.
- A document that was read, verified and parsed and says what the held one says
  is counted (`unchanged`), unlike a file never touched — that distinction is
  what shows an operator the delivery pipeline is alive.

Adoption propagates to exactly three places:

```c
px_protection_refresh(objs);        /* protection class cache; chapter 3 */
dmesh_scope_refresh(objs);          /* mediated workload scope; chapter 3 */
px_peer_generation_changed(objs);   /* rebind or reset peer channels; chapter 2-0 */
```

## 1.6 What a rejected generation does: nothing

A missing, malformed, oversized, unsigned or rolled-back generation changes
nothing — it never revokes membership, withdraws a target or admits an
unverified registration. Outcomes are counted as
`dmesh_control_events_total{kind="topology",reason}` with reasons `adopted`,
`unchanged`, `rollback`, `unsigned`, `bad-key-id`, `bad-sig`, `malformed`,
`overflow`, `unreadable`. This is the fail-static rule every later chapter leans
on: a stuck publisher shows up as a stale generation, not as an outage.

## 1.7 Bootstrap and rotation

A DPU must hold the controller's public key before it can verify anything, so
that key cannot arrive in a generation. It is deployment-time material in
`DPUMESH_CONTROLLER_KEY_DIR`, loaded with the checks `dmesh_grant_load_key`
applies, and a DPU that cannot load one refuses to start.

The directory holds an overlap set of at most `DMESH_CONTROLLER_KEYS_MAX` public keys
and the envelope names which one signed. Adding a key is a file drop, retiring
one a delete; a generation naming a key id the set does not hold is refused and
counted. An empty directory and an over-full one both refuse startup on the
consumer side, exactly as the registration keyring does, and the three key
directories stay disjoint for the reason *The node-scoped feeds* gives.

## 1.8 Resolution — the consumer side

Names live outside, handles inside: the workload asks, its broker relays the
round trip (§2-1.9), and the DPU answers from the held generation. The answer
carries the DPU-interned compact id — a node-local transport identifier, never
workload identity. No registry file exists anywhere.

```text
Host Pod            per-Pod broker                     DPU
  │── RESOLVE ────▶│──── RESOLVE(name | ip:port) ────▶│  answered from the adopted,
  │◀─ RESOLVE_ACK ─│◀─── RESOLVE_ACK(interned id) ────│  Ed25519-verified generation
```

The native API resolves `"name"` (the Pod's own namespace) or `"name.namespace"`
in `dmesh_create_qp`. The preload facade resolves the IPv4 destination passed to
`connect`; a ClusterIP answers `meshed` only when its Service has a live
registered backend here (the generation names unmeshed Services too),
`status = not-meshed` makes leaving the mesh an explicit, logged kernel-TCP
decision. `status = no-generation`, a failed channel or a failed resolution
refuses the connect; there is no environment-controlled TCP fallback. Answers
are cached per key for one generation interval
(`src/core/dmesh_resolve.c`) and re-resolved after that or on a connection
error, so the cache is never staler than the generation.

## 1.9 When the controller is unavailable

Held data remains fail-static; new identity issuance stops.

```text
   controller down
        ├─ established streams            unaffected
        ├─ new streams, existing Pods     unaffected
        ├─ new streams, intra-node        unaffected (registration is node-local)
        ├─ a new Pod registration          refused: no WorkloadGrant can be issued
        ├─ feed delivery readiness         Unhealthy after the next failed cycle
        ├─ mediated policy lookups         use no answer for an unavailable route
        └─ topology/membership revocation  stops at the last verified generations
```

`dpumeshd` marks free slots unhealthy after a controller/DPU delivery failure,
so kubelet does not allocate a channel whose authority cannot be refreshed.
Already allocated slots keep their broker and mappings. A rejected, absent, or
unavailable document never becomes an empty document and therefore never
revokes state accidentally.

---

# 2-0. Node authentication

Which DPU is on the other end of a channel. Settled on every channel, before
any Pod identity is discussed.

## 2-0.1 The node credential

One static Ed25519 keypair per DPU, generated on the DPU at first boot into a
0400 file that never leaves it (`dmesh_peer_node_key_load`). It is a signing
key: it signs the certificate the DPU presents and the handshake transcript
under it, which is what proves possession. The traffic key is the session's own,
agreed fresh on every connection and never derived from this one. First boot
writes the private half with `O_CREAT|O_EXCL|O_NOFOLLOW`; later boots read it
back only if the file is regular, owned by the effective uid, has no group or
other bits, and is exactly 32 bytes. The public half goes to a separate readable
file; `dpumeshd` reports it to the controller, which publishes it in that node's
`node=` line.

```text
   DPU A                                                          DPU B
   Ed25519 private ─generated here, never leaves──               Ed25519 private
        │ public half only                                             │
        ▼                                                              ▼
   dpumeshd A ───────┐                                    ┌─────── dpumeshd B
                     ▼                                    ▼
                 controller: node=A,…,<A pub>   node=B,…,<B pub>
                                    │ signed generation
             ┌──────────────────────┴──────────────────────┐
             ▼                                             ▼
     DPU A: "B's key is this"                     DPU B: "A's key is this"
```

A DPU authenticates a peer with a key it obtained from the generation, not from
the peer. An all-zero static key is the placeholder a node carries until its DPU
has generated a credential; it binds nothing, so it is not a key.

## 2-0.2 One rule on top of a stock handshake

The channel runs TLS 1.3, mutually authenticated: each end presents a
self-signed certificate carrying its node static key, no certificate authority
is trusted, and resumption is off, so every connection performs a fresh key
agreement. One rule is added:

> the peer's static public key must equal the one the held generation binds to
> the peer's claimed node name. A name the generation does not bind, or a key
> that differs, refuses the channel.

The two halves are separate refusal reasons because they are different
operational events: `node-unbound` is a name the generation does not carry,
`node-key` a name it carries with a different key. The binding lookup happens
before a channel may exist at all, on outbound open and accepted inbound
connection alike, so an unbound name never reaches the handshake.

Keys are pairwise: a fleet-shared key would let one compromised DPU read
every conversation in the cluster. No per-workload key exists on this wire.

## 2-0.3 Incarnation

The incarnation advances on every entry to `AUTHENTICATING` and travels as the
first thing the initiator writes into the completed session, ahead of any frame:

```text
   dpumesh-peer-v1\n<local node>\n<peer node>\n<incarnation>\n
```

The handshake settles the peer's key and this prologue settles a name against
it, so the pair authenticates the protocol version, both node names and the
incarnation the connection's handles will carry. A session key is never reused,
so a prologue cannot be replayed into another connection. Every frame carries it
and it is matched on every path, because an asynchronous completion outlives the
state that named it — the same arithmetic `dma_generation` applies to a Pod
slot.

## 2-0.4 Simultaneous open

Simultaneous opens converge on the connection initiated by the
lexicographically smaller node name, which both ends compute independently, so
no negotiation is needed. A pending handshake is never replaced by a new open —
callers poll. A reopen while one is pending replaces its transport connection,
which is closed rather than overwritten.

## 2-0.5 Rebinding on adoption

`dmesh_peer_table_rebind` re-checks every live channel on every adoption and
resets one whose peer the generation dropped or re-keyed. This is where node
eviction takes effect: when the controller drops a node, every DPU refuses it at
once and consistently. It is the counterpart of 2-2's rule that an individual
refusal never tears a channel down.

## 2-0.6 Lifetime

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
     │                          │        │ pairwise key agreed
     │                          ▼        ▼
     │◀───────────────────────  OPEN
     │
     │   idle > CHANNEL_IDLE (only with no streams and nothing in flight),
     │   the generation drops or re-keys the peer, or the transport faults.
     │   Teardown is synchronous: the reset poisons every stream and releases
     │   every pinned extent before it returns, so there is no draining state.
```

The LRU that reclaims a slot when all `DMESH_CHANNEL_MAX` are in use applies the same
rule: a channel holding handles or in-flight bytes is skipped, so a new
connection never displaces live traffic — it is refused instead.

## 2-0.7 Channels and queue pairs

To keep a completion on its owner the channel is one queue pair per (node pair,
destination worker), and authentication runs on each one: a worker holds its own
credential context and settles the peer's key itself, so the pairwise key is per
(node pair, worker) and none crosses a worker boundary. `DMESH_CHANNEL_MAX × A`
is therefore the number to check against RDMA resource limits, not
`DMESH_CHANNEL_MAX` alone. `GENERATION_INTERVAL` and `DMESH_CHANNEL_IDLE_NS`
interact: a channel evicted and reopened between two generations pays setup
twice.

## 2-0.8 What this layer establishes, and what it does not

```text
  establishes   the far end holds the private key the generation binds to that name
                checked once per channel, amortized over every stream
                traffic is encrypted under a key only this pair holds
                it lapses the moment the generation drops or re-keys the peer

  does not      that the node is honest              ← "authenticated ≠ trusted"
  establish     that the Pods it names are real      ← chapter 2-2
                that it consumes resources politely  ← chapter 4
```

---

# 2-1. Pod registration

Registration binds one allocated slot and Comch connection to one live
Kubernetes Pod and container. The host kernel supplies process evidence; the
controller supplies cluster authority; the DPU supplies the connection nonce.
None of those values is accepted from the workload as identity.

## 2-1.1 Allocation and evidence

A client-only process sends an empty Service in `HELLO`; a server sends the
Kubernetes Service it provides. The same accepted `SOCK_SEQPACKET` connection
becomes the application↔broker control channel.

```text
workload             dpumeshd host service       broker                  DPU
   │                         │                      │                      │
   │ HELLO(Service)          │                      │                      │
   ├── allocated slot ──────▶│ MSG_PEEK framing     │                      │
   │                         │ SO_PEERCRED PID       │                      │
   │                         │ starttime/cgroup/     │                      │
   │                         │ starttime → Pod UID,  │                      │
   │                         │ container ID          │                      │
   │                         │ create worker cgroup  │                      │
   │                         │ fork + namespace      │                      │
   │                         ├─ socket+cgroup fd ───▶│ consume HELLO         │
   │                         │ verify PID/parent/    │ open DOCA + Comch     │
   │                         │ cgroup, final GO      │                      │
   │                         │                      │◀─ REG_CHALLENGE(nonce)│
   │                         │◀─ grant request ─────│                      │
   │                         │  broker SO_PEERCRED, retained evidence      │
   │                         ├── node-mTLS ───────────────▶ controller     │
   │                         │◀── signed WorkloadGrant ──── controller     │
   │                         ├─ fixed grant bytes ──▶│                      │
   │                         │                      ├─ WORKLOAD_ASSERT ───▶│
   │                         │                      ├─ POD_REGISTER ──────▶│
   │                         │                      │◀─ POD_ASSIGNED ──────│
   │                         │                      ├─ MMAP_EXPORT × N ───▶│
   │                         │                      │◀─ INIT READY ────────│
   │◀── READY + SCM_RIGHTS ────────────────────────│                      │
```

`dpumeshd` reads `SO_PEERCRED`, then reads `/proc/<pid>/stat`, cgroup v2, and
the start time again. The two start-time reads fence PID reuse; parsing begins
after the last `)` because a process name may contain spaces and parentheses.
The cgroup must contain both a canonical Pod UID and one 64-hex container ID in
the Kubernetes hierarchy.

The controller resolves exactly one Pod with that UID and requires all of the
following:

1. the Pod is not terminating and `spec.nodeName` equals the node certificate;
2. `automountServiceAccountToken` is explicitly false and no projected volume
   contains a ServiceAccount token;
3. exactly one regular container requests and limits
   `dpumesh.io/channel` to one;
4. that container's `status.containerID` equals the kernel-derived ID;
5. the Pod has a canonical IPv4 address;
6. a non-empty requested Service is unique in the Pod namespace, has a
   non-empty selector matching the Pod labels, and has a usable IPv4 ClusterIP
   and first port;
7. an EndpointSlice for that Service names the Pod UID with
   `conditions.ready: true`.

The broker asks for the grant only after receiving the DPU nonce. `dpumeshd`
recognizes the broker by its `SO_PEERCRED` PID, uid, retained start time, and
supervision table, and requires its Service to equal the launch record. It
retries the same nonce across a bounded controller-observation window when the
container status is still converging. Exactly one successful grant is issued
and presented to the DPU.

The slot listener is mode 0666 because kubelet bind-mounts the inode into a
non-root container. Its parent directory is mode 0755, but a Pod sees only its
allocation mount. Slot state serializes one worker per slot. Unbounded request
threads are not created: each slot owns one accept loop, the broker manager owns
one loop, and all sockets have request deadlines.

## 2-1.2 The WorkloadGrant

`struct dmesh_workload_assert_msg` is a fixed 1545-byte version-3 record.
Numeric fields are little-endian byte arrays; every text field is ASCII,
NUL-terminated, and zero-padded; Ed25519 covers every byte preceding `sig`.

```text
 offset  size  field                    authority
 ───────────────────────────────────────────────────────────────────────
      0     1  type                     WORKLOAD_ASSERT
      1     1  version                  3
      2     2  flags, reserved          zero
      4     8  issued_at                controller clock
     12     8  expires_at               issued + bounded TTL
     20    16  assert_id                controller randomness; replay key
     36    32  nonce                    DPU connection challenge
     68     4  channel_slot             Device Plugin slot
     72     8  channel_generation       dpumeshd monotonic slot reuse fence
     80    16  daemon_incarnation       random per dpumeshd process
     96    32  key_id                   per-node grant public-key selector
    128    64  cluster_id               controller configuration
    192   254  node_name                node certificate mapping
    446    64  pod_uid                  kernel evidence + Kubernetes object
    510    64  namespace                Kubernetes Pod
    574   254  pod_name                 Kubernetes Pod
    828   254  service_account          Kubernetes Pod
   1082   254  container_name           extended-resource owner
   1336    65  container_id             kernel evidence + container status
   1401    64  service_name             ready, dialable selected Service or empty
   1465    16  pod_ip                   Kubernetes status
   1481    64  sig                      Ed25519 over bytes [0,1481)
```

The grant binds three independent freshness dimensions: expiry bounds controller
observation age, the nonce binds the exact Comch connection, and
`(daemon_incarnation, channel_slot, channel_generation)` prevents an allocation
from crossing a daemon or slot reuse. Changing any field invalidates the
signature.

## 2-1.3 Verification

The DPU selects the grant public key by `(its own node name, key_id)` from the
held topology; the configured registration keyring is available during initial
bring-up. Verification and lifecycle checks produce
`dmesh_control_events_total{kind="assert",reason}`.

| Order | Check | Refusal |
|---|---|---|
| 1 | type, version 3, fixed canonical padding and syntax | `bad-type`, `bad-version`, `noncanonical` |
| 2 | signed cluster and node equal local configuration | `wrong-node` |
| 3 | issue/expiry order, maximum 300-second lifetime, one-sided 30-second issue skew, unexpired | `bad-time` |
| 4 | nonce equals this connection's challenge, constant time | `bad-nonce` |
| 5 | Ed25519 signature over the canonical prefix | `bad-sig` |
| 6 | assertion id has not been consumed | `replay` |
| 7 | slot is in range and equals this Comch registration slot | `wrong-channel` |
| 8 | daemon incarnation is current and no live registration belongs to another incarnation | `wrong-incarnation` |
| 9 | channel generation is newer than the accepted generation for that slot | `wrong-channel` |

Canonical and arithmetic checks precede asymmetric verification. The
successfully verified assertion id is consumed before the registration retains
claims. Verification occurs once per Comch connection and never on the data
path. A grant signed for another node cannot select a usable local public key,
and its signed node field independently fails the node comparison.

## 2-1.4 Replay

```text
   nonce             fresh per Comch connection.  An assertion cannot move to
                     another connection, survive a reconnect, or survive a DPU
                     restart.  This is the hard bound.
   consumed ring     ASSERT_REPLAY_SLOTS ids, evicting.  The per-connection
                     nonce is what makes eviction safe.
   consumed flag     an assertion used by one registration is not reusable.
```

The ring needs no lock: the Comch control PE is its only caller.

## 2-1.5 What the registration retains

A successful verification builds the Linkerd workload JSON from the signed
namespace and Pod name and keeps the signed Pod UID, namespace, ServiceAccount,
Pod IP and Service with the registration.

```text
   pod->workload          {"ns":…,"pod":…}     source_workload for outbound discovery
   pod->pod_uid           signed
   pod->namespace_name    signed  ┐
   pod->service_account   signed  ├─ the inbound verdict's inputs (chapter 3)
   pod->pod_ip            signed  ┘
   pod->granted_service   signed
```

These are retained because intra-node the placement step of 2-2 is satisfied by
the live registration instead of the generation: **within a node, the
registration is what the generation is across one.** One admission path serves
both. A re-tenanted slot clears all of it — the new tenant states its own.

A Pod may request a Service name in its channel HELLO, but that request carries
no authority. The controller includes it in the grant only when the observed
Pod is a selected endpoint of that Service. The Pod enters backend selection after
`POD_INIT_RESULT(READY)`.

## 2-1.6 The second gate: `POD_REGISTER`

```text
   POD_REGISTER(service_name)
      ├─ the slot is still quiescing                      → refused
      ├─ already registered, same connection and Service  → the existing id (idempotent)
      ├─ already registered, different Service            → conflicting replay, refused
      ├─ no verified assertion, or one already consumed   → refused
      ├─ service_name ≠ the asserted Service                → refused
      ├─ no interned id for <namespace>/<name>            → fails closed, refused
      └─ pod_id and service_id assigned                   → POD_ASSIGNED
```

The Service name is authoritative for identity, so it must equal the one the
connection's assertion named. The compact id is the DPU's own interning of the
generation: serving an identity requires the generation that defines it, so a
Service no generation defines cannot be registered. A client-only registration
needs no id. The idempotent path lets a host that lost `POD_ASSIGNED` or
`POD_INIT_RESULT` recover by repeating the request.

## 2-1.7 Membership and revocation

The controller publishes the grant-eligible `(Pod UID, Service)` pairs for the
certificate's node:

```text
member=<pod-uid>,<service-name>
member=<pod-uid>,-              ← client-only registration is authorized
```

Names, not node-local numbers, cross this feed. The grant checks decide both an
assertion and a membership entry, so deleting a Pod, changing its labels, losing
endpoint readiness, or losing its running container status withdraws its pair.
Every grant-eligible Pod also contributes the bare form, which is what a Pod
registering without Service membership holds.

The Comch control thread adopts each newer generation and closes the exact
registration whose pair has left it, through the same teardown a disconnect
uses. **Withdrawal takes two consecutive generations**
(`DMESH_MEMBERSHIP_ABSENCES_TO_REVOKE`): a generation whose snapshot predates a
registration omits it without meaning it, so one absence is not authority to
tear a Pod down.

## 2-1.8 Quiescence

Unregister, revocation and Comch disconnect share one path, and the order in it
is the content.

```text
   begin       stop every producer first (clear the producer and egress masks)
               clear dma_ready and registered, drop the pod_id → slot mapping
               keep every imported handle published until both barriers pass
               ↑ revocation begins while the Pod's Comch connection is still
                 live, so the first gate runs against a Pod that is still mapped

   gate 1      every EU acknowledges RING_DEL              "dpa-ring-del"
   gate 2      the workers that own destination lanes drain them and run one
               further progress pass, so DOCA has released the buffer
               references its completion callbacks hold    "proxy-reclaim"
   gate 3      imported mappings destroyed                 "mapping-destroy"
               → POD_QUIESCED
```

Each ARM worker first closes the connection and L7 state it owns; the gates run
only after every producer has joined that barrier. An acknowledgement the DPU
channel has no posted receive for is held as a fence and retried whenever that
EU releases its execution unit, and the control thread resends `RING_DEL` to
unacknowledged EUs every 10 ms.

A quiescence that has not passed every gate within `DMESH_CLEANUP_STALL_NS` is
reported with the gate holding it, and again every interval it remains there —
one line cannot show whether the gate moved. The report names the expected and
acknowledged EU masks, each worker's completion-queue occupancy and deferred
receive count, and each expected EU's posted receives, because the DPA drops a
ring acknowledgement when the channel has no posted receive and receives are
withheld while a worker's completion queue is above its backpressure mark. Until
it passes, the slot and its imported mappings are held.

## 2-1.9 The per-Pod broker

A workload that owns `/dev/infiniband` can program DMA, so every DOCA object is
owned by a trusted host broker: the device, progress engine, Comch connection,
exported mmap handles, and DMA-registered memory. The workload receives one
allocation socket from kubelet and never receives a device descriptor.

`dpumeshd` creates one broker and one empty private-root mountpoint for one slot
generation. A root-only launch socket
carries exactly two descriptors: the already accepted workload connection and
an opened worker-cgroup directory. The launched supervisor must be a direct
child of `dpumeshd`; its child must present uid 0, the expected parent PID, and a
fresh PID/start-time identity before the final launch barrier opens. No token or
command-line secret is used.

The broker's privilege reduction is ordered around device initialization:

1. enter a fresh PID and mount namespace and replace `/proc`;
2. move through the supplied dirfd into
   `dpumeshd.service/workers/pod<uid>.g<generation>`;
3. unshare mount, cgroup, and network namespaces and create a private tmpfs;
4. consume the fixed 76-byte workload `HELLO`;
5. open the DOCA device, create Comch, obtain the DPU challenge, acquire and
   present the WorkloadGrant, and export all mappings;
6. pivot into the empty tmpfs, detach the host root, drop supplementary groups,
   uid/gid and capabilities, set `no_new_privs`, and deny both exec syscalls;
7. send `READY` and progress the channel as uid/gid 65532.

Opening the device and loading its providers require the visible host root, so
bootstrap is trusted node code. The steady process has a private empty network
namespace and root, private PID/cgroup/mount namespaces, host uid/gid 65532, no
capabilities, and no executable transition. The seccomp filter permits the
remaining DOCA syscall surface and therefore is not a general allowlist. The
supervisor removes the empty host mountpoint after the broker exits; daemon
startup removes empty mountpoints left by a host restart.

`READY` passes the attach set with one `SCM_RIGHTS` control message:

```text
forward-ring memfd[0..K-1]
reverse-ring memfd[0..L-1]
TX memfd
RX memfd
pod-global doorbell eventfd                  total = K + L + 3 descriptors
```

Every memfd is sealed with
`F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL`; `F_SEAL_WRITE` is absent because the
application and broker update shared pages. The application validates message
version, exact descriptor count, geometry, sizes, and seals before mapping.
Equal offsets in the two mappings refer to the same pages; equal virtual
addresses are neither required nor transmitted.

The application↔broker protocol is version 3:

| Message | Direction | Exact contract |
|---|---|---|
| `HELLO` | application → broker | 76 bytes; magic, type, version, zero padding, NUL-terminated optional Service |
| `READY` | broker → application | 48 bytes and one ordered descriptor set |
| `ERROR` | broker → application | 136 bytes; initialization failure is terminal |
| `RESOLVE` | application → broker | fixed header plus DPU resolution payload and request id |
| `RESOLVE_ACK` | broker → application | exact version and matching request id |
| `TRANSPORT_DOWN` | broker → application | fixed terminal reason; no same-channel reconnect |

Data bytes never traverse this socket. The broker forwards only resolution
requests and converts accumulated DPU reverse doorbells into one eventfd edge;
the application drains and interprets reverse rings. The IPC has no operation
that accepts an application-created memfd or eventfd.

One slot owns at most one broker. `dpumeshd` stores the worker by final PID and
rechecks its start time on every manager request. A broker exit removes that
entry, closes the slot's worker reference, and holds a five-second cleanup reuse
barrier when registration reached the DPU. Only then does the same slot return
to `FREE_LISTENING`. The slot generation increases before each authorization,
so late grants and completions cannot enter its next tenant.

The broker is a child of the `dpumeshd` service rather than an independently
persistent unit. A daemon stop terminates every broker; parent-death signals
also kill the supervisor and its namespace-init child. Unexpected broker-control
EOF or `TRANSPORT_DOWN` makes the application raise `SIGTERM` and exit with
status 75. Kubernetes restart policy creates a new application process, slot
generation, broker, nonce, and grant.

| Event | Result |
|---|---|
| normal channel destruction or application EOF | broker sends `POD_UNREGISTER`, waits for `POD_QUIESCED`, destroys Host exports, and exits |
| broker or Comch failure | control socket becomes terminal; application exits; slot observes the cleanup barrier before reuse |
| `dpumeshd` stop or restart | all child brokers terminate and free slots are advertised only after feed/controller readiness returns |
| membership withdrawal | DPU removes routing and starts ring/mapping quiescence for the registration |
| process signal | broker leaves its progress loop and executes unregister/cleanup |

The launch itself constrains which process can reach Comch, while the signed,
nonce-bound WorkloadGrant proves the Kubernetes and allocation identity the DPU
cannot observe. Both gates are required: device confinement does not establish
a Service, and a signed claim does not transfer device ownership to a workload.

---

# 2-2. Identity across a node boundary

A destination must establish the source workload's identity without trusting the
source's node.

## 2-2.1 Two bindings

Neither component can produce the other's:

| Binding | Established by | Evidence | Scope |
|---|---|---|---|
| Comch channel ↔ Pod UID | `dpumeshd` | `SO_PEERCRED`, peer cgroup, `/proc/<pid>/stat` | node; never leaves it |
| Pod UID ↔ node, namespace, ServiceAccount, Pod IP | controller | Kubernetes objects | cluster; travels in the generation |

```text
   binding 1 — node scope                binding 2 — cluster scope
   ══════════════════════                ═════════════════════════
   "this channel is Pod P"               "Pod P is on node A, is ns/sa, has IP x"

   evidence   host kernel                evidence   Kubernetes objects
   by         dpumeshd                   by         controller
   read by    its own DPU, only          read by    every DPU
   travels    never leaves the node      travels    the signed generation
              └──────────────────────────────────────────────────────────┐
   The local binding does not have to be verifiable off-node. A destination
   needs binding 2, and binding 2 is already a signed table it holds. ◀────┘
```

The source uses binding 1 to know which Pod it carries and writes that UID into
the stream open; the destination uses binding 2 to decide whether that Pod is
the source node's to speak for. The only peer-supplied values that matter are a
UID string and a port.

## 2-2.2 The check

The channel is authenticated once (2-0), long-lived, and carries every stream
between the two nodes.

```text
  Pod P (node A)      DPU A                      DPU B           Pod Q (node B)
      │                 │                          │                    │
      │ ──── DMA ─────▶ │ OPEN(src=P, dst=Q, port) │                    │
      │                 ├─────────────────────────▶│                    │
      │                 │  on the authenticated pair channel            │
      │                 │◀──── HANDLE h ───────────┤                    │
      │                 │ ──── RDMA (h, bytes) ───▶│ ──── SG-DMA ─────▶ │
```

`STREAM_OPEN` carries `src_pod_uid`, `dst_pod_uid`, `src_service_key`,
`dst_port`, a source-local correlation token and `src_generation`. The
destination checks it in two stages.

In the channel layer (`dmesh_peer_stream_open`):

```text
   1  the channel is OPEN and the incarnation matches      incarnation / state
   2  canonical text, reserved zero, port non-zero         malformed
   3  the peer's open-rate token bucket allows it           open-rate
   4  the peer holds fewer than PEER_STREAMS_MAX            streams
   5  the generation places src_pod_uid on this channel's node  not-on-peer
   6  dst_pod_uid names a live local registration          no-pod
```

In the proxy layer (`px_peer_destination_opened`), before any byte is accepted:

```text
   7  the destination Pod is data-ready and has an interned Service
   8  the generation places src_pod_uid in the Service src_service_key claims
   9  dst_port is the port the destination's Service publishes
  10  the controller's mediated scope allows this destination Pod
  11  the inbound verdict, from generation-owned identity     (chapter 3)
  12  the protection classes of caller and callee             (chapter 3)
```

Step 5 is the identity check and the whole reason the generation is signed by a
key no DPU holds:

```c
if (!table->ops->pod_on_node(table->ops_ctx, open->src_pod_uid, channel->node_name))
        return peer_refuse(table, channel, DMESH_PEER_REFUSE_NOT_ON_PEER);
```

— a lookup and a string compare against the adopted table. Step 8 is what makes
step 12 meaningful: a source that claims a Service must be an endpoint of it in
the generation before its protection class is believed.

**The identity check is a lookup in a signed table.** No asymmetric
operation happens on the connection path: the generation is verified once on
adoption and the DPU pair once when the channel opens, and both amortize over
every stream between the two nodes. A node may claim any Pod the cluster says is
on it — its own Pods, whose memory it already holds — and nothing else.

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
```

`src_generation` is not an input to the identity check. It is carried so a
destination could notice it is behind and adopt sooner; no consumer reads it
yet, and the field is part of the wire ABI so adding one does not change the
frame.

## 2-2.3 Why a table and not a credential

| | Destination holds | Per-stream cost | One compromised DPU obtains |
|---|---|---|---|
| believe the peer | nothing | none | every identity in the cluster |
| peer forwards a controller-signed assertion | a cache | one verification per source Pod | that node's Pods |
| **hold the signed topology** | the cluster's Pod table | a lookup | that node's Pods |

Rows two and three deny the same thing and differ only in whether the
destination spends memory or cycles. A DPU is memory-rich and cycle-poor
relative to what it protects — a connection costs tens of ARM core-µs to build
and tear down, an asymmetric verification is the same order of magnitude, and a
`pod=` line is under two hundred bytes, so ten thousand meshed Pods are a couple
of megabytes. The implementation uses the held-table row and refuses a
generation above `DMESH_GEN_POD_MAX=65536`; it has no per-stream forwarded
assertion mode.

Row two is also what every certificate-based mesh does, with the signed
statement travelling per connection instead of held at rest. The same cluster
authority signs the same binding either way; only where the signature is spent
differs.

## 2-2.4 Intra-node: the same check, degenerate

Source and destination are on this DPU, so steps 5 and 8 are satisfied by the
live registration rather than by the generation, which is why 2-1.5 retains
`namespace_name`, `service_account` and `pod_ip` on `pod_state`. Steps 7 and
9–12 are unchanged, so one admission path serves both cases.

## 2-2.5 Freshness

| Window | Size | Applies to |
|---|---|---|
| generation publication | `GENERATION_INTERVAL` + adoption lag | how stale a *placement* can be, both directions |
| local membership withdrawal | 2 × generation cadence | how long a deleted Pod's *local registration* survives |
| assertion lifetime | `DMESH_ASSERT_MAX_LIFETIME_SEC` | how long a relayed assertion stays usable; the nonce is the hard bound |
| clock skew | `DMESH_ASSERT_CLOCK_SKEW_SEC`, one-sided | controller↔DPU clock spread |

For a hostile node the operative bound is the first row: it can claim a Pod that
left it until every destination adopts the generation that moved it. An honest
node stops originating the moment the local registration ends — the Comch
connection drops and the sweep closes its streams — so the other three never
extend an honest node's claim cross-node. A recreated Pod carries a new UID, so
no window lets an old identity inherit a new placement.

## 2-2.6 A peer whose claims are refused

```text
   (a) the destination's generation is behind
       Pod P moved C → A; the destination has not adopted that generation yet.
   (b) the source's generation is behind
       Pod P left A; the source has not noticed yet.       (a) and (b): both honest.
   (c) the source is hostile
       It claims an identity it never held.
```

A single refusal cannot distinguish them, and both skew cases have an honest
explanation, so tearing the channel down would turn ordinary skew into an
outage. The question is better asked as what a refusal *consumes*: a lookup and
a reply, so what needs bounding is the rate.

A destination bounds and reports; the controller evicts. Per-peer limits cap
what any peer can hold and the open-rate bucket caps what a flood of refusals
costs, so a peer refused often becomes slow rather than disconnected and the
honest Pods on that node keep their streams. Removing a node is a cluster-wide
decision and belongs to the component holding cluster-wide authority: dropping
it from the generation makes every DPU refuse it at once, through 2-0's rebind.
The same division applies to a misbehaving Pod: `px_poison` ends the connection,
and the membership generation removes the Pod. Refusals and
poison are counted per worker and surfaced as
`dmesh_control_events_total{kind="peer",reason}` and in the worker stat line.

---

# 3. Authorization

Identity says who; this chapter says whether. [`DATA.md`](DATA.md) owns the
inbound verdict's per-protocol rules; what is control-plane is where its inputs
come from, who evaluates it, and what decides when there is no answer.

## 3.1 The asymmetric split

A session crossing two nodes builds one proxy, not two. The destination needs a
verdict, not a proxy:

| | Source DPU | Destination DPU |
|---|---|---|
| discovery, balancing, retries, protocol | full outbound stack, per session | — |
| authorization | — | policy evaluation |
| cost scaling | per session | **per destination Pod and port** |

`Inbound::build_policies(workload, …)` binds one watch set to one workload
string, so the adapter calls it once per registered destination Pod rather than
once per process; streams share the watch and pay only `connection_verdict`,
which the connection caches per destination so a session alternating backends
does not re-enter the policy layer per unit. A
cross-node L7 stream costs one outbound session plus a lookup. A full stack on
both sides would double the measured session cost.

The registered workload string indexes the policy watch without placing a
proxy listener in the Pod. The DPU destination context includes its real
Kubernetes `nodeName`, so stock endpoint discovery can apply locality without
an empty-node lookup.

## 3.2 Watch lifetime

A watch is held for as long as its destination Pod is served, not asked for per
stream. The store evicts an idle watch and respawns it holding the configured
default, so a verdict taken against a respawned watch is taken against this
proxy's configuration — an enforcement point that only asks while streams arrive
lapses to the default exactly when a Service falls quiet, which is when a caller
its policy refuses would find it open.

```text
   dmesh_l7_workloads(worker) → [(workload, ip:port), …]   what this worker serves
                    │
                    ▼  reconcile(), on the maintenance pass
      start a watch for a destination that has none   (before its first caller)
      drop a watch whose destination is gone          (a held watch never expires)
```

Registration and its end happen on the control thread and each worker's stores
are its own, so workers reconcile against that list rather than being told;
`l7_inbound_forget` ends the watches for a workload whose registration ended.

Each port's watch latches on its first answer. Until the policy controller has
answered there is no verdict — deciding on the configured default would decide
with something the cluster never said about that port. Once an answer lands it
governs, whether it names a `Server` or is the cluster's own default, and it is
never withdrawn. A verdict asked re-entrantly from inside the data path answers
"no verdict" rather than inventing one.

## 3.3 Where the verdict's inputs come from

The stock evaluation matches an `AuthorizationPolicy`'s `networks` clause before
its identity clause, and an empty match denies. Two inputs decide it and a Pod
supplies neither:

```text
   client address    client_addr = pod_ip:src_port, the source Pod's real
                     cluster IP — intra-node from the signed assertion retained
                     on pod_state, cross-node from the generation's pod= line.
                     A zero source port reads as one, because zero is not a
                     socket address.  A synthetic address would make every
                     realistic policy unevaluable, so nothing may stand in.

   client identity   <service-account>.<namespace>.serviceaccount.identity.<trust-domain>
                     built from the same two signed sources, and presented as a
                     TLS *state* rather than a string, because that is the only
                     shape Authentication::TlsAuthenticated matches — the same
                     substitution DmeshIo makes for the byte stream.
```

Neither is ever peer- or Pod-supplied, and neither is the DPU's own
control-plane certificate: that is proof of the DPU, not of the originating Pod.
The watch is named by the destination — its workload names the policy, its
address and the port its Service publishes name the watch. The client's own port
names nothing the policy controller has heard of.

## 3.4 Protection classes

The `protected=` lines grade a Service. Every registration stays
grant-verified either way, so what the generation grades is the interaction
rules, not registration validity. The grading cannot be inferred from Pod
input, or a Pod would opt itself out.

`px_protection_refresh` caches the grading per interned Service on adoption, so
a data worker's decision is a byte read, and `px_parse_l7`'s two fail-closed
decisions ask the Service rather than the process. `dmesh_inbound_admits` states
three rules as one decision:

```c
if (verdict >= 0)   return verdict;              /* a served verdict decides alone */
if (strict)         return 0;                    /* protected callee, no answer */
if (caller_strict) { *mixed = 1; return 0; }     /* protected caller, unprotected callee */
return 1;                                        /* an ungraded Service carries the stream */
```

For the inbound gate, only `PX_PROTECT_STRICT` is strict: a known relaxed
Service or a Service with no generation grade carries a stream when no verdict
exists. A protected caller reaching an unprotected callee is refused and
counted `mixed-callee-unprotected` unless the callee's own policy admitted it.
The L7-adapter decline path has a separate rule: a protected or unknown Service
is refused, while a generation-known relaxed Service may use the L4 path. This
behavior is compiled in; there is no `DPUMESH_L7_FAIL_CLOSED` runtime switch.

## 3.5 Scope of the control-plane credential

The DPU authenticates to Linkerd's destination and policy services with one
credential per node and names the workload it asks about as a plain string, so
the upstream API cannot express "only the Pods this node serves" — a compromised
DPU could otherwise read every workload's outbound policy.

Where the API cannot express the restriction, the controller mediates:
`GET /workload-scope?pod_uid=…` answers only for Pods the generation places on
the asking node. The DPU sends HTTP to `dpumeshd`'s local scope listener;
`dpumeshd` forwards the bytes through a TLS 1.3 connection carrying its node
certificate. The controller derives the asker exclusively from the certificate
URI SAN and verifies that node against the operator node file. The DPU's query
cannot select or override that identity.

`doca/control_scope.{c,h}` asks once per registration on the control thread and
again on every adoption; a data worker reads `pod_state.scope_state` and asks
about nothing the controller has not allowed, counted as `inbound.out-of-scope`.
An unanswered question withdraws nothing and opens nothing: the Pod is
`UNKNOWN`, there is no verdict, and the protection class decides. An unset
`DPUMESH_CONTROLLER_SCOPE_URL` puts every Pod in that state.

## 3.6 Reach

The backend set comes from the generation: `endpoint=` joined with `pod=` for
placement, split into a local set and a remote set
(`dmesh_topology_remote_endpoints`, `dmesh_service_has_remote`).
`px_resolve_backend` prefers local, because local endpoints are a DMA away, and
falls to `PX_DST_REMOTE` — the third outcome `px_unit_prepare` distinguishes
from "host not ready", so a Service healthy elsewhere is reported as remote-only
rather than poisoned as a Service with no replicas. Having no local replica
costs latency, never the connection.

The local half stays registration-derived rather than generation-derived: a Pod
that registered between a generation's snapshot and its publication is absent
from that one generation without having stopped serving, and the fail-static
table already promises it keeps serving intra-node.

## 3.7 The Linkerd control plane

The Linkerd static library creates destination, identity, and policy clients
from `LINKERD2_PROXY_*` environment variables when either configured L7 Service
list is non-empty. With both lists empty, worker construction returns without a
Linkerd runtime and the native L4 data plane remains active. An enabled L7 mode
requires reachable stock Linkerd services, identity files, and the signed
Service-target feed; incomplete configuration fails preflight.

The application can request only a Service name; it cannot assert Pod UID,
namespace, labels, ServiceAccount, node or Linkerd workload. The gateway is
byte-transparent, so it cannot mint or terminate mesh identity.

### Identity

1. The DPU configuration provides a ServiceAccount token with the Linkerd
   identity audience, trust roots, and a key and CSR whose DNS SAN is
   `<service-account>.<namespace>.serviceaccount.identity.<trust-domain>`.
2. The embedded proxy sends `Identity.Certify(token, identity, CSR)` to
   `linkerd-identity` over the configured control connection.
3. The returned leaf and intermediate certificates are installed in the proxy's
   in-memory credential watch; destination and policy clients use it for mTLS.
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

`LINKERD_*_ADDR` names the reachable endpoint for each service. TLS remains
end-to-end between the embedded proxy and the configured Linkerd service; the
configured TLS identity is checked independently for identity, destination,
and policy connections.

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
nonexistent DPU ports, which does not disable per-session outbound policy
watches.

An invalid or unroutable policy fails the protected L7 session. A control-plane
disconnect retains only state Linkerd's watches already hold; a new lookup that
cannot obtain policy fails and is never converted to an unobserved TCP dial. A
declined protected session ends rather than being forwarded as plain L4.

### Destination

The destination presented to Linkerd is the Service's real ClusterIP and port.
The adapter keeps its synthetic `10.96.0.<interned-id>:9092` address only as an
internal backend key; the target feed names Services by `namespace/name` and the
adapter resolves each to the DPU-interned id.

Destination and profile streams may update policy metadata while a session is
live. DPUmesh remains the authority for the set of node-local registered Pods.
The feed snapshots the ClusterIP and ready endpoint IPs, each address paired
with a port from the subset that published it and with the Pod UID the
generation places there. That UID is what makes an endpoint Linkerd selects
checkable against a live local registration; [`DATA.md`](DATA.md) states what
the adapter does with each outcome, and none of them is ever replaced by a TCP
dial. Within a Service, DPUmesh retains backend selection: the resolution
enforces that the selected endpoint is live and node-local, and Linkerd's own
endpoint weighting inside a Service is not claimed.

### Policy boundary

Linkerd's stock `OutboundPolicies.Get/Watch` response contains protocol and
route configuration. It does not expose the inbound `AuthorizationPolicy`
allow/deny decision, which a sidecar's inbound half enforces against the
authenticated peer identity. There is no Linkerd inbound proxy in the DPUmesh
byte path, so the DPU is that enforcement point instead:

- `InboundServerPolicies.WatchPort` is discovered per registered destination Pod
  and port, and the verdict is the stock evaluation reached through the fork's
  `connection_verdict`;
- the inputs are the controller-signed Pod IP and ServiceAccount, so the verdict
  never rests on the shared `dpumesh-dpu` certificate;
- `source_workload` remains trustworthy input to stock outbound discovery;
- a DPU asks about a Pod only where the controller's mediated lookup places that
  Pod on its own node.

## 3.8 What is counted

Admissions are counted like refusals, and for the same reason: the destination
emits the inbound family at connection level, and a verdict that admitted is as
much a fact about this enforcement point as one that refused. Without it a
healthy run is indistinguishable from an enforcement point that never ran.

```text
   dmesh_control_events_total{kind="inbound",reason=…}
      admitted · denied · no-policy · no-port · out-of-scope
      mixed-callee-unprotected
```

---

# 4. Resources and lifetime across the boundary

Identity is settled and policy has admitted. A peer DPU is still authenticated,
not trusted: everything it sends is input — stream opens, lengths, handles, and
the rate of all of them — so every rule here is a bound or a check, and every
refusal is counted by reason.

`doca/peer_channel.{c,h}` carries the state machine, disjoint full-duplex handle
namespaces, the five control messages plus DATA, bounded parsing and both sides'
bounds; `doca/dpu_proxy.c` carries the hooks it binds to.
`tests/peer_channel_test.c` drives the module end to end through a recording
transport that stands in for a carrier.

## 4.1 Custody across the boundary

[`DATA.md`](DATA.md) states the intra-node rules — L4 end-to-end, L7 hop-by-hop
in three bounded stages. The boundary changes one release point in each.

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

**L4 keeps the end-to-end loop, stretched by one round trip.** Pieces whose
destination is remote park in a per-peer un-ACKed slot pool
(`DMESH_PEER_TX_SLOTS`), and `STREAM_ACK { incarnation, handle, seq_first,
seq_count }` retires the named run — only then does `px_custody_sub` run and the
sender's `TX_ACK` follow. A destination sends `STREAM_ACK` when bytes have
landed in the destination Pod's host RX mapping, staging up to
`DMESH_STREAM_ACK_BATCH` acknowledgements before flushing.

**L7 keeps its hop-by-hop composition; the wire hop joins it.** The Pod-side
release (`dmesh_l7_release` on stack consumption) is untouched; an egress arena
chunk whose destination is remote retires on `STREAM_ACK`.

```text
   Pod P ──DMA──▶ staging A ──RDMA──▶ staging B ──SG-DMA──▶ Pod Q
                                                              │
        TX_ACK to P ◀─── STREAM_ACK ◀───────────────────── landed
        (L4: staging piece · L7: arena chunk retired by the ACK)
```

Releasing at the source would credit bytes nobody received. Errored transfers
follow the intra-node rule: a fault releases custody, poisons the streams it
carried, and is counted — never silently retried per stream.

A QP's window is `TX_BLOCKS_PER_CONN × TX_BLOCK_SIZE` at the defaults, with two
qualifications. Both constants are seeds — the live values are clamped to the TX
mapping's geometry and blocks come from a shared per-process pool, so the window
is a ceiling, not a reservation. And the credit round trip is fabric RTT plus
destination SG-DMA completion plus `STREAM_ACK` batching: against a 125 KB
bandwidth-delay product at 100 Gb/s × 10 µs the window covers it ~33×, at
400 Gb/s × 20 µs the product is 1 MB and the margin is 4×. One stream fills the
pipe either way, but the margin thins as fabrics get faster. The window is what
makes end-to-end credit affordable; a smaller one would have forced weaker
semantics.

The transport carries no flow control of its own, and neither does the DPA or
the SG-DMA engine inside a node. Forward-ring credits, arrival custody and lane
credits do that work and continue to.

## 4.2 What a peer may consume

At the **destination** a peer is admitted against `DMESH_PEER_STREAMS_MAX`
concurrent streams, `DMESH_PEER_STAGING_MAX` staging bytes,
`DMESH_PEER_TX_SLOTS` pending landing completions and `DMESH_PEER_OPEN_RATE`
opens per second (a token bucket), and is refused beyond them rather than
letting one node's traffic displace another's. Staging is charged per `DATA`
arrival, and a delivery the node holds keeps its charge and defers its
acknowledgement until `dmesh_peer_delivered` reports the bytes in the
destination Pod's mapping — which is what makes the staging bound real. A
receive slot per accepted extent bounds the number of one-byte extents a hostile
peer can leave pending independently of the byte bound.

At the **source** a stalled peer is bounded too: `DMESH_PEER_TX_INFLIGHT_MAX`
caps the un-ACKed bytes (L4 pieces plus L7 arena chunks) one peer may hold, and
beyond it streams to that peer stall via the existing `px_stall` path. Without
it, un-ACKed L7 chunks of one dead peer would drain the shared egress arena
every other L7 connection allocates from.

## 4.3 What the transport must provide

| Property | Why |
|---|---|
| ordered delivery within one handle | [`DATA.md`](DATA.md) preserves per-connection order end to end |
| reliable delivery, or a visible fault | custody cannot release on a silent loss |
| completion reported to the source | `STREAM_ACK` is what returns the sender's capacity |
| reordering across handles permitted | independent streams must not head-of-line each other |
| a fault surfaces as channel loss | per-stream recovery is not attempted |

The unit that crosses is a staging extent — the same contiguous run the SG-DMA
engine sources from, at most `PX_ARRIVAL_COALESCE_MAX`. Nothing is re-segmented
at the boundary. Control messages are all setup or teardown; the data path
carries only a handle and bytes.

| Message | Direction | Fields |
|---|---|---|
| `STREAM_OPEN` | source → destination | `incarnation`, `source_token`, `src_pod_uid[64]`, `src_service_key[128]`, `dst_pod_uid[64]`, `dst_port`, `src_generation` |
| `STREAM_OPEN_ACK` | destination → source | `incarnation`, `source_token`, `handle`, `status` |
| `STREAM_FIN` | either | `incarnation`, `handle` |
| `STREAM_ACK` | receiver → sender | `incarnation`, `handle`, `seq_first`, `seq_count` |
| `POD_GONE` | source → destination | `incarnation`, `pod_uid[64]` |
| `DATA` | either | `incarnation`, `handle`, `seq`, bytes |

`STREAM_ACK` names a run of consecutive sequences, the encoding
`dmesh_tx_ack_entry` already uses on the reverse ring: one acknowledgement per
released extent rather than one per transport unit.

## 4.4 Handles

The destination DPU allocates the handle, because it owns that namespace. The
space is per peer, so nothing cluster-wide has to allocate it.

```c
/* One per (peer, handle). Self-contained by value, as px_unit is, so it
 * survives the teardown of anything that named it. */
struct dmesh_peer_handle {
    uint32_t incarnation;      /* the channel incarnation that issued it */
    uint32_t wire_handle;      /* owner bit plus allocator-local index */
    int32_t  dst_pod_idx;      /* destination slot; validated against pod generation */
    uint32_t dst_pod_generation;
    uint16_t dst_port;
    uint16_t up_port;          /* the intra-node upstream this stream feeds */
    char     src_pod_uid[64];  /* the key POD_GONE and peer loss sweep on */
    uint8_t  in_use, reserved[3];
    uint32_t staging_bytes;
    uint32_t rx_seq;
    uint8_t  rx_seq_valid, rx_fin, tx_fin;
};
```

Two generations guard it: the channel incarnation rejects a handle from a
previous connection to the same peer, and the pod generation rejects one whose
destination slot has been re-tenanted — the pair `px_batch` already carries, for
the same reason. The channel incarnation is matched on **every** path; the Pod
generation pair is matched on the error, retry and rearm paths, and the success
path checks `pod_data_ready`.

A stream is full-duplex with one handle, because a `dmesh_qp_t` is one
full-duplex byte stream. The owner bit gives the two nodes disjoint wire
namespaces even when each independently allocates index 1, and which node owns
the high half is deterministic from the authenticated node names. Each side owns
its streams on its own worker by its own rule: at the source the Pod's port
decides (`dmesh_worker_for_port`), at the destination the DPU that allocates the
handle encodes its own worker in it. The two are independent because the two
DPUs have independent worker sets.

One connection also carries one cross-node destination, because it holds one
pin. A protocol-aware stream picks a backend per request and may therefore ask
for a second remote endpoint; that ask is refused by name and counted
`peer.second-remote-destination` rather than delivered to the first, since a
destination silently wrong is worse than one refused. Reaching two needs a pin
per destination, which the connection structure does not contain.

| Event | Within a node | Across nodes |
|---|---|---|
| normal close | FIN fan-out (`px_try_fin`), then the upstream is freed | stream FIN; the destination releases the handle |
| source Pod gone | a worker sweeps the upstream table | **the source DPU says so on the channel**; the destination releases that Pod's handles |
| peer gone | the Pod's Comch connection drops | the destination releases every handle whose source is that node |

The middle row is the one new mechanism: within a node a sweep suffices because
the DPU sees the Pod leave; across nodes a destination cannot, so the source has
to say. If the source crashes before sending `POD_GONE`, the handles survive
until the channel faults or idles out — bounded by `DMESH_PEER_STREAMS_MAX`,
which is why that bound is per peer rather than global.

A policy change does not disturb an established handle. Admission changes within
one watch update and nothing is admitted while a rule denies, which is a
statement about new streams; caching the verdict per (connection, destination)
is that rule.

## 4.5 Losing a channel

Losing a channel ends the streams on it. Each DPU tells its own Pods by the path
it already uses when a peer Pod disappears — `px_conn_peer_disconnected`
delivers EOF to the survivor and removes the connection. Streams are not
migrated across a reconnection: the transport's contract is that a connection
can fail and the application retries, so preserving session state buys nothing.

---

# 5. Operations and reference

## 5.1 Naming and identifier spaces

Names on the outside, handles on the inside, each allocated by whoever owns it.

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
public API accepts `"name"` — resolved in the calling Pod's own namespace — or
`"name.namespace"`, the DNS convention applications already use.

A Pod on another node has no `pod_id`, because that space is node-local by
construction, and it does not need one: the DPU ignores what a host names as the
destination of a reply and routes it from conntrack. `dmesh_qp_t.remote_pod` is
advisory and reports `DMESH_POD_REMOTE` for such a peer. A pod id was never
workload identity; it is a transport identifier for one node's slot table.

### Wire representations

| Identifier | Representation |
|---|---|
| pod id | nonnegative `int8_t` on the wire |
| service id | nonnegative `int8_t` on the wire |
| port | `uint16_t` |
| sequence | per-connection `uint16_t` |
| internal routing fields | `int32_t` |

`POD_REGISTER` is a fixed 72-byte message carrying the requested Service name
beside the pod id, where `-1` asks the DPU to assign one. WorkloadGrant v3 is a
1545-byte canonical message whose numeric fields are explicit little-endian
bytes, whose text is NUL-terminated and zero-padded, and whose final 64 bytes
are an Ed25519 signature over every preceding byte. Section 2-1.2 gives its
complete layout. Forward and reverse descriptors use fixed-width fields and
compile-time layout assertions. Host and DPU endpoints are little-endian.

## 5.2 gRPC authority

A gRPC client target is the DPUmesh Service name supplied to each connection
attempt. HTTP/2 `:authority`, TLS SNI and certificate identity remain gRPC
values. `GRPC_ARG_DEFAULT_AUTHORITY` is preserved; otherwise the target is used.

The C++ integration creates a targeted QP for each EventEngine `Connect`. The
server receives QPs through the experimental `PassiveListener` endpoint
injection API. Protobuf messages, stubs and handlers are unchanged.

## 5.3 Control channels

| Input | State supplied |
|---|---|
| DOCA Comch | pod registration, resolution, mappings, readiness, teardown, doorbells |
| signed feeds | node membership; Service targets and ready endpoints; the topology generation |
| root-only files | the registration, feed and controller keyrings; the DPU's own node credential; the admission switch |
| delivered files | Linkerd identity material, authenticated by the certificate issued against it |

Resolution answers follow the topology generation, so nothing reloads out of
band. Dynamic instances of an existing Service join and leave through Comch
registration.

## 5.4 Encryption and observability

Node-local traffic is plaintext because it crosses PCIe and DPU memory and never
a network carrier. Across the boundary, encryption applies to each
worker-local DPU-pair channel rather than to each stream, so negotiation
amortizes across the workloads whose streams land on that worker. The TLS
identity is the node credential. Per-workload separation is carried by
authenticated stream metadata, handle tables and the per-Pod mappings at both
ends, not by a separate TLS session. The node credential and session traffic
keys stay on the DPU.

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

## 5.5 Configuration

Configuration follows the authority boundary. Workload declarations are part of
the PodSpec. The controller and host/DPU services use command-line flags and
root-owned configuration. Cryptographic material is split by purpose, and a
consumer receives only the key role needed to perform its function.

### 5.5.1 DPU runtime

`dpumesh_dpu` runs once on the BlueField Arm OS. PCI functions are command-line
arguments (`-p` device, `-r` representor and `-l` log level). The following
environment is the DPU runtime contract.

Identity and verified state:

| Name | Required/default | Meaning |
|---|---|---|
| `DPUMESH_CLUSTER_ID` | required | cluster identifier bound into every WorkloadGrant |
| `DPUMESH_NODE_NAME` | required | Kubernetes node served by this DPU |
| `DPUMESH_REGISTRATION_KEY_DIR` | required | per-node Ed25519 public keys used to verify WorkloadGrant v3 |
| `DPUMESH_FEED_KEY_DIR` | unset | HMAC keyring for membership and Service-target feeds |
| `DPUMESH_CONTROLLER_KEY_DIR` | unset | Ed25519 public keys for topology generations |
| `DPUMESH_MEMBERSHIP_FILE` | unset | signed node membership document; the deployment uses `/etc/dpumesh/feeds/membership.v1`; unset disables membership revocation input |
| `DPUMESH_TOPOLOGY_FILE` | unset | signed cluster topology document; the deployment uses `/etc/dpumesh/feeds/topology.v1`; unset leaves cluster resolution unavailable |
| `DPUMESH_NODE_KEY_FILE` | unset | DPU-private node credential for peer transport |
| `DPUMESH_NODE_KEY_PUBLIC_FILE` | unset | public half exported for `dpumeshd` to report to the controller |
| `DPUMESH_ADMISSION_FILE` | unset | optional protected-session switch; `drain` closes new protected admission |
| `DPUMESH_CONTROLLER_SCOPE_URL` | unset | HTTP endpoint for mediated workload-scope lookup |
| `DPUMESH_IDENTITY_TRUST_DOMAIN` | `linkerd.cluster.local` | trust domain used for workload identities |

Execution geometry is expressed as `N/K/A` as defined in
[`DATA.md`](DATA.md):

| Name | Default | Bounds and effect |
|---|---|---|
| `DPUMESH_DPA_THREADS` | detected execution units | `N`, 1–32 |
| `DPUMESH_RINGS_PER_POD` | 2 | `K`, 1–16; must equal the host value |
| `DPUMESH_ARM_WORKERS` | 1 | `A`, at most 16 and reduced to a divisor of `K` |
| `DPUMESH_DPA_WAKE_US` | 0 | periodic DPA wake interval; zero disables it |
| `DPUMESH_PERF_STATS` | unset | enables rate-limited transport diagnostics when set to a nonzero value |

`bench/bench.sh` defaults to `N=32`, `K=8`, `A=8`. Setting
`DPUMESH_THROUGHPUT_WORKERS` to 4, 6, 8 or 12 derives `K=A=W` and chooses the
largest multiple of `W` not greater than 32 for `N`. `bench/native_deploy.sh`
uses the validated hardware profile `K=8` and two allocatable slots.

The inter-node carrier is configured per Arm worker. An unset transport leaves
remote destinations unavailable while preserving node-local service.

| Name | Default | Meaning |
|---|---|---|
| `DPUMESH_PEER_TRANSPORT` | unset | `rdma` or `tcp` |
| `DPUMESH_PEER_BIND` | `0.0.0.0` | carrier bind address; RDMA requires a concrete address |
| `DPUMESH_PEER_PORT` | 47900 | base port; worker `w` uses base plus `w` |
| `DPUMESH_PEER_HANDSHAKE_TIMEOUT_MS` | 5000 | maximum unauthenticated inbound lifetime |

Every worker either receives a carrier or remains without one. This avoids a
geometry in which reachability depends on the workload's worker assignment.

The L7 engine is selected by namespace-qualified Service names:

| Name | Default | Meaning |
|---|---|---|
| `DPUMESH_L7_SVC` | empty | Services using full protocol-aware processing |
| `DPUMESH_L7_OPAQUE_SVC` | empty | Services using the opaque L7 path |
| `DPUMESH_L7_LINKERD_WORKER` | 0 | owning Arm worker index or `all` |
| `DPUMESH_L7_SERVICE_TARGETS_FILE` | required when L7 is enabled | signed Service-target feed consumed by the embedded runtime; the receiver installs `/etc/dpumesh/feeds/service-targets.v1` |

A Service cannot appear in both mode lists. With both lists empty, the runtime
does not start Linkerd workers; this is the native hardware deployment profile.
When an L7 list is configured, the embedded proxy also consumes its standard
`LINKERD2_PROXY_*` control endpoint, identity, token and trust-anchor settings.

### 5.5.2 Workload PodSpec

A meshed workload receives no Kubernetes credential and no device privilege.
Its image contains the DPUmesh adapter. Exactly one regular container requests
and limits one `dpumesh.io/channel` extended resource; kubelet then mounts only
the assigned slot socket at `/run/dpumesh/channel.sock`.

```yaml
spec:
  automountServiceAccountToken: false
  securityContext:
    runAsNonRoot: true
    runAsUser: 65532
    runAsGroup: 65532
    seccompProfile: {type: RuntimeDefault}
  containers:
  - name: app
    image: registry.example/app-with-dpumesh-adapter:tag
    env:
    - {name: DPUMESH_SERVICE, value: echo}
    - {name: DPUMESH_RINGS_PER_POD, value: "8"}
    securityContext:
      allowPrivilegeEscalation: false
      readOnlyRootFilesystem: true
      capabilities: {drop: ["ALL"]}
    resources:
      requests: {dpumesh.io/channel: 1}
      limits: {dpumesh.io/channel: 1}
```

`DPUMESH_SERVICE` is the unqualified Kubernetes Service served by this Pod; it
is omitted for a pure client. The controller resolves it in the Pod namespace
and verifies that the Pod is a ready endpoint of that Service.
`DPUMESH_RINGS_PER_POD` must match the node runtime. The POSIX adapter additionally
uses `DPUMESH_PORT` for the intercepted listening port and
`DMESH_PRELOAD_DEBUG` for diagnostics. `LD_PRELOAD` is supplied in the image or
PodSpec when the POSIX adapter is used.

Completion draining is process-local:

| Name | Default | Meaning |
|---|---|---|
| `DPUMESH_DRAIN_NAP_US` | 10 | minimum polling sleep; zero uses doorbells only |
| `DPUMESH_DRAIN_NAP_CAP_US` | 100 | backoff cap before publishing `arm_epoch` and blocking |
| `DPUMESH_DRAIN_SHARDS` | online CPU count | shard ceiling, also bounded by registered EQs and allowed CPUs |

Application-controlled values select transport behavior but confer no identity.
Pod UID, container ID, node, slot and Service authority come from kernel cgroup
evidence plus the controller's latest successfully polled Kubernetes snapshot.

### 5.5.3 Controller

`controller/dpumesh_controller.py` runs as one non-root Deployment. Its
ServiceAccount has only `get` and `list` on Pods, Services and EndpointSlices.
The manifest mounts three disjoint signing-key classes, node configuration and
TLS material read-only.

| Flag | Default | Meaning |
|---|---|---|
| `--key-dir` | required | Ed25519 topology signing directory |
| `--registration-key-dir` | required | per-node Ed25519 WorkloadGrant signing directories |
| `--feed-key-dir` | required | HMAC membership and Service-target signing directory |
| `--nodes-file` | required | node name, RDMA address, grant key id/public key and DPU public key records |
| `--output` | `/run/dpumesh/topology.v1` | current topology output |
| `--interval` | 5 s | topology publication interval, range 1–300 s |
| `--protected` | empty, repeatable | `namespace/name` Services using strict L7 handling |
| `--listen` | `0.0.0.0` | node API bind address |
| `--listen-port` | 8080 | node API port; the supplied manifest selects 8443 |
| `--tls-cert`, `--tls-key`, `--client-ca` | required | TLS server identity and node client CA |
| `--resource-name` | `dpumesh.io/channel` | resource required on an authorized container |
| `--cluster-id` | required | WorkloadGrant cluster binding; lowercase DNS subdomain, at most 63 bytes |
| `--grant-ttl` | 60 s | grant lifetime, range 1–300 s |
| `--api-server` | in-cluster API | Kubernetes API endpoint |
| `--api-token-file`, `--api-ca-file` | projected ServiceAccount files | Kubernetes API credential and CA |

The API uses TLS 1.3. Every data-bearing request requires a client certificate
with exactly one URI SAN `spiffe://dpumesh.io/node/<node>` matching a configured
node. `GET /healthz` is available to the Kubernetes probe without a client
certificate. Section 1.1 defines all routes and response semantics.

### 5.5.4 Host runtime

`node/dpumeshd.py` is a root system service. It combines Device Plugin state,
slot listeners, kernel evidence collection, node mTLS, feed delivery, worker
cgroups and broker supervision so allocation and identity use one generation
counter and one lock domain.

| Flag | Default | Meaning |
|---|---|---|
| `--node-name` | required | served Kubernetes node |
| `--slots` | 8 | advertised `dpumesh.io/channel` devices, 1–127 |
| `--runtime-dir` | `/run/dpumesh` | runtime state root |
| `--slot-dir` | `/run/dpumesh/slots` | per-slot listeners |
| `--manager-socket` | `/run/dpumesh/manager.sock` | root-private manager endpoint |
| `--device-plugin-dir` | `/var/lib/kubelet/device-plugins` | kubelet plugin directory |
| `--cgroup-root` | required | systemd-delegated service cgroup |
| `--controller-url` | required | HTTPS controller origin |
| `--controller-ca`, `--controller-cert`, `--controller-key` | required | node mTLS material |
| `--controller-timeout` | 5 s | controller request deadline |
| `--broker-bin` | `/opt/dpumesh/bin/dmesh_broker` | supervised broker executable |
| `--dpu-feed-host` | required | paired DPU management address |
| `--dpu-feed-port` | 4788 | feed receiver port |
| `--node-rdma-addr` | required | canonical IPv4 and port published in topology, as `IPv4:PORT` |
| `--feed-interval` | 2 s | healthy delivery period |
| `--feed-timeout` | 10 s | DPU delivery deadline |
| `--scope-listen-address` | `192.168.100.1` | DPU-facing scope tunnel bind address |
| `--scope-listen-port` | 28089 | scope tunnel port; zero disables it; at most 16 requests are relayed concurrently |
| `--pci-addr` | empty | host DOCA PCI function passed only to brokers |
| `--rings-per-pod` | 8 | ring count, range 1–16 |
| `--launch-timeout`, `--request-timeout` | 10 s | broker startup and slot request deadlines |
| `--worker-cpu-max` | `100000 100000` | cgroup v2 `cpu.max`; a quota alone uses period 100000 |
| `--worker-memory-high` | 768 MiB | cgroup `memory.high` |
| `--worker-memory-max` | 1 GiB | cgroup `memory.max` |
| `--worker-pids-max` | 64 | cgroup `pids.max` |

The packaged unit reads `/etc/dpumesh/dpumeshd.env` and maps its fields to the
flags above. This is the complete service configuration surface:

| Environment field | Runtime option |
|---|---|
| `DPUMESH_NODE_NAME` | `--node-name` |
| `DPUMESH_SLOTS` | `--slots` |
| `DPUMESH_CONTROLLER_URL` | `--controller-url` |
| `DPUMESH_DPU_FEED_HOST`, `DPUMESH_DPU_FEED_PORT` | `--dpu-feed-host`, `--dpu-feed-port` |
| `DPUMESH_NODE_RDMA_ADDR` | `--node-rdma-addr` |
| `DPUMESH_PCI_ADDR` | `--pci-addr`; forwarded only to the broker |
| `DPUMESH_RINGS_PER_POD` | `--rings-per-pod`; forwarded to the broker |
| `DPUMESH_WORKER_CPU_MAX` | `--worker-cpu-max` |
| `DPUMESH_WORKER_MEMORY_HIGH`, `DPUMESH_WORKER_MEMORY_MAX` | `--worker-memory-high`, `--worker-memory-max` |
| `DPUMESH_WORKER_PIDS_MAX` | `--worker-pids-max` |

The supplied systemd unit delegates `cpu`, `memory` and `pids`, bounds the
manager to reserved CPUs and 2.5 GiB, grants only the capabilities required for
namespace/cgroup/device setup, and makes package/config trees read-only. Worker
leaves live at
`/sys/fs/cgroup/system.slice/dpumeshd.service/workers/pod<uid>.g<generation>`.
They are host infrastructure and are accounted through kubelet's system
reservation rather than a workload Pod cgroup.

### 5.5.5 DPU feed receiver

`dpu/feed_receiver.py` is an unprivileged system service on the Arm OS. It
listens on the host–DPU management link, bounds payloads before allocation,
checks SHA-256 transfer integrity and atomically renames complete files into
`/etc/dpumesh/feeds`. It serves one connection at a time, so two host deliveries
cannot race an installation. The signed consumers supply authenticity. It also
returns the DPU node public key for controller registration and never exposes
the private key. The service account owns only the feed directory;
`/etc/dpumesh` and every verification-key directory remain root-owned.

| Flag | Default | Meaning |
|---|---|---|
| `--bind` | `192.168.100.2` | paired-host management address |
| `--port` | 4788 | receiver port |
| `--timeout` | 30 s | accepted connection deadline |
| `--node-public-key` | `/etc/dpumesh/node-static.pub` | public key returned by `DMESHNODE1` |

Feed limits are 256 KiB for membership, 16 MiB for topology and 1 MiB for
Service targets. A reconnect sends a document only when the installed file's
digest differs.

### 5.5.6 Files and ownership

| Path | Location and owner | Purpose |
|---|---|---|
| `/etc/dpumesh/controller.keys/` | controller Secret; private | topology signing source |
| `/etc/dpumesh/registration.keys/<node>/` | controller Secret; private | WorkloadGrant signing source |
| `/etc/dpumesh/feed.keys/` | controller Secret; private | node-scoped feed signing source |
| `/etc/dpumesh/controller.pub.keys/` | DPU, root-readable | topology verification |
| `/etc/dpumesh/registration.keys/` | DPU, root-readable | WorkloadGrant verification |
| `/etc/dpumesh/feed.keys/` | DPU, root-readable | membership and Service-target verification |
| `/etc/dpumesh/node-static.key` | DPU only | peer-channel private identity |
| `/etc/dpumesh/node-static.pub` | DPU | public identity returned to `dpumeshd` |
| `/etc/dpumesh/feeds/{membership.v1,topology.v1,service-targets.v1}` | DPU feed user writes; DPU runtime reads | atomically installed control documents |
| `/etc/dpumesh/tls/{controller-ca.crt,node.crt,node.key}` | host root | `dpumeshd` mTLS identity |
| `/etc/dpumesh/dpumeshd.env` | host root | systemd runtime configuration |
| `/run/dpumesh/slots/channel-NNN.sock` | host root; one is bind-mounted read-only into a Pod | allocation endpoint |
| `/run/dpumesh/manager.sock` | host root | internal manager endpoint |
| `/opt/dpumesh/bin/dmesh_broker` | host immutable package | broker executable |

Controller, grant and feed keys are different material. The DPU receives public
keys for the two Ed25519 roles and the symmetric key only for its node-scoped
feed verification. `dpumeshd` holds no signing key and no Kubernetes bearer
token.

## 5.6 Deployment, operation and validation

The one-node hardware workflow is executable documentation:

```bash
./bench/native_deploy.sh all       # build images/runtime, deploy and smoke-test
./bench/native_deploy.sh build     # rebuild images and the DPU runtime
./bench/native_deploy.sh deploy    # install controller, host/DPU services and workloads
./bench/native_deploy.sh smoke     # PING plus a real DPU data-path request
./bench/native_deploy.sh status    # controller/workload/systemd state
```

`all` builds the native libraries and three images, imports images into
containerd, provisions the three key roles and node TLS, installs the DPU feed
receiver and host service, starts the DPU process, applies
`bench/k8s/native-hw.yaml`, waits for rollouts and requires an `OK` result with
`fail=0` and `drops=0`.

Controller provisioning is also independently available:

```bash
./bench/dpumesh_controller.sh prepare
IMG_CONTROLLER=bench/dpumesh-controller:native ./bench/dpumesh_controller.sh deploy
./bench/dpumesh_controller.sh node-record
./bench/dpumesh_controller.sh nodes-config
./bench/dpumesh_controller.sh topology-show
./bench/dpumesh_controller.sh receiver-status
./bench/dpumesh_controller.sh status
```

DPU build and measurement commands are:

```bash
./bench/bench.sh geometry
./bench/bench.sh build
./bench/bench.sh restart
./bench/bench.sh ping
./bench/bench.sh point REQ REPLY CONC DUR WARMUP THREADS [RECONNECT]
./bench/bench.sh latency
./bench/bench.sh bandwidth
./bench/bench.sh rate
./bench/bench.sh all
./bench/bench.sh dpulog [LINES]
./bench/bench.sh dpubanner
./bench/bench.sh dpucpu
```

The `latency`, `bandwidth` and `rate` commands write CSV files under
`OUT`, defaulting to `/tmp/dpumesh-bench`. `ci/health-check.sh` records the DPU
geometry, deployed workload set and client CPU affinity, then runs a bounded
native data-path probe. `native_deploy.sh all` performs the controller, host
service, Device Plugin, DPU, workload rollout and smoke checks.

Operational state is fail-closed for new allocations. `dpumeshd` advertises
slots Healthy only while controller fetch, DPU delivery and node-key report all
succeed. Existing brokers keep their held transport state, while new slot use
is unavailable. Deleting a workload closes its slot connection; the supervised
broker unregisters at the DPU, exits, and only then releases its worker cgroup
and slot generation for reuse. A `dpumeshd` stop marks devices unhealthy,
stops listeners and terminates its broker children through the service cgroup.

The optional admission file accepts `drain` to refuse new protected sessions;
any other readable content opens admission, and an unreadable file is treated
as open. Control outcomes are exported through
`dmesh_control_events_total{kind,reason}` and refused sessions through
`dmesh_sessions_declined_total{reason}`.

## 5.7 Fixed limits

Values marked security-bound affect authorization, replay or isolation; the
remaining values bound resource cost.

| Name | Value | Meaning |
|---|---:|---|
| `GENERATION_INTERVAL` | 5 s | controller publication cadence |
| `CONTROLLER_REQUEST_MAX` | 32 | concurrent controller request handlers |
| `CONTROLLER_REQUEST_TIMEOUT` | 10 s | TLS handshake and HTTP connection deadline |
| `DMESH_ASSERT_MAX_LIFETIME_SEC` | 300 s | maximum WorkloadGrant lifetime |
| `DMESH_ASSERT_CLOCK_SKEW_SEC` | 30 s | tolerated controller-to-DPU future skew |
| `DMESH_REGISTRATION_REPLAY_SLOTS` | 4096 | consumed assertion identifiers retained |
| `MAX_PODS` | 127 | DPU Pod table and Device Plugin slot ceiling |
| `DMESH_CHANNEL_IDLE_NS` | 60 s | idle peer-channel eviction threshold |
| `DMESH_CHANNEL_MAX` | 256 | peer channels held by one DPU |
| `DMESH_PEER_STREAMS_MAX` | 4096 | concurrent streams admitted from one peer |
| `DMESH_PEER_STAGING_MAX` | 16 MiB | destination staging bytes per peer |
| `DMESH_PEER_OPEN_RATE` | 1000/s | stream-open token rate per peer |
| `DMESH_PEER_TX_INFLIGHT_MAX` | 16 MiB | unacknowledged source bytes per peer |
| `DMESH_PEER_TX_SLOTS` | 8192 | unacknowledged extents per source peer |
| `DMESH_GEN_POD_MAX` | 65,536 | Pods in one topology generation |
| `DMESH_GEN_NODE_MAX` | 1,024 | nodes in one topology generation |
| `DMESH_GEN_SERVICE_MAX` | 4,096 | Services in one topology generation |
| `DMESH_GEN_ENDPOINT_MAX` | 65,536 | endpoints in one topology generation |
| `DMESH_TOPOLOGY_MAX_BYTES` | 16 MiB | topology document bound |
| `DMESH_CONTROLLER_KEYS_MAX` | 4 | topology verification keys during rotation |
| `DMESH_REGISTRATION_MAX_KEYS` | 4 | WorkloadGrant verification keys during rotation |
| queue pairs per node pair | `A` | one per destination Arm worker |
| `DMESH_STREAM_ACK_BATCH` | 64 | delivery acknowledgements staged per frame |

Documents and wire structures are rejected when they exceed these limits; they
are never silently truncated.

## 5.8 Architectural invariants

- Kubernetes contains the controller and application workloads. `dpumeshd` runs
  on the host; the DPU runtime and feed receiver run on BlueField. All three are
  system services outside Kubernetes.
- Workloads hold no Kubernetes token, DPU device node, host path, PCI address,
  signing key or privileged capability. Their complete DPUmesh capability is one
  allocated Unix socket.
- `dpumeshd` derives Pod UID and container ID from `SO_PEERCRED`, cgroup v2 and a
  PID-starttime fence. The controller binds that evidence to its latest Pod,
  container, resource, node, Service and EndpointSlice state.
- A WorkloadGrant is bound to cluster, node, Pod, container, Service, slot,
  generation, daemon incarnation, DPU nonce, issue/expiry time and assertion id.
- One broker is a direct child of `dpumeshd`, runs in a bounded worker cgroup and
  private namespaces, pivots to an empty root, uses uid/gid 65532, has no
  capabilities, sets `no_new_privs` and applies an exec-deny seccomp filter.
- Service names and Pod IDs are transport identifiers only after authorization;
  namespace-qualified Kubernetes objects remain the control-plane authority.
- Peer TLS authenticates a DPU/node credential. Exact workload identity travels
  inside that authenticated channel as topology-bound stream metadata.
- The destination authorizes before a Pod sees data. Cross-node source custody
  ends only after the destination publishes completion and returns `STREAM_ACK`.
- A generation, feed, grant, peer frame or IPC message that is malformed,
  unauthenticated, replayed, stale or over bound is refused as a unit.
- L7 processing is active only for configured Service lists. Empty lists select
  the complete L4 native path without requiring Linkerd control endpoints.

## 5.9 Scope boundaries

Node-local identifiers are intentionally not cluster identities. `pod_id` and
interned Service IDs occupy a signed one-byte wire space local to one DPU. A
remote Pod is addressed by its topology Pod UID and node, then receives a local
handle at the destination. A connection carries one remote destination; a
second destination on the same stream is refused.

Each Arm worker owns its peer table. When a carrier is enabled,
`px_peer_configure` binds one instance to every worker and `px_peer_accept`
passes authenticated lower connections into that worker's upper state.
`src_generation` is carried in `STREAM_OPEN` as an observation; topology
consumers advance only by verifying their locally delivered generation.

Destination-side HTTP authorization is a connection verdict over the union of
route authorizations. The destination DPU does not run a second HTTP parser.
The source DPU owns outbound L7 parsing, while the destination records inbound
connection-level bytes, streams, verdicts and identities.

One DPU, `dpumeshd` and its broker population form one node trust domain. A
compromised node credential can speak for workloads that the signed generation
places on that node, but not for workloads assigned to another node. Isolation
between local workloads is enforced by controller grants, per-slot generations,
DPU mapping tables, broker processes and worker cgroups.
