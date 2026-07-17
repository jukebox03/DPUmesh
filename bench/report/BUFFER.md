# Per-connection TX buffers: do the feared costs materialize?

**Conclusion — no. Against real microservice connection patterns, the three
suspected weaknesses of per-connection contiguous buffers (low memory
efficiency, poor scaling with many in-flight connections, miscellaneous
per-operation overhead) do not materialize: the elastic block design makes a
connection's footprint follow its in-flight demand instead of its existence,
and on hardware the entire allocator disappears from the steady-state path —
one shared-pool operation per connection lifetime, ~3 µs per reconnect, and
unchanged tail latency at churn rates ~8× beyond what real services generate.**

All numbers below: 2026-07-14, matched full deploy, `fail=0` in every run.

---

## 1. Design under test

A conn owns a **contiguous byte-ring over an elastic chain of 64 KiB blocks**
(`DPUMESH_TX_BLOCK`) carved from the pod's shared 32 MiB TX mmap (512 blocks,
lock-free Treiber free-list). The chain is demand-following:

- **Lazy**: a conn holds **0 bytes** until its first send (one CAS grabs block 1).
- **Grow**: only when in-flight bytes exceed a block — "the occasional extra
  buffer" — one CAS, up to `DPUMESH_TX_MAXB` (4 → 256 KiB in-flight cap).
- **Shrink**: drained blocks recycle owner-locally (cushion `DPUMESH_TX_H`=1),
  surplus returns to the pool; close returns everything.
- Messages never straddle a block (pad + fresh block), so every message is
  contiguous.

## 2. What real services actually do with connections

Verified against the benchmark suites' source (receipts in the research notes;
gRPC services are the primary reference).

| workload | conns per pod | created | destroyed | steady churn |
|---|---|---|---|---|
| Online Boutique frontend (gRPC) | **7** (1/backend svc, `mustConnGRPC`) | lazily at first RPC; backoff re-dial (1 s → ×1.6 → 120 s) after failure/rollout | idle 30 min (grpc-go default), pod lifecycle, GOAWAY | **~0/s** + bursts on rollout/scale |
| DSB hotelReservation (gRPC + consul) | ~6 channels × backend instances (`round_robin`) | first RPC + **each backend added** (resolver `UpdateState` dials new subchannels) | backend removed, failure | ~0/s + scale events |
| DSB socialNetwork (Thrift `ClientPool`) | 0–512 per downstream, **tracks in-flight concurrency** (min=0, max=`connections`=512) | `Pop()` on empty pool → lazy `new` | **any conn older than 10 s** (`keepalive_ms`), any RPC exception | **≈ active conns / 10 s** → O(10–100)/s per pod under load |
| DSB nginx tier (Lua cosockets) | per-worker keepalive pool ≤100 (512 for compose) | per-request `connect()` when idle pool exhausted | pool overflow closed immediately, idle 10 s | ∝ concurrency variance |

Two patterns emerge. **gRPC = conn-per-topology**: few, long-lived connections
whose *count* changes at runtime (lazy dial, instance scale, pod churn) —
so a static boot-time partition is impossible, but churn is rare. **Thrift/nginx
= conn-per-concurrency**: counts follow load and DSB *by design* destroys every
pooled conn at 10 s of age — the high-churn upper bound.

## 3. The three concerns, tested

### 3a. "Low memory efficiency" — footprint follows demand, not conn count

Never-sent conn = 0 B; active conn = ⌈in-flight/64 KiB⌉ ≤ 4 blocks; idle
once-active conn = 1 cushion block (64 KiB, `DPUMESH_TX_H=0` drops it to 0).
Measured occupancy confirms demand-following: 8 conns pushing 1.03 M small
messages took **114 block grabs total** — pool usage tracked the ~2 KiB
actually in flight, not the connection count. A static scheme sized for the
same peak (256 KiB × conns) would reserve 128 MiB for 512 conns; the elastic
pool serves the same demand from 32 MiB.

### 3b. "Not scalable for many in-flight connections" — bounded by bytes, not conns

