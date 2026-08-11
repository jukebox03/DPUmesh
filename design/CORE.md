# DPUmesh Core Architecture

DPUmesh is a BlueField transport with registered host memory, DPA forwarding,
ARM routing, and DPU-initiated SG-DMA.

## Topology

`N`, `K`, `A`, and `L` denote DPA EUs, rings per pod, ARM data workers, and RX
landing stripes.

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
  ├─ connection and conntrack state
  ├─ L4 routing or framed L7 load balancing
  ├─ payload SG-DMA
  └─ reverse publication
          │
          ▼
L reverse rings
          │ REV_DONE / TX_ACK
          ▼
host PE progress thread

DPU main thread: registration, teardown, and host doorbells
```

## Host memory and rings

One `dpumesh_ctx` owns K forward rings, K reverse rings, registered TX/RX
mappings, the TX block pool, EQ registry, PE progress thread, and a tail timer
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
once every landing it holds has been released.

## DPA and ARM execution

Each forward ring is a bounded MPSC queue. A producer reserves a monotonic
ticket, writes one descriptor, and publishes `publish_seq = ticket + 1`. The DPA
consumes consecutive tickets, copies request bytes into DPU staging, and sends
completion metadata to the connection owner.

An ARM data worker polls its own completion handles. One iteration:

1. consumes up to 64 DPA completions;
2. parses and routes connection data;
3. submits and progresses SG-DMA;
4. retires completed destination lanes;
5. emits `REV_DONE` and exact per-sequence `TX_ACK` entries;
6. publishes reverse-ring entries.

A worker stays hot while an iteration advances work. Otherwise it arms its DPA
and SG-DMA completion handles, rechecks, and blocks on them, its cross-worker
eventfd, and a 1 ms interval. The 1 ms keepalive wakes only EUs serving at
least one forward ring; a ringless EU parks until a control message arrives.

Same-owner lanes use a private FIFO. Cross-owner delivery and ACK custody use
bounded MPSC queues. L4 selects one backend for a connection. Services in
`DPUMESH_PROXY_L7_SVC` use 16-byte frames and select a ready backend per frame.

## DMA fault handling

Each ARM worker uses one DMA context shared by its pods. `IO_FAILED` stalls and
restarts that context without changing pod readiness. A failed payload batch
whose destination generation is still current is retried once at the head of
its lane FIFO. The exclusive retry preserves lane order.

Pod readiness is controlled by the registration connection. Disconnect cleanup
removes the pod from routing, clears DMA readiness, drains worker custody, and
destroys imported mappings.

## Reverse publication

Each landing stripe has one reverse ring, one worker producer, and one host
consumer. A 32-byte slot contains `REV_DONE` or `TX_ACK` and becomes visible
when:

```text
publish_seq = consumer ticket + 1
```

The host drains visible entries and writes one monotonic `consumer_head`. Before
blocking, it increments `arm_epoch` and rechecks the rings. After a publication
the producing worker reads that control block and, on a new epoch, requests a
doorbell that the DPU main thread sends as one Comch message.

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
 |                              drain DMA and reverse publishers
 |                              destroy imported mappings
 |<---------------- POD_QUIESCED -------------|
```

Control messages are idempotent. Pod generations bind imported mappings, DPA
rings, and asynchronous completions to one registration. The host retains its
exports until `POD_QUIESCED`.

## Bounds

| Item | Value |
|---|---:|
| Transport unit | 8 KiB |
| Host TX mapping | 8,192 units / 64 MiB |
| Host RX mapping | 8,192 units / 64 MiB |
| RX landing stripes | L = A |
| TX block | 512 KiB |
| QP TX window | 8 blocks / 4 MiB |
| Forward ring | 4,096 descriptors |
| Reverse ring | 8,192 entries |
| Reverse entry | 32 B |
| DPA EUs | automatic 16, maximum 32 |
| Rings per pod | default 2, maximum 8 |
| ARM data workers | default 1, maximum 8 |
| Payload DMA retries | 1 |
| L7 frame | 128 KiB |

The implementation preserves per-connection order, exact TX/RX custody,
single-producer reverse rings, generation-safe teardown, and bounded
nonblocking backpressure.
