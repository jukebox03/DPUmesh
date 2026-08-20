# DPUmesh Data Plane

DPUmesh is a BlueField transport. A sending application writes into host memory
that the DPU has mapped, the DPU's data-path accelerator reads it, and a DPU CPU
thread writes it into the receiving application's mapped memory.

A Service can be assigned to the DPU's L7 layer instead of plain byte
forwarding. That layer is the embedded Linkerd port, and it runs on the same ARM
data workers this transport does. The first half of this document is the
transport; the second is the L7 layer and the C ABI where the two meet
(`linkerd/include/dmesh_l7.h`). The normative definitions are
`doca/dpu_worker.c`, `doca/dpu_proxy.c`, `linkerd/include/dmesh_l7.h` and
`linkerd/rust/src/lib.rs`. Who may talk to whom is
[`CONTROL.md`](CONTROL.md); the application's own contract is
[`API.md`](API.md).

## Terms

| Term | Meaning |
|---|---|
| channel | one process's transport: its registration, its registered memory and its rings |
| QP | one full-duplex byte stream on a channel |
| EQ (event queue) | what one application thread polls for the events of the QPs it owns |
| forward ring | host→DPU descriptor queue |
| reverse ring | DPU→host completion queue |
| DPA EU | one execution unit of the BlueField data-path accelerator |
| ARM data worker | a DPU CPU thread owning routing, DMA and reverse publication for its connections |
| PE (progress engine) | a DOCA completion queue that one thread polls |
| SG-DMA | scatter-gather DMA: one operation writing several source pieces into one destination range |
| transport unit | the fixed 8 KiB block the transport submits at once |
| staging | DPU memory holding arrived bytes until they have been sent onward; an *extent* is one contiguous range of it |
| custody | the rule that arrived bytes stay valid, and their sender stays uncredited, until released |
| landing stripe | one disjoint region of the receiving pod's RX mapping, written by one worker |
| lane | the queue of pending deliveries for one (destination pod, landing stripe) pair |
| egress arena | DPU buffers holding bytes the L7 layer produced, until DMA sends them |
| L7 layer | the DPU-side proxy a Service can be assigned to, instead of plain byte forwarding |
| session | one client connection together with the Linkerd outbound stack built for it |
| `DmeshIo` | the byte-stream endpoint the Linkerd stack reads from and writes to |
| session token | `(worker, slot, generation)`, which names a session for its whole lifetime |
| backend registry | the per-worker map from a session to the DPUmesh channel serving it |
| backend channel | the DPUmesh path a session's bytes take to the backend Pod |
| decline | the layer refusing a connection, by code, instead of taking it |

## Topology

`N`, `K`, `A`, and `L` denote DPA EUs, forward rings per pod, ARM data workers,
and RX landing stripes.

```text
1 ≤ L = A ≤ K ≤ N
K % L = 0
N % A = 0
```

The data-plane mappings are:

```text
forward ring       = source port % K
ARM owner          = port % A
RX landing stripe  = destination port % L
reverse ring       = destination port % L
```

A connection remains on one ARM worker. Each worker owns its connection tables,
DPA completion PE, SG-DMA context, DMA callbacks, destination lanes, and reverse
ring producers. One worker may own multiple rings.

## Data path

```text
host QP
  │
  ▼
K forward rings
  │
  ▼
DPA EUs
  │ completion metadata
  ▼
A ARM workers
  ├─ connection state and the upstream-port map
  ├─ routing: L4, or the L7 layer
  ├─ payload SG-DMA, from arrival staging or the egress arena
  └─ reverse publication
          │
          ▼
L reverse rings
          │ REV_DONE (bytes delivered) / TX_ACK (send capacity returned)
          ▼
host progress thread

DPU main thread: registration, teardown, and host doorbells
```

## Host memory and rings

One channel owns K forward rings, K reverse rings, registered TX/RX mappings,
the TX block pool, the EQ registry, the progress thread, and a tail timer
thread. The 64 MiB RX mapping is divided into L equal landing stripes.

The timer holds no transmit state. It writes the readiness fd of an EQ whose
earliest retained tail has come due, and parks while no tail is retained on the
channel. Each QP carries a transmit gate that serializes its public TX calls
against the deadline pass `dmesh_poll_eq` runs.

Each QP is a full-duplex ordered byte stream bound to one EQ. Its TX cursors
maintain:

```text
free ≤ sent ≤ committed ≤ write
```

`dmesh_alloc` reserves registered bytes. `dmesh_post_send` commits complete
8 KiB transport units, and `dmesh_flush` publishes a trailing partial unit.
`TX_ACK` advances `free`, and the one that leaves the QP with nothing in flight
arms a still-retained tail for its EQ. `REV_DONE` creates RX, FIN, and accept
events.

