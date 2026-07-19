# DPUmesh Core Architecture

This whitepaper describes the 2026-07-19 implementation across
`src/dmesh_core.c`, `doca/`, and `doca/device/dpa_kernel.c`. It separates the
host API, the BlueField ARM control/data path, and the DPA forwarding kernel, and
states the lifecycle barriers that make imported host memory safe to reclaim.

## 1. End-to-end structure

```text
Host process                         BlueField                     backend
┌──────────────────┐       ┌────────────────────────────┐      ┌─────────┐
│ native/preload   │       │ ARM control + proxy        │      │ TCP app │
│ channel/CQ/QP    │Comch  │ registration, conntrack,   │ TCP  └─────────┘
│ registered TX/RX ├──────►│ routing, SG-DMA egress     ├──────────►
└────────▲─────────┘       └──────────▲─────────────────┘
         │ host RX DMA               │ forward completion
         └───────────────────────────┤
                                     │
                               ┌─────┴─────┐
                               │ DPA EUs   │
                               │ TX DMA +  │
                               │ ring ACKs │
                               └───────────┘
```

Host-to-DPU request bodies are read by DPA EUs from registered host TX memory.
The DPU ARM proxy owns backend TCP connections. Replies are copied by ARM SG-DMA
into the destination host RX mmap. The DPA is forward-only; it does not implement
the reverse path.

## 2. Host transport

### Channel and progress

`struct dpumesh_ctx` owns the DOCA device, Comch client, K exported forward
rings, TX block pool, RX memory, CQ registry, accept queue, and one PE progress
thread. Public `dmesh_channel_t` wraps this context and the DPU-assigned pod id.

The progress thread is the only Comch PE owner. It receives control messages and
data completions, publishes QPs onto their CQ ready-lists, and signals the CQ
eventfd only on empty-to-nonempty transitions. Each CQ ready-list is
single-producer/single-consumer: the progress thread produces and the CQ's one
application thread consumes.

### TX storage and forwarding rings

Each QP reserves bytes from an elastic chain of 64 KiB registered blocks. The
default per-QP ceiling is four blocks (256 KiB in flight), with one recycled
block retained as hysteresis. `dmesh_alloc()` reserves a block region and one
send unit together, so post cannot later fail for per-QP capacity. A shared
descriptor ring can still apply bounded backoff at doorbell time.

K forward rings shard QPs by connection. All bytes for one QP use one ring, while
different QPs can occupy different DPA EUs. The ring uses ticket generations in
the cell sequence, so a producer cannot overwrite a cell until the DPA has
consumed the prior generation. The default K is two.

### RX and completions

The RX mmap is divided into 4,096 slots of 8 KiB by default. A `RECV` completion
holds a slot credit until `dmesh_wc_release()`. Per-QP inboxes preserve ordering;
the CQ ready-list avoids a global scan. `CONN_REQ` QPs are assigned to whichever
CQ drains the shared accept queue, then remain bound to that CQ.

### Concurrency invariants

- Exactly one thread progresses the host Comch PE.
- Exactly one application thread polls a given CQ.
- QP completions never migrate between CQs.
- A poll batch is processed fully before freeing any QP referenced by that batch.
- The application does not access TX bytes after post or RX bytes after release.

## 3. Control protocol

DOCA Comch carries lifecycle control. The current host/DPU message sequence is:

```text
Host                                      DPU ARM
 |---- registration + exported handles ---->|
 |<--------------- POD_ASSIGNED -------------|  slot reserved
 |                                            |  import K rings and TX/RX mmap
 |                                            |  register rings on target EUs
 |                                            |  wait RING_ADD_ACK generation G
 |                                            |  initialize ARM egress lanes
 |<------ POD_INIT_RESULT(READY, G) -----------|
 |=============== data path ==================|
 |---------------- POD_UNREGISTER ------------>|
 |                                            |  registered=0, dma_ready=0
 |                                            |  RING_DEL to all target EUs
 |                                            |  wait matching RING_DEL_ACK
 |                                            |  drain egress queues/inflight/refs
 |<--------------- POD_QUIESCED --------------|
```

`POD_ASSIGNED` is phase one only. `POD_INIT_RESULT` carries `READY` or an explicit
registration, mmap, or DPA failure. The host waits up to 30 seconds for both
phases. It never exposes a partially initialized channel.

Every slot incarnation has a monotonically increasing `dma_generation`.
`RING_ADD_ACK`, `RING_DEL_ACK`, DPA forwarding completions, and ARM egress
operations carry that generation. A completion with a stale generation is
dropped instead of being applied to a reused slot.

## 4. DPA forwarding

At startup the ARM side selects N available DPA execution units, clamped to
`MAX_DPA_EU=8`. If `DPUMESH_DPA_THREADS` is unset, `dpa.c` uses the available-EU
query and the configured default/clamp behavior; a deployment may set N
explicitly. Each EU can hold up to eight registered forward rings.

