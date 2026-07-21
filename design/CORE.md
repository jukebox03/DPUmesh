# DPUmesh Core Architecture

This whitepaper describes the current implementation shared by the native API,
preload facade, BlueField ARM process, and DPA kernel. Its organizing
principle is custody: every byte, descriptor, credit, and imported mapping has a
single owner until an explicit acknowledgement transfers or releases it.

## 1. Data-plane shape

```text
host process                         BlueField
┌────────────────────┐     ┌──────────────────────────────┐
│ channel / CQ / QP  │     │ ARM control + stream proxy   │
│ registered TX mmap ├─DPA►│ backend sockets / conntrack  ├── TCP backend
│ registered RX mmap │◄─DMA┤ SG-DMA reverse egress        │
└────────────────────┘     └──────────────▲───────────────┘
                                          │
                                   DPA execution units
                              forward-ring drain + TX DMA
```

Host-to-DPU bodies are DMA-copied from host TX memory into DPU staging by DPA
execution units. BlueField ARM owns service selection, upstream connections, and
response routing. The ARM proxy gathers frame bodies directly from those staging
extents into SG-DMA operations, without a second body copy. Responses land in the
destination host RX mapping and remain there until the application releases the
completion. This is zero-copy at the native RX boundary and staging-to-egress
gathering, not an end-to-end zero-copy claim. The DPA path is forward-only.

## 2. Host ownership and concurrency

One `dpumesh_ctx` owns the DOCA device, Comch client, K forward rings, TX block
pool, RX mapping, accept queue, CQ registry, and PE progress thread. The PE
thread is the sole Comch progress owner. Each CQ has an SPSC ready-list: the PE
publishes a port once, and the CQ's one application thread drains it.

The CQ also owns one eventfd shared by accept, RX/FIN, and TX-ready transitions;
there is no per-QP fd.

Per-QP inboxes preserve arrival order without scanning the global port table.
An atomic arm/disarm handshake closes the empty-inbox/ready-edge race. A QP
never migrates between CQs, and an application must finish dispatching a returned
CQ batch before freeing any QP named by it.

TX readiness uses a separate atomic bitmap because its producers include both the
PE ACK path and application-owner threads returning blocks to the shared pool.

The accept path creates a `SERVER_PENDING` port before publishing the first
request. The public QP allocation now occurs before consuming the shared accept
descriptor, so host OOM cannot consume the request and strand that pending port.

## 3. TX byte ring and automatic physical batching

Each QP owns a logical byte ring backed by an elastic chain of registered physical
blocks. Four monotonic cursors describe custody:

```text
free (ACKed) ≤ sent (described) ≤ committed (posted) ≤ write (reserved)
```

The default block is 64 KiB and the default QP ceiling is four blocks. Messages
never straddle physical blocks; unused block tails are padding. The shared block
pool lets idle QPs retain no eager TX allocation. The ceiling is a fixed bound:
the implementation grows a QP's physical chain on demand up to that bound, but
does not dynamically tune the configured number of blocks per QP. Blocks are
storage and reclamation units; 8 KiB descriptors are the wire transport units.

The public send state machine is:

1. `alloc` reserves a contiguous range and records its exact mmap offset.
2. `post_send` validates that exact pointer, commits it, and emits every newly
   complete transport unit.
3. `flush` emits the newest partial unit as well.
4. `BATCH_FWD_ACK(port, seq)` advances the free cursor and recycles blocks.

Adjacent small posts naturally share a physical descriptor. The current wire
unit is 8 KiB, but the public send policy does not depend on callers knowing it.
A short physical-block tail that can no longer grow is emitted before later-block
bytes to preserve order. Configuration clamps
`max_blocks_per_qp × ceil(block/slot)` to the
64-entry send-unit FIFO, so the complete committed window has reclaim metadata
before it is admitted. A shared forward ring still uses bounded backoff as a
fail-safe against a dead DPA consumer.

Emission writes a descriptor and advances the producer state of a shared ring.
The DPA execution unit already drains that ring, so neither a complete-unit post
nor an explicit tail flush performs a send syscall or emits a separate control
doorbell. Automatic batching is byte coalescing inside descriptors.

A reservation that needs another physical block probes both admission sources
before changing the write cursor or sealing a padded tail. It first verifies that
the QP block window can advance, then preclaims a recycled or shared physical
block. Either `EAGAIN` path is therefore a true no-op on the logical byte ring;
retrying the same body cannot inherit padding from a failed attempt.

