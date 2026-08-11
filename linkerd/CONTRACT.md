# Interface Contract

Between the DPUmesh datapath and the linkerd port. The DPUmesh implementation is
normative: `doca/comch_common.h`, `doca/dpa_common.h`, `doca/ring.h`,
`doca/dpu_worker.c`, `doca/dpu_proxy.c`. The port conforms.

The port's own datapath (`port/DPUMesh/*.c`) is replaced by the DPUmesh
datapath. It remains in tree as the reference point for convergence.

## 1. Operating modes

One data plane carries every mode: payload lands in pod staging and is forwarded
by scatter-gather DMA, with no copy on the ARM. Modes differ in how far the L7
layer is involved, and are selected per service.

| Mode | L7 contribution | Bytes traverse the L7 layer |
|---|---|---|
| `decision` | authorization, discovery, endpoint choice, identity, telemetry | no |
| `opaque` | the above, plus mTLS | yes |
| `l7` | the above, plus HTTP routing, retries, timeouts | yes |

In `decision` mode the L7 layer answers one question per connection and receives
one report per close; payload never reaches it. §8 defines both calls. When the
L7 layer is unavailable the data plane falls open to its own load balancing,
without policy.

## 2. Thread model

DPUmesh runs N DPA execution units, K forward rings per pod, and A ARM worker
threads. A worker owns its connection table, conntrack, SG-DMA engine and
progress engine; nothing is shared across workers and no lock is taken between
them. Worker-local state is reached through `__thread px_cur_worker`.

The port therefore runs one tokio `current_thread` runtime per ARM worker. A
`multi_thread` runtime is excluded: work stealing moves a task across threads,
and worker-local state is not thread-safe.

The DPUmesh worker loop owns the iteration. It calls the L7 layer once per
revolution and folds the result into its own progress test, which governs
arming, parking and the 1 ms backstop:

```c
did = dpu_progress_worker_pe(...);
run = dpu_worker_run(...);
lnk = l7_worker_step(worker_id);
if (did || run || lnk) continue;
/* arm -> recheck -> epoll_wait(1 ms) */
```

The L7 layer exposes a single-step entry point rather than owning a loop of its
own. Connection slots are sized at attach time, not by a compile-time constant.
The L7 layer does not arm its own timer; the worker loop provides the backstop.

## 3. Control protocol

Host↔DPU control messages. Values are wire ABI below 256, and the type byte is
at offset 0: the host dispatches on `recv_buffer[0]`.

| Value | Message | Direction | Payload |
|---|---|---|---|
| 1 | `POD_REGISTER` | H→D | `pod_id` (−1 requests assignment), `service_id` |
| 2 | `MMAP_EXPORT` | H→D | one region, tagged by `mmap_type` |
| 5 | `POD_ASSIGNED` | D→H | `pod_id`, `landing_stripes` |
| 6 | `POD_INIT_RESULT` | D→H | terminal: ready, or failure cause |
| 7 | `POD_UNREGISTER` | H→D | stop routing, quiesce remote references |
| 8 | `POD_QUIESCED` | D→H | remote mappings reclaimed |
| 9 | `REV_DOORBELL` | D→H | reverse-ring wake |

`mmap_type`: `DMA_BUFFER=1`, `DMA_RING=2`, `DMA_HOST_RX_BUFFER=3`,
`DMA_REV_RING=4`. One message per region.

Sequence:

```
H→D  POD_REGISTER
D→H  POD_ASSIGNED
H→D  MMAP_EXPORT x { DMA_RING, DMA_BUFFER, DMA_HOST_RX_BUFFER, DMA_REV_RING x stripes }
D→H  POD_INIT_RESULT (ready)      traffic is admitted only after this
     ...
H→D  POD_UNREGISTER
D→H  POD_QUIESCED                 host exports are destroyed only after this
```

`POD_INIT_RESULT` reports ready only once all K forward rings, the host TX and RX
mappings, an installation acknowledgement from every target DPA execution unit,
and the ARM egress engine are in place. Teardown is a barrier, not a comch
disconnect: the host holds its exports until `POD_QUIESCED`.

