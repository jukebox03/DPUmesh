# DPUmesh Naming, Identity, and Control Plane

DPUmesh separates application names from data-plane addresses. Applications use
configured logical Service names; Kubernetes Service names are one common
deployment convention, not a transport requirement. A process-local registry
translates names and optional socket destinations to compact service ids, while
the DPU assigns ephemeral backend slots at registration.

Sections 1-6 and the DPUmesh half of section 8 are implemented; the rest of
7-12 is planned design, and section 13 is the item-by-item split.

## 1. One registry, two facades

```text
ClusterIP:port    service-name    service-id
10.96.23.17:9091  echo-dpumesh    13
```

The native API resolves `service-name` in `dmesh_create_qp()`. The preload facade
resolves the IPv4 `ClusterIP:port` passed to `connect()` and falls back to kernel
TCP when no row matches. Both paths use `src/dmesh_resolve.c`; neither public
surface accepts an integer service id.

The registry path is `$DPUMESH_CONFIG`, defaulting to
`/etc/dpumesh/registry`. Blank lines, comments, and malformed rows are ignored.
`0.0.0.0:0` is a name-only entry usable by the native facade. The table is
loaded once under a mutex and then read without locks. It does not reload; the
complete file must exist before first resolution.

## 2. Local identity

`$DPUMESH_SERVICE` names the Service implemented by the current process. Unset
means client-only. A matching name is translated to the internal id carried in
`POD_REGISTER`. A nonempty unknown name is logged and leaves the process
client-only rather than inventing an identity.

The host never chooses its pod id. The DPU assigns a live slot, returns
`POD_ASSIGNED`, and associates a monotonically increasing DMA generation with
that slot. Replayed registration on the same Comch connection returns the same
assignment. The id is observable through `dmesh_pod_id()` only after the later
readiness barrier completes.

`$DPUMESH_PORT` is used only by the preload listener facade. Native servers and
the gRPC PassiveListener receive inbound QPs as `DMESH_EVENT_CONN_REQ` events;
they do not bind a numeric transport port through the native API.

## 3. Routing meaning

A QP names a service, not a backend. In default L4 passthrough, its first data
causes the DPU to select one ready backend and the resulting byte stream remains
pinned. An optional per-service codec can delimit frames and select an upstream
for each request frame. Backend identities and upstream ports remain internal;
the public QP continues to expose one response byte stream without a stream id.

The DPU derives a service's current backend set from live registered pod slots.
A pod participates only after `POD_INIT_RESULT(READY, L)` and is removed from
routing as soon as unregister or disconnect clears its live state. VMs,
bare-metal processes, and pods can join or leave a configured Service without
rewriting the registry. Backend loss terminates pinned L4 streams; new
connections select from the current live set.

Selection within the live set is per-service round robin over a shared cursor.
A framed service recomputes the set at each frame boundary; an L4 connection
resolves once and holds its pin.

## 4. gRPC authority is separate

The C++ client target is a configured Service name passed to each QP creation
attempt. It is not an IP address or gRPC resolver URI.

HTTP/2 `:authority`, TLS SNI, and certificate identity remain
application-layer values. An explicit `GRPC_ARG_DEFAULT_AUTHORITY` is preserved;
an absent value defaults to the target.

The per-channel EventEngine creates a targeted QP for each `Connect`. The server
uses the experimental `PassiveListener` endpoint-injection API.

The repository implements endpoint injection, not a global
`dpumesh:///service` resolver. Generated protobuf code, stubs, handlers, and RPC
methods are unchanged; only client/server bootstrap chooses the DPUmesh runtime.

## 5. Identifier spaces

Every routable entity is named by a small integer whose width is fixed by the
host-to-DPU wire format.

```text
pod id        nonnegative int8    POD_ID_SPACE ids over MAX_PODS live slots
service id    nonnegative int8    same width and numbering as pod ids
port          uint16              DPU-synthesized upstream ports use the upper half
sequence      uint16              per connection
```

`dmesh_rev_done_entry` packs `src_pod_id`, `src_service`, and `dst_service` as
`int8_t` inside a 16-byte reverse-ring entry whose layout is fixed by static
assertion on both sides. The ARM egress unit repeats those widths and adds an
`int8_t` destination slot index. `POD_REGISTER` is a fixed 12-byte struct that
the DPU validates by exact length.