Each landing stripe aggregates `K/L` credit counters. The host shards returned
credits by landing position. The DPU reads the counters with one SG-DMA and uses
their sum as the stripe credit.

A stripe is filled by a byte cursor. The cursor returns to the stripe head only
once every delivery it holds has been released by the receiving application.

## DPA and ARM execution

Each forward ring is a bounded multi-producer, single-consumer queue. A producer
reserves a monotonic ticket, writes one descriptor, and publishes
`publish_seq = ticket + 1`. The DPA
consumes consecutive tickets, copies request bytes into DPU staging, and sends
completion metadata to the connection owner.

An ARM data worker owns its completion and DMA progress. One drain pass:

1. consumes up to 64 DPA completions;
2. parses and routes connection data;
3. submits and progresses SG-DMA;
4. retires completed destination lanes;
5. emits `REV_DONE` and the `TX_ACK` run each released extent covers;
6. publishes reverse-ring entries.

A worker stays hot while a drain pass advances work. Otherwise it arms its DPA
and SG-DMA completion handles, rechecks, and waits on them, its cross-worker
eventfd, and a 1 ms maintenance deadline. No ARM tick reaches the EUs: an EU
schedules itself.

An EU polls the rings it holds and never sleeps waiting for data. The DPA
scheduler's watchdog still requires it to surrender its execution unit
periodically, so each EU owns a second DPA thread pinned to the same unit. Releasing the unit is one
operation, and it takes one of two forms:

- an EU holding rings or an unacknowledged teardown fence notifies its helper
  and reschedules. Same-unit affinity means the scheduler cannot run the helper
  until the unit is actually free, so the helper's wake always reaches a parked
  thread and the EU resumes polling;
- a ringless EU arms its consumer and producer notifications, rescans both, and
  parks only if the rescan is empty. Rescheduling promises re-entry on a *new*
  completion only, so a message already queued at arming time has to be found
  by that rescan rather than parked on. A ringless EU is woken by `RING_ADD`.

With the Linkerd backend, the pinned ARM thread hosts a Tokio `current_thread`
runtime. Its persistent driver invokes the same DPUmesh drain path and also
polls Linkerd output wakers. The null backend uses the C-owned event loop.

A worker delivering to a lane it owns uses a private FIFO. Delivery and
acknowledgement across workers use bounded multi-producer, single-consumer
queues.

## Thread placement

| Thread | Work |
|---|---|
| Host application | API calls and QP operations |
| Host progress | reverse-ring drain and event-queue readiness |
| Host tail timer | deadlines of retained partial sends |
| ARM main | registration, revocation, teardown and the messages that wake the host |
| ARM data worker × A | DPA completions, routing, DMA and reverse publication |
| DPA EU × N | forward-ring drain and completion metadata |

The ARM main thread owns the control connection and the messages that wake the
host; no other thread sends them. Every row above exists in both builds — what
a Linkerd build changes is only what runs *inside* the data worker, which is
the second half of this document.

## Routing

A public QP names a Service. Backend pod and upstream port remain internal, and
the backend set contains ready Pods registered on this DPUmesh node.

A connection's routing is resolved once. At L4 the connection keeps one backend
for its life: the first data selects one ready backend and the connection
remains pinned. A Service assigned to an L7 payload mode is instead presented to
the L7 layer, which is handed the arrived byte ranges and names the backend per
delivery; `DMESH_L7_BACKEND_ANY` selects a backend with the DPUmesh per-Service
round-robin cursor.

## DMA fault handling

Each ARM worker uses one DMA context shared by its pods. `IO_FAILED` stalls and
restarts that context without changing pod readiness. A failed payload batch
whose destination generation is still current is retried once at the head of
its lane FIFO. The exclusive retry preserves lane order.

Pod readiness is controlled by the registration connection. Disconnect cleanup
removes the pod from routing and clears DMA readiness. Connection state remains
worker-owned during teardown: every worker closes its own sessions and
upstream-port entries, then destination workers drain all lanes. A second worker progress pass
fences DOCA buffer release after completion callbacks before the main thread
destroys imported mappings.

## Reverse publication

Each landing stripe has one reverse ring, one worker producer, and one host
consumer. A 32-byte slot contains `REV_DONE` or `TX_ACK` and becomes visible
when:

```text
publish_seq = consumer ticket + 1
```

A `TX_ACK` slot names a run of consecutive sequences. A worker merges physically
adjacent arrivals of one connection into one staging extent while the parser has
not consumed them, and acknowledges the whole extent once its last byte has left
the egress path. Send capacity returns in extent-sized steps.

