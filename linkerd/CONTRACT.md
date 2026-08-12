# Interface Contract

Between the DPUmesh datapath and the linkerd port. DPUmesh is normative:
`doca/comch_common.h`, `doca/dpa_common.h`, `doca/ring.h`, `doca/dpu_worker.c`,
`doca/dpu_proxy.c`. The port conforms.

The datapath is DPUmesh's. The port's own is not compiled into this
integration; it stays in tree as the reference point for convergence.

`design/L7.md` describes the layer this contract joins. This document states
what the two sides owe each other.

## 1. Operating modes

What the modes mean and how a service is assigned to one is defined in
[`design/L7.md`](../design/L7.md). This section states only what each mode
implies for the ABI in §8.

| Mode | What the layer is called with |
|---|---|
| `decision` | `l7_resolve` once per connection and `l7_report` at close; no payload is handed over |
| `opaque` | the above, plus `l7_conn_segment` for every arriving byte, as one unframed stream |
| `l7` | the above; the layer finds its own message boundaries and emits each message itself |

The mode constants are shared with the data plane and asserted equal at compile
time, so a renumbering on either side fails the build rather than the link. When
the layer declines a connection, or is not attached, the data plane forwards at
L4 without policy.

## 2. Thread model

DPUmesh runs N DPA execution units, K forward rings per pod, and A ARM worker
threads. A worker owns its connection table, conntrack, SG-DMA engine and
progress engine; nothing is shared across workers and no lock is taken between
them. Worker-local state is reached through `__thread px_cur_worker`.

The port therefore runs one tokio `current_thread` runtime per ARM worker,
created on that worker's thread. A `multi_thread` runtime is excluded: work
stealing moves a task across threads, and worker-local state is not thread-safe.

The DPUmesh worker loop owns the iteration. It calls the layer once per
revolution and folds the result into its own progress test, which governs
arming, parking and the 1 ms backstop:

```c
did = dpu_progress_worker_pe(...);
run = dpu_worker_run(...);          /* includes l7_worker_step() */
if (did || run) continue;
/* arm -> recheck -> epoll_wait(1 ms) */
```

The layer exposes a single-step entry point rather than owning a loop, arms no
timer of its own, and sizes its connection slots at attach time.

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
| 10 | `POD_IDENTITY` | H→D | `workload[64]`, the name this pod runs as |

`mmap_type`: `DMA_BUFFER=1`, `DMA_RING=2`, `DMA_HOST_RX_BUFFER=3`,
`DMA_REV_RING=4`. One message per region.

```text
H→D  POD_IDENTITY                 omitted by a pure client
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

```text
kind(1) reserved(7) payload(16) publish_seq(8)

kind = DONE(1)    src_pod_id, src_service, dst_service, src_port, dst_port,
                  seq, length, pos                                    16 B
kind = TX_ACK(2)  port, seq                                            4 B
```

A 128-byte control structure on its own cache lines carries `consumer_head`,
which the host publishes after a drain batch, and `arm_epoch`, which the host
increments before blocking. `REV_DOORBELL` wakes a blocked host.

Completion notification and transmit-credit return share this ring. It is the
sole DPU→host reverse mechanism.

## 6. Flow identity

The proxy routes on socket addresses and needs the source workload; DPUmesh
routes on pod and service identifiers. `dmesh_l7_flow` carries both, plus the
connection's mode, its direction, and `peer_pod` — the other end of the session,
which is what lets the layer recognise a reply as belonging to a connection it
already opened.

Workload identity arrives on `POD_IDENTITY`, before `POD_REGISTER`. The
registration message is a fixed 12-byte struct checked by exact length on the DPU
and by static assertion on both sides; growing it for a variable-length field
would make every identity change a lockstep deployment. A separate message
preserves both properties and is idempotent on replay. Identity binds to the slot
the connection owns and is cleared when the slot takes a new tenant, so a reused
slot never inherits the previous one's identity.

The layer reads identity and never asserts it. Where mTLS is absent this is also
the authorization input: the DPU binds it to the comch connection and the
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
| Identity message | 68 B |

## 8. Adapter API

The layer sees these entry points and nothing else. DOCA resources, progress
engines, DMA engines and rings stay with DPUmesh. The header is
`linkerd/include/dmesh_l7.h`.

```c
/* DPUmesh calls into the L7 layer */
int  l7_worker_attach(int worker_id);
int  l7_worker_step(int worker_id);                      /* 1 if progress was made */
int  l7_conn_open(int worker_id, uint64_t conn, const struct dmesh_l7_flow *);
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);
void l7_conn_eof(int worker_id, uint64_t conn);
void l7_conn_close(int worker_id, uint64_t conn);
void l7_worker_detach(int worker_id);

/* The L7 layer calls into DPUmesh */
int  dmesh_l7_backends(int worker_id, int32_t service, int32_t *out, int max);
int  dmesh_l7_send(int worker_id, uint64_t conn, int32_t backend_pod,
                   const uint8_t *buf, size_t len);
uint8_t *dmesh_l7_tx_reserve(int worker_id, uint64_t conn, uint32_t *cap);
int  dmesh_l7_tx_commit(int worker_id, uint64_t conn, int32_t backend_pod,
                        uint32_t len);
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);

/* `decision` mode */
int  l7_resolve(int worker_id, const struct dmesh_l7_flow *flow,
                struct dmesh_l7_verdict *out);    /* { allow, backend_pod } */
void l7_report (int worker_id, uint64_t conn, uint64_t bytes_in,
                uint64_t bytes_out, uint64_t duration_ns, int reason);