Routing state inside one ARM worker is wider: `dmesh_l7_ctx.hosts`,
`dmesh_l7_decision.cluster` and `.host`, and a connection's pinned backend are
`int32_t`. A wider identifier therefore survives inside a worker's routing
decision but cannot cross the reverse ring, the egress unit, or a control
message.

This asymmetry governs the planned sections. Cluster capacity, endpoint
capacity, and the representation of remote peers are all bounded by it, and
widening it is a wire-ABI change requiring lockstep host and DPU deployment.

## 6. Control-plane boundary today

The repository includes a static registry and deployment tooling, not an
orchestrator controller. It does not implement admission identity, EndpointSlice
watching, or registry reload. Registry consistency and `$DPUMESH_SERVICE`
injection are deployment invariants.

Workload identity is delivered: `POD_IDENTITY` carries a workload name on the
registration connection before `POD_REGISTER`, and the DPU binds it to the slot
that connection owns, clearing it when the slot takes a new tenant. The identity
is granted by the DPU rather than claimed by the pod, and reaches the L7 layer
on every connection it opens. Dynamic instances of a
configured Service are supported; new Service names require registry updates.
DPUmesh readiness is established by its initialization barrier.

Two control channels exist and neither carries policy. The Comch channel
attaches and detaches rings — registration, mmap export, slot assignment,
readiness, unregistration, quiesce, and the reverse-ring doorbell. The static
registry translates names to service ids.

A backend is reachable only over the PCIe Comch attachment that registered it.
The DPU opens no sockets and holds no other node's memory keys, so a service's
backend set is confined to one host. DPUmesh is a node-local mesh.

## 7. Configuration plane (planned)

A control plane delivers configuration by push, but the registry's lock-free
read depends on the table never changing after load. Editing it in place moves
that cost onto the data path.

Configuration is therefore versioned rather than edited. A configuration thread
builds a complete new snapshot and publishes it with one release store; a worker
loads the pointer once at the start of an iteration and reads only that snapshot
for the whole iteration. The discipline already exists in the transport: a pod
slot carries a monotonically increasing DMA generation, and deferred work naming
a stale generation is refused rather than repaired.

```text
struct dmesh_config_snapshot {
    uint64_t generation;
    endpoints[] : cluster    -> local endpoint ids, ready bit
    links[]     : peer node  -> address, lane queue pairs, lane count
    identity[]  : slot       -> SPIFFE ID, bound to the slot's DMA generation
};
```

Load-balancing policy, remote endpoints and authorization are not snapshot
contents. Section 8 obtains them per connection from the L7 layer, so nothing
that a control plane pushes needs a versioned table here.

The configuration thread publishes the pointer itself. The main thread is not
involved: a single writer already serializes swaps, and routing the swap through
the main thread would place configuration work on the host doorbell path that
section 11 exists to keep clear.

Reclamation is by epoch. Each worker publishes the generation it observed, and a
snapshot is freed once every worker has observed a later one. Worker iterations
are bounded, so the wait is bounded.

The static registry remains the configuration source when no control plane is
configured, and continues not to reload. Section 1's contract holds unchanged
for that mode; a configured control plane replaces the table by generation swap
rather than by reload.

A precomputed per-cluster endpoint array also replaces the live-slot scan that a
framed service performs at each frame boundary. The default L4 path resolves
once per connection and is unaffected, so no throughput claim attaches to this
change.

## 8. Control-plane consultation

DPUmesh implements no control-plane client. The L7 layer is one:
linkerd2-proxy runs on the DPU ARM and already holds sessions with the mesh
control plane for discovery, authorization and certificates. The DPU ARM has its
own address and reaches those services directly, so the host is not on that path.
Relative to a sidecar deployment only the endpoint of that connection moves.

DPUmesh consults it once per connection instead. Given a caller identity and a
target service, the L7 layer answers allow-or-deny and an endpoint; at close,
DPUmesh reports the connection's byte counts and duration so the balancer's load
view stays accurate for connections whose payload never traversed it. Per-request
cost is zero and no second subscription exists to keep consistent.

DPUmesh's half of this is implemented: a service in `DPUMESH_L7_DECISION_SVC`
is resolved once at establishment, the answer pins the stream, a denial poisons
it, and the close reports load. The interface is `l7_resolve` / `l7_report` in
`linkerd/CONTRACT.md`; what remains is an L7 layer that answers from cluster
state rather than from local candidates.

