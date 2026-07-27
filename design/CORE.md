# DPUmesh Core Architecture

DPUmesh is a BlueField transport with registered host memory, DPA forwarding,
ARM routing, and DPU-initiated SG-DMA.

## Data path

```text
host QP
  │
  ▼
forward ring (source port % K)
  │
  ▼
DPA EU
  │ completion metadata
  ▼
ARM worker (connection owner)
  ├─ connection and conntrack state
  ├─ L4 routing or framed L7 load balancing
  ├─ payload SG-DMA and DMA completion
  └─ reverse ring publication
          │ REV_DONE / TX_ACK
          ▼
host PE progress thread

DPU main thread: registration, teardown, and host doorbells
```

`N`, `K`, and `A` denote DPA EUs, rings per pod, and ARM data workers.

```text
1 ≤ A ≤ K ≤ N
K % A = 0
N % A = 0
EU % A = ring % A = port % A
```

A connection remains on one ARM worker. A worker owns its connection tables,
DPA completion PE, SG-DMA engine, DMA completion callbacks, destination lanes,
and reverse-ring producers.

## Host

One `dpumesh_ctx` owns the Comch client, K forward rings, K reverse rings, the
registered TX and RX mappings, the TX block pool, EQ registry, and PE progress
thread.

Each QP is a full-duplex ordered byte stream bound to one EQ. Its TX cursors
maintain:

```text
free ≤ sent ≤ committed ≤ write
```

`dmesh_alloc` reserves registered bytes. `dmesh_post_send` commits complete
8 KiB transport units, and `dmesh_flush` publishes a trailing partial unit.
`TX_ACK` advances `free`. `REV_DONE` creates RX, FIN, and accept events.

TX capacity pressure returns `EAGAIN` and arms one
`DMESH_EVENT_TX_READY` transition. RX landing credits return when the
application releases the receive buffer.

## Forward rings and DPA

Each forward ring is a bounded MPSC queue. A host producer reserves a monotonic
ticket, writes one descriptor, and publishes `publish_seq = ticket + 1`. The DPA
consumes consecutive tickets and publishes one `consumer_head` after each
drain.

Each EU processes ring control, copies request bytes into DPU staging, and sends
completion metadata to the ARM worker selected by the connection owner mapping.
Pod generation and descriptor sequence fields reject stale asynchronous work.

## ARM workers

One worker iteration:

1. consumes up to 64 DPA completions;
2. processes connection parsing and routing;
3. submits and progresses SG-DMA;
4. retires completed destination lanes;
5. emits `REV_DONE` and exact per-sequence `TX_ACK` entries;
6. publishes pending reverse-ring entries.

Same-owner lanes use a private FIFO. Cross-owner delivery and ACK custody use
bounded MPSC queues. Idle workers wait on their DPA PE, DMA PE, and worker event
descriptors.

L4 selects one backend for the lifetime of a connection. Services listed in
`DPUMESH_PROXY_L7_SVC` use a 16-byte framed codec and select a ready backend per
complete frame. Response bytes remain ordered within the client stream.

## Reverse rings and host wake

Each region has one worker producer and one host consumer. A 32-byte reverse
slot contains `REV_DONE` or `TX_ACK` and becomes visible when:

```text
publish_seq = consumer ticket + 1
```

The worker publishes all entries accumulated during its current iteration.
The host drains visible entries and writes one monotonic `consumer_head`.

Before blocking, the host increments `arm_epoch` and rechecks the rings. A
worker reads the reverse control cache line after publication. A new armed epoch
requests one Comch doorbell through the DPU main thread.

## Registration and teardown

```text
Host                                      BlueField ARM / DPA
 |-- POD_REGISTER -------------------------->|
 |<---------------- POD_ASSIGNED ------------|
 |-- forward rings, TX/RX, reverse rings --->|
 |                              RING_ADD to EUs
 |<----------- POD_INIT_RESULT(READY) --------|

 |-- POD_UNREGISTER ------------------------>|
 |                              stop routing
 |                              RING_DEL to EUs
 |                              drain ARM DMA and reverse publishers
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
| TX block | 512 KiB |
| QP TX window | 8 blocks / 4 MiB |
| Send-unit reclaim FIFO | 512 entries |
| Forward ring | 4,096 descriptors |
| Forward descriptors per ring round | 32 |
| Reverse ring | 8,192 entries |
| Reverse entry | 32 B |
| Reverse publication stage | 64 entries |
| DPA EUs | automatic 16, explicit maximum 32 |
| Rings per pod | default 2, maximum 8 |
| ARM data workers | default 1, maximum 8 |
| L7 frame | 128 KiB |

The implementation preserves per-connection order, exact TX and RX custody,
one reverse-ring producer, generation-safe teardown, and bounded nonblocking
backpressure.
