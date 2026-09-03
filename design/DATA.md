# DPUmesh Data Plane

DPUmesh is a BlueField service mesh. A sending application writes into mapped
host memory, the DPA moves it into DPU staging, an ARM worker runs the embedded
Linkerd path, and DMA lands the result in the receiving application's mapped
memory, or the peer channel carries it to the DPU that holds the destination.
Each Service is deployed in one of two modes, which decides what Linkerd runs
for it: an opaque byte stream, or an HTTP/1, HTTP/2 or gRPC stack. The first
half of this document is the DMA transport and the second is Linkerd and the C
ABI where they meet (`linkerd/include/dmesh_l7.h`). The normative definitions
are `doca/dpu_worker.c`, `doca/dpu_proxy.c`, `linkerd/include/dmesh_l7.h` and
`linkerd/rust/src/lib.rs`. Who may talk to whom is
[`CONTROL.md`](CONTROL.md); the application's own contract is
[`API.md`](API.md).

## Terms

| Term | Meaning |
|---|---|
| channel | one process's transport: its registration, its registered memory and its rings |
| broker | the trusted per-Pod host process owning a channel's DOCA objects; the workload runs against shared mappings it hands over |
| drain shard | one host drain thread; landing stripes partition across shards |
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
| L7 layer | the embedded Linkerd proxy on every ARM data worker |
| session | one client connection together with the Linkerd outbound stack built for it |
| `DmeshIo` | the byte-stream endpoint the Linkerd stack reads from and writes to |
| session token | `(worker, slot, generation)`, which names a session for its whole lifetime |
| backend registry | the per-worker map from a session to the DPUmesh channel serving it |
| backend channel | the DPUmesh path a session's bytes take to the backend Pod |
| decline | the layer refusing a connection, by code, instead of taking it |

## Topology

The deployed geometry is `N/K/A/L=32/8/8/8`: 32 DPA EUs, eight forward rings
per Pod, eight ARM data workers, and eight RX landing stripes.

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
ring producers. Every Pod contributes one ring to each worker. The 32 EUs form
eight four-EU shards, and Pod rings are distributed within their owning shard.
Each worker also owns an independent imported handle for every Pod RX mapping;
DPU-local mappings shared across workers are made thread-safe before they start.

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
  ├─ Linkerd opaque or protocol-aware routing
  ├─ payload SG-DMA, from arrival staging or the egress arena
  └─ reverse publication
          │
          ▼
L reverse rings
          │ REV_DONE (bytes delivered) / TX_ACK (send capacity returned)
          ▼
host drain threads

