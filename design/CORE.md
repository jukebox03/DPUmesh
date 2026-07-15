# DPUmesh Core — Internals Whitepaper (implementation-facing)

How the user-facing surface in **[API.md](API.md)** is implemented. `API.md` says *what* the
transport does; this says *how*. It is a spec of the current data path, not a change log.

The system spans **three planes**, all driving **one** host→DPU→host data path:

| Plane | Runs on | Sources |
|---|---|---|
| **Host transport** | application host (x86) | `src/dmesh_core.{h,c}` (core + connection lifecycle; **internal, not installed**) carrying **two sibling surfaces**: `include/dpumesh/dmesh.h` → `src/dmesh_api.c` (the native verbs-shaped API) and `src/dmesh_preload.c` (the POSIX socket shim). Neither surface is built on the other. |
| **DPU control plane** | BlueField ARM | `doca/dpu_worker.c`, `object.{c,h}`, `dpu_proxy.c` (reverse egress), `dpu_l7.c` (L7 hook), `comch_*.c` |
| **DPA data plane** | BlueField DPA EUs | `doca/device/dpa_kernel.c` (EU kernel, dpacc-built) + `doca/dpa.c` (ARM-side setup) |

Host↔DPU uses **DOCA Comch** for the control path (register, mmap-export, batched ACK/completion)
and **DMA** for bodies. The **DPA is forward-only** (host→DPU staging); the **reverse path
(DPU→host) is ARM SG-DMA** through the proxy engine (`dpu_proxy.c`). Wire layouts are frozen by
`_Static_assert` in `doca/dpa_common.h` / `doca/comch_common.h`.

