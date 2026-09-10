# DPUmesh Data Plane

Placement (2026-09-10): applications are host Pods; ARM/DPA runtime and the feed receiver share a DPU Pod. Node admin is a host systemd service and each broker is its host-process child. The current registration and lifecycle contract is [CONTROL](CONTROL.md).

DPUmesh is a reliable full-duplex byte transport for Kubernetes Services. A
workload publishes descriptors over registered host memory; BlueField DPA and
ARM workers route the stream to a local registered mapping or an authenticated
peer channel. Selected Services enter a worker-local Linkerd runtime for opaque,
HTTP/1, HTTP/2, and gRPC processing.

This document defines execution ownership, byte custody, ordering,
backpressure, Linkerd arena batching, peer RDMA leases, and teardown. Public API
semantics are in [API.md](API.md); identity and feed authority are in
[CONTROL.md](CONTROL.md).

## 1. Execution model

```text
workload process                 BlueField DPU                  workload process
┌─────────────────┐       ┌──────────────────────────┐       ┌─────────────────┐
│ channel / EQ/QP │       │ DPA execution units      │       │ channel / EQ/QP │
│ TX/RX mappings  │◀─────▶│ ARM data workers         │──────▶│ TX/RX mappings  │
└────────┬────────┘ PCIe  │ routing / DMA / Linkerd  │  PCIe └────────┬────────┘
         │                └────────────┬─────────────┘               │
   per-Pod broker                     │                       per-Pod broker
                                      ▼
                             authenticated peer channel
```

The workload and broker are separate processes. The workload owns API objects
and application-visible memory. The broker owns DOCA objects and passes sealed
ring and mapping descriptors through its allocated Unix socket. Payload bytes
do not pass through broker buffers.

| Object | Meaning and owner |
|---|---|
| channel | one workload process, registration, and memory domain |
| EQ | single-consumer event queue owned by one polling thread |
| QP | one ordered full-duplex byte stream bound to an EQ |
| forward ring | host-to-DPU descriptors; each Pod spans `K` rings |
| reverse ring | DPU-to-host receive, credit, close, and error completions |
| DPA execution unit | drains assigned forward rings and stages payload |
| ARM data worker | owns connection state, routing, DMA, Linkerd, and peer progress |
| staging extent | DPU memory holding source bytes until downstream custody completes |
| egress arena | DPU memory holding bytes emitted by Linkerd |

Each connection belongs to one ARM worker. Cross-worker handoff uses bounded
queues; connection state, Linkerd sessions, peer streams, and unpublished arena
batches remain worker-confined. Reverse rings have a single DPU producer.

## 2. Registration and memory

Channel creation completes this barrier before the API returns:

```text
POD_REGISTER → POD_ASSIGNED → import TX and ring mappings
→ import worker-private RX mappings → all RING_ADD_ACK
→ POD_INIT_RESULT(READY, L)
```

Registration carries identity metadata bound to this connection's nonce: the
daemon incarnation, slot generation, Pod UID and authorized Service.
The host also checks that requested ring geometry matches its allocation.
Identity metadata reaches the DPU over the paired host's mutually authenticated
control session, as [`CONTROL.md`](CONTROL.md) §2-1 defines. The
DPU admits the channel only when that identity and node-scoped control state
agree.

The channel exports these data structures:

- a 64 MiB TX mapping divided into 8 KiB transport units;
- a 64 MiB RX mapping divided into `L` worker-private landing stripes;
- `K` forward rings with 4,096 descriptors each;
- reverse rings with 8,192 32-byte entries each.

The host and DPU use monotonically advancing producer and consumer positions.
Descriptors carry stream identifiers, sequence numbers, source offsets, length,
destination Service, and connection fields. A Pod-slot generation fences late
DMA completions from a new occupant of the same slot.

Graceful channel destruction repeatedly requests `POD_UNREGISTER` while the DPU
removes rings, retires DMA, closes streams, and releases mappings. The host keeps
the exported memory alive until `POD_QUIESCED` or its cleanup deadline.

## 3. Intra-node path

### 3.1 Host transmit

`dmesh_alloc()` reserves contiguous bytes from the QP's registered TX byte ring.
`dmesh_post_send()` commits the initialized prefix and transfers ownership to
the transport. Complete 8 KiB units are published immediately. A partial tail
is published when the stream is otherwise idle, when its bounded deadline
expires, or when `dmesh_flush()` is called.

```text
application payload
  → registered TX reservation
  → ordered forward descriptor
  → DPA copy into DPU staging
```