The control plane's protocol is therefore the L7 layer's concern. An xDS client,
CDS and EDS are not implemented here.

Cluster ids are drawn from the identifier space of section 5. An answer naming a
service outside that space is logged and left unroutable, matching the discipline
section 2 applies to an unknown `$DPUMESH_SERVICE`. A remote endpoint in an
answer carries its node, that node's DPU address, its slot on that node, and its
locality.

## 9. Node boundary (planned)

### 9.1 Ownership invariant

> The host memory of node N is touched only by the DPU of node N.

One-sided delivery, in which a sending DPU writes directly into a remote host's
landing stripe, breaks it and turns three local barriers into distributed
problems. Teardown is a two-party handshake where the host retains its exports
until `POD_QUIESCED` and a remote sender is not a participant, so a write may
already be in flight against reclaimed memory. Stripe credit is `K/L` counters
that the owning DPU reads with one SG-DMA and sums, so a remote writer would
have to poll another node's credit state. And the sender would have to hold the
receiving host's memory keys.

Delivery is therefore two-sided. A sender posts to the receiving DPU's staging,
and the receiving worker treats the arrival exactly as a local DPA completion and
runs the unchanged local path from there. The cost is one remote ARM hop,
reported as its own figure rather than folded into aggregate ARM cost.

### 9.2 Identifiers stay node-local

Section 5 forbids widening `src_pod_id`, so no node names another node's slots
on the wire.

The link protocol carries its own (link, lane, stream) tuple. Each DPU maps a
remote peer to a slot in its own identifier space — a remote-backed slot, which
has no imported host mapping and whose egress target is a link lane instead of an
SG-DMA context. The reverse ring, the egress unit, the registration message, and
the host library are all unchanged, because every identifier they carry is local.

This extends a rule the transport already applies. The host never chooses its pod
id, and DPU-synthesized upstream ports already occupy a reserved range of the
port space; a remote peer is named by the same authority under the same rule.

The capacity cost is explicit: remote-backed slots consume the same identifier
space as local pods, so a node's local pods plus its distinct remote peers are
bounded together.

### 9.3 Data path

```text
   sending node                            receiving node

   host app TX                             host PE, REV_DONE
        |                                        ^
        v                                        |
   forward ring (source port % K)          pod RX landing stripe
        |                                        ^
        v                                        |
   DPA EU                                  SG-DMA (unchanged)
        |                                        ^
        v                                        |
   ARM worker (port % A) ----- lane ---->  ARM worker
   remote decision, lane post   <-- credit and ack --
```

### 9.4 Worker scope

On the receiving side only the first step of a worker iteration changes:
consuming DPA completions becomes consuming DPA and link completions. Bytes
arriving from a link and bytes arriving from the local DPA are indistinguishable
below that point.

The sending side is larger. Steps three through six of the iteration are
slot-bound: an egress lane is keyed by destination slot and landing region,
submission uses that slot's SG-DMA context, and retirement validates the slot's
generation. For a remote destination, submission posts to a link lane instead,
and lane retirement and acknowledgement wait on the remote node's confirmation.

### 9.5 Credit

Three tiers, of which one is new.

| Tier | Span | Released by | State |
|---|---|---|---|
| 1 | sender host TX slot | `TX_ACK`, emitted after the destination landing DMA completes | implemented; timing changes |
| 2 | link staging on the receiving DPU | receiving worker, after its landing DMA completes | new |
| 3 | receiving host landing stripe | existing sharded credit counters | implemented; unchanged |

Tier 1 is not new machinery, but its meaning changes. Today a sender's TX slot is
freed once the destination's landing DMA completes on the same node; for a remote
destination that acknowledgement crosses the link, so the slot is held for a
network round trip as well. The application-visible bound is the QP TX window,
which already exceeds the link's bandwidth-delay product by a wide margin, so the
window does not need to grow. The consequence that does matter is failure: a lost
remote node stalls tier 1 indefinitely.

### 9.6 Failure detection

Local backend loss is exact and immediate, because the Comch disconnect that
removes the slot is itself the liveness signal. Remote loss is detected by link
completion errors or by timeout.

Remote loss terminates the pinned streams that named the lost node, as local
backend loss does, and additionally force-releases their outstanding tier-1
custody. Without that release the sending application blocks in `dmesh_alloc`
indefinitely, because the acknowledgement that frees its TX slots can no longer
arrive. The existing release path covers a vanished sender, not a vanished
receiver.

