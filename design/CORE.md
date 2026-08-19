# DPUmesh Core Architecture

DPUmesh is a BlueField transport. A sending application writes into host memory
that the DPU has mapped, the DPU's data-path accelerator reads it, and a DPU CPU
thread writes it into the receiving application's mapped memory.

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
| staging | DPU memory holding arrived bytes until they have been sent onward |
| custody | the rule that arrived bytes stay valid, and their sender stays uncredited, until released |
| landing stripe | one disjoint region of the receiving pod's RX mapping, written by one worker |
| lane | the queue of pending deliveries for one (destination pod, landing stripe) pair |
| egress arena | DPU buffers holding bytes the L7 layer produced, until DMA sends them |

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

A connection's routing is resolved once. At L4 the connection keeps one backend
for its life. A service assigned to the L7 layer instead hands that layer the
arrived byte ranges and lets it name the backend per delivery;
`design/L7.md` describes that path.

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

## Bounds

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