The QP window is bounded by eight 512 KiB blocks. Shared block-pool exhaustion
or a full QP window returns `EAGAIN` and arms the QP. Capacity recovery produces
one `DMESH_EVENT_TX_READY`; readiness is a retry hint, not reserved capacity.

### 3.2 DPA and ARM processing

DPA execution units poll their assigned forward rings, validate descriptors,
and copy payload into the source Pod's DPU staging region. Each completion is
handed to the connection's ARM worker. The worker preserves stream order while
it resolves a Service, checks policy, selects a destination, and creates an
egress unit.

For a local destination, the worker builds a scatter-gather DMA operation from
source staging or the Linkerd egress arena directly into the destination Pod's
RX landing stripe. Completion places `REV_DONE` on the destination reverse ring.
The destination drain thread converts it to `DMESH_EVENT_RECV` and wakes the EQ
that owns the QP.

### 3.3 Receive and reverse publication

A receive event points into the registered RX mapping. Its buffer remains valid
until `dmesh_release_rx_buffer()` returns the credit. The reverse path carries
three distinct facts:

| Event | Meaning |
|---|---|
| `REV_DONE` | destination bytes are visible in its RX mapping |
| `TX_ACK` | source bytes no longer need their TX/staging custody |
| FIN/close acknowledgement | the corresponding stream half or connection is retired |

An ARM worker may stage reverse entries, but it publishes them in connection
order and wakes the owning host only after the producer position is visible.
ACK entries coalesce consecutive sequence runs without changing release order.

### 3.4 Custody

Custody identifies the component responsible for preserving bytes or reporting
their terminal failure.

| Boundary | Custody begins | Custody ends |
|---|---|---|
| application → host transport | successful `dmesh_post_send` | source `TX_ACK` or terminal QP error |
| DPA → ARM staging | forward completion | downstream delivery or ordered drop |
| ARM → local destination | DMA submission | destination `REV_DONE` publication |
| destination application | `DMESH_EVENT_RECV` | `dmesh_release_rx_buffer` |
| Linkerd → egress arena | accepted `AsyncWrite` prefix | local DMA completion or remote `STREAM_ACK` |
| source DPU → peer DPU | accepted peer DATA frame | destination `REV_DONE` followed by `STREAM_ACK` |

Pool capacity is returned only at the matching custody boundary. A zero-byte
FIN consumes ordering capacity but carries no payload DMA.

### 3.5 Faults and close

Connections preserve independent input and output half-close state. DATA is
submitted before its FIN. A graceful destroy flushes committed output, sends
FIN, returns held RX credit, and waits for the DPU close acknowledgement before
reusing the source port. Abort discards unpublished output and resets the
stream.

A payload DMA error poisons its delivery unit. The worker recreates its DMA
context and retries a current-generation batch once, in lane order. A repeated
failure completes as an ordered drop, releases custody, and reports the stream
failure without unpublishing healthy Pods.

## 4. Linkerd data path

### 4.1 Service modes and session identity

Each adopted generation assigns a Service one of three data treatments:

| Mode | Behavior |
|---|---|
| native L4 | connection-level routing in the C data plane |
| opaque | one Linkerd byte-stream proxy with connection pinning |
| protocol-aware | HTTP/1, HTTP/2, or gRPC parsing, policy, and per-request routing |

The C data plane passes a flow identity containing the request connection
handle, source Pod and port, source IP, workload identity, Service id, protocol
mode, and direction. A generation-safe handle binds every callback to its ARM
worker and connection lifetime.

Each selected worker owns one single-thread Tokio runtime. Protocol-aware
sessions have session-local outbound stacks. Opaque sessions share a workload
stack while retaining independent endpoint, route, and stream state.

### 4.2 Input

The ARM worker offers completed staging extents to `DmeshIo` as `(offset,
length)` segments. `DmeshIo::poll_read` copies directly from that mapped extent
into Linkerd's `ReadBuf`; there is no DPU-side intermediate payload queue.
Staging custody remains with the session until Linkerd consumes the segment and
`dmesh_l7_release` advances the C connection window.

At most 256 KiB of input is held per L7 connection. Segment admission and drain
work are bounded per worker pass so one session cannot monopolize the runtime.

### 4.3 Egress arena batching