### 9.7 Link encryption

Inline IPsec encrypts the DPU-to-DPU link. Security associations are per node
pair rather than per connection, so negotiation is bounded by the number of node
pairs and consumes no ARM cycles. Application TLS termination stays out of scope;
certificate identity remains an application-layer value (section 4).

### 9.8 Naming

The public API's QP and a link's RC queue pairs are the same word for different
objects. Internally, link names a node pair and lane names a link's per-worker
queue pair; QP remains the public API term only.

## 10. Identity, policy, and selection (planned)

The split follows section 8. DPUmesh asserts *who the caller is* and enforces the
answer; the L7 layer decides *what is allowed and where it goes*. Local
membership stays with self-registration because only it knows which pods hold a
DMA attachment on this node.

### 10.1 Workload identity

Delivery and slot binding are section 6's, and already carry a name. What
remains is its content: a production identity is a SPIFFE ID of the form
`spiffe://cluster.local/ns/<ns>/sa/<sa>`, and an unrecognised one follows
section 2 — it is logged, and no identity is invented.

### 10.2 Trust boundary

Remote peer identity travels in link metadata, and the link's IPsec SA
authenticates the peer DPU. It does not authenticate the peer workload: a node
trusts its peer DPU to assert its own local workloads' identities correctly. The
trust boundary is the node, not the workload. That is weaker than per-workload
mTLS and is stated rather than implied.

### 10.3 Authorization

Authorization is evaluated once per connection, at the point where a connection's
service and mode are resolved — the one-time resolution that already happens on a
connection's first data. DPUmesh supplies the caller identity and target service
and enforces the answer; the L7 layer decides. A denial terminates the stream
through the existing terminal-stream path. Per-request cost is zero.

When the L7 layer is unreachable the connection falls open to the data plane's
own selection and runs without authorization. The fallback is counted.

### 10.4 Health checking

| | Local backend | Remote backend |
|---|---|---|
| membership | self-registration | L7 layer |
| liveness | Comch disconnect, exact and immediate | L7 layer |
| failure accrual | L7 layer, from reported connection outcomes | L7 layer |

Local liveness is exact and immediate and needs no probe: a Comch disconnect is
the fact itself. Everything else — readiness, failure accrual, ejection — comes
from the L7 layer, which already derives it from cluster state and from the
outcomes DPUmesh reports at connection close. DPUmesh runs no probes of its own,
and answering Kubernetes readiness probes on a workload's behalf stays out of
scope.

### 10.5 Membership

```text
routable(service) = live registered local pods  U  endpoints the L7 layer returns
```

The two sources do not compete: only self-registration knows which local pods
hold a DMA attachment, only the L7 layer knows cluster-wide readiness. Where they
overlap — a locally registered pod the L7 layer has not yet marked ready — the
answer gates new selection while transport liveness governs existing pins. A
readiness gate therefore delays new traffic without importing propagation delay
into established streams.

### 10.6 Selection

Round robin over the current live set treats a local backend, one DMA away, and a
remote backend, a network and a remote ARM hop away, as equivalent. Average
latency then degrades as nodes are added, so locality awareness becomes required
rather than optional once the mesh spans nodes.

Selection is the L7 layer's answer, and locality is one of the inputs it already
weighs. DPUmesh contributes what only it knows: which endpoints are local, and
therefore one DMA away. Local endpoints are preferred; remote endpoints are
selected when the local set is saturated, ejected, or empty.

### 10.7 Telemetry

A worker only increments its own counters, so there is nothing shared to contend
on. Counters cover requests, bytes, latency distribution, per-backend failures,
and the local-to-remote ratio. The configuration thread reads all workers'
counters periodically and exports them. No host CPU is consumed.

The same counters are reported per connection to the L7 layer at close, which is
how a connection whose payload never traversed the proxy still appears in mesh
telemetry and in the balancer's load view.

## 11. Thread placement (planned)

The main ARM thread emits host doorbells. The host increments `arm_epoch` before
blocking on its reverse rings, a worker records the pending epoch and wakes the
main thread, and the main thread's drain sends the Comch doorbell. The main
thread is therefore on the host wake-up path, and latency at low load is
dominated by it.