No partial-batch timer exists yet. Until one is added, `flush` and graceful close
are the bounded-latency boundaries for the newest partial.

Descriptor sequence numbers and FIN state are published only after enqueue
succeeds. FIN failure is returned to the caller instead of being silently
latched as sent. Local close still releases the QP so channel teardown can
continue and DPU disconnect cleanup remains available.

### TX writable notification

Every QP carries a one-shot `IDLE → ARMED → READY → IDLE` state and a wait
reason. `dmesh_alloc(EAGAIN)` installs the arm internally:

1. a QP-window failure snapshots the free-block boundary and write cursor;
2. a shared-pool failure snapshots the pool epoch and registers the port in a
   channel-wide waiter bitmap;
3. the state is release-published as `ARMED`; and
4. the relevant capacity source is rechecked after publication.

The post-arm recheck is part of the correctness contract. An ACK or block return
that races with the failed allocation either observes `ARMED` and performs the
transition, or changes the snapshot inspected by the recheck. Capacity cannot
return in the gap and leave a sleeping owner without an event.

A QP-window arm becomes ready only when ACK progress crosses a physical-block
boundary or the QP fully drains. ACKs inside the same block do not cause a false
wakeup, because they cannot back the next logical block. A returned shared block
increments a pool epoch and claims at most one live waiter from the round-robin
bitmap: one returned block advertises one retry opportunity, not writability for
every blocked QP.

A successful transition sets the QP bit in its CQ's multi-producer bitmap and
writes the CQ eventfd if notification was requested. `dmesh_poll_cq` consumes that
bit as `DMESH_WC_TX_READY` before bulk established-RX work, avoiding TX starvation
on a read-heavy CQ. The completion is a retry hint, not a block reservation.
`dmesh_alloc` returning `EAGAIN` again rearms the QP. An opportunistic successful
retry, QP close, or CQ unbind cancels an obsolete state and removes any stale bit.

## 4. RX custody

The host RX mmap is divided into 8 KiB landing units. ARM emits batched reverse
completion records; the PE routes each record to a QP inbox or the shared accept
queue. A `RECV` completion transfers temporary read access to the application.
`wc_release` returns the landing credit exactly once, allowing that offset to be
reused.

RX queue capacity and reverse admission are coupled. The transport never treats
dropping an RX completion as ordinary flow control: loss of a credit or body is a
correctness fault, not a throughput policy.

Reverse records carry a per-destination-QP sequence. Several adjacent landing
fragments may share one sequence when they belong to the same delivery unit. The
host accepts that adjacent run once and ignores stale notification replays without
returning the original landing credit twice. FIN participates in the same replay
filter.

## 5. Registration and readiness

Initialization has two externally visible phases but one success result:

```text
Host                                      BlueField ARM / DPA
 |-- POD_REGISTER(service, requested=-1) -->|
 |<-------------- POD_ASSIGNED --------------|
 |-- K ring exports, TX mmap, RX mmap ------->|
 |                                      RING_ADD generation G
 |                                     <----- ACK from every EU
 |<---------- POD_INIT_RESULT(READY) ----------|
```

The control state machine is designed for replay:

- One Comch connection owns at most one registration.
- Repeating the same `POD_REGISTER` is idempotent and returns the same pod id.
- The host retries registration every 100 ms until assignment and again until a
  terminal initialization result.
- A replay after terminal state resends both assignment and result.
- Missing DPA `RING_ADD_ACK`s trigger generation-matched, idempotent `RING_ADD`
  replay every 10 ms.
- Received assignment and result messages require exact wire sizes and matching
  pod identity.
- Comch receive-queue requests are capped to the device-reported maximum.

The channel becomes public only after every import and every target-EU ACK is
complete. Registration, mmap, and DPA failures are terminal. The host deadline
is 30 seconds, preventing an indefinitely half-initialized channel.

Replay is confined to initialization. Once READY is delivered there is no
registration heartbeat or periodic control-plane poll; ordinary progress comes
from the data and completion rings.

## 6. DPA forwarding

K forward rings per pod shard QPs by source port. Every QP remains on one ring,
preserving its send order; different QPs spread across execution units. Ring
cells carry producer generations so a host producer cannot overwrite a cell
until its previous occupant has been consumed.

Each DPA EU processes ring add/delete control, drains valid descriptors, copies
request bytes into DPU staging, and publishes completion metadata to ARM. Ring
control ACKs include pod id, EU id, status, and DMA generation. Duplicate
`RING_ADD` overwrites the same logical registration rather than accumulating
another ring, which makes ACK recovery safe.