The host drains visible entries and writes one monotonic `consumer_head`. Before
blocking, it increments `arm_epoch` — the counter that says the host is about to
sleep — and rechecks the rings. After a publication the producing worker reads
that control block and, on a new epoch, asks the DPU main thread to send one
Comch message that wakes the host.

## Registration and teardown

```text
Host                                      BlueField ARM / DPA
 |-- POD_REGISTER -------------------------->|
 |<---------------- POD_ASSIGNED ------------|
 |-- forward rings, TX/RX, reverse rings --->|
 |                              RING_ADD to EUs
 |<------ POD_INIT_RESULT(READY, L) ----------|

 |-- POD_UNREGISTER ------------------------>|
 |                              stop routing
 |                              RING_DEL to EUs
 |                              close worker-owned sessions and ports
 |                              drain DMA and reverse publishers
 |                              second progress pass: DOCA buffers released
 |                              destroy imported mappings
 |<---------------- POD_QUIESCED -------------|
```

Control messages are idempotent. Pod generations bind imported mappings, DPA
rings, and asynchronous completions to one registration. The host retains its
exports until `POD_QUIESCED`.

`RING_DEL` is the teardown fence: the EU removes the ring and acknowledges on
the same ordered producer every forward DMA uses, so an acknowledged deletion
means no outstanding operation can still name the ring. The slot and its
imported mappings are held until that acknowledgement lands. An EU whose DPU
channel has no posted receive keeps the fence — one entry per ring it can hold,
so a burst of teardowns displaces none of them — and probes it again each time
it releases its execution unit, never on the forwarding path; the control
thread independently resends `RING_DEL` to unacknowledged EUs every 10 ms.
Forwarding backpressure withholds DPU receives, so a floor of them stays posted
to keep the fence sendable: the data path must not be able to hold the control
path shut.

What a registration must present before any of this runs, and what withdraws it
afterwards, is [`CONTROL.md`](CONTROL.md).

## Custody: two domains

Custody has **two** semantics in this tree, not one, and they differ in what
event returns the sender's capacity. The second is the L7 layer's, so the calls
it turns on — `dmesh_l7_release`, the egress arena, `DmeshIo` — are specified in
*The adapter ABI* below; they are named here because the contrast between the
two domains is the thing worth stating once.

**L4 and opaque connections: custody is end-to-end.** An arrival — one coalesced
run of DPA completions, at most `PX_ARRIVAL_COALESCE_MAX`, the same contiguous
run the SG-DMA engine sources from — is created holding `bytes + 1` custody. The
release chain:

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
staging extent held — one acknowledgement per extent, not per transport unit. So
the sender's capacity genuinely returns only after the bytes landed in the
destination Pod's RX mapping. Three deliberate exceptions, kept because the
alternative is a leak:

- an **errored** batch still releases custody (`px_lane_drop_dead`) — capacity
  returns for bytes that never landed, and the affected connection is poisoned;
- a **dropped** range (poison, window drop, conn delete) releases immediately
  via `px_advance` with no DMA;
- a zero-byte (FIN/notify-only) batch never DMAs and completes at submit.

**L7 connections: custody is hop-by-hop, three bounded stages.** An L7-produced
unit carries no arrival custody: `px_unit_attach_chunk` sets `piece->arr = NULL`,
so `px_piece_release` is a no-op for every L7 egress byte.

| Hop | Bound | Released when |
|---|---|---|
| Pod TX ring → DPU staging | sender's TX window + `PX_L7_CUSTODY_MAX` per conn | the Linkerd stack **consumed the staging segments** — `dmesh_l7_release` is called when `rx_has_data()` goes false, after `DmeshIo::poll_read` copied them out |
| inside the session | Tokio channel and stack buffers | stack progress |
| egress arena → destination RX | arena chunk pool | the DMA batch carrying the chunk completes — the chunk returns with the unit at `px_engine_emit` |

Custody is returned for what was copied out of staging, not for what was
forwarded. The output-mode A/B (`DMESH_L7_TX_RESERVE`) selects how bytes *enter*
the arena; it moves no release point. There is no L7 mode in which the Pod's TX
credit is coupled to destination DMA.

The flow-control property survives as composition rather than as one credit
loop: a slow destination exhausts arena capacity → `px_ship_arm_bytes` finds no
chunk and returns 0 → the session's write stalls → the stack stops reading →
staging fills to
`PX_L7_CUSTODY_MAX` → `dmesh_l7_release` stops → TX credit stops → the Pod
stalls. Every hop is bounded, so the composition is lossless backpressure; what
is lost relative to L4 is the *identity* of the loop — the Pod's credit no
longer names delivered bytes.