Conn count itself consumes nothing until first send, so the real ceiling is
**in-flight bytes ≤ 32 MiB (shared)** — matched to the DPU staging bound, i.e.
flow control, not allocator failure. `waits=0` in every realistic run (the
reserve path never once blocked on the pool). At gRPC-representative counts
(7–50 conns/pod) there is no pressure at any load we can generate. The one real
sizing consideration: ≥512 *once-active idle* conns/pod (DSB Thrift pool max)
would pin the pool via the 64 KiB cushion → set `DPUMESH_TX_H=0` (idle conns
then hold 0) or raise the pool; unmeasured, flagged as follow-up.

### 3c. "Miscellaneous overhead" — measured at and beyond real churn rates

64 B RPCs, `bench.sh point dpumesh … [reconn]` (churn knob: close+reconnect
every `reconn` completions; counters from `dpumesh_get_tx_pool_stats`):

| run | rate | p50/p95/p99 (µs) | pool CAS | reconnect cost |
|---|---|---|---|---|
| conc 1, no churn | 8,009 rps | 114 / 226 / 285 | **1 grab in 79 k msgs** | — |
| conc 1, reconn=100 (~78/s) | 7,811 (−2.5%) | 114 / 228 / 286 | 1/conn | **3.0 µs** local |
| conc 1, reconn=10 (~755/s) | 7,549 (−5.7%) | 114 / 228 / 287 | 1/conn | 2.6 µs local |
| conc 32×8 thr, no churn | 0.1044 Mrps | 2454 / – / 2641 | 114 / 1.03 M msgs (10⁻⁴/msg) | — |
| conc 32×8 thr, reconn=100 (**~780/s**) | 0.1008 (−3.5%) | 2348 / – / 2714 | 1/conn | 2.7 µs local |
| 32 KiB × conc 32 (permanent grow+pad) | **7.9 Gb/s** | 637 / 1041 / 1101 | 1 per 2.3 msgs | — |

- **Steady state**: the per-message path touches no shared state — cursor
  arithmetic plus an owner-local recycle; one pool CAS per **connection
  lifetime** (the lazy first block).