DPU main thread: registration, teardown, and host doorbells
```

## Host memory and rings

One channel owns K forward rings, L reverse rings, registered TX/RX mappings,
the TX block pool, the EQ registry, its drain threads, and a tail timer
thread. The 64 MiB RX mapping is divided into L equal landing stripes.

A meshed Pod runs no DOCA object. The per-Pod broker owns the device, the
progress engine, the Comch control connection and the memfds backing every
ring and mapping; the workload maps the same pages and runs everything else —
forward-ring production, reverse-ring interpretation, delivery, and event
readiness. Two kinds of descriptor cross the process boundary at attach: the
sealed memfds and one pod-global doorbell eventfd. The broker never reads a
completion entry. [`CONTROL.md`](CONTROL.md) §2-1.9 owns the launch and trust
story.

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

The pinned ARM thread hosts a Tokio `current_thread` runtime. Its persistent
driver invokes the same DPUmesh drain path and also polls Linkerd output
wakers. Linkerd is the only L7 consumer linked into the DPU binary.

A worker delivering to a lane it owns uses a private FIFO. Delivery and
acknowledgement across workers use bounded multi-producer, single-consumer
queues.

## Thread placement

| Thread | Work |
|---|---|
| Host application | API calls and QP operations |
| Host drain × D | reverse-ring interpretation, delivery, event-queue readiness |
| Host broker | DOCA PE progress and the doorbell relay |
| Host tail timer | deadlines of retained partial sends |
| ARM main | registration, revocation, teardown and the messages that wake the host |
| ARM data worker × A | DPA completions, routing, DMA and reverse publication |
| DPA EU × N | forward-ring drain and completion metadata |

The ARM main thread owns the control connection and the messages that wake the
host; no other thread sends them. Every row above exists in both builds — what
a Linkerd build changes is only what runs *inside* the data worker, which is
the second half of this document.

## Routing

A public QP names a Service. Backend Pod and upstream port remain internal.
Linkerd receives every supported stream: an opaque connection carries no message
boundaries and stays pinned to the backend it first selected, while a
protocol-aware session routes each request through its session-local stack and
may reach a different endpoint with each one. A selection resolves to a ready
Pod registered on this DPU, or to the Pod UID the generation places on another
node and the peer channel that reaches it. One connection carries one cross-node
destination: a stream already pinned to a remote endpoint is refused a second
one by name and counted, rather than delivered to the first.

## DMA fault handling

Each ARM worker uses one DMA context shared by its pods. `IO_FAILED` stalls and
restarts that context without changing pod readiness. A failed payload batch
whose destination generation is still current is retried once at the head of
its lane FIFO. The exclusive retry preserves lane order. Each batch is bounded
independently by the device's queried memcpy byte limit, the SG element limit,
destination credits, and the contiguous tail of its landing stripe. A unit that
cannot fit one valid DMA operation is rejected through the stream error path so
its source custody is released.

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
the egress path. Send capacity returns in extent-sized steps. If release occurs
on another worker, the handoff publishes to the reverse-ring owner's MPSC queue
and wakes that owner after publication; the final ACK therefore cannot remain in
a parked worker when the stream goes quiet.

The host consumer is the workload's drain side. Stripes partition across D
drain shards (`stripe % D`); shards spawn one per registered EQ, bounded by L
and by the cores the process may run on, and a per-stripe claim lock keeps a
stripe on exactly one thread while the partition moves. An awake EQ thread
also assists in line from `dmesh_poll_eq`, so a loaded consumer waits on no
drain-shard handoff. Whoever claims a stripe drains its visible entries and
writes one monotonic `consumer_head`.

While completions keep arriving, a shard re-checks the rings on an
exponentially backed-off sleep and publishes no `arm_epoch`, so the DPU stays
silent and the cross-process wake chain is never entered: work resets the
sleep to its minimum, each empty check doubles it, and past the cap the shard
increments `arm_epoch` — the counter that says the host is about to sleep —
re-checks once, and blocks. With more live EQs than allowed cores the polled
regime is disabled, because every polling delay then lands directly in
closed-loop RTT and the precise doorbell wake wins.

After a publication the producing worker reads the ring's control block and,
on a new epoch, asks the DPU main thread to send one Comch `REV_DOORBELL`.
That message lands in the broker, which forwards each batch as one tick on
the pod-global doorbell eventfd. Every shard watches the eventfd
edge-triggered and never reads it, so one tick edges them all.

## Closing a stream

A QP's close is an ordered descriptor on its forward ring — FIN behind the last
byte, or a reset that waits for nothing — and the host carries it in the same
per-port custody FIFO a payload unit enters. While that entry stands the port is
held: neither a new client QP nor an inbound connection may take it.

The worker publishes the close `TX_ACK` only once the stream has left its
tables — for a paired L7 session, after both output directions published FIN and
the upstream port was freed. A source Pod's `(pod, port)` is the key its Linkerd
session is held under, so acknowledging earlier would let the host reopen that
key into a session still closing, and the new stream would arrive in the old
one. Replies close on DPU-assigned upstream ports that no host allocates, and
keep their immediate acknowledgement.

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
| egress arena → local destination RX | arena chunk pool | the DMA batch carrying the chunk completes — the chunk returns with the unit at `px_engine_emit` |
| egress arena → remote destination RX | per-peer un-ACKed slot and byte bounds | `STREAM_ACK`, after the destination publishes the Pod's `REV_DONE` |

Custody is returned for what Linkerd consumed from source staging. Output
always enters the arena through the reservation API. A local arena chunk
returns on local DMA completion; a remote chunk remains charged to its peer
until that peer acknowledges the destination Host landing.

The flow-control property survives as composition rather than as one credit
loop: a slow destination exhausts arena capacity → `dmesh_l7_tx_reserve` lends
no chunk → the session's write stalls → the stack stops reading →
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

The DPU binary always links `libdmesh_l7.a`, the adapter archive `linkerd/rust/`
builds, and, through it, the pinned proxy fork in `linkerd/port/linkerd2-proxy/`;
Meson refuses a configuration without them. There is no reference consumer and no
runtime bypass for a Service the deployment assigned to Linkerd. The ordered L4
machinery of the first half of this document is the substrate those sessions and
the peer channel run on, not an alternative to them.

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

Services are named at DPU startup and resolved against each adopted generation.

| Variable | Mode | Payload path |
|---|---|---|
| `DPUMESH_L7_OPAQUE_SVC` | opaque stream | Linkerd adapter |
| `DPUMESH_L7_SVC` | protocol-aware stream | Linkerd adapter |

Every data Service is assigned exactly once, by name rather than by the surface
its Pods use: an HTTP/1.1 Service under the shim is protocol-aware, and a gRPC
Service assigned opaque is not. Duplicate assignments are rejected, and a
declined session is terminated under fail-closed policy. Which Services are
graded protected, and what decides fail-closed for a Service no generation
grades, is [`CONTROL.md`](CONTROL.md).

## Worker runtime

Each of the eight DPUmesh data-worker threads calls:

```c
int l7_worker_run(int worker_id, void *driver);
```

The call creates one Tokio `current_thread` runtime on the pinned ARM thread and
blocks until the worker stop flag is set. Adapter state is thread-local, so a
connection's DPU state, Linkerd session, DMA callbacks, and reply path share one
owner. `DPUMESH_L7_LINKERD_WORKER=all` instantiates this runtime on all eight
workers. Request ports select a worker by modulo, and DPU-assigned upstream
ports preserve that owner for replies.

Each worker owns independent control-plane clients, session slots, backend
registry, and metrics registry. Worker `n` exposes its admin endpoint at the
configured base port plus `n`. The eight runtimes are therefore worker-local
shards of one DPU proxy rather than eight proxies: connection state, policy
state, backend registry, DMA callbacks and reply routing all stay on the worker
port affinity selected.

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
                      └─ dmesh_l7_tx_commit(local Pod) or
                         dmesh_l7_tx_commit_remote(Pod UID)
                           └─ egress arena → SG-DMA or peer channel → backend pod

backend reply staging
  └─ reply DmeshIo
       └─ Linkerd outbound stack
            └─ client DmeshIo tx
                 └─ dmesh_l7_tx_commit(..., ORIGIN)
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
between workers. `publish` refuses a duplicate live key.

The outbound connector calls `take_session` once per endpoint it dials. The
first takes the channel published when the session opened; each later endpoint
is minted its own `DmeshIo`, and its handle is queued for the worker, which
adopts it on its next pass. A session therefore holds one endpoint per backend
Linkerd selected, each carrying its own `BackendRoute`, and spread across a
Service comes from the route rather than from how many channels the client
opened.

Every refusal the connector can answer with is a named cause, because discovery
may replace the original synthetic service address with a concrete endpoint
address:

| `TakeError` | The connector asked for |
|---|---|
| `NotPublished` | a session that published no channel |
| `AlreadyTaken` | an endpoint this session was already dialled for |
| `TargetMismatch` | an endpoint in another Service, before endpoints are authoritative |
| `EndpointUnresolved` | an address no live registration serves |
| `EndpointStale` | a mapping that predates the held generation |

`TargetMismatch` is the guard that holds only while no endpoint resolver has
been installed, because nothing else can then tell one Service's address from
another's. Once a resolver is installed, Service identity is not what guards the
dial — liveness and node placement are, and a route may cross Services.

A close evicts its own key before the next generation publishes. A
session-to-service index avoids walking unrelated services while preserving
service-local publication order.

The normal Linkerd stack continues to key discovery, protocol, balancer and
transport caches by destination. Protocol-aware HTTP/1, HTTP/2 and gRPC
sessions build a session-local stack, so connection pools cannot consume a
different frontend's backend channel. Opaque sessions share one workload stack
and carry `SessionToken` through the byte stream, balancer and connector. In
both cases `Backends::take_session` consumes only the exact session key.

The protocol-aware cache boundary is:

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

Opaque sharing does not share a backend byte stream: it shares only stack
configuration and caches; every physical connect still carries and consumes
the originating session token.

A service DPUmesh has provided is never dialed over TCP. When its channel is
missing the connector fails the connection and counts it, because a stream on
the TCP path would run without the policy the mesh applied.

## L7 output and teardown

Linkerd output is drained from `DmeshIo` into the DPUmesh egress arena. The tx
waker notifies the persistent driver when new output is available.

```text
Linkerd buffer → DmeshIo tx queue → DPUmesh egress arena → DMA
```

Output takes the reservation path: the adapter copies queued bytes straight
from the endpoint into the chunk `dmesh_l7_tx_reserve` lends and publishes it
with `dmesh_l7_tx_commit` for an exact local Pod, the origin, or an ordinary
local choice, and `dmesh_l7_tx_commit_remote` for an exact remote Pod UID. A
commit of `0` cancels the reservation and leaves the bytes queued, so a refusal
costs no ordering; a negative result closes the session. When no chunk is
available, queued bytes wait for a later driver pass.

EOF is directional. `l7_conn_eof` closes one endpoint's input half; Linkerd may
continue reading the reverse half and producing output. Once an endpoint has
drained all queued bytes, the adapter retries `dmesh_l7_tx_fin` until the DPU
accepts that ordered FIN. The paired session is retired normally only after
both output directions published FIN. A stack endpoint that disappears before
that two-FIN completion is a terminal failure: the adapter calls
`dmesh_l7_session_failed`, and the datapath aborts the remaining halves,
releases their custody and retains only any peer EOF tombstone required to
reject late traffic. A late endpoint registration for a closed generation is
aborted.

Pod disconnect follows the same ownership rule. The control thread only
unpublishes the pod; each ARM worker removes the connection, reply and
upstream-port objects it owns and closes any Linkerd session on that worker.
Every worker declares itself quiet before the destination lanes are drained,
and one further progress pass runs after that. Each worker then releases the
buffers on its private RX mmap handle, and the control thread destroys all A
handles before publishing `POD_QUIESCED`.

## Inbound authorization

The destination DPU is the inbound enforcement point for the Pods it serves —
the role a sidecar's inbound half plays — and it needs a verdict rather than a
proxy. Inbound policy discovery is on: one `build_policies(workload)` store is
bound per registered destination Pod and one `WatchPort` runs per Pod and
port, shared by every stream arriving at it. A stream therefore costs an
evaluation, not a session build, and the cost scales with destination Pods and
ports rather than with sessions. Both are dropped when the registration ends.

The sidecarless Pod template carries `linkerd.io/control-plane-ns=linkerd` so
the stock policy controller indexes that workload. Its DMA service ports are
also listed in `config.linkerd.io/skip-inbound-ports`: destination discovery
must publish the Kubernetes endpoint without looking for a Pod-local proxy,
because the proxy and enforcement point are on the DPU. Omitting either half
is invalid: without the label `WatchPort` returns `unknown server`; without the
skip list destination translation rejects the endpoint for lacking injected
proxy metadata.

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

An authorization reaches this evaluation only on a route the `Server`'s
`proxyProtocol` admits — a gRPC `Server` carries `GRPCRoute`s, and an
`HTTPRoute` parented to it is indexed under a protocol that `Server` does not
speak, so neither it nor the `AuthorizationPolicy` that targets it appears in
the policy at all. The port then holds the deny-by-default a `Server` with no
authorization has, which is the same outcome as a policy that refuses.

Each adopted generation publishes which Service every address it names belongs
to — the session key, the ClusterIP and the ready endpoints — and pairs each
endpoint with the Pod UID the generation places there. An endpoint resolver
answers `SessionOwn` for the session's *own* two addresses and nothing else:
another Service's ClusterIP resolving that way would be `BackendRoute::Any`,
which routes on the original Service's backend with no error and no counter.
Every other address resolves through its Pod UID to a live local registration,
and each negative outcome is a distinct decline — `EndpointUnresolved`,
`EndpointStale`, or a remote placement, which records its Pod UID and is sent
over the peer channel — never a round robin and never a TCP fallback. Ports do not participate in
identity: the address's IP is what names the Pod.

A route may therefore send a stream into a Service other than the one the
client addressed, which is what a weighted `backendRefs` is. Admission moves
with it: the inbound verdict is taken against the destination Pod's own Service
and the port that Service serves, not against the Service the client asked for,
because a watch held on the caller's port returns a verdict about nothing. The
verdict is cached per destination on the connection, so a session alternating
backends does not re-enter the policy layer on every unit.

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

DMA sessions override endpoint `ConditionalClientTls` to `Disabled` before the
TLS and tagged-transport layers. Their far end is a Pod, not a second Linkerd
byte proxy: adding sidecar TLS there would deliver ciphertext and a transport
header to the application. Node-local isolation is the registered DMA mapping;
node-to-node confidentiality and mutual authentication belong to the peer
channel's transport, whose authenticated node key the held topology binds.
[`CONTROL.md`](CONTROL.md) carries that transport, the seam, and its deployment
status.

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

The connection handle's low 24 bits are:

```text
((uint64_t)(uint8_t)pod << 16) | port
```

Production handles add a worker-local incarnation in the high bits. It is an
identifier, not a pointer, and a late callback cannot match a reused port.

### DPUmesh calls Linkerd

| Entry point | Result |
|---|---|
| `l7_conn_open` | `0` accepts; a negative decline code refuses the required Linkerd session |
| `l7_conn_segment` | accepted prefix in `[0, len]`; negative closes the adapter session |
| `l7_conn_eof` | closes the input half for the named direction |
| `l7_conn_close` | drops the session and releases all held extents |
| `l7_control_event` | one control-plane admission outcome by kind and reason |
| `l7_inbound_verdict` | destination-side admission for one inbound stream: `1` admits, `0` refuses, negative means no verdict and the destination Service's protection class decides |
| `l7_inbound_forget` | drops the policy watches held for a destination Pod whose registration ended |

`l7_conn_segment` may retain the accepted prefix after the call. The staging
memory remains valid until the matching release.

### Linkerd calls DPUmesh

| Entry point | Result |
|---|---|
| `dmesh_l7_workloads` | every destination Pod this worker serves, as an inbound policy subject |
| `dmesh_l7_tx_reserve` | writable egress-arena region or `NULL` |
| `dmesh_l7_tx_commit` | publishes a reservation or returns it unused |
| `dmesh_l7_tx_commit_remote` | publishes a reservation to one exact remote Pod UID |
| `dmesh_l7_tx_fin` | publishes one ordered output FIN; `0` is backpressure and is retried |
| `dmesh_l7_session_failed` | aborts a paired session that ended without both orderly output FINs |
| `dmesh_l7_release` | returns staging custody and sender credit |
| `dmesh_l7_verify_feed` | signed prefix of an authoritative feed document, or `-1` |
| `dmesh_l7_svc_for_name` | node-local interned id for a `namespace/name` Service key, or `-1` |
| `dmesh_l7_pod_for_uid` | live pod id for a Pod UID, or a distinct negative for no registration, placed elsewhere, or absent from the held generation |

`DMESH_L7_BACKEND_ANY` delegates backend selection to the DPUmesh balancer.
`DMESH_L7_ORIGIN` sends bytes to the source of the request connection.

### Modes and decline codes

| Value | Mode |
|---|---|
| `1` | opaque stream |
| `2` | protocol-aware stream |

The Linkerd consumer accepts both modes.

| Code | Meaning |
|---|---|
| `-1` | adapter error |
| `-2` | no Linkerd state on this worker |
| `-3` | unsupported mode |
| `-4` | the session already holds a reply direction, or its backend key is live |
| `-5` | reply has no matching request session |

DPUmesh counts each decline by cause and reports the totals in the DPU log.
A declined connection is refused because every supported Service requires the
policy and routing state of its Linkerd mode.

## Build contract

The Linkerd archive must:

- export `l7_worker_run`, the connection API and inbound verdict API;
- leave every `dmesh_l7_driver_*` symbol undefined for DPUmesh to provide;
- build with the pinned Rust toolchain and lock files.

Meson links the archive supplied by `-Dl7_lib_path` and defines
`DMESH_L7_RUNTIME_OWNER`.

## Control plane

Destination, policy and identity addresses, the identity directory, the token
file and the trust anchors are the proxy's stock startup environment
(`LINKERD2_PROXY_DESTINATION_SVC_ADDR`, `LINKERD2_PROXY_POLICY_SVC_ADDR`,
`LINKERD2_PROXY_IDENTITY_SVC_ADDR`, `LINKERD2_PROXY_IDENTITY_DIR`,
`LINKERD2_PROXY_IDENTITY_TRUST_ANCHORS`; [`CONTROL.md`](CONTROL.md) §5.5.1). A
deployed Linkerd control plane is mandatory: missing addresses, identity material
or a versioned Service target feed fail preflight, and no mock fallback exists.

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
| DPA EUs | 1–32 |
| Rings per Pod | 1–16 |
| ARM data workers | 1–16; 16 is outside the supported range ([`CONTROL.md`](CONTROL.md) §5.5.1) |
| Payload DMA retries | 1 |
| Payload DMA batch bytes | queried device memcpy limit |
| Egress arena | 1,024 chunks of 64 KiB |
| L7 custody per connection | 256 KiB |

The implementation preserves per-connection order, exact TX/RX custody,
single-producer reverse rings, generation-safe teardown, and bounded
nonblocking backpressure.

The L7 layer adds:

- outbound opaque and protocol-aware streams;
- one worker-local Linkerd runtime on every ARM data worker;
- protocol-aware sessions own a session-local stack; opaque sessions share a
  workload stack but retain distinct backend channels and session tokens;
- the Linkerd-selected endpoint is preserved as an exact local Pod or remote
  topology Pod UID;
- the deployed Linkerd destination, identity and policy services are reached
  through the node agent's control-plane relay; identity material and the
  signed Service target feed are required configuration;
- endpoint TLS/tagged transport is disabled for DMA sessions; node-local
  isolation is the registered mapping and node-to-node security is the peer
  channel's transport;
- Linkerd output is copied once, into the DPUmesh egress arena;
- a declined session is counted by cause and refused fail-closed.