Staging is per source Pod (`pod_state.local_mmap`, mirroring the host TX ring
1:1 — the DPA writes `staging offset == host TX offset`), so a stalled
destination fills the stalling Pod's staging and no other Pod's. Two shared
structures sit beside it: the host-side TX block pool is per process, so a Pod's
stalled QP can starve that same Pod's other QPs of blocks; and the DPU-side
proxy node pools are global. The arrival, piece and unit pools are sized for
every Pod's full staging simultaneously, so stalls alone cannot exhaust them —
the egress chunk arena is not.

---

# The L7 layer

![DPUmesh with embedded Linkerd thread model](figures/dpumesh_threads.png)

[PDF](figures/dpumesh_threads.pdf)

The DPU binary selects the L7 consumer at build time:

```text
-Dl7_backend=null      linkerd/shim/l7_null.c
-Dl7_backend=linkerd   linkerd/rust/libdmesh_l7.a
```

The L7 runtime does not alter the Host↔DPU protocol, wire descriptors or the
Host API: host channels continue to register services, publish forward
descriptors, drain the reverse ring and return credits exactly as the first half
of this document and [`API.md`](API.md) specify. The gRPC integration continues
to use one `DmeshRuntime` per process and its EQ reactors; a service assigned to
an L7 mode is intercepted only after it reaches the DPU ARM worker.

## Ownership

DPUmesh owns all transport and hardware state. The Linkerd static library owns
the per-worker Rust runtime, `DmeshIo` endpoints and outbound proxy tasks.

| DPUmesh | Linkerd static library |
|---|---|
| Host API, gRPC transport adapter, control protocol | Tokio `current_thread` runtime |
| DOCA device, DPA execution units, progress engines | persistent async driver |
| forward and reverse rings, credits, upstream-port map | `DmeshIo` endpoint pairs |
| connection routing and conntrack | Linkerd outbound stack |
| staging custody, egress arena, SG-DMA | adapter session state |
| ARM data-worker creation, affinity, stop state | control-plane clients |

The Rust runtime receives opaque DPUmesh worker contexts. It does not
initialize or destroy DOCA resources. The flow and byte-transfer ABI belongs to
`dmesh_l7.h`.

![DPUmesh and Linkerd ownership boundary](figures/linkerd_driven.png)

[PDF](figures/linkerd_driven.pdf)

## Service modes

Services are assigned at DPU startup.

| Variable | Mode | Payload path |
|---|---|---|
| `DPUMESH_L7_DECISION_SVC` | decision | DPUmesh data path |
| `DPUMESH_L7_OPAQUE_SVC` | opaque stream | Linkerd adapter |
| `DPUMESH_L7_SVC` | protocol-aware stream | Linkerd adapter |

A service absent from these lists uses L4 forwarding. Duplicate assignments are
rejected. The Linkerd consumer accepts opaque and protocol-aware streams; its
decision entry point answers every question with a decline. Under fail-closed no
verdict is not permission, so DPUmesh counts the decline as
`dmesh_control_events_total{kind="admission",reason="no-verdict"}` and ends the
connection rather than forwarding it at L4. Because that makes such a Service
unable to carry traffic at all, the deployment script refuses
`DPUMESH_L7_DECISION_SVC` under `L7_BACKEND=linkerd` instead of deploying it.
Which Services are graded protected, and what decides fail-closed for a Service
no generation grades, is [`CONTROL.md`](CONTROL.md).

## Worker runtime

Each DPUmesh data-worker thread calls:

```c
int l7_worker_run(int worker_id, void *driver);
```

The call creates one Tokio `current_thread` runtime on the already pinned ARM
thread and blocks until the worker stop flag is set; `driver` remains valid for
that whole time. Adapter state is thread-local, and all connection calls for a
worker execute on that same worker thread.

`DPUMESH_L7_LINKERD_WORKER`, default `0`, selects which workers own Linkerd
session state: a worker id names one, and `all` names every ARM data worker.
DPUmesh validates the value against the ARM worker count once, at
initialization. A worker without session state runs the same persistent driver
without a proxy and returns `DMESH_L7_DECLINE_NOT_ATTACHED` for Linkerd session
opens.

With one selected worker, the completion of a request for a service the L7
layer carries is routed to that worker instead of the one its port hashes to,
so a selected flow never meets a worker that would decline it. With `all` no
flow can meet such a worker, so requests keep the ordinary port policy and
spread across workers. A reply needs no rule either way: the DPU allocates its
upstream port so that `port % A` is the owning worker, and the reply returns to
that worker. Traffic for services the L7 layer does not carry always keeps the
port policy.