An EU loop performs three operations:

1. Process ring add/delete control and return a generation-tagged ACK.
2. Drain valid host descriptors from its registered rings.
3. DMA request bytes into DPU memory and publish a forwarding completion to ARM.

The kernel validates ring slot, pod id, and generation. Parking/re-arm logic keeps
idle use bounded while a grace polling interval avoids a large hot-path wakeup
penalty.

## 5. BlueField ARM proxy

The ARM process owns service configuration, pod objects, backend endpoints,
load-balancing state, connection tracking, and the reverse SG-DMA engine.

```text
DPA completion
      │
      ▼
ingest/reap shard → parse or passthrough → backend socket
      ▲                                      │ reply
      │                                      ▼
REV_DONE/credit ← SG-DMA completion ← egress lane/worker
```

`DPUMESH_INGEST_SHARDS` selects up to eight ingest shards. QPs are mapped
deterministically so their conn state remains shard-local. With share-nothing
routing, each shard owns its conntrack and upstream-port residue; shared routing
uses a lock around the common table.

`DPUMESH_ARM_EGRESS_THREADS` selects one inline egress path or multiple workers,
up to eight. Each worker owns its DOCA DMA context, PE, inventory, batch pool, and
a partition of destination lanes. Lane records carry the destination pod
generation. DMA completion returns byte custody and emits reverse-done credit to
the source side.

The default proxy mode is raw passthrough. Per-service frame/L7 codecs may expose
message boundaries, but they are not used for gRPC. Feeding HTTP/2 through the
current simple frame codec would corrupt the stream because it is not an HTTP/2
parser.

## 6. Graceful remote reclaim

Channel shutdown first requires all host QPs and CQs to be gone. The host then
sends `POD_UNREGISTER` while its PE remains live. The DPU performs these ordered
steps:

1. Clear `registered` and `dma_ready`, preventing new routing and DMA submissions.
2. Flush stale generation-tagged forwarding completions.
3. Send ring delete to every EU used by the pod and collect matching ACKs.
4. Drain or discard every ARM egress lane item, DMA batch, inflight counter, and
   source-window custody reference for that generation.
5. Destroy DOCA buffer arrays before their imported mmaps.
6. Send `POD_QUIESCED` and release the pod slot.

Only after receiving the barrier does the host stop Comch and destroy exported
memory and device objects. If the five-second wait expires, the host logs a
warning and continues bounded teardown; the DPU continues asynchronous cleanup.
Unexpected disconnect also enters asynchronous cleanup and has been shown to
recover liveness and slot reuse. Forced death while DMA is already in flight is
not equivalent to the graceful barrier and is not claimed to provide complete
memory-isolation safety.

## 7. Name resolution and facades

`src/dmesh_resolve.c` loads one immutable table on first use. A row is:

```text
ClusterIP:port service-name service-id
```

The native facade resolves a peer name. The preload facade resolves a socket
destination and falls back to kernel TCP when the row is absent. Both facades
call the same internal core; preload is not layered on the public native API.

The C++ gRPC integration is a native API consumer. Its runtime owns one channel,
reactors own CQs, and each gRPC EventEngine Endpoint owns a DPUmesh QP. Endpoint
reads copy RX bytes into gRPC slices before returning credits; writes copy gRPC
slices into registered TX reservations.

## 8. Configuration and hard bounds

| Item | Default or bound | Meaning |
|---|---:|---|
| RX slot size | 8 KiB | One DPA DMA unit and one `RECV` fragment |
| RX slots | 4,096 | Channel landing capacity |
| TX block | 64 KiB | Largest default contiguous alloc/post |
| TX blocks/QP | 4 | 256 KiB default in-flight cap |
| K rings/pod | 2 | QP sharding across EUs |
| N DPA EUs | auto-detected, fallback 4, max 8 | Forward executors; deployment can override |
| Ingest shards | max 8 | ARM completion/routing workers |
| Egress engines | max 8 | ARM SG-DMA workers |
| Pod slots | 32 at default K | Derived from EU/ring capacity |

Host and DPU must agree on K and structure layout. ABI-incompatible public header
changes require incrementing `ABI_MAJOR` so the SONAME prevents silent layout
mismatch.

## 9. System invariants

- L4 mode never assumes application message boundaries.
- One QP's bytes remain ordered, and a connection is not silently repicked after
  backend failure.
- All remote references to a host export quiesce before graceful host unmap.
- Every cross-pod DMA operation is validated against the current slot generation.
- Every `RECV` credit is returned exactly once; the release API is idempotent.
- Backpressure is reported as `EAGAIN`, not converted into an unbounded wait.
- Routine logging is warning-level; high-volume diagnostic logging is explicit.