## 4. Forward descriptor

Host→DPU. 64 bytes, one cache line, offsets fixed by static assertion.

| Offset | Field | Width | Note |
|---|---|---|---|
| 0 | `mmap` | 4 | DPA mmap handle |
| 4 | `addr` | 8 | |
| 12 | `size` | 4 | fixed width |
| 16 | `seq` | 2 | per-connection sequence |
| 18 | `src_port` | 2 | |
| 20 | `dst_port` | 2 | `PORT_BLANK` (0) selects the accept queue |
| 22 | `src_service` | 1 | |
| 23 | `dst_service` | 1 | routing input when the pod is blank |
| 24 | `dst_pod_id` | 4 | `POD_BLANK` (−1) defers to `dst_service` |
| 32 | `src_pod_id` | 4 | |
| 56 | `publish_seq` | 8 | ticket + 1 |

The ring is multi-producer. A producer claims a ticket, withdrawing it in reverse
order when the ring is full so that the published sequence stays gapless; it
writes the payload, then publishes `publish_seq = ticket + 1` with a release
store. The consumer position lives in a separate 64-byte control structure.
There is no producer tail and no validity flag.

## 5. Reverse completion ring

DPU→host. 32 bytes per entry, `publish_seq` at offset 24, ring size 8192.

```
kind(1) reserved(7) payload(16) publish_seq(8)

kind = DONE(1)    src_pod_id, src_service, dst_service, src_port, dst_port,
                  seq, length, pos                                    16 B
kind = TX_ACK(2)  port, seq                                            4 B
```

A 128-byte control structure on its own cache lines carries `consumer_head`,
which the host publishes after a drain batch, and `arm_epoch`, which the host
increments before blocking. `REV_DOORBELL` wakes a blocked host.

Completion notification and transmit-credit return share this ring. There is no
second reverse mechanism: a host-side DPA thread over a second PCI function and a
descriptor-push channel are both superseded.

## 6. Flow identity

The proxy requires the original destination, the peer address and the source
workload; DPUmesh routes on pod and service identifiers.

Workload identity arrives on its own control message, sent on the registration
connection before `POD_REGISTER`. The registration message is a fixed 12-byte
struct checked by exact length on the DPU and by static assertion on both sides;
growing it for a variable-length field would make every identity change a
lockstep deployment. A separate message preserves both properties and is
idempotent on replay. Identity binds to the slot's DMA generation, so a reused
slot never inherits the previous tenant's identity. The registry supplies the
mapping from service identifier to cluster address.

The L7 layer reads identity and never asserts it. In modes that omit mTLS this is
also the authorization input: the DPU binds it to the comch connection and the
registered memory region rather than accepting a claim from the pod.

## 7. Width and packing

Wire structures carry fixed-width integers only: no `size_t`, no enumerations, no
pointers. Every wire structure asserts its size and the offsets of its published
fields at compile time. Both endpoints are little-endian; no conversion is
performed.

| Constant | Value |
|---|---|
| Forward descriptor | 64 B |
| Reverse entry | 32 B |
| Reverse ring | 8192 entries |
| Control-path send tasks | 8192 |
| Pods per node | 32 |

## 8. Adapter API

The L7 layer sees these entry points and nothing else. DOCA resources, progress
engines, DMA engines and rings stay with DPUmesh.

```c
/* DPUmesh calls into the L7 layer */
int  l7_worker_attach(int worker_id, void *worker_ctx);
int  l7_worker_step(int worker_id);                      /* 1 if progress was made */
int  l7_conn_open(int worker_id, uint64_t conn, const struct dmesh_l7_flow *);
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);
void l7_conn_eof(int worker_id, uint64_t conn);
void l7_conn_close(int worker_id, uint64_t conn);
void l7_worker_detach(int worker_id);

/* The L7 layer calls into DPUmesh */
int  dmesh_l7_send(int worker_id, uint64_t conn, const uint8_t *buf, size_t len);
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);

/* `decision` mode */
int  l7_resolve(int worker_id, const struct dmesh_l7_flow *flow,
                struct dmesh_l7_decision *out);   /* { allow, backend_pod } */
void l7_report (int worker_id, uint64_t conn, uint64_t bytes_in,
                uint64_t bytes_out, uint64_t duration_ns, int reason);
```