Under `all`, each worker builds a complete proxy with its own control-plane
clients, session slots, backend registry and metrics registry. The inbound,
outbound and control listeners are already ephemeral; the admin server is the
one fixed address, so worker `n` serves it at the configured port plus `n`.

![Persistent runtime loop on a Linkerd-enabled ARM worker](figures/l7_interaction.png)

[PDF](figures/l7_interaction.pdf)

DPUmesh implements the driver backend consumed by `dmesh_doca::runtime::run`:

| Entry point | Contract |
|---|---|
| `dmesh_l7_driver_notification_fds` | return completion, optional DMA and wake fds |
| `dmesh_l7_driver_arm` | ask the completion engines to notify, and mark the worker parked |
| `dmesh_l7_driver_drain` | perform bounded DPUmesh progress and return `Idle`, `Pending` or `Progressed` |
| `dmesh_l7_driver_clear_notifications` | clear those notifications, drain the wake fd and mark the worker active |
| `dmesh_l7_driver_maintenance` | run the 1 ms worker maintenance action |
| `dmesh_l7_driver_stopped` | expose the DPUmesh worker stop state |
| `dmesh_l7_driver_ready` | publish successful worker initialization |
| `dmesh_l7_driver_failed` | publish failed worker initialization |

The driver iteration is:

```text
maintenance when due
drain DPU completions, cross-worker work, SG-DMA, registrations and Linkerd output
if progress: yield to runtime tasks and repeat
arm DPU notifications
drain again
wait for an fd, Linkerd waker, or maintenance deadline
clear notifications and repeat
```

The runtime supplies a per-queue drain budget of 64. The arm-and-recheck
sequence closes the notification race before the runtime sleeps.

## Session identity

A session is named by a token, internal to the Rust side and not part of the C
ABI:

```rust
pub struct SessionToken { worker: u16, slot: u32, generation: u32 }
```

A slot is reused; its generation is not. Registrations, lifecycle events, task
ownership and backend-registry keys all carry the token, so an event from a
closed session cannot bind to the session now holding its slot. A slot whose
generation space is exhausted is retired rather than issued again.

The acceptor owns one task per session. `ConnClosed` and `ConnError` cancel
exactly the task their token names and are awaited outside event dispatch;
when the event stream or the drain signal ends, every task is cancelled and
joined before the acceptor returns.

## Connection path

```text
request staging
  └─ l7_conn_segment
       └─ client DmeshIo
            └─ Linkerd outbound stack
                 └─ backend DmeshIo tx
                      └─ dmesh_l7_send(..., BACKEND_ANY)
                           └─ DPUmesh egress arena → SG-DMA → backend pod

backend reply staging
  └─ reply DmeshIo
       └─ Linkerd outbound stack
            └─ client DmeshIo tx
                 └─ dmesh_l7_send(..., ORIGIN)
                      └─ DPUmesh egress arena → SG-DMA → client pod
```

The backend `DmeshIo` is published before the Linkerd connector resolves the
service address. The reply connection attaches to the request session by
`peer_pod` and destination port.

## Backend channels

A session publishes the endpoint that provides its service into its worker's
registry, keyed by

```rust
struct BackendKey { worker: u16, service: SocketAddr, session: SessionToken }
```

The registry is owned by the worker, not global: the adapter, the acceptor and
the outbound connector of one worker share one instance, and no lock is shared
between workers. `publish` refuses a duplicate live key. The outbound connector
calls `take_session`, and every refusal it can answer with is a named cause,
because discovery may replace the original synthetic service address with a
concrete endpoint address:

| `TakeError` | The connector asked for |
|---|---|
| `NotPublished` | a session that published no channel |
| `AlreadyTaken` | a channel a connector already holds |
| `TargetMismatch` | an endpoint the newest generation places in another Service |
| `EndpointUnresolved` | an address no live registration serves |
| `EndpointRemote` | an endpoint the generation places on another node |
| `EndpointStale` | a mapping that predates the held generation |

A close evicts its own key before the next generation publishes. A
session-to-service index avoids walking unrelated services while preserving
service-local publication order.

The normal Linkerd stack continues to key discovery, protocol, balancer and
transport caches by destination. DPUmesh does not change those stock keys.
Instead, the acceptor builds one complete outbound stack per `SessionToken`;
its connector is bound to that token and takes only the channel owned by that
token. The discovery-selected address is retained for routing metadata and
diagnostics, but cannot substitute another session's DMA channel. All caches
and reconnect state in that stack are therefore session-local: two sessions to
the same service use distinct backend channels, and closing generation N drops
its stack and removes only its registry key before a reused slot publishes
generation N+1.

The cache boundary is the same for detected HTTP/2 and opaque traffic:

```text
DmeshTarget(SessionToken)
  -> per-session Outbound::mk
       -> discovery cache (private to the stack)
       -> protocol cache (private to the stack)
       -> logical/concrete endpoint caches (private to the stack)
       -> reconnect service (private to the stack)
       -> DmeshOrTcp(session)
            -> Backends::take_session(session)
```

Two targets with the same `OrigDstAddr` never enter the same cache instance.
The registry tests publish two same-service keys and take them in the opposite
order, and also take a session whose discovery endpoint differs from its
original service address, so publication order and endpoint rewriting cannot
select another session's channel.

This is the session-isolated model. Sharing one backend HTTP/2 transport
across several frontend sessions would require a DPU-side transport identity,
an independent upstream-port lifetime and response routing by Linkerd stream;
that is a separate ABI design.

A service DPUmesh has provided is never dialed over TCP. When its channel is
missing the connector fails the connection and counts it, because a stream on
the TCP path would run without the policy the mesh applied.

## L7 output and teardown

Linkerd output is drained from `DmeshIo` into the DPUmesh egress arena.
`dmesh_l7_send` accepts a prefix; the adapter restores an unaccepted suffix to
the front of its tx queue, so output for each connection is published in
order. The tx waker notifies the persistent driver when new output is
available.

```text
Linkerd buffer → DmeshIo tx queue → DPUmesh egress arena → DMA
```

Output takes the reservation path: the adapter copies queued bytes straight
from the endpoint into the chunk `dmesh_l7_tx_reserve` lends and publishes it
with `dmesh_l7_tx_commit`. A commit of `0` cancels the reservation and leaves
the bytes queued, so a refusal costs no ordering; a negative result closes the
session. `DMESH_L7_TX_RESERVE=0` selects the `dmesh_l7_send` path, which
copies through a temporary buffer, so the two can be compared on hardware. It
is a startup selection, not an automatic fallback: when the reservation path
has no chunk, the queued bytes wait for a later driver pass.

Closing either transport direction closes the paired session. Teardown aborts
both endpoints, discards their queued input and output, releases all
outstanding input, then removes session and backend-registry state. A late
endpoint registration for a closed session is aborted. A stack endpoint that
finishes first also closes the session.

Pod disconnect follows the same ownership rule. The control thread only
unpublishes the pod; each ARM worker removes the connection, reply and
upstream-port objects it owns and closes any Linkerd session on that worker.
Every worker declares itself quiet before the destination lanes are drained,
and one further progress pass runs after that, so DOCA has returned the buffer
references its completion callbacks hold before the imported host mapping is
destroyed; `POD_QUIESCED` cannot precede that fence.

## Inbound authorization

The destination DPU is the inbound enforcement point for the Pods it serves —
the role a sidecar's inbound half plays — and it needs a verdict rather than a
proxy. Inbound policy discovery is on: one `build_policies(workload)` store is
bound per registered destination Pod and one `WatchPort` runs per Pod and
port, shared by every stream arriving at it. A stream therefore costs an
evaluation, not a session build, and the cost scales with destination Pods and
ports rather than with sessions. Both are dropped when the registration ends.

The verdict is the stock evaluation, reached through `connection_verdict` in
the fork's `linkerd/app/inbound/src/policy.rs`; the authorization types and
their matching rules are private to that crate, and a reimplementation outside
it would be a second policy engine that could disagree. It is per protocol
variant, because that is how a `ServerPolicy` carries its authorizations:

| `ServerPolicy::protocol` | Connection verdict |
|---|---|
| `Detect { tcp_authorizations, .. }` | any authorization admits |
| `Tls(a)`, `Opaque(a)` | any authorization in `a` admits |
| `Http1`, `Http2`, `Grpc` | any authorization of any route admits — the union |

The HTTP variants carry no connection-level list, so the connection is refused
exactly when no route could ever admit this client. Route-level differences
are not enforced: that needs a second parser, which is the cost the
source/destination split exists to avoid.

Each adopted generation publishes which Service every address it names belongs
to — the session key, the ClusterIP and the ready endpoints — and pairs each
endpoint with the Pod UID the generation places there. When Linkerd selects an
endpoint, the connector takes the session's channel unless that generation
places the address in another Service, which is refused and counted as
`dmesh_backend_target_mismatches`. The session key and ClusterIP are the
session's own; every other address resolves through its Pod UID to a live local
registration, and the three negative outcomes are distinct declines —
`EndpointUnresolved`, `EndpointRemote`, `EndpointStale` — never a round robin
or a TCP fallback. Ports do not participate in identity: the address's IP is
what names the Pod. The channel taken is the session's own, so discovery cannot
move a session to another Service.