```
 HOST src                         BlueField DPU                              HOST dst
 ════════                         ═════════════                             ════════
 write→byte-ring ─┐   ┌─ DPA EU (N≤8, forward-only) ─┐  ┌─ ARM ingest shards (M) ───┐
 flush → K fwd    ├──▶│ dma_copy host→DPU staging     │  │ own consumer PE (channel  │
 rings (Vyukov    │   │ (≤8KB, 20B completion imm)    │  │  k → shard k%M) → window  │
 MPSC, src%K)     ┘   └───────────────────────────────┘  │  →parser→conntrack→lane   │
                                                          └────────────┬──────────────┘
        reverse = ARM SG-DMA egress (dpu_proxy.c, n_eng workers) ◀──────┘
        staging→dst host RX buffer; completed batches → ARM emit (main):
              ─▶ BATCH_REV_DONE / TX_ACK (all Comch sends) ─▶ host PE thread
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
  First block is LAZY (grabbed on the first `dmesh_alloc`), which fills the chain in place.

`dmesh_alloc` = `tx_reserve` — it hands the ring pointer out and the caller fills in place, so the
native path never copies (the shim's `stream_write` is the one caller that adds a memcpy, because
`write(2)` demands one). **`tx_reserve` never blocks**: `NULL`+`EAGAIN` when the block window is full,
the `ibv_post_send` contract. It also cannot fail *later* — the init clamp
`maxb × ceil(block_size/slot_size) ≤ TX_SU_DEPTH` makes the block window bind strictly before the
send-unit FIFO, so anything reserve admitted is guaranteed carveable and `tx_next_send` needs no
capacity check. The descriptor's `body_buf_slot` carries the **byte offset**
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
(a lock-free SPMC Vyukov queue) so the accept path promotes it (`SERVER_PENDING` coalesces pipelined
messages 2..P into the inbox so they don't re-hit the accept queue). The app reads via `dmesh_poll_cq`
(pop **that CQ's** ready list → `inbox_pop` → hand out a pointer into `rx_dma_buffer[pos]`, released by
`dmesh_wc_release` → `rx_free`
returns the RX credit).

### 1.6 Concurrency model
Exactly **one internal thread** (the PE). Everything else runs on app threads. Channels are lock-free:
conn **inbox** (SPSC: PE producer, owning app thread consumer), **per-CQ ready list** (SPSC: PE
producer, that CQ's one polling thread), **TX byte-ring** (SPSC: app owns `tx_w/c/s`, PE owns `tx_f`),
**accept ring** (SPMC: PE producer, N consumers via CAS), **forward rings** (MPSC). The shared TX block
pool is lock-free (Treiber); the only mutexes (`port_lock` at create/accept/destroy, `block_lock` at the
close-drain block handoff, `cq_lock` at CQ register/unregister) are never on the data path.

**The CQ is the unit of RX parallelism** (`struct dmesh_cq`): each owns its ready list and its eventfd,
and `psl->cq` binds a conn to the CQ that created or accepted it, so `arm_ready_after_push` pushes to
*that* CQ's ring. N threads with N CQs share nothing on RX. A conn arrives with no CQ yet, so a new
inbound conn wakes every registered CQ (`cqs[]` under the cold `cq_lock`, once per conn); whichever CQ
accepts it owns it thereafter. Measured: the CQ was never the bottleneck (one shared CQ under a mutex
scales 4.6× too — multi-CQ is worth ~5% Mrps / ~17% p50 at 8 threads); it exists so the API does not
bake "one RX consumer per process" into a ceiling the host will hit once the DPU stops binding.

**Readiness is armed from `dmesh_create_cq`, never lazily.** A `notify_enabled` flag used to gate
`arm_ready_after_push` and was only set by the fd getter — so a client that only ever *polled* silently
received nothing on established conns (a new conn's first message rides the accept queue, which is not
gated, so connects worked and replies vanished). The flag is gone; `dmesh_cq_fd` is purely the optional
idle-sleep path. The eventfd is written **once per delivery** (no coalescing → a wakeup is never lost).

**The lost-edge (Dekker) invariant is per-conn and unchanged by multi-CQ:** `psl->on_ready` is armed by
the PE with a `seq_cst` fence + `acq_rel` exchange after a push, and disarmed by the consumer when
`dpumesh_conn_recv` drains the inbox empty, then re-checked. Only *which ring* the push lands in
changed. `psl->cq` is published with `user` **before** the `role` release-store, so the acquire-load of
`role` synchronizes-with it upstream of the Dekker pair.

---

## 2. Wire protocol (host ↔ DPU ↔ DPA)

### 2.1 Control path — DOCA Comch (`comch_*.c`)
Host = **Comch client**, DPU = **Comch server** (over one PCIe connection). Message types
(`enum dmesh_msg_type`, `comch_common.h`):

| Type | Dir | Struct |
|---|---|---|
| `POD_REGISTER` (1) | H→D | `dmesh_register_msg{service_id, pod_id=−1}` |
| `MMAP_EXPORT` (2) | H→D | `dmesh_mmap_msg{mmap_type, host_addr, size, export_desc}` |
| `BATCH_FWD_ACK` (3) | D→H | `dmesh_batch_tx_ack_msg{count, acks[≤16]}` — free the sender's TX bytes |
| `BATCH_REV_DONE` (4) | D→H | `dmesh_batch_rev_done_msg{count, entries[≤16]}` — deliver reverse DMAs |
| `POD_ASSIGNED` (5) | D→H | `dmesh_pod_assigned_msg{pod_id}` |

**Dispatch asymmetry (a real trap):** H→D structs lead with a 4-byte `enum` and the server switches on
it; D→H structs lead with a **`uint8_t` at offset 0** and the host dispatches on `recv_buffer[0]` — so
every D→H type must keep its value < 256 and be added to the client switch, or it silently drops.
The batch capacities (`BATCH_TXACK_MAX` / `BATCH_REVDONE_MAX`) are equally **two-sided wire ABI**: both
binaries compile them and the host silently CLAMPS an oversized `count` — change them only with host
AND DPU rebuilt together (a full deploy), or the clamped tail acks vanish and the sender's conn wedges.

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
One 1-producer/1-consumer Comch channel **per EU**, channel `k` bound to ingest consumer PE `k % M` —
its owning shard's PE (§4.1; M = 1 collapses to the single consumer PE):
`RING_ADD` (D→DPA, add a forward ring), `RING_DEL` (D→DPA, drop a pod's ring on disconnect — keyed by
`pod_id`, which is unique per EU because K ≤ N makes `k_j` injective), `WAKE` (D→DPA, ~1 kHz keepalive to
re-activate a parked EU), `FWD_DONE` (DPA→D, a forward dma_copy completed — carries `comch_dma_comp_msg`).
Without `RING_DEL` the DPA's `rings[]` was append-only, so a restarting pod (same recycled `pod_id` ⇒ same
EUs) accreted entries until `MAX_DPA_RINGS`, after which the add was dropped **silently** (the msgq is
fire-and-forget).

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

### 4.1 Thread model — a three-stage pipeline (ingest → emit → egress)
The ARM control plane is **M ingest shards** (`DPUMESH_INGEST_SHARDS`, default 1), **one emit thread**
(main), and **`n_eng` egress workers** (§5). Every stage is event-driven — each thread sleeps on its
own PE notification fd / eventfd (epoll: arm → re-check → block, 1 ms backstop); nothing busy-spins.

- **Ingest (reap + parse/route) — M shard threads.** DPA channel `k` is bound to consumer PE
  `consumer_pes[k % M]`; shard `m` OWNS PE `m` outright: it reaps it (the consumer callbacks fill the
  shard's private SPSC queue), drains a bounded batch per pass through `px_ingest_forward`
  (window → parser → conntrack → lane), and sends the ~1 kHz DPA `WAKE` keepalive for its own channels
  — only a PE's owner may submit on its channels (the WAKE-race rule). **M = 1 is the original single
  funnel** (ingest inline on main; `DPUMESH_INGEST_REAP=1` moves reap+process to one dedicated reaper
  thread instead).
- **Routing modes (M ≥ 2).** Default **② share-nothing**: conn table, pools, and conntrack are
  per-shard; an upstream id is allocated OWNER-STRIDED (`(uP − BASE) % M` = the client's shard) so a
  backend reply names its session's owner — a reply that lands on another shard's EU is handed to the
  owner via that shard's cross-shard MPSC inbox (a parked shard is woken by eventfd). Optional
  **① shared** (`DPUMESH_SHARD_SHARED=1`): one conntrack/route table under `routing_lock`, handled
  where it lands (measured ~7 % below ②). **③** (sharding the emit path) is a scaffold flag only —
  see the emit bullet for why.
- **Emit — one thread (main), ALL Comch sends.** The Comch server rides the single ctrl PE; PEs are
  not thread-safe, and a send must never run inside a consumer callback (re-entering the PE corrupts
  it). Main progresses the ctrl PE, drains the shards' deferred TX_ACKs (`pending_txack` — an ingest
  thread never sends), runs `px_drain` (submit + emit for the egress engine, §5), and flushes partial
  ACK/REV_DONE batches on a lull. This single D→H funnel is why ③ is not yet real: parallel emit
  needs a coordinated DPU+host multi-channel transport.
- **Cross-thread state, kept minimal.** Shard queues are acquire/release SPSC; the LB cursor
  `svc_rr` is a relaxed atomic (a racing double-pick only skews balance); the route-affinity table
  takes `routing_lock` only when M > 1, and only for `rg ≠ 0` traffic; `px_arrival` custody
  (`unfreed`) is an atomic counter — ingest (window release, drops) and emit (egressed bytes)
  decrement it from different threads, and whichever reaches 0 releases exactly once.
- **Measured** (scale_log 2026-07-14): shards = 2 ⇒ +33–44 %; shards = 2 + `n_eng` = 2 ⇒ ≈ 2× the
  single-funnel small-RPC ceiling. `DPUMESH_DIAG=1` prints a once/sec pipeline dump (batch fill,
  flush-size histogram, queue depths) that stays quiet while idle.

### 4.2 Routing — cluster registry + load balancer
A **service is a cluster** of backend pods (Envoy model). The healthy backend set is **derived from
`pods[]` on demand** — `collect_live_hosts(svc)` = the slots with `registered && service_id==svc &&
dma_ready`. There is **no service→backend table** to keep in sync: a pod register/disconnect changes the
derived set automatically (a dead backend is simply not returned — no stale-entry blackhole). `pods[]` is
the single source of truth; the only added state is a per-service round-robin cursor `svc_rr[svc]`.

- **`lb_pick(svc)`** — the load balancer: **round-robin** over `collect_live_hosts(svc)` (Envoy's default
  policy), `-1` if the cluster has no healthy backend. The cursor is a relaxed atomic (shards may race
  it, skewing only balance). The one seam a weighted / least-request / hash policy would replace.
- **`dpu_route_l4(svc, rg)`** — the DEFERred-segment route: `rg != 0` → **route-affinity pin**
  (`route_group_backend[svc][rg]`; reuse if the pin is live, else `lb_pick` + record — overwrite-on-reuse,
  self-healing; under `routing_lock` when M > 1, §4.1); `rg == 0` → `lb_pick(svc)`.
- **Connection stickiness (`px_resolve_backend`, dpu_proxy.c).** For a request seg the backend is resolved
  with Envoy-style precedence: **(1) an L7 host override** (§5.1, validated live) > **(2) the connection's
  sticky pin** (`px_conn.pinned_backend`, same cluster, still live) > **(3) `dpu_route_l4`** (recorded as
  the session pin when sticky). **Sticky is the default** — a connection keeps the backend the LB first
  picked (session affinity, socket-like ordering; the src↔backend pairing persists). `DPUMESH_LB_PER_REQUEST_SVC`
  lists services that load-balance **every** message instead (Envoy HTTP per-request). A conn is
  processed by ONE ingest shard (§4.1), so its sticky pin needs no lock; the only shared routing
  state is the atomic cursor + the locked `rg` table.

### 4.3 Conntrack — Model B (the DPU owns every connection)
`struct dpu_conntrack` = `upstream[65536]` (by `uP`) + an open-addressed reuse hash table keyed
`(client_pod, client_port, backend_pod) → uP`. Under ② sharding (M ≥ 2, §4.1) each ingest shard owns
a PRIVATE conntrack and allocates `uP` owner-strided, so the table stays single-threaded at any M. **Request** (`dst_pod = BLANK`): route → backend →
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
running (auto-detected) N — so it grows with N: BF3 N=8 ⇒ 32. Per-pod ACK/REV_DONE **batches** (`txack_batch[≤16]`,
`rev_done_batch[≤16]`) coalesce so the host PE reaps one message per K responses (the host per-RTT reap
is the 2-pod throughput cap); flushed on full or on the idle lull, and touched ONLY by the emit thread
(an ingest shard defers its acks via `pending_txack`, §4.1).

**Disconnect: unpublish everything, destroy nothing.** A pod can die *while it has in-flight DMA* against
its memory. `pods_remove_connection` therefore:
- sends **`RING_DEL`** to each EU holding one of its forward rings (while `pod_id`/`k_rings` are still valid),
- sets `dma_ready=0` and **NULLs every host-exported handle** (`ring_mmaps`, `ring_host_addrs`,
  `remote_mmap`, `host_rx_mmap`, `buf_arrs`). These pointers ARE the egress's liveness gate —
  `px_lane_refresh_credit` only checks `ring_mmaps[r] && ring_host_addrs[r]`, so leaving them set lets it
  keep credit-refresh DMA-reading the dead pod's **unmapped** host memory (§5),
- **destroys nothing.** An in-flight DMA may still reference a mapping, and destroying it faults the
  engine's shared `doca_dma` ctx. Safe reclaim needs a quiesce protocol (RING_DEL ack + per-pod egress
  in-flight refcount) that does not exist, so these handles **leak per reconnect** — deliberately, and
  cheaply: the 32 MB staging that dominated the leak is now **allocated once per slot and reused**
  (`setup_pod_dma`), since it is DPU-local and holds nothing host-specific.

**Re-tenanting a slot.** `dma_generation` is bumped per incarnation (before `dma_ready` is published) and
stamped into async DMA ops. Slot indices are recycled, so it is what distinguishes "this pod's DMA failed"
from "the previous tenant's did", and what tells the egress its per-lane credit state is stale (§5).
The egress side complements all this: a not-ready pod's queued lane units are dropped to the done-queue
(custody released, no delivery).

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
- **Backend death mid-flight.** If a destination pod disconnects while units are queued/in-flight, its lane
  is drained (`px_lane_drop_dead` → done-queue, custody released, no delivery) and none of its mappings are
  destroyed (§4.4), so in-flight SG-DMA finishes without faulting the engine ctx. The LB meanwhile routes
  new traffic only to live backends.
- **A failed DMA is a NORMAL event, not a bug.** The egress DMAs into *peers'* host memory, and a dying pod
  takes that memory with it **before** comch reports the disconnect — so `dma_ready` can never gate the
  window shut, and some DMA will land in unmapped memory. The QP then takes `LOCAL_QP_OPERATION_ERROR` and
  DOCA stops the engine's **shared** ctx, killing every pod's egress. So the engine must SURVIVE it:
  - **Latch:** any path that can observe the fault sets `eng->dma_stalled` — `px_dma_err_cb` (on
    `IO_FAILED`, from the error itself), the SG-batch submit, and `px_lane_refresh_credit` (on
    `BAD_STATE`). Latching only in the batch submit is not enough: after a fault the refresh never lands,
    so credits never arrive and the lane breaks out *before* reaching that submit — the unlatched retry
    spin is what once flooded DOCA's `alloc_init ... state IDLE` forever.
  - **Recover:** `px_engine_recover` waits for the ctx to reach IDLE (DOCA flushes in-flight tasks through
    `px_dma_err_cb`), then `doca_ctx_start`s it, rearming every lane's `refresh_inflight` — a refresh whose
    callback never ran would otherwise leave that lane unable to ever refresh credit again, starving a
    *healthy* pod forever. It drives the PE itself and reports progress so the main loop cannot park
    mid-recovery.
  - **Learn:** an `IO_FAILED` credit refresh is the *earliest* reliable death signal (earlier than comch),
    so it drops that pod's `dma_ready` — guarded by `dma_generation` (the callback may land after the
    slot's next tenant registered) and by `IO_FAILED` specifically (a ctx fault flushes *healthy* pods'
    tasks through the same callback).
- **Per-lane state is per-incarnation.** `px_lane_rearm` resets `cursor`/`sent_entries`/`cached_freed` when
  `pods[].dma_generation` moves. A restarted pod exports a fresh RX buffer whose freed-counter restarts at
  0, so credits inherited from the previous tenant are wrong in a way that never self-corrects
  (`inflight = sent_entries - cached_freed` stays huge ⇒ `avail_entries` pins at 0 ⇒ the lane never sends).
  That starvation was the ROOT of the pod-restart wedge: the starved lane retried credit-refresh every
  pump, which is what made hitting the death window a certainty rather than a rare race.
  **Limitation (high-churn, unchanged):** a MASS simultaneous death (≳13 pods at once) can leave a DMA
  **hung** at the hardware level; a hung task cannot be drained, so the ctx never reaches IDLE and
  `px_engine_recover` cannot complete. Recovery is validated for single-pod death (14 sequential restarts,
  0-fail); mass death still needs prevention (bound in-flight DMA per pod, or a per-pod ctx).
- **Egress.** A receiving conn always lands in **one lane** `[dst_pod][region = dst_port % K]` (FIFO), so
  delivery order = segment order. A lane's units are gathered into **one chained-source `doca_dma`**
  (SG) copy: DPU staging → the destination host's RX buffer, then a batched `REV_DONE` notify. Chunks
  are delivered as ≤ 8 KB byte-stream pieces (`PX_ENTRY_BYTES_MAX`); the backend frames its own bytes.
- **Custody at egress.** The sender's TX bytes are held until the egress DMA has **read** the staging
  bytes; only then does the batched `TX_ACK` fire (`px_arrival_release`) — releasing earlier would let
  the host overwrite a body mid-DMA.
- **Workers.** `DPUMESH_ARM_EGRESS_THREADS` (`n_eng`) egress workers — stage three of the §4.1
  pipeline — each owning its own `doca_dma`/PE/inventory and a set of lanes (`pod_idx % n_eng`); a
  worker retires completed batches into a done-queue that the emit thread drains, so REV_DONE +
  custody TX_ACK sends stay on main. `n_eng = 1` runs inline on the ARM thread and **wedges under
  overload** (event-loop idle-park under egress backpressure); **`n_eng = 2`** is required —
  busy-polling workers keep egress draining so the stall never forms.

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

Runs an unmodified, dynamically-linked POSIX socket app over DPUmesh by interposing libc. It is the
**second surface on the core, a sibling of the native API — not a client of it**: it includes
`src/dmesh_core.h` (`-Isrc`) for the QP internals (`rx_slot` cursor, `peer_closed`) and the internal
lifecycle (`dmesh_accept` / `dmesh_next_ready` / `dmesh_send_fin`) that `<dpumesh/dmesh.h>` does not
expose.

**It also owns the POSIX byte-stream semantics**, as two file-local statics: `stream_read` (copy out of
the RX mmap + the `rx_pos` partial-consumption cursor) and `stream_write` (copy into the TX ring +
carve any length across ≤`block_size` reserves). Both are the price of the socket contract — `read(2)`
mandates a copy into caller memory, `write(2)` mandates any length — and neither belongs in the
transport. That seam is exactly why the native path is zero-copy: the cost lives only where the
contract demands it.

The keystone: when a socket becomes dmesh-backed (`connect` to a registry `ClusterIP:port`, or `listen`
on the advertised port), the shim **`dup2`s a private eventfd over the app's fd number** — the fd stays
a real kernel fd, so `epoll`/`poll`/`select`/`close`/`dup` work **natively** (kernel-TCP and dmesh fds
share one epoll set, zero interposition of the readiness syscalls). A single **dispatcher thread** owns
the shim's one CQ and is its sole consumer and sole `dmesh_destroy_qp` caller (app `close` only queues)
— it asserts each conn's eventfd on delivery. Every client conn is `dmesh_pin_route`d (one backend,
in-order — the socket contract). `connect` keys on the registry `ClusterIP:port` (not port alone), so
same-port services on distinct ClusterIPs resolve apart, and `getpeername` returns that dialed address.
Blocking is emulated (`SO_RCVTIMEO` honored). **Lost-wakeup discipline** bridges the edge-triggered
dmesh ready list to level-triggered POSIX readiness: over-assert the eventfd on every successful read,
drain-and-retry-once on `EAGAIN`. `shutdown(SHUT_WR)` sends the FIN — an approximate half-close.

**Known gap — `O_NONBLOCK` is not honored on the write path.** `stream_write` blocks under
backpressure (correct for a blocking socket) but the shim tracks `e->nonblock` and honors it only at
`recv`/`accept`. Returning `EAGAIN` instead would need honest `EPOLLOUT`, which the eventfd keystone
**cannot express** — an eventfd is always writable, so an app would livelock on epoll→write→EAGAIN. The
fix is a socketpair fd-realization (two independent readiness directions; you de-assert writability by
filling the peer's buffer, mirroring the ring's occupancy so the kernel derives writability for you) —
which upgrades the **per-message** readiness signal from an eventfd counter bump to an AF_UNIX skb
alloc. Gated on `grow_waits` ever being non-zero; measured at **0**, so the path never executes.

Limits: AF_INET `SOCK_STREAM` only; no fork-shared sockets; Go/static/stdio binaries bypass
`LD_PRELOAD`.

---

## 7. Invariants (must hold across changes)

| Invariant | Why |
|---|---|
| `num_slots × slot_size == DPU_BUFFER_SIZE` (32 MB) | staging mirrors the host TX offset-for-offset → bounds occupancy |
| a conn's live window ≤ `maxb` blocks (`b − tail_blk < maxb` before backing block `b`) | slot `b%maxb` can never alias a still-live block `b−maxb` → no write-side overwrite/wedge |
| `maxb × ceil(block_size/slot_size) ≤ TX_SU_DEPTH` (clamped at init) | makes the block window bind **strictly before** the send-unit FIFO, so the FIFO can never fill: what `tx_reserve` admitted is always carveable, and `tx_next_send` needs no capacity check (it used to back off on a branch this clamp makes unreachable) |
| `tx_reserve` probes the block window **before** mutating `tx_w` | on `EAGAIN` the write head must be exactly where it was, or a retry strands the padded tail |
| `psl->cq` is published with `user` **before** the `role` release-store | `arm_ready_after_push` loads `cq` after an acquire-load of `role`; publishing later would let the PE push to a NULL/stale ready list |
| readiness is armed at `dmesh_create_cq`, never on first fd request | gating the arm on a fd getter made `poll_cq` silently under-deliver for a pure-polling client (established conns only — a new conn's first message rides the ungated accept queue, hence connects-work/replies-vanish) |
| host `K` == DPU `K` (`DPUMESH_RINGS_PER_POD`) | forward rings pair 1:1; a mismatch stalls `dma_ready` |
| `slot_size ≤ 8192` | the DPA `dma_copy` cap (one wire descriptor) |
| `sizeof(dma_desc) == 64` (one cache line) | the DPA's line-granular `valid=0` writeback must not touch a neighbour |
| D→H message type is a `uint8_t` at offset 0, value < 256, in the client switch | host dispatches on `recv_buffer[0]` |
| one PE thread writes `tx_f`, inbox tail, every CQ's ready tail + eventfd | the lock-free SPSC channels assume a single producer |
| a CQ's ready list is popped by exactly ONE thread | SPSC on the consumer side too — that is *why* you make several CQs, not a limitation of one |
| all Comch sends on the emit (main) thread, never inside a PE callback | one ctrl PE, not thread-safe; batch accumulators are main-only (ingest defers via `pending_txack`) |
| a consumer PE is progressed by exactly ONE thread (its shard), which also sends its channels' `WAKE` | DOCA PEs are single-threaded; a foreign submit races the owner's progress (the WAKE race) |
| ② conntrack is per-shard; `uP` is owner-strided (`(uP − BASE) % M`) | each conntrack stays single-threaded; a backend reply dispatches to its session's owner shard |
| `px_arrival.unfreed` is atomic; the decrement that reaches 0 releases exactly once | ingest and emit account the same arrival from two threads |
| publish-role-last (host `ports[]`), publish-registered/`dma_ready`-last (DPU `pods[]`) | a reader must never observe a half-built slot |
| client ports `[1, 32768)`, upstream `uP` `[32768, 65535]` | loopback host holds both roles in one table |
| `n_eng = 2` (`DPUMESH_ARM_EGRESS_THREADS`) | `n_eng = 1` wedges under egress backpressure |
| a disconnected pod's host-exported mappings are UNPUBLISHED (NULLed) on disconnect and **never destroyed** | the NULL is the egress's only liveness gate (else it DMA-reads unmapped host memory); a destroy faults the shared `doca_dma` ctx, killing EVERY pod's egress. They leak until a quiesce protocol exists; the 32 MB staging is reused per slot instead |
| per-lane credit state is reset whenever `pods[].dma_generation` moves | a re-tenanted slot's new pod exports a fresh RX freed-counter (restarts at 0); inherited credits pin `avail_entries` at 0 and the lane never sends again |
| a DMA error is EXPECTED (peers' memory can vanish), so every path that observes a ctx fault must latch `dma_stalled` and let `px_engine_recover` restart the ctx | gating cannot close the window (comch reports the death too late); an unlatched retry spin floods `alloc_init ... state IDLE` forever |

## 8. Baked sizing (see API.md §9 for the full env/knob table)
`num_slots = 4096`, `slot_size = 8192` (32 MB TX/RX/staging per pod); TX blocks `block_size = 64 KB`
(pool of `n_blocks = 512`), `maxb = 4` (≤ 256 KB in-flight/conn), `cushion_h = 1`,
`TX_SU_DEPTH = 64` (and it **clamps** `maxb` — §7); `N` DPA EUs
**auto-detected** = min(device EUs, `MAX_DPA_EU=8`) (BF3 → 8; `DPUMESH_DPA_THREADS` overrides);
`K = 2` forward rings/pod; per-EU ring cap `MAX_DPA_RINGS = 8` ⇒ concurrent pods ≤ `MAX_DPA_RINGS × N / K`
(BF3 → 32); `M = 2` ingest shards (`DPUMESH_INGEST_SHARDS`, ② share-nothing) + `n_eng = 2` ARM egress
workers — the measured ≈2× config; `PX_HEAD_MAX = 4 KB`, `PX_SEAM_MAX = 512 KB`. `DPU_BUFFER_SIZE`,
`slot_size`, and `K` are the constrained ones (§7). A programmatic `dpumesh_config` overrides
`num_slots`/`slot_size` without a rebuild.