Forward completion metadata also retains the host descriptor sequence. ARM
tracks the latest accepted sequence per source QP and discards stale completion
replays before they can append the same staging extent twice. The sequence is
committed only after transient ingest allocation succeeds, so a backpressured
completion can be retried without being mistaken for a replay. A forward gap is
reported because one QP is expected to arrive in descriptor order.

## 7. ARM proxy and reverse egress

ARM owns the backend byte streams. In L4 mode a connection is pinned to one
backend. Services listed in `DPUMESH_PROXY_L7_SVC` instead use the in-tree L7
codec. The current codec recognizes a 16-byte header—magic, sequence, payload
length, and auxiliary value—and accepts complete frames up to 128 KiB. It is a
benchmark protocol, not an HTTP/2 parser; the gRPC path uses raw L4 passthrough.

The L7 request parser retains a bounded head across ingress extents, selects one
live backend for each complete frame, and records an internal upstream mapping.
The response parser recovers the client QP from that mapping. Request and response
bodies remain scatter/gather views over DPU staging. Each complete frame is one
egress unit and one DMA task, preventing neighboring frames from sharing a DMA
completion boundary. Replies from multiple upstreams receive one monotonically
increasing delivery sequence and are serialized into the client's single public
byte stream. Backend FIN releases only that internal upstream; client FIN fans out
behind queued data and closes the downstream connection.

A pinned stream terminates when its backend is lost. A new connection creates a
QP and selects from the current ready backend set.

Ingest shards own completion parsing and connection state. Egress workers own
destination lanes, DOCA DMA engines, and reverse batches. Every queued lane item
and DMA operation carries the destination pod generation. A stale completion can
therefore be discarded instead of touching a reused pod slot.

## 8. Remote reclaim

Graceful shutdown is another replayable barrier:

```text
Host                                      BlueField
 |-- POD_UNREGISTER (retry every 100 ms) ---->|
 |                                  stop new routing
 |                                  RING_DEL to every EU
 |                                  wait generation ACKs
 |                                  drain ARM lanes/inflight/refs
 |                                  destroy imported handles
 |<---------------- POD_QUIESCED --------------|
```

`POD_UNREGISTER` is idempotent. Missing delete ACKs are retried every 10 ms, and
a repeated unregister after cleanup causes a fresh quiesced reply. The host keeps
Comch and all exports alive while waiting. Only a received barrier authorizes the
normal unmap order: DPA buffer arrays, imported mappings, then host exports and
device objects.

The local five-second timeout is a bounded failure policy, not proof that remote
DMA stopped. Disconnect cleanup continues on the DPU. Complete containment after
SIGKILL or hardware failure requires a stronger platform isolation mechanism and
is not claimed here.

Local teardown joins the PE before freeing CQ state and releases each per-port
inbox and lazy reclaim array before the port table, so host-only and hardware
lifecycle checks observe the same ownership closure.

## 9. Fixed bounds and invariants

| Item | Default / bound |
|---|---:|
| Wire slot and RX landing unit | 8 KiB |
| Host TX/RX capacity | 4,096 slots each |
| TX block | 64 KiB default |
| QP TX window | 4 blocks default, FIFO-clamped |
| Send-unit reclaim FIFO | 64 descriptors/QP |
| Forward rings per pod | 2 default |
| DPA execution units | deployment-selected, max 8 |
| ARM ingest shards | deployment-selected, 1 default |
| ARM egress workers | deployment-selected, 1 default |
| L7 parse head | 4 KiB |
| L7 frame | 128 KiB |
| Initialization deadline | 30 s |
| Graceful local cleanup deadline | 5 s |

The implementation relies on these invariants:

- one PE owner and one consumer per CQ;
- per-QP wire order equals committed byte order;
- `post_send` emits complete units and `flush` emits the trailing partial;
- a failed reservation does not advance the QP write cursor or add block padding;
- one automatic TX-ready arm produces at most one queued retry hint;
- QP-window readiness requires reusable local capacity, while one pool return
  wakes at most one shared-pool waiter;
- every RX landing and TX block remains owned until its matching credit or ACK;
- stale forward and reverse notifications do not duplicate byte delivery or
  release custody twice;
- every asynchronous DPA/ARM result is validated against its slot generation;
- graceful unmap follows remote quiescence, never mere send submission;
- backpressure is surfaced as `EAGAIN`; batching size remains transport-private.
