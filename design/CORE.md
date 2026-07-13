# DPUmesh Core — Internals Whitepaper (implementation-facing)

How the user-facing surface in **[API.md](API.md)** is implemented. `API.md` says *what* the
transport does; this says *how*. It is a spec of the current data path, not a change log.

The system spans **three planes**, all driving **one** host→DPU→host data path:

| Plane | Runs on | Sources |
|---|---|---|
| **Host transport** | application host (x86) | `src/dmesh_core.c` (core) + `include/dpumesh/dmesh.h` → `src/dmesh.c` (façade) + `src/dmesh_preload.c` (shim) |
| **DPU control plane** | BlueField ARM | `doca/dpu_worker.c`, `object.{c,h}`, `dpu_proxy.c` (reverse egress), `dpu_l7.c` (L7 hook), `comch_*.c` |
| **DPA data plane** | BlueField DPA EUs | `doca/device/dpa_kernel.c` (EU kernel, dpacc-built) + `doca/dpa.c` (ARM-side setup) |

Host↔DPU uses **DOCA Comch** for the control path (register, mmap-export, batched ACK/completion)
and **DMA** for bodies. The **DPA is forward-only** (host→DPU staging); the **reverse path
(DPU→host) is ARM SG-DMA** through the proxy engine (`dpu_proxy.c`). Wire layouts are frozen by
`_Static_assert` in `doca/dpa_common.h` / `doca/comch_common.h`.

```
 HOST src                         BlueField DPU                              HOST dst
 ════════                         ═════════════                             ════════
 write→byte-ring ─┐   ┌─ DPA EU (N≤8, forward-only) ─┐  ┌─ ARM (1 control thread) ──┐
 flush → K fwd    ├──▶│ dma_copy host→DPU staging     │  │ comp_queue → dpu_proxy:   │
 rings (Vyukov    │   │ (≤8KB, 20B completion imm)    │  │  window→parser→conntrack  │
 MPSC, src%K)     ┘   └───────────────────────────────┘  │  →per-(dst,region) lane   │
                                                          └────────────┬──────────────┘
        reverse = ARM SG-DMA egress (dpu_proxy.c, n_eng workers) ◀──────┘
        staging→dst host RX buffer ─▶ BATCH_REV_DONE ─▶ host PE thread
                                    ─▶ conn inbox ─▶ ready list ─▶ event_fd ─▶ read
```

---

## 1. Host transport (`src/dmesh_core.c`)

### 1.1 The channel — `struct dpumesh_ctx`
One per process (wrapped by `dmesh_channel_t`). Holds: the DOCA device, the **Comch client** (control
path to the DPU) and **Comch consumer** (fast-path RX completions), the **TX mmap** `dma_buffer`
(`num_slots × slot_size` = 32 MB, the DMA source), the **RX mmap** `rx_dma_buffer` (DMA landing zone),
the **K forward descriptor rings**, the **port (connection) table**, one **PE progress thread**, and
one **readiness eventfd**.

**Registration.** `dmesh_create_channel(service_id)` opens the device, connects Comch, and sends
`POD_REGISTER{service_id, pod_id=-1}`; the DPU assigns a `pod_id` (its `pods[]` slot index) and replies
`POD_ASSIGNED`. The host then exports its TX buffer, RX buffer, and each forward ring to the DPU via
`MMAP_EXPORT` (DOCA PCI mmap export descriptors). `pod_id` stays −1 until assigned.

### 1.2 The port table = the connections — `struct dmesh_port_slot`
`ports[65536]`, indexed by **local port = connection id**. Client conns use `[1, 32768)`; DPU-assigned
upstream ids (`uP`, server conns) use `[32768, 65535]` — the split lets a loopback host hold both roles
in one table with no collision. Each slot carries: `role` (FREE / CLIENT / SERVER / SERVER_PENDING),
the learned peer `(pod, port)`, the app's conn handle `user`, an **inbound SPSC ring** (`inbox`, lazily
malloc'd), and the **TX byte-ring cursors** (§1.3). Inbound is demuxed **purely by `dst_port`** → that
conn's inbox. **Publish-role-last**: `user` and buffers are written before `role` is RELEASE-published,
so the PE never enqueues to a half-built slot.