Each Linkerd endpoint owns at most one unpublished batch: a 64 KiB registered
arena chunk leased from the C data plane and named by a process-unique token.
Scalar and vectored `AsyncWrite` calls append an ordered prefix of the caller's
slices into that chunk through `dmesh_l7_tx_batch_write`, on the owning worker
thread and under the endpoint lock. The C data plane owns payload storage; Rust
retains the token, byte count, exact route, and sealed state, never a caller or
arena pointer.

```text
Linkerd output slices
  → one copy into the endpoint's arena batch
  → local SG-DMA or peer DATA custody
  → destination RX mapping
```

Tokens are validated within the owning connection's batch list. The first
accepted write fixes the endpoint's route; origin, local-backend, and
remote-backend endpoints hold independent batches even when they share one
request connection.

A write that finds its batch full, or no chunk available, accepts nothing and
parks the writer. An accepted write marks the endpoint dirty and raises the
worker's driver signal. `poll_flush` reports a latched transport error and
otherwise returns at once: accepted bytes already belong to the transport.
`poll_shutdown` closes admission; the driver publishes the buffered DATA and
only then the FIN.

Each worker runs one driver task on its runtime. A pass drains the C engine and,
for every endpoint that is dirty or owes a retry, publishes its batch with
`dmesh_l7_tx_batch_flush`, wakes a writer parked without a buffered batch,
returns consumed staging custody, and publishes an ordered FIN once the
endpoint has shut its write half with nothing buffered. A refused publication
keeps the batch and its bytes for a later pass and never copies again. A pass
visits at most 64 sessions and 256 KiB of published output before yielding to
the stack. A pass that finds no work arms the completion and DMA notifications,
drains once more, and waits on those descriptors, the wake eventfd, the driver
signal, and a 1 ms maintenance deadline held by one timer entry; after a wait,
only the sources that fired are cleared. Abort and C connection close reclaim
unpublished batches. A published chunk belongs to its DMA unit or peer stream
until the completion path returns it.

The egress arena holds 1,024 chunks. Admission returns the accepted byte count,
zero for retry, or a terminal error. A successful flush transfers the whole
batch and never a prefix.

### 4.4 Routing and authorization

Linkerd selects one of four exact output routes:

| Route | C data-plane action |
|---|---|
| `Any` | select an eligible backend of the session Service |
| `Origin` | send the response to the request source |
| `Local(pod)` | DMA to the live registered Pod selected by Linkerd |
| `Remote(uid)` | open or reuse the stream pinned to that topology Pod UID |

Endpoint addresses are resolved through the signed topology generation. A local
Pod UID must map to a live registration. A remote Pod UID must map to an
authenticated peer node. Stale, missing, or mismatched endpoints fail closed.
Inbound policy is evaluated against the destination Pod and Service reached by
the selected route.

### 4.5 Adapter ABI

DPUmesh calls Linkerd through these worker-confined entry points:

| Entry point | Contract |
|---|---|
| `l7_conn_open` | create the named session or return a stable decline code |
| `l7_conn_segment` | accept a staging prefix and retain its custody |
| `l7_conn_eof` | close one input half |
| `l7_conn_close` | abort tasks and release held staging |
| `l7_inbound_verdict` | admit or refuse a destination-side stream |
| `l7_inbound_forget` | remove policy subjects for a withdrawn Pod |

Linkerd calls the C data plane through:

| Entry point | Contract |
|---|---|
| `dmesh_l7_tx_batch_write` | append scalar/vectored bytes to a tokenized arena batch |
| `dmesh_l7_tx_batch_flush` | publish the complete batch to its exact route |
| `dmesh_l7_tx_batch_cancel` | return an unpublished batch |
| `dmesh_l7_tx_fin` | publish one ordered output FIN |
| `dmesh_l7_release` | release consumed staging custody |
| `dmesh_l7_session_failed` | terminate a session without orderly output completion |
| `dmesh_l7_workloads` | enumerate live inbound policy subjects |
| topology helpers | verify feeds and resolve Service and Pod identifiers |

## 5. Cross-node path

### 5.1 Channel and stream

One peer transport per ARM worker carries multiple application streams. The
wire is an ordered sequence of `STREAM_OPEN`, `STREAM_OPEN_ACK`, `DATA`,
`STREAM_FIN`, `STREAM_ACK`, and `POD_GONE` frames. Handles use disjoint owner
namespaces, and every frame carries a channel incarnation.

The peer TLS 1.3 session mutually authenticates node static keys. The held
topology generation binds the claimed node name, address, and public key. A peer
node remains untrusted input: frame lengths, handles, Pod UIDs, Service keys,
sequence numbers, open rate, stream count, staging bytes, and unacknowledged
bytes are validated and bounded.