Configuration work does not go there. A snapshot rebuild would delay doorbells
for its whole duration regardless of how little CPU it averages, and the delay is
worst exactly where the transport's latency figure is most exposed. A separate
configuration thread exists to hold that work. Its scope is what section 7 leaves
in the snapshot — links, local membership, identity — together with counter
export; discovery, policy and probing belong to the L7 layer.

| Thread | Now | Planned |
|---|---|---|
| host app x M | alloc, post_send, flush, EQ events | unchanged |
| host PE progress x 1 | reverse drain, `REV_DONE` / `TX_ACK`, epoch increment | unchanged |
| ARM main x 1 | control messages, teardown, doorbells | unchanged |
| ARM data worker x A | DPA completions, routing, SG-DMA, reverse publication | adds link completions, per-connection consultation and enforcement, local counters, the L7 layer's single-step call |
| ARM config x 1 | none | new: snapshot build and publish, link establishment, counter export |
| DPA EU x N | forwarding, completion metadata | unchanged |

No data-path thread is added, and A and N do not change.

The configuration thread is unpinned, but leaving it unpinned is an action rather
than an omission. The main thread pins itself to one CPU before creating other
threads, and a thread created afterwards inherits that single-CPU mask; each data
worker escapes it by pinning itself. The configuration thread must explicitly
restore the process's allowed CPU set, or it runs on the main thread's core —
precisely the contention this section avoids. Its CPU is reported on its own line
and never folded into the ARM total.

## 12. Open decisions (planned)

- **Lane granularity and worker-count uniformity.** A lane per (node pair,
  worker) preserves ordering, because a connection is already bound to one worker
  by port. Computing the remote owner then requires either equal worker counts on
  both nodes or a per-node divisor carried in `links[]`. The first is simpler; the
  second admits heterogeneous nodes and needs rebalancing semantics.
- **Staging credit sizing**, and whether staging is per (node pair, worker) or a
  shared pool per node pair. Per-worker staging has no contention and costs the
  worker count in memory.
- **Snapshot rebuild bound.** A full rebuild is linear in total endpoints even
  when the update is incremental.
- **Cluster and endpoint capacity.** Section 5's identifier space holds fewer
  entries than a large mesh declares, and remote-backed slots consume the same
  space. Exceeding it requires widening the wire format.
- **Retry, timeout, and circuit breaking** exist only where payload traverses the
  L7 layer. On the paths where it does not, retry would require buffering a
  request body, which contradicts the zero-copy custody model; that tension is a
  separate subject.
- **Consultation latency.** Connection setup now waits on an answer from the L7
  layer. The bound, and whether a short-lived connection should skip the wait and
  fall open, are unmeasured.

## 13. Status

| Item | Status |
|---|---|
| Registry, two facades, name resolution | implemented |
| `$DPUMESH_SERVICE` local identity, DPU slot assignment | implemented |
| Self-registration membership, round-robin selection, L4 pinning | implemented |
| Framed L7 per-frame backend selection | implemented |
| L7 layer: modes, custody, egress arena, backend selection | implemented |
| gRPC endpoint injection | implemented |
| Identifier spaces and reverse-ring widths | implemented |
| Generation snapshot and configuration thread | planned |
| Control-plane consultation: DPUmesh side | implemented |
| Control-plane consultation: answers from cluster state | planned |
| Two-sided link, remote-backed slots, three-tier credit | planned |
| Link IPsec | planned |
| Workload identity delivery and slot binding | implemented |
| Authorization: identity assertion and enforcement | planned |
| Health checking and ejection | L7 layer |
| Locality-aware selection | L7 layer, with local-endpoint input |
| Telemetry export | planned |
| Registry reload for the static-file mode | not planned |
| Application TLS termination, JWT, global rate limiting, DNS proxy | not planned |

## 14. Sequence

1. **Configuration plane** — snapshot, configuration thread, epoch reclamation,
   static-file mode retained.
2. **Control-plane consultation** — query interface to the L7 layer, and the
   fallback that runs when it does not answer.
3. **Node boundary** — link and lanes, remote-backed slots, three-tier credit,
   locality-aware selection, IPsec, remote failure handling.
4. **Identity and policy** — identity message and generation binding,
   per-connection enforcement, counter export and per-connection reporting.

Steps 1 and 2 leave the data path node-local and are verifiable against the
existing single-node configuration. Step 3 is the first to require a second DPU.