While identity is unavailable the proxy does not serve: sessions are opened,
their outbound connections fail, and each failure is counted. Nothing is
forwarded without the policy the service selected.

## Transport security

Node-local DPUmesh traffic is plaintext. A session's bytes travel Pod
registration memory, PCIe and DPU memory; what separates one Pod from another
is the DPU's per-Pod mapping and routing, not a wire an attacker could reach.
The source workload used for policy comes from the registration path, which
makes it node-agent-attested, connection-bound authorization. Node-to-node
mTLS terminates on the DPU and is [`CONTROL.md`](CONTROL.md)'s peer channel.

This is a discovery contract, not a proxy code path. `push_tcp_endpoint`
layers `tls::Client` and `TaggedTransport` above the connector, so an endpoint
whose metadata carries `tls_identity` or a tagged transport port is given a
real handshake and a transport header **over its `DmeshIo`**. A destination
service serving node-local backends must return neither for them. Returning
them changes the shape of the data path rather than its configuration —
per-byte cryptography on the ARM cores, and a header written into a stream
whose far end is a pod, not a proxy — and invalidates every performance figure
collected without it. When node-to-node arrives, the choice belongs to a
per-service mode (`intra-plaintext`, `intra-mtls`, `inter-mtls`), never to a
global constant.

## The adapter ABI

### Flow identity

```c
struct dmesh_l7_flow {
    uint32_t src_ip, dst_ip;        /* 0, 4 */
    uint16_t src_port, dst_port;    /* 8, 10 */
    int32_t  src_pod;               /* 12 */
    int32_t  dst_service;           /* 16 */
    int32_t  peer_pod;              /* 20 */
    uint8_t  mode;                  /* 24 */
    uint8_t  is_reply;              /* 25 */
    char     workload[384];         /* 26 */
    char     source_identity[254];  /* 410 */
};                                  /* 664 bytes */
```

Addresses use host byte order. `workload` and `source_identity` are
NUL-terminated. The C and Rust layouts are checked by unit and ABI tests.

`src_ip` is the source Pod's real cluster address — from its signed assertion
within a node, from the generation across one. It is not synthetic and cannot
be: the stock inbound evaluation matches an authorization's `networks` before
its identity clause and an empty match denies, so an address the cluster never
assigned would make every realistic policy refuse every connection.

`source_identity` is
`<service-account>.<namespace>.serviceaccount.identity.<trust-domain>`, built
from the same signed claims. It is empty when the source is not attested,
which the evaluation reads as an established connection carrying no client
identity. No part of either field comes from a Pod: the workload is bound to
the Pod registration, whose node-agent-signed assertion is the only thing that
names it. The registration contract is [`CONTROL.md`](CONTROL.md).

The connection handle is:

```text
((uint64_t)(uint8_t)pod << 16) | port
```

It is an identifier, not a pointer.

### DPUmesh calls Linkerd

| Entry point | Result |
|---|---|
| `l7_conn_open` | `0` accepts; a negative decline code selects L4 fallback, or refusal under required registration |
| `l7_conn_segment` | accepted prefix in `[0, len]`; negative closes the adapter session |
| `l7_conn_eof` | closes the input half for the named direction |
| `l7_conn_close` | drops the session and releases all held extents |
| `l7_resolve` | decision verdict; the Linkerd consumer declines every question |
| `l7_report` | terminal accounting for decision mode |
| `l7_control_event` | one control-plane admission outcome by kind and reason |
| `l7_inbound_verdict` | destination-side admission for one inbound stream: `1` admits, `0` refuses, negative means no verdict and the destination Service's protection class decides |
| `l7_inbound_forget` | drops the policy watches held for a destination Pod whose registration ended |

`l7_conn_segment` may retain the accepted prefix after the call. The staging
memory remains valid until the matching release.

### Linkerd calls DPUmesh

| Entry point | Result |
|---|---|
| `dmesh_l7_backends` | live backend pod identifiers for a service |
| `dmesh_l7_send` | accepted output prefix; `0` is retryable backpressure |
| `dmesh_l7_tx_reserve` | writable egress-arena region or `NULL` |
| `dmesh_l7_tx_commit` | publishes a reservation or returns it unused |
| `dmesh_l7_release` | returns staging custody and sender credit |
| `dmesh_l7_verify_feed` | signed prefix of an authoritative feed document, or `-1` |
| `dmesh_l7_svc_for_name` | node-local interned id for a `namespace/name` Service key, or `-1` |
| `dmesh_l7_pod_for_uid` | live pod id for a Pod UID, or a distinct negative for no registration, placed elsewhere, or absent from the held generation |

`DMESH_L7_BACKEND_ANY` delegates backend selection to the DPUmesh balancer.
`DMESH_L7_ORIGIN` sends bytes to the source of the request connection.