`STREAM_OPEN` carries source and destination Pod UIDs and the source Service
key. The destination accepts it only when its topology and live registration
state support those claims and its inbound policy admits the stream.

### 5.2 Delivery custody

DATA carries one extent of at most 64 KiB. The source retains its staging piece
or L7 arena chunk in an unacknowledged slot. The destination acknowledges DATA
only after local DMA completes and `REV_DONE` is published to the destination
Pod. Consecutive acknowledgements are batched without weakening that boundary.

A channel bounds streams, staging bytes, open rate, unacknowledged slots, and
unacknowledged bytes. Exhaustion parks the source stream. Peer failure poisons
its streams, returns locally held resources through terminal completion paths,
and never redirects payload to plaintext transport.

### 5.3 Carrier interface

The carrier moves whole TLS-ciphertext messages and exposes connection,
progress, send, receive, status, close, and poll-descriptor operations. TCP and
RDMA implement the same message contract. A carrier definition is valid only
when each optional lease group is complete:

```text
TX: tx_reserve + tx_commit + tx_cancel
RX: rx_acquire + rx_release
```

The RDMA carrier owns one protection domain, completion queue, and completion
channel for the RDMA device selected by its bind address. Each connection owns
a queue pair, one registered buffer, 16 SEND slots, and 32 RECV slots.

TX slots follow `FREE → RESERVED → POSTED → FREE`. Reservation returns a
writable registered pointer and capacity. Commit posts one nonempty prefix as a
signaled SEND and consumes the lease. Cancel returns an unposted slot. Only the
matching successful SEND completion returns a posted slot.

RX slots follow `POSTED → READY → LEASED → POSTED`. Acquire dequeues the oldest
completed message. Release consumes the lease and reposts that exact slot. A
held RX lease blocks another acquire, preserving FIFO.

Each direction permits one outstanding lease per connection. A token validates
the process-unique connection epoch, slot, per-slot generation, direction,
pointer, and capacity or length. Invalid tokens cannot alter slot state.
Generation exhaustion makes the connection terminal. Valid commit, cancel, and
release calls clear their lease even when the operation detects a terminal
fault. Queue-pair destruction precedes memory deregistration.

The message operations use the same leases internally: transmit reserves,
copies, and commits; receive acquires, copies, and releases. TLS therefore sees
one whole-message interface while the carrier keeps registered-buffer ownership
explicit.

## 6. Observability

The Linkerd adapter exports counters that distinguish admission, buffering,
publication, and backpressure:

| Metric | Meaning |
|---|---|
| `tx_arena_copy_bytes` | bytes copied into arena batches |
| `tx_accepted_bytes` | bytes transferred to DMA or peer custody |
| `tx_publications` | batches transferred from endpoint ownership |
| `tx_retries` | writes refused by a full batch or a dry arena, and refused publications |
| `tx_errors` | terminal TX errors |

Worker metrics also expose drain/progress calls, task and queue depth, DMA
in-flight and retries, stalled connections, deferred receives, ACK/FIN backlog,
park state, and wake state. After quiescence, active sessions, live tasks, DMA,
unpublished arena batches, ACK backlog, and FIN backlog must be zero; all 1,024
arena chunks must be free.

## 7. Bounds and invariants

| Resource | Bound |
|---|---:|
| DPA execution units `N` | 1–32 |
| rings per Pod `K` | 1–16 |
| ARM data workers `A` | 1–16 |
| Pod table | 127 |
| transport unit | 8 KiB |
| TX/RX mapping | 64 MiB each |
| QP TX window | 4 MiB |
| egress arena | 1,024 × 64 KiB |
| L7 custody per connection | 256 KiB |
| peer channels per worker set | 256 |
| peer streams per channel | 4,096 |
| peer DATA extent | 64 KiB |
| peer unacknowledged bytes | 16 MiB |
| RDMA SEND/RECV slots per connection | 16 / 32 |

The implementation preserves these invariants:

1. bytes are delivered in QP order and FIN follows all accepted DATA;
2. a capacity credit is returned only after its custody boundary completes;
3. publication runs on the owning worker under the endpoint lock and never
   re-enters the endpoint;
4. retries preserve accepted bytes and do not duplicate payload;
5. generation and token checks isolate every reusable slot and connection;
6. teardown revokes stack references before returning mapped memory;
7. authentication, topology, routing, or transport failure closes the affected
   scope without opening a kernel or plaintext fallback.