`l7_conn_segment` hands over a pointer into shared staging. The region stays
valid until the corresponding `dmesh_l7_release`; the L7 layer must not retain it
past that call and must not copy on the assumption that it may.

`l7_resolve` answers from the outbound stack's discovery, policy and balancer
without consuming payload. `l7_report` returns per-connection load so that
balancer state remains accurate for connections whose bytes never traversed the
proxy.

The port's existing foreign-function surface maps onto these as a translation
between a pull model and a push model.

## 9. Processing model

Arriving payload lands in a pod-shared staging region and is forwarded by
scatter-gather DMA from where it lands; the ARM does not copy it. Extents are
handed to the L7 layer by pointer. Because the region is shared and reclaimed by
custody, the L7 layer must release each extent after consuming it; until release,
the bytes remain valid. Failure to release corrupts data, so release is mandatory
rather than advisory.

Bytes the L7 layer produces are written into an ARM-side arena that the egress
engine can source. Scatter-gather admits arbitrary lengths, so no alignment or
maximum-multiple chunking is imposed on writes.

Backpressure is DPUmesh's: receive credit and worker stall govern both
directions. A buffer limit inside the L7 layer tracks arena availability rather
than acting as an independent bound.

## 10. Port divergences

What the port carries today, against what this contract requires.

| Area | Port | Contract |
|---|---|---|
| Control messages | three, no registration or teardown barrier | §3 |
| Metadata | one message carrying every export descriptor | one `MMAP_EXPORT` per region |
| Descriptor | validity flag, producer tail, `size_t` size, no routing fields | §4 |
| Reverse path | host-side DPA over a second PCI function, or descriptor push | §5 |
| Credits | absent | carried on the reverse ring |
| Identity | asserted by the host shim | carried on registration, read-only |
| Slots | compile-time constant of eight | sized at attach |
| Driver | owns a loop | single-step entry point |
| Release | no-op | mandatory |
| Writes | 128-byte aligned chunking | arbitrary length |
| Query interface | absent | `l7_resolve` / `l7_report` |
| Build | `linkerd/doca/build.rs` compiles the port's twelve datapath sources and requires its DPA kernel archive | the datapath is DPUmesh's; the crate compiles its own sources only |
| Backend channel | `backend::take` removes the entry, so an address yields one channel and later connections fall back to a TCP dial | an address yields a channel per connection for as long as it is meshed |

## 11. Open decisions

1. Adopting the reverse ring, retiring both of the port's reverse designs.
2. Making the connection-slot count a runtime parameter.
3. Which side produces the single-step driver patch.
4. Whether inbound proxying is planned; the contract covers outbound only.
5. Moving flow identity onto the registration path, retiring the host shim's
   identity construction.
6. Adding `l7_resolve` / `l7_report`. Without them, `decision` mode can only be
   approximated by observing the connector's endpoint choice, which distorts the
   balancer's load view.
7. **One proxy, many workloads.** `LINKERD2_PROXY_IDENTITY_LOCAL_NAME` and
   `LINKERD2_PROXY_POLICY_WORKLOAD` hold a single value: the proxy is built on the
   assumption that it represents one workload. Here one proxy serves every meshed
   pod on the node, so the caller's identity has to arrive per connection —
   `dmesh_l7_flow.workload` in §8 — rather than being derived from the proxy's own
   certificate. Whether the policy layer accepts an identity supplied that way is
   unsettled, and it is the precondition for authorization in every mode.
8. **Proxy credentials on the DPU.** The proxy authenticates to the certificate
   authority with a Kubernetes ServiceAccount token projected into its pod. The
   DPU's proxy is not a pod and has no such token. Either the DPU joins the
   cluster as a node whose proxy runs as a workload, or a credential is
   provisioned for it out of band.