### Modes and decline codes

| Value | Mode |
|---|---|
| `1` | decision |
| `2` | opaque stream |
| `3` | protocol-aware stream |

The Linkerd consumer accepts modes 2 and 3.

| Code | Meaning |
|---|---|
| `-1` | adapter error |
| `-2` | no Linkerd state on this worker |
| `-3` | unsupported mode |
| `-4` | the session already holds a reply direction, or its backend key is live |
| `-5` | reply has no matching request session |

DPUmesh counts each decline by cause and reports the totals in the DPU log
whenever they move. A declined connection is refused rather than forwarded at
L4, because a Service the L7 layer was configured to carry must not be
forwarded without the policy that layer applies.

## Build contract

The Linkerd archive must:

- export `l7_worker_run`, the connection API, `l7_resolve` and `l7_report`;
- leave every `dmesh_l7_driver_*` symbol undefined for DPUmesh to provide;
- build with the pinned Rust toolchain and lock files.

Meson links the archive with `-Dl7_backend=linkerd` and defines
`DMESH_L7_RUNTIME_OWNER`. The null consumer uses the C attach/step/detach loop
and does not define this macro.

## Control plane

Destination, policy and identity addresses, the identity directory, the token
file and the trust anchors are startup configuration (`LINKERD_DST_ADDR`,
`LINKERD_POLICY_ADDR`, `LINKERD_IDENTITY_ADDR`, `LINKERD_IDENTITY_DIR`,
`LINKERD_TRUST_ANCHORS`). A deployed Linkerd control plane is mandatory:
missing addresses, identity material or a versioned Service target feed fail
preflight, and no mock fallback exists.

The Service target feed names Services by `namespace/name`; the adapter
verifies it through `dmesh_l7_verify_feed` (feed keyring), parses only the
signed prefix, and resolves each key to the node-local interned id through
`dmesh_l7_svc_for_name`, so it holds no key material and no id table of its
own. Each outbound Policy Watch uses the registration-bound workload from
`struct dmesh_l7_flow`, never the process-wide Linkerd fallback. Registration,
feeds, membership withdrawal and the identity lifecycle are specified in
[`CONTROL.md`](CONTROL.md).

## Observability

The proxy's own metrics registry carries the `dmesh` counters: sessions
opened, closed and active, registrations pending and orphaned, endpoints
aborted, tasks live and cancelled, backend take errors and target mismatches,
per-session stack build phases, and retired slots. Refused sessions are
counted by cause as `dmesh_sessions_declined_total{reason}`, and registration,
membership, revocation and admission outcomes as
`dmesh_control_events_total{kind,reason}`. The control events are decided on
the Comch control thread rather than on a worker, so they are process-global
and every worker's admin endpoint reports the same values. After traffic
quiesces, active sessions, pending registrations and live tasks are zero.

---

# Bounds

| Item | Value |
|---|---:|
| Transport unit | 8 KiB |
| Staging extent | 64 KiB / 65,534 sequences |
| Host TX mapping | 8,192 units / 64 MiB |
| Host RX mapping | 8,192 units / 64 MiB |
| RX landing stripes | L = A |
| TX block | 512 KiB |
| QP TX window | 8 blocks / 4 MiB |
| Forward ring | 4,096 descriptors |
| Reverse ring | 8,192 entries |
| Reverse entry | 32 B |
| DPA EUs | automatic 32, maximum 32 |
| Rings per pod | default 2, maximum 8 |
| ARM data workers | default 1, maximum 8 |
| Payload DMA retries | 1 |
| Egress arena | 1,024 chunks of 16 KiB |
| L7 custody per connection | 256 KiB |

The implementation preserves per-connection order, exact TX/RX custody,
single-producer reverse rings, generation-safe teardown, and bounded
nonblocking backpressure.

The L7 layer adds:

- outbound opaque and protocol-aware streams; decision mode is declined;
- one selected Linkerd worker, which every selected L7 flow is routed to, or a
  proxy on every worker with requests kept on the port policy;
- concurrent sessions to one service address are isolated, each owning its own
  outbound stack and backend channel;
- backend selection uses `DMESH_L7_BACKEND_ANY`; DPUmesh selects the pod;
- the deployed Linkerd destination, identity and policy services are reached
  through the node agent's control-plane relay; identity material and the
  signed Service target feed are required configuration;
- node-local transport is plaintext, with identity granted at pod
  registration; a destination service must return no `tls_identity` and no
  tagged transport port for a node-local backend;
- Linkerd output is copied once, into the DPUmesh egress arena;
- a declined session is counted by cause and refused fail-closed.