```

**Custody.** `l7_conn_segment` hands over a pointer into shared staging. The
region stays valid until the matching `dmesh_l7_release`; the layer must not
retain it past that call and must not copy on the assumption that it may. Failure
to release corrupts data, so release is mandatory rather than advisory. Segments
arrive in stream order and releases are reported in the same order.

**Partial transfer.** `l7_conn_segment` returns the bytes taken, in `[0, len]`,
and the remainder is offered again. `dmesh_l7_send` returns the bytes accepted,
where zero means the egress arena is momentarily full. Negative is terminal for
the connection in both directions.

**Egress.** `dmesh_l7_tx_reserve` lends DMA-able memory so an encoder writes its
output once, with no second copy on publication; `dmesh_l7_tx_commit` publishes a
prefix of it, or returns it unused when the length is zero. One reservation per
connection is outstanding at a time. A delivery that fits one reservation is one
scatter-gather source. `dmesh_l7_send` is for a caller that hands over a whole
message and needs it delivered whole, and chains as many sources as that takes.

**Backend choice.** The layer names the backend, not the data plane.
`dmesh_l7_backends` lists a service's live candidates and both publishing calls
take the chosen pod; `DMESH_L7_BACKEND_ANY` returns the choice to the data
plane's balancer. The reply direction is routed by conntrack and ignores the
argument. Granularity is the layer's to choose: routing per message reorders
responses on a protocol that matches them positionally, and is correct only where
responses carry their own correlation.

**Re-entrancy.** Calls are never re-entrant for one connection, but a *different*
connection may be closed while a segment is being delivered: issuing an upstream
port reclaims the connection state of its previous tenant. Per-connection state
must therefore not sit behind one shared borrow.

**Backpressure** is DPUmesh's. Receive credit, custody limit and worker stall
govern both directions; a buffer limit inside the layer tracks arena
availability rather than acting as an independent bound.

## 9. Build integration

`linkerd/doca` compiles under a feature that selects which datapath backs the
foreign-function surface.

```toml
[features]
default = ["own-datapath"]
own-datapath = []
```

With the feature on, the crate compiles its own C datapath and links its DPA
kernel archive — the port's standalone binary. With it off, `build.rs` compiles
nothing and the crate contributes Rust only: the IO endpoint, the backend
registry, and the acceptor's types. The DOCA handle, the probe, the error type
and the driver are gated with it, since an embedder that supplies the datapath
supplies those symbols too.

`src/api.rs` holds what both configurations need — `FlowId`, `DmeshEvent`,
`Registrar`, `Registration`, `MAX_CONNS` — so gating the driver does not gate the
acceptor's interface. `linkerd-app` and `linkerd-app-outbound` take the crate
with `default-features = false`; feature unification restores the datapath for
the port's own binary, which therefore builds unchanged.

DPUmesh selects the consumer at build time:

```text
-Dl7_backend=null                     the reference consumer in linkerd/shim/
-Dl7_backend=linkerd -Dl7_lib_path=…  the Rust staticlib
```

## 10. Port divergences

What the port carries, against what this contract requires.

| Area | Port | Contract |
|---|---|---|
| Control messages | three, no registration or teardown barrier | §3 |
| Metadata | one message carrying every export descriptor | one `MMAP_EXPORT` per region |
| Descriptor | validity flag, producer tail, `size_t` size, no routing fields | §4 |
| Reverse path | host-side DPA over a second PCI function, or descriptor push | §5 |
| Credits | absent | carried on the reverse ring |
| Identity | asserted by the host shim | carried on `POD_IDENTITY`, read-only |
| Backend choice | the connector dials what it is given | named per delivery from `dmesh_l7_backends` |
| Backend channel | `backend::take` removes the entry, so an address yields one channel and later connections fall back to a TCP dial | an address yields a channel per connection for as long as it is meshed |
| Slots | compile-time constant of eight | sized at attach |
| Driver | owns a loop | single-step entry point |
| Release | no-op | mandatory |
| Writes | 128-byte aligned chunking | arbitrary length |
| Query interface | absent | `l7_resolve` / `l7_report` |

The backend-channel row is what bounds the integration today: one connection at
a time is sound, and beyond that an address has already been handed out.

## 11. Open decisions

1. Adopting the reverse ring, retiring both of the port's reverse designs.
2. Making the connection-slot count a runtime parameter.
3. Which side carries the single-step driver decomposition.
4. Whether inbound proxying is planned; this contract covers outbound only.
5. Retiring the host shim's identity construction in favour of `POD_IDENTITY`.
6. **Balancing without shared state.** Each ARM worker holds its own instance and
   sees only its share of the connections, so their balancers cannot share a
   sequence. Independent sequences all start at the same backend: with four
   connections over three backends, every one chose the same backend, where the
   data plane's single global counter covered all three. A shared-nothing
   balancer has to derive its spread from the flow, and even then covers the
   backends only in proportion to the connections it sees.
7. **One proxy, many workloads.** `LINKERD2_PROXY_IDENTITY_LOCAL_NAME` and
   `LINKERD2_PROXY_POLICY_WORKLOAD` hold a single value: the proxy is built on
   the assumption that it represents one workload. Here one proxy serves every
   meshed pod on the node, so the caller's identity arrives per connection —
   `dmesh_l7_flow.workload` — rather than from the proxy's own certificate.
   Whether the policy layer accepts an identity supplied that way is unsettled,
   and it is the precondition for authorization in every mode.
8. **Proxy credentials on the DPU.** The proxy authenticates to the certificate
   authority with a Kubernetes ServiceAccount token projected into its pod. The
   DPU's proxy is not a pod and has no such token. The mock control plane
   sidesteps this for bring-up; production needs either the DPU joining the
   cluster as a node whose proxy runs as a workload, or a credential provisioned
   out of band.