- **Churn**: local close+connect+pin ≈ **3 µs**; end-to-end new-conn cost
  (DPU upstream create + remote accept) ≈ 76–316 µs (noisy, 10 s runs). Even at
  755–780 reconnects/s — ~8× DSB's worst per-pod rate, and ~10⁴× gRPC steady
  rates — p50/p95 are unchanged and throughput drops ≤ 5.7% (which includes the
  closed-loop window-drain bubble, an artifact real servers don't pay).
- **The "occasional extra buffer"**: in the worst case we can construct (every
  message pads and grows), grow runs once per 2.3 messages and the link still
  moves 7.9 Gb/s; `waits` there (0.57/msg) is the `MAXB` in-flight cap doing
  flow control, not allocator contention.

Scaled to reality: a DSB Thrift pod churning 100 conns/s pays 100 × 3 µs =
**0.03% of one core**; a conn living its designed 10 s amortizes even the full
end-to-end setup to **≤ 0.003%** of its lifetime. gRPC services, with conn
lifetimes of minutes to hours, round to zero.

## 4. Recorded experimental setup

**Build / date.** 2026-07-14, git `7509a07` + working tree (includes the elastic
buffer commit `a202796` and the `dpumesh_get_tx_pool_stats` counters). Full
`bash bench/bench.sh deploy` (host lib + DPU binary + images + pods rebuilt
together — matched wire ABI). Every run `fail=0`.

**Host.** Benchmark cores 0–7 isolated by `taskset` (profile `fair`), CPU
governor `performance`, frequency fixed at 2.5 GHz (set by deploy).

**Pod placement (fair pinning; 1 host core per DPUmesh pod).**

| pod (service id) | core | role in these runs |
|---|---|---|
| bench-dpumesh (10) | 0 | client daemon — **all 1–8 worker threads + the lib PE thread share this one core** |
| echo-dpumesh (11) | 1 | Greeter backend actually serving the traffic |
| echo-dpumesh-13 (13) | 6 | service-11 LB backend — observed ~idle (conns pinned to pod 11) |
| echo-dpumesh-14 (14) | 7 | service-11 LB backend — observed ~idle |
| bench-tcp / echo-tcp (+Envoy) | 2 / 3 | idle (TCP baseline not exercised here) |
| loopback / preload | 4,5 | idle validators |

Client env: `BENCH_WORKER_ID=10`, `BENCH_DST_POD_ID=11` (service 11 = pods
{11,13,14}; every conn is sticky to its first backend so it stays on one backend),
`BENCH_RX_SCRATCH` default 64 KiB.

**DPU process (`dpumesh_dpu`, BlueField-3, log level 40) — all data-path knobs
at defaults:** `DPUMESH_PROXY` unset → passthru parser; `DPUMESH_ARM_EGRESS_THREADS`
unset → n_eng=1 (egress inline on the main ARM loop); `DPUMESH_INGEST_REAP`
unset → off; `DPUMESH_INGEST_SHARDS` unset → M=1 (single ARM funnel);
`DPUMESH_RINGS_PER_POD=2` (K=2 forward rings/pod); DPA EU count auto-detected.

**Buffer knobs under test — all defaults:** `slot_size` 8 KiB × `num_slots`
4096 = **32 MiB TX pool per pod**; `DPUMESH_TX_BLOCK` 64 KiB → 512 blocks;
`DPUMESH_TX_MAXB` 4 (256 KiB per-conn in-flight cap); `DPUMESH_TX_H` 1
(one recycled-block cushion).

**Per-run parameters** (`bash bench/bench.sh point dpumesh <req> <reply> <conc>
<dur> <warmup> <threads> [reconn]`):

| family | req/reply (B) | conc | threads | dur (s) | warmup | reconn |
|---|---|---|---|---|---|---|
| latency ×3 | 64 / 8 | 1 | 1 | 10 | 1000 | 0, 100, 10 |
| rate ×3 | 64 / 8 | 32 | 8 | 10 | 1000 | 0, 1000, 100 |
| grow/pad ×1 | 32768 / 8 | 32 | 1 | 10 | 200 | 0 |

**Core usage** (sampled during a repeat of the steady rate point — 64 B,
conc 32×8 thr, reconn=0 — on a same-day redeploy of the identical tree; pin
affinities re-verified. Host: `top -b -d 2` over ~12 s; DPU: `bench.sh dpucpu`
= per-thread `top -bH -d 1 -n 2`. Two independent samplings agreed):

| where | %CPU during run | note |
|---|---|---|
| bench-dpumesh process (core 0) | **4.5–6.7 %** | 8 worker threads + PE thread combined |
| echo-dpumesh process (core 1) | **1.5–2.5 %** | single-loop Greeter |
| echo-13 / echo-14 (cores 6/7) | ~0 % | pinned conns landed on pod 11 |
| dpumesh_dpu threads (ARM) | ≤ **4 %** per thread (5 active threads), RSS 2.3 GB | ARM funnel far from saturation at 0.104 Mrps |

Both host cores and the DPU ARM threads are mostly idle at this closed-loop
operating point — the measured rate is latency-bound (Little's law:
256 outstanding / 2.45 ms ≈ 0.104 Mrps), not CPU-bound, so the churn/allocator
deltas in §3c are not masked by CPU saturation on either side.

## 5. Reproduce

```bash
bash bench/bench.sh deploy
bash bench/bench.sh point dpumesh 64 8 1  10 1000 1  100   # churn: reconnect every 100
bash bench/bench.sh point dpumesh 64 8 32 10 1000 8  0     # steady, read grabs/waits
bash bench/bench.sh point dpumesh 32768 8 32 10 200 1 0    # grow/pad worst case
```

OK-line fields: `reconns/reconn_us` (churn), `grabs/rets/recyc/waits/pads`
(pool event deltas; see BENCH.md §4). Sources for §2: DeathStarBench
`socialNetwork/src/ClientPool.h`, `config/service-config.json`,
`hotelReservation/dialer/dialer.go`; microservices-demo `src/frontend/main.go`;
grpc-go `dialoptions.go` (idleTimeout), gRPC `connection-backoff.md`.