### 1.3 TX — elastic per-connection block chain
The 32 MB TX mmap is carved into a shared pool of `n_blocks` fixed **blocks** of `block_size` (default
**64 KB** = the max contiguous message = the allocation unit). Each conn owns an **elastic chain** of up to
`maxb` (default 4 ⇒ ≤ 256 KB in-flight) blocks that it grabs on demand and returns as they drain — so its
footprint tracks its in-flight demand instead of a fixed slab. The pool is a **lock-free Treiber** free-list
(`block_free = tag<<32 | head`); a conn's chain + FOUR logical byte cursors live in its `dmesh_port_slot`:

```
  free(tx_f) ≤ send(tx_s) ≤ commit(tx_c) ≤ write(tx_w)   — logical offsets that span the chain
  logical block k = cursor / block_size,  physical block = pblk[k % maxb],  offset = cursor % block_size
```

- **reserve(len ≤ block_size)** → a pointer into DMA memory. A message never straddles a block (it pads to
  the next block if it won't fit) so each is contiguous. Each new logical block is backed ONCE
  (`head_blk_next`), **waiting until block k−maxb has drained** (`k − tail_blk < maxb`) so the live window
  stays ≤ `maxb` blocks and slot `k%maxb` is free — this is the per-conn in-flight cap and the write-side
  backpressure (busy-spin on the conn's own ACKs).
- **commit(len)** → advances `tx_w`, `tx_c` (byte-granular, no per-slot waste); records the block's content
  end (`blk_used`, so the send side can skip a padded tail).
- **flush** → `tx_next_send` carves `[tx_s, tx_c)` into **≤ `slot_size` (8 KB) descriptors** — coalescing
  many small writes into few large DMAs, the throughput lever — never crossing a block boundary; `tx_sent`
  records `(seq → end cursor)` in the per-conn **send-unit FIFO** (`su_seq`/`su_end`, `TX_SU_DEPTH` = 64 deep).
- **reclaim** → `BATCH_FWD_ACK(port,seq)` pops the FIFO front (FIFO — a conn ships + is ACKed in seq order)
  and advances `tx_f`; a fully-drained tail block is **recycled** into a small per-conn free-list (depth ≤
  `cushion_h`, default 1) for reuse. Steady sliding reuses a recycled block (**0 pool ops**); GROW grabs from
  the pool only when the free-list is empty; SHRINK returns a block only when it exceeds the cushion; a
  fully-drained conn compacts back to logical 0. So a conn grows toward `maxb` under load, shrinks toward one
  block as it quiets, and holds nothing when idle — the grow/shrink hysteresis is exactly the cushion `H`.
  First block is LAZY (grabbed on the first write). `dmesh_alloc`/`dmesh_commit` fills the same chain in place.

`dmesh_write` = reserve + memcpy + commit; the descriptor's `body_buf_slot` carries the **byte offset**
`pblk[k%maxb] × block_size + off` into the mmap, and `enqueue` sets `dma->addr = dma_buffer + offset` — the
DPA mirrors that offset into staging, so the whole allocator is **host-side only**. A CLOSED conn's blocks
return once its chain drains — by close if already drained, else by the PE on the last ACK
(`try_return_blocks`): the chain is **owner-thread-local while live** (the PE only advances `tx_f`), and the
close handoff uses `block_lock` for exactly-once return, mirroring the port table's publish-role-last rule.

### 1.4 Forward rings — lock-free MPSC
`K` = `DPUMESH_RINGS_PER_POD` (default 2) forward descriptor rings; a conn pins to **one** ring
(`src_port % K`) so its messages stay FIFO on one EU (per-conn send order), while different conns spread
across K rings/EUs. Each ring is a **Vyukov bounded MPSC queue**: `enq_pos` hands out a monotonic ticket
`t`, the producer owns slot `t % size` and writes it when the cell is free (`seq==t`, or the prior
generation published and the DPA cleared `valid==0`). A stalled producer leaves `seq` unadvanced so a
lapping producer **waits** instead of overwriting — lossless with just the `valid` flag, no lock. The
descriptor (`struct dma_desc`, **exactly 64 B = one cache line**) publishes `valid=1` last; the DPA
clears it after consuming. One descriptor per cache line is load-bearing (the DPA's line-granular
writeback of `valid=0` must not clobber a neighbour).

### 1.5 RX — the PE progress thread
One **PE thread** sleeps on the Comch PE notification fd (`epoll_wait`, arm → re-poll once → sleep — no
busy-poll). Each fast-path completion invokes the Comch consumer callback → `rx_data_hook`, which
dispatches on the message type:
- `BATCH_FWD_ACK` → loops `tx_reclaim_ack` (TX reclaim, §1.3).
- `BATCH_REV_DONE` → loops `process_rx_dma_entry`: the body has already DMA-landed at byte-offset
  `pos` in `rx_dma_buffer` (**zero-copy** — no staging copy); it builds an `sw_descriptor_t` with
  `body_buf_slot = pos` and calls `rx_deliver_desc`.

`rx_deliver_desc` demuxes by `dst_port`: a **live** conn → `inbox_push` (on the empty→non-empty edge,
publish the conn to the **ready list** + write the eventfd); a **new** server conn (`dst_port ≥
UPORT_BASE`, no live slot) → create a `SERVER_PENDING` slot, push message-1 onto the **accept ring**
(a lock-free SPMC Vyukov queue) so `dmesh_accept` promotes it (`SERVER_PENDING` coalesces pipelined
messages 2..P into the inbox so they don't re-hit the accept queue). The app reads via `dmesh_next_ready`
(pop the ready list) → `dmesh_read` (`inbox_pop` → read `rx_dma_buffer[pos]` in place → `rx_free`
returns the RX credit).

### 1.6 Concurrency model
Exactly **one internal thread** (the PE). Everything else runs on app threads. Channels are lock-free:
conn **inbox** (SPSC: PE producer, owning app thread consumer), **ready list** (SPSC: PE producer, the
one event-loop consumer), **TX byte-ring** (SPSC: app owns `tx_w/c/s`, PE owns `tx_f`), **accept ring**
(SPMC: PE producer, N worker consumers via CAS), **forward rings** (MPSC). The shared TX block pool is
lock-free (Treiber); the only two mutexes (`port_lock` at connect/accept/close, `block_lock` at the
close-drain block handoff) are never on the data path. The eventfd is written **once per delivery** (no
coalescing → a wakeup is never lost).

---

## 2. Wire protocol (host ↔ DPU ↔ DPA)

### 2.1 Control path — DOCA Comch (`comch_*.c`)
Host = **Comch client**, DPU = **Comch server** (over one PCIe connection). Message types
(`enum dmesh_msg_type`, `comch_common.h`):

| Type | Dir | Struct |
|---|---|---|
| `POD_REGISTER` (1) | H→D | `dmesh_register_msg{service_id, pod_id=−1}` |
| `MMAP_EXPORT` (2) | H→D | `dmesh_mmap_msg{mmap_type, host_addr, size, export_desc}` |
| `BATCH_FWD_ACK` (3) | D→H | `dmesh_batch_tx_ack_msg{count, acks[≤14]}` — free the sender's TX bytes |
| `BATCH_REV_DONE` (4) | D→H | `dmesh_batch_rev_done_msg{count, entries[≤16]}` — deliver reverse DMAs |
| `POD_ASSIGNED` (5) | D→H | `dmesh_pod_assigned_msg{pod_id}` |

**Dispatch asymmetry (a real trap):** H→D structs lead with a 4-byte `enum` and the server switches on
it; D→H structs lead with a **`uint8_t` at offset 0** and the host dispatches on `recv_buffer[0]` — so
every D→H type must keep its value < 256 and be added to the client switch, or it silently drops.

### 2.2 Data path — descriptor + completion
- **`struct dma_desc`** (64 B, one cache line): `{mmap, addr, size, seq, src_port, dst_port,
  src_service, dst_service, dst_pod_id, src_pod_id, route_group, valid}` — the **oriented endpoint
  tuple**. The body is NOT in it (only a pointer + tuple). A CLIENT sends `dst_pod = BLANK` (DPU routes
  by `dst_service`); a backend reply sends a concrete `dst_pod` (direct).
- **`struct comch_dma_comp_msg`** (20 B immediate): the DPA→DPU completion — `{type, src_pod, dst_pod,
  dst_service, src_port, dst_port, seq, length, pos, route_group}`. `type` at offset 0 (peeked).
  `src_service` is **not** on the wire (the 20 B budget carries `route_group`); the DPU derives it from
  `src_pod`'s registration (assumes one service per pod).

### 2.3 DPU ↔ DPA — Comch msgq (`comch_msgq.c`, `enum dpa_msg_type`)
One 1-producer/1-consumer Comch channel **per EU**, all draining into the DPU's single consumer PE:
`RING_ADD` (D→DPA, add a forward ring), `WAKE` (D→DPA, ~1 kHz keepalive to re-activate a parked EU),
`FWD_DONE` (DPA→D, a forward dma_copy completed — carries `comch_dma_comp_msg`).

---

## 3. DPA data plane — forward-only (`device/dpa_kernel.c` + `dpa.c`)

**N** EU threads (**auto-detected** = min(device EUs, `MAX_DPA_EU`=8) — BF3 reports 254 → **N=8**;
`DPUMESH_DPA_THREADS` overrides) share one `doca_dpa` device; each is pinned to a distinct EU. A pod exports
**K** forward rings, ring `j` landing on EU `(pod_id·K + j) % N`, and a conn's ring (`src_port % K`) selects
its EU. `dpa.c` wires each ring's `dpa_ring_info` (buf_arr, host TX
mmap+base, DPU staging mmap+base) and hands it to the EU via `RING_ADD` or an h2d memcpy.

**EU kernel loop** (`run_dma_manager`): drain DPU→DPA msgs → poll all forward rings → drain producer
completions. Per valid descriptor (`process_fwd_ring`, batch ≤32): reject `size > 8 KB`; wait until the
DPU consumer has credit; compute `moff = desc->addr − host_addr` (the sender's host TX byte offset) and
`staging_base = dpu_addr` (the pod's 32 MB staging); issue **`doca_dpa_dev_comch_producer_dma_copy`**
copying `ALIGN_UP_128(size)` bytes host TX → `staging_base + moff`, **piggybacking the 20 B completion**
as the copy's immediate to the DPU consumer. Clear `valid=0` (batched window writeback). A FIN (`size==0`)
still issues a 128 B minimum copy so the engine never sees a zero-length descriptor.

**Per-conn contiguous staging (the mirror).** Because the DPA lands every chunk at the host TX byte
offset, a conn's bytes are **contiguous in staging regardless of which EU carried them** — the L7 parser
sees one contiguous per-conn byte stream. Occupancy mirrors the host TX blocks (a conn holds ≤ `maxb`
blocks and the shared pool totals `num_slots × slot_size`), so `num_slots × slot_size == DPU_BUFFER_SIZE`
bounds staging and it never overflows.

**Park/wake.** On a dedicated EU, the kernel polls continuously for lowest latency and parks only on
sustained idle (to satisfy the FlexIO watchdog). Parking is guarded by an arm → re-scan → don't-sleep-
if-found sequence: a one-shot `WAKE` must never be consumed-then-parked (that would deadlock the EU).
The ARM sends `WAKE` on a ~1 kHz cadence because a silent `valid=1` store generates no completion.

---

## 4. DPU ARM control plane (`dpu_worker.c` + `object.{c,h}`)

### 4.1 Main loop — single thread
The ARM runs **one control thread**, event-driven (epoll over the consumer PE fd + the ctrl PE fd) with
a 1 ms keepalive tick. Each pass: `doca_pe_progress` both PEs (the consumer callbacks push `FWD_DONE`
completions onto the `comp_queue`) → drain up to 128 completions through `px_ingest_forward` → `px_drain`
(SG-DMA egress) → flush partial ACK/REV_DONE batches when idle. All Comch sends happen here, **never
inside a consumer callback** (re-entering the PE would corrupt it). One consumer PE draining all EU
channels into one `comp_queue` keeps the forward ring single-producer and the routing/conntrack tables
**lock-free by virtue of single-threading**.

### 4.2 Routing — cluster registry + load balancer
A **service is a cluster** of backend pods (Envoy model). The healthy backend set is **derived from
`pods[]` on demand** — `collect_live_hosts(svc)` = the slots with `registered && service_id==svc &&
dma_ready`. There is **no service→backend table** to keep in sync: a pod register/disconnect changes the
derived set automatically (a dead backend is simply not returned — no stale-entry blackhole). `pods[]` is
the single source of truth; the only added state is a per-service round-robin cursor `svc_rr[svc]`.

- **`lb_pick(svc)`** — the load balancer: **round-robin** over `collect_live_hosts(svc)` (Envoy's default
  policy), `-1` if the cluster has no healthy backend. The one seam a weighted / least-request / hash
  policy would replace.
- **`dpu_route_l4(svc, rg)`** — the DEFERred-segment route: `rg != 0` → **route-affinity pin**
  (`route_group_backend[svc][rg]`; reuse if the pin is live, else `lb_pick` + record — overwrite-on-reuse,
  self-healing); `rg == 0` → `lb_pick(svc)`.
- **Connection stickiness (`px_resolve_backend`, dpu_proxy.c).** For a request seg the backend is resolved
  with Envoy-style precedence: **(1) an L7 host override** (§5.1, validated live) > **(2) the connection's
  sticky pin** (`px_conn.pinned_backend`, same cluster, still live) > **(3) `dpu_route_l4`** (recorded as
  the session pin when sticky). **Sticky is the default** — a connection keeps the backend the LB first
  picked (session affinity, socket-like ordering; the src↔backend pairing persists). `DPUMESH_LB_PER_REQUEST_SVC`
  lists services that load-balance **every** message instead (Envoy HTTP per-request). Single ARM thread →
  cursor + pin need no lock.

### 4.3 Conntrack — Model B (the DPU owns every connection)
`struct dpu_conntrack` = `upstream[65536]` (by `uP`) + an open-addressed reuse hash table keyed
`(client_pod, client_port, backend_pod) → uP`. **Request** (`dst_pod = BLANK`): route → backend →
find/create upstream `uP` → rewrite the tuple so the backend sees a connection from the proxy
(`src = dst_port = uP`). **Reply** (concrete `dst_port = uP`): look up `upstream[uP]` → rewrite
`dst → (client_pod, client_port)`. **TX_ACK translation** is the key trap: a client request's reverse
leg carries `src_port = uP`, but the client tracks its TX bytes by its real port, so the ACK is
translated `uP → client_port` before sending, or the client's bytes leak. A client **FIN** frees `uP`
and its reuse HT entry.

### 4.4 Pod registration, teardown & batching
`pods[MAX_PODS]` (array = `MAX_DPA_RINGS × MAX_DPA_EU / K`, sized for the max EU config); a registering conn
is assigned `pod_id =` its slot index (RELEASE-published after its fields; `dma_ready` is a separate later
gate for the data plane). Slots are stable (never compacted) and **reused** on a later connect. The LIVE
concurrent-pod cap (`pods_add_connection`) is `MAX_DPA_RINGS × N / K` — the forward-ring capacity for the
running (auto-detected) N — so it grows with N: BF3 N=8 ⇒ 32. Per-pod ACK/REV_DONE **batches** (`txack_batch[≤14]`,
`rev_done_batch[≤16]`) coalesce so the host PE reaps one message per K responses (the host per-RTT reap
is the 2-pod throughput cap); flushed on full or on the idle drain.

**Disconnect = graceful for the egress engine.** A backend can die *while it has in-flight egress SG-DMA*
into its RX buffer. `pods_remove_connection` sets `dma_ready=0` (the egress then skips the pod) but
**DEFERS destroying `host_rx_mmap`** — an egress worker thread may still hold an in-flight `doca_buf` over
it, and destroying it concurrently would fault the whole engine's shared `doca_dma` ctx. The handle is
kept in the slot and destroyed at slot **reuse** (`pods_add_connection`), by which point the old pod is
long gone and no egress references it (reconnect latency ≫ in-flight drain). The egress side complements
this: a not-ready pod's queued lane units are dropped to the done-queue (custody released, no delivery),
and a `doca_dma` `BAD_STATE` fault stalls that engine's submit instead of retry-spinning (§5).

---

## 5. Reverse egress = the ARM SG-DMA proxy engine (`dpu_proxy.c`)

The **sole DPU→host reverse path** (there is no DPA reverse ring). Both directions run the same engine;
only destination resolution differs.

- **Input window.** Each forward completion appends a `px_arrival` (a zero-copy view over DPU staging,
  in stream order) to the conn's window. `px_view` extends a contiguous view across arrivals whose
  staging bytes **physically abut** (the per-conn mirror), stopping at a discontinuity (the host TX
  byte-ring wrap). A **seam** buffer copies the unconsumed tail into one contiguous run **only** when a
  parse stalls across that boundary (e.g. a > 8 KB frame split across arrivals).
- **Parser.** Per **connection** (selected from the addressed service), the parser returns route
  segments `{off, len, dst}`. Parsers: **passthru** (one segment per arrival, `dst = DEFER` → the §4.2
  route: LB + connection stickiness; replies always use this), **frame** (a demo `[u32 len][u8 svc][payload]`,
  routing each frame by its `svc` byte), or **L7** (the real hook, §5.1). `DPUMESH_PROXY` picks the deploy
  default; `DPUMESH_PROXY_FRAME_SVC` / `_L7_SVC` assign parsers per service.
- **Backend death mid-flight.** If a destination pod disconnects while units are queued/in-flight, its
  lane is drained (`px_lane_drop_dead` → done-queue, custody released, no delivery) and its `host_rx_mmap`
  is kept alive until slot reuse (§4.4) so the in-flight SG-DMA finishes without faulting the engine ctx.
  A `doca_dma` `BAD_STATE` sets `eng->dma_stalled` (stop submitting) rather than retry-spinning. The LB
  meanwhile routes new traffic only to live backends — a single/moderate death is not a wedge.
  **Limitation (high-churn):** a MASS simultaneous death (≳13 pods at once) can leave enough in-flight
  SG-DMA to torn-down host RX memory that one DMA **hangs** at the hardware level; the hung task poisons the
  engine's shared `doca_dma` ctx and `doca_ctx_stop` cannot drain it — so that engine wedges until the DPU
  process restarts. A ctx restart cannot recover a hung DMA; the real fix is prevention (bound in-flight DMA
  per pod, or a per-pod ctx). Single/moderate churn (≤ ~6-at-once) is unaffected.
- **Egress.** A receiving conn always lands in **one lane** `[dst_pod][region = dst_port % K]` (FIFO), so
  delivery order = segment order. A lane's units are gathered into **one chained-source `doca_dma`**
  (SG) copy: DPU staging → the destination host's RX buffer, then a batched `REV_DONE` notify. Chunks
  are delivered as ≤ 8 KB byte-stream pieces (`PX_ENTRY_BYTES_MAX`); the backend frames its own bytes.
- **Custody at egress.** The sender's TX bytes are held until the egress DMA has **read** the staging
  bytes; only then does the batched `TX_ACK` fire (`px_arrival_release`) — releasing earlier would let
  the host overwrite a body mid-DMA.
- **Workers.** `DPUMESH_ARM_EGRESS_THREADS` (`n_eng`) egress workers, each owning its own
  `doca_dma`/PE/inventory and a set of lanes (`pod_idx % n_eng`). `n_eng = 1` runs inline on the ARM
  thread and **wedges under overload** (event-loop idle-park under egress backpressure); **`n_eng = 2`**
  is required — busy-polling workers keep egress draining so the stall never forms.

### 5.1 The L7 hook (`dpu_l7.c` / `dpu_l7.h`) — the router filter
`int dmesh_l7_route(const uint8_t *head, uint32_t len, const struct dmesh_l7_ctx *ctx,
struct dmesh_l7_decision *out)` — Envoy's router filter. The engine shows only the message **head** (a
bounded ≤ `PX_HEAD_MAX` = 4 KB window) plus the cluster's live endpoints in `ctx->hosts[]`; the hook
returns `>0` = decided (fill `out`), `0` (head not fully here → grow), or `<0` (malformed → poison the
conn). The decision `out = { total_len, cluster, host }`: `total_len` = the whole message length (the body
is **streamed from staging via SG, never linearized** — no per-slot memcpy); `cluster` = the service to
route to (default = the addressed service; overwrite to content-route); `host` = `DMESH_LB_DEFER` (the
engine load-balances the cluster, §4.2) **or** a concrete pod to **override** (session persistence, à la
Envoy `setUpstreamOverrideHost`). The backend is resolved once per message so all chunks pin to it in order.
The hook is stateless, no malloc, no locks. The default `dpu_l7.c` is a demo (svc-byte → cluster, then LB);
a real parser needs no transport change. `hash` (consistent-hash LB) + per-conn `session` state are
append-only phase-2 fields of the decision struct.

---

## 6. LD_PRELOAD shim (`src/dmesh_preload.c`)

Runs an unmodified, dynamically-linked POSIX socket app over DPUmesh by interposing libc. The keystone:
when a socket becomes dmesh-backed (`connect` to a registry `ClusterIP:port`, or `listen` on the advertised port), the
shim **`dup2`s a private eventfd over the app's fd number** — the fd stays a real kernel fd, so
`epoll`/`poll`/`select`/`close`/`dup` work **natively** (kernel-TCP and dmesh fds share one epoll set,
zero interposition of the readiness syscalls). `read`/`write`/`send`/`recv` route to `dmesh_read` /
`dmesh_write`+`dmesh_flush`. A single **dispatcher thread** is the channel's sole `dmesh_accept` /
`dmesh_next_ready` consumer and sole `dmesh_close` caller (app `close` only queues) — it asserts each
conn's eventfd on delivery. Every client conn is `dmesh_pin_route`d (one backend, in-order — the socket
contract). `connect` keys on the registry `ClusterIP:port` (not port alone), so same-port services on
distinct ClusterIPs resolve apart, and `getpeername` returns that dialed address (not `127.0.0.1`).
Blocking is emulated (`SO_RCVTIMEO` honored). **Lost-wakeup discipline** bridges the
edge-triggered dmesh ready list to level-triggered POSIX readiness: over-assert the eventfd on every
successful read, drain-and-retry-once on `EAGAIN`. `shutdown(SHUT_WR)` sends the FIN
(`dmesh_send_fin`) — an approximate half-close (late replies are undeliverable). Limits: AF_INET
`SOCK_STREAM` only; no fork-shared sockets; Go/static/stdio binaries bypass `LD_PRELOAD`.

---

## 7. Invariants (must hold across changes)

| Invariant | Why |
|---|---|
| `num_slots × slot_size == DPU_BUFFER_SIZE` (32 MB) | staging mirrors the host TX offset-for-offset → bounds occupancy |
| a conn's live window ≤ `maxb` blocks (`b − tail_blk < maxb` before backing block `b`) | slot `b%maxb` can never alias a still-live block `b−maxb` → no write-side overwrite/wedge |
| host `K` == DPU `K` (`DPUMESH_RINGS_PER_POD`) | forward rings pair 1:1; a mismatch stalls `dma_ready` |
| `slot_size ≤ 8192` | the DPA `dma_copy` cap (one wire descriptor) |
| `sizeof(dma_desc) == 64` (one cache line) | the DPA's line-granular `valid=0` writeback must not touch a neighbour |
| D→H message type is a `uint8_t` at offset 0, value < 256, in the client switch | host dispatches on `recv_buffer[0]` |
| one PE thread writes `tx_f`, inbox tail, ready tail, eventfd | the lock-free SPSC channels assume a single producer |
| one ARM control thread | conntrack / routing / batch tables are lock-free by single-threading; forward ring stays single-producer |
| publish-role-last (host `ports[]`), publish-registered/`dma_ready`-last (DPU `pods[]`) | a reader must never observe a half-built slot |
| client ports `[1, 32768)`, upstream `uP` `[32768, 65535]` | loopback host holds both roles in one table |
| `n_eng = 2` (`DPUMESH_ARM_EGRESS_THREADS`) | `n_eng = 1` wedges under egress backpressure |
| a disconnected pod's `host_rx_mmap` is destroyed at slot REUSE, never on disconnect | an egress worker may hold an in-flight `doca_buf` over it; concurrent destroy faults the engine's `doca_dma` ctx |

## 8. Baked sizing (see API.md §9 for the full env/knob table)
`num_slots = 4096`, `slot_size = 8192` (32 MB TX/RX/staging per pod); TX blocks `block_size = 64 KB`
(pool of `n_blocks = 512`), `maxb = 4` (≤ 256 KB in-flight/conn), `cushion_h = 1`; `N` DPA EUs
**auto-detected** = min(device EUs, `MAX_DPA_EU=8`) (BF3 → 8; `DPUMESH_DPA_THREADS` overrides);
`K = 2` forward rings/pod; per-EU ring cap `MAX_DPA_RINGS = 8` ⇒ concurrent pods ≤ `MAX_DPA_RINGS × N / K`
(BF3 → 32); `n_eng = 2` ARM egress workers; `PX_HEAD_MAX = 4 KB`, `PX_SEAM_MAX = 512 KB`. `DPU_BUFFER_SIZE`,
`slot_size`, and `K` are the constrained ones (§7). A programmatic `dpumesh_config` overrides
`num_slots`/`slot_size` without a rebuild.
