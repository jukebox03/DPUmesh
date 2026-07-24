# DPUmesh Core Architecture

This whitepaper describes the native API, preload facade, BlueField ARM process,
and DPA kernel. Each byte, descriptor, credit, connection, and imported mapping
has one owner until transfer or release.

## 1. Data path

```text
host QP/port
    │
    ▼
forward ring (port % K)
    │
    ▼
DPA EU (EU % A == ring % A)
    │
    ▼
ARM worker (port % A)
    ├── connection + conntrack
    ├── parse + route/LB
    └── SG-DMA submit + completion
             │
             ▼
       main Comch emitter
```

`N`, `K`, and `A` are the numbers of DPA EUs, forward rings per pod, and
homogeneous ARM data workers. A valid topology satisfies:

```text
1 ≤ A ≤ K ≤ N
K % A == 0
N % A == 0
```

The mapping preserves `EU % A == ring % A == port % A`. One connection remains
on one ARM worker in both directions.

## 2. Host ownership

One `dpumesh_ctx` owns the DOCA device, Comch client, K forward rings, TX block
pool, RX mapping, accept queue, EQ registry, and PE progress thread. The PE
thread owns Comch progress.

Each EQ has one application consumer, an SPSC ready list, and one eventfd for
accept, RX, FIN, and TX-ready events. A QP remains bound to one EQ.

Each QP has a logical TX byte ring backed by registered 64 KiB blocks. Four
monotonic cursors track custody:

```text
free ≤ sent ≤ committed ≤ write
```

`dmesh_alloc` reserves contiguous bytes. `dmesh_post_send` commits them and
emits complete 8 KiB transport units. `dmesh_flush` emits a trailing partial
unit. `BATCH_FWD_ACK` advances the free cursor and returns blocks.

TX backpressure returns `EAGAIN`. QP-window and shared-pool waiters use one-shot
`IDLE → ARMED → READY → IDLE` notification state. `DMESH_EVENT_TX_READY` is a
retry signal.

The RX mapping is divided into 8 KiB landing units. A `RECV` event transfers
temporary access to the application. `dmesh_release_rx_buffer` returns each
landing credit once.

## 3. Registration

```text
Host                                      BlueField ARM / DPA
 |-- POD_REGISTER(service, requested=-1) -->|
 |<-------------- POD_ASSIGNED --------------|
 |-- K ring exports, TX mmap, RX mmap ------->|
 |                                      RING_ADD generation G
 |                                     <----- ACK from target EUs
 |<---------- POD_INIT_RESULT(READY) ----------|
```

Registration messages are idempotent. The host retries registration every
100 ms and DPA ring installation every 10 ms. The channel becomes public after
all mappings are imported and every target EU acknowledges the active
generation. The initialization deadline is 30 seconds.

## 4. DPA forwarding

Each host QP selects `ring = source_port % K`. Ring placement uses:

```text
owner          = ring % A
owner_ring     = ring / A
rings_per_owner = K / A
eus_per_owner   = N / A
EU = owner + A × ((pod_id × rings_per_owner + owner_ring) % eus_per_owner)
```

Each EU owns one DPA thread and Comch channel. It processes ring control, drains
forward descriptors, copies request bytes into DPU staging, and publishes
completion metadata to its ARM worker.

Ring control and completion records carry pod generation and descriptor
sequence. Generation and sequence checks reject stale records.

## 5. ARM workers

Each ARM worker owns:

- one DPA consumer PE and completion queue;
- one connection-table and conntrack shard;
- parser and routing state;
- destination regions where `region % A == worker`;
- one SG-DMA engine and PE;
- one TX-ACK SPSC ring and one completed-unit SPSC ring.

One worker iteration reaps up to 64 DPA completions, retries stalled parsers,
submits SG-DMA, progresses DMA completions, and retires destination lanes. Idle
workers wait on DPA PE, DMA PE, and worker event descriptors.

DPU-created upstream ports satisfy `upstream_port % A == owner`, preserving
reply ownership. Cross-worker completion transfer uses a bounded MPSC safety
queue.

The main ARM thread owns Comch sends, `REV_DONE` batching, custody release, and
pod lifecycle control. Shared-pool returns wake workers recorded in an atomic
waiter mask.

## 6. Routing

L4 assigns each connection to one ready backend. Backend loss terminates that
pinned stream.

Services in `DPUMESH_PROXY_L7_SVC` use a 16-byte framed codec with a 128 KiB
frame limit. Each complete request frame selects a ready backend through an
atomic round-robin cursor. Backend selection does not migrate connection state.
Responses enter the client byte stream in backend completion order.

Request and response bodies remain scatter/gather views over DPU staging until
SG-DMA retirement releases custody.

## 7. Teardown

```text
Host                                      BlueField
 |-- POD_UNREGISTER ------------------------>|
 |                                  stop routing
 |                                  RING_DEL to target EUs
 |                                  wait generation ACKs
 |                                  drain worker lanes and DMA
 |                                  drain main pending emissions
 |                                  destroy imported mappings
 |<---------------- POD_QUIESCED --------------|
```

`POD_UNREGISTER` is idempotent. The host retains Comch and exported mappings
until `POD_QUIESCED`. Each worker contributes one quiesced bit for the pod.
The host then destroys DPA buffer arrays, imported mappings, exports, and device
objects in order.

## 8. Bounds and invariants

| Item | Default / bound |
|---|---:|
| Wire slot and RX landing unit | 8 KiB |
| Host TX/RX capacity | 4,096 slots each |
| TX block | 64 KiB |
| QP TX window | 4 blocks, FIFO-clamped |
| Send-unit reclaim FIFO | 64 descriptors/QP |
| Forward rings per pod | 2 default, max 8 |
| DPA EUs | auto max 16, explicit max 32 |
| ARM data workers | 1 default, max 8 |
| L7 parse head | 4 KiB |
| L7 frame | 128 KiB |
| Initialization deadline | 30 s |
| Graceful local cleanup deadline | 5 s |

The implementation maintains:

- one host PE owner and one consumer per EQ;
- one ARM owner per connection direction;
- ordered transport units within each QP;
- exact TX block and RX landing custody;
- generation checks on asynchronous DPA and ARM results;
- remote quiescence before host mapping destruction;
- `EAGAIN` for bounded resource backpressure.
