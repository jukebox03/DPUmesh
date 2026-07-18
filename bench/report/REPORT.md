# DPUmesh vs TCP + Envoy — L4 performance evaluation

Real BlueField-3 hardware, **intra-node** (pod → local DPU → pod). One workload — a greeter
RPC (N-byte request → 8-byte reply, 16-byte `bench.h` frame) — driven over every transport by
the **same pure-C** client and server, so the language runtime (GC, scheduler) is never the
variable. Figures in `figures/`, raw data in `data/`, provenance in `data/meta.txt`. Every
experiment below lists its pods, pinning, exact command, and repetitions.

```
tcp-direct   bench-direct ─────────────────────────────► echo_sock              (kernel TCP, no proxy)
tcp-envoy    bench-tcp[bench_sock+Envoy·1core] ─svc─► [echo_sock+Envoy·1core]   (a real L4 sidecar mesh)
dpumesh      bench-dpumesh ─dmesh.h─► DPU(DOCA Comch+DMA) ─► echo-dpumesh        (transport offloaded)
```

## Abstract

At L4 / 1 KB / one connection, ranked by throughput: **direct kernel TCP ≫ TCP+Envoy > DPUmesh.**
This holds *after* correcting a benchmark-fairness gap the first report missed — the TCP baseline
coalesces its window into one `write()` while DPUmesh flushed **per RPC**. Giving DPUmesh matched
coalescing lifts it **+32 % (conc 32) / +77 % (conc 64)** and collapses its loaded latency to the
single-RPC floor, but it still lands **below Envoy** (0.61× / 0.83×) and far below direct TCP. The
offload is real — it returns *client* host cycles — but it **relocates** the transport onto 3.3–4.6
DPU-ARM cores; counting host+DPU, DPUmesh spends **7.5–10.9× TCP's total CPU per request**. That ARM
cost is **per-RPC overhead, not core speed**: symmetric coalescing **halves** ARM µs/RPC. An honest
negative for single-node L4; the value-prop regimes (heavier L7 sidecar, busy apps) remain the open
question.

---

## Setup & reproducibility

**Hardware / software.** Node `rapids4` — Intel Xeon Gold 6554S + BlueField-3 DPU, DOCA 3.1.0,
kernel 5.15, `performance` governor pinned at 2.5 GHz (`data/meta.txt`).

**Bring-up (one command).** `bench.sh deploy` builds the DPU transport, the container images and
the pods, starts the DPU and every pod together, and pins them. The headline operating point:

```bash
DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 DPUMESH_RINGS_PER_POD=2 \
  bash bench/bench.sh deploy          # the "(4,4)" config; §5 sweeps (2,2) and (1,1)
```

**Pinning (`bench.sh pin fair`).** One host core per app: bench-dpumesh→0, echo-dpumesh→1,
bench-tcp→2, echo-tcp→3, echo-dpumesh-13/14→6/7, bench-direct→8. The dpumesh app gets a full core
(its transport is on the DPU); each TCP app shares its core with its Envoy sidecar — the sidecar
tax, counted honestly.

**Transports.** All are driven by a one-line TCP control protocol (`nc`, port 9092):
`RUN <req> <reply> <conc> <dur> <warmup> <threads>`.
- `tcp-direct` — `bench-direct` (`bench_sock`) → `echo_sock`, **no sidecar** (isolates the Envoy tax).
- `tcp-envoy` — `bench-tcp` (`bench_sock`+Envoy `tcp_proxy`) → svc → (`echo_sock`+Envoy).
- `dpumesh` — `bench_dpumesh` (`dmesh.h`) → DPU → `echo_dpumesh`. Control arg 8 is `batch` (below).

**Workload.** 1 KB request, 8 B reply, 16 B frame, **one connection** (`threads=1`) — one backend
per side, a like-for-like pod pair. Closed loop, warmup excluded, median + 95 % bootstrap CI
(`suite/analyze.py`). A point is trusted only at **fail = 0, reorder = 0**, and Little's-law ratio
(throughput × avg-RTT / conc) in 0.98–1.02.

**The matched-batching knobs (the fairness fix, §2–§4).** The TCP client/server each coalesce a
whole batch into one syscall (`bench_sock.c`, `echo_sock.c`); DPUmesh, by default, rings one
doorbell per RPC. Two off-by-default knobs give it the same coalescing:
- **client** — the 8th `RUN` arg `batch=1`: `bench_dpumesh.c` marks every frame in the burst it
  issues per loop pass `DMESH_SEND_MORE` and flushes once.
- **server** — `/tmp/reply_batch=1` on the echo pods (polled every 0.5 s): `echo_dpumesh.c`
  coalesces a CQ batch's replies into one doorbell/conn.

**Reproduce each figure.**

| figure | command | data |
|---|---|---|
| RTT vs size, throughput/CPU sweeps | `REPS=8 DUR=15 bench/suite/run_suite.sh rtt conc bw` | `data/summary.csv` |
| batching ablation (§2, §3) | `bench/suite/batch_ablation.sh` | `data/batch_ablation.csv` |
| DPU-ARM cost (§4) | `bench/suite/batch_cpu.sh` | `data/batch_cpu.csv` |
| host+DPU CPU (§4) | `for c in 8 32 64; do bench/suite/cpu_probe.sh <tr> 1024 8 $c 30; done` | `data/cpu.csv` |
| config / N-pod (§5, §6) | `bench/suite/npod.sh <cfg> 32 20 3` per deployed config | `data/{cpu_configs,npod}.csv` |
| plots | `python3 bench/suite/plot_batch.py data figures` ; `python3 bench/suite/plot.py …` | `figures/` |

**Provenance note.** §1–§4 (latency, throughput, batching, ARM CPU) were **re-measured 2026-07-18**
on the current build (which adds the off-by-default batching knobs and an atomic fix to a lane-inbox
data race in `doca/dpu_proxy.c`); the unbatched/TCP anchors reproduced the frozen numbers within CI.
§5–§6 (config frontier, N-pod, busy-app, goodput) are from the frozen `commit 4beb0be` (4,4) campaign
— same workload and pinning — and are marked where they are single-rep.

---

## 1. Unloaded latency (conc = 1)

Nothing to coalesce at conc=1, so this is a clean transport comparison. Median of 6,
`figures/fig_rtt_vs_size.png`:

| request | tcp-direct | tcp-envoy | dpumesh |
|---|---:|---:|---:|
| 64 B | 23 µs | 60 µs | 114 µs |
| 1 KB | 23 µs | 62 µs | 114 µs |
| 64 KB | 45 µs | 100 µs | 173 µs |

DPUmesh's unloaded RPC is **~52 µs slower than Envoy and ~91 µs slower than direct TCP** at 1 KB.
The 114 µs floor is the DPU pipeline traversal (host→DPU DMA → ARM ingest/route → ARM SG-DMA →
backend, and the mirror on the reply); it is real and batching-independent.

## 2. Throughput vs concurrency — and the batching-fairness correction

The first report's throughput columns compared TCP **batched** against DPUmesh **per-RPC-flush** and
called it "transport only." It is not: above conc=1 the *issue/flush granularity* differs. §2 gives
DPUmesh the matched coalescing (`batch=1` + `reply_batch=1`). Median of 6, `data/batch_ablation.csv`,
`figures/fig_tput_vs_conc.png` + `figures/fig_p50_vs_conc.png`:

| conc | tcp-direct | tcp-envoy | dpumesh (per-RPC) | dpumesh (batched) |
|---:|---:|---:|---:|---:|
| 1 | 0.040 | 0.016 | 0.0085 | 0.0083 |
| 8 | 0.299 | 0.123 | 0.065 | 0.066 |
| 32 | 0.934 | 0.417 | 0.193 | **0.255 (+32 %)** |
| 64 | 2.25 | 0.591 | 0.278 | **0.491 (+77 %)** |

**Reading.** Matched batching is a real, sizeable lever — and it removes almost all of the *loaded*
latency penalty: batched p50 stays at ~117–122 µs across the sweep (the conc=1 floor) while per-RPC
p50 climbs to 240 µs. So the loaded-latency gap the headline showed was mostly per-RPC serialization,
not the transport. **But it does not change the verdict:** even fully batched, DPUmesh is **0.61× of
Envoy at conc32 and 0.83× at conc64**, and 4–5× below direct TCP. The ranking survives; the gap just
narrows. (A prior engineering log's 4.5× coalescing win does **not** reproduce on the current
ARM-SG-DMA reverse path — current best is +77 %.)

## 3. Batching ablation — coalescing must be symmetric

Four-way ablation (none / request-only / reply-only / both) isolates which leg the coalescing helps.
Median of 6, 0 fail / 0 reorder, `figures/fig_batch_ablation.png`:

| conc | none | request-only | reply-only | **both** |
|---:|---:|---:|---:|---:|
| 32 | 0.193 | 0.213 (1.10×) | 0.208 (1.08×) | **0.255 (1.32×)** |
| 64 | 0.278 | 0.277 (0.99×) | 0.285 (1.02×) | **0.491 (1.77×)** |

**Reading.** The effect is **super-additive**: request-only or reply-only *alone* buys ≤10 % (conc32)
or ≈0 % (conc64); only coalescing **both** legs pays. Whichever leg still flushes per RPC re-serializes
the whole pipeline. So the answer to "which DMA leg is the limit?" is *neither in isolation* — it is
the symmetric per-RPC flush.

## 4. CPU cost — host + DPU ARM

**Host CPU** (`data/cpu.csv`, `figures/fig_cpu_accounting.png`; `host-eff` = client+server %core per
Krps, lower = better):

| transport, conc=32 | achieved | host %core | host-eff | DPU-ARM |
|---|---:|---:|---:|---:|
| tcp-direct | 0.933 | 100.6 | **0.109** | 0 |
| tcp-envoy | 0.402 | 101.3 | 0.252 | 0 |
| dpumesh | 0.199 | 85.5 | 0.435 | 3.3–4.6 cores |

The offload returns *client* cycles (dpumesh client ~30 % of a core vs Envoy's ~55 %), but the saving
is **client-only**: total host CPU **per request** is *worse* than Envoy's (0.44 vs 0.25 host-eff — it
sustains ~half the rate), and the transport is not removed, only **relocated** onto the DPU (idle
baseline 0.19 core, `data/idle.txt`). Counting host+DPU occupancy per request, DPUmesh costs
**7.5–10.9× TCP** across all configs (`data/npod.csv`; ARM and Xeon cores summed as an occupancy
proxy — it excludes DPA-EU and power, so read it as occupancy, not a cost-of-ownership number).

**ARM cost is per-RPC, not core-bound** (`data/batch_cpu.csv`, `figures/fig_arm_cpu.png`). Batching
raises throughput *while lowering* ARM occupancy, so ARM µs/RPC drops sharply:

| conc | ARM µs/RPC (per-RPC) | ARM µs/RPC (batched) | Δ |
|---:|---:|---:|---:|
| 32 | 22.4 (429 %) | 12.1 (312 %) | **−46 %** |
| 64 | 16.7 (463 %) | 7.3 (359 %) | **−56 %** |

Coalescing amortizes roughly half the ARM cost away — direct evidence that the bottleneck is per-RPC
fixed overhead (doorbell, per-message DMA, REV_DONE/TX_ACK), not raw ARM core speed.

## 5. DPU config frontier — (1,1) / (2,2) / (4,4)

One connection pins to one forward ring, so extra ingest/egress threads add little single-conn
parallelism (`data/cpu_configs.csv`, conc=32):

| config | throughput | DPU-ARM | p50 |
|---|---:|---:|---:|
| (4,4) | 0.192 | 426 % | 175 µs |
| (2,2) | 0.199 | 279 % | 174 µs |
| (1,1) | 0.100 | 99 % | 338 µs |

**(2,2)** matches (4,4)'s single-pod throughput at ~35 % less ARM, and is the only config whose total
CPU/request *falls* as pods are added (`figures/fig_frontier.png`: 2.3 → 1.7 %core/Krps for N=1→3);
(4,4) balloons and (1,1) is throughput-capped at ~0.10 Mrps. **Recommended default: (2,2)**; use (4,4)
only where multi-connection/multi-pod scaling is proven.

## 6. Value-prop probes (frozen (4,4) campaign; single-rep — directional)

- **N-pod amortization fails** (`figures/fig_npod.png`). The DPU-ARM cost **scales with traffic**, it
  is not a fixed overhead: at (4,4), ARM grows 377 %→724 % as N=1→3. Total CPU/request stays ~flat
  (≈2–2.7 %core/Krps) vs TCP's 0.25, and each dpumesh pod delivers only ⅓–½ a TCP pod — so sharing one
  DPU across pods does **not** divide a fixed cost.
- **Busy-app — the one regime it wins** (`figures/fig_busyapp.png`, `data/busy.csv`, 1 rep). Inject
  `W` µs of app-work per request: at `W=0` dpumesh is worst, but for **every W ≥ 5 µs it edges Envoy
  by ~15–23 %** and ties direct TCP — because a CPU-bound app spends the server core on the app, not on
  kernel-TCP or a co-located sidecar. Real but modest, and only where absolute throughput is already low.
- **Goodput vs size** (`figures/fig_goodput_vs_size.png`). At 1 KB TCP is ~2×; on large transfers both
  reach ~14–15 Gb/s (direct ~30). Above 1 KB the per-QP window collapses (achieved N≈1–4), so ≥64 KB is
  not an equal-concurrency comparison — the figure annotates achieved N.

## 7. What this does NOT test

1. **L7 — the likeliest to change the verdict.** Envoy here runs cheap L4 `tcp_proxy`; a real mesh
   parses HTTP/gRPC, a much heavier host tax that §6's busy-app hints is where offload could pay. Needs
   an HTTP/gRPC workload + Envoy HCM + the DPU L7 codec (currently frame-level).
2. **Inter-host.** All intra-node; a cross-host NIC path changes the kernel-TCP baseline.
3. **Matched-batched host/total CPU.** §4's ARM halving is measured; the host-side CPU of the *batched*
   cells was not separately measured, so the batched total-CPU ratio is not recomputed.
4. **Batching across configs, ≥10-rep busy-app/N-pod, energy/DPA-EU counters** — deferred future work.

## 8. Bottom line

For single-node L4 / 1 KB, DPUmesh is slower and costlier than both TCP paths, and matched batching —
though a real +32–77 % lever that restores its latency — narrows but does not close the gap. The
offload genuinely moves transport CPU off the host, but it relocates rather than removes it, and the
ARM cost is dominated by per-RPC overhead the current pipeline pays on every message. The case for the
architecture rests on the untested regimes (L7 sidecar tax, busy apps), not on this one.

---

commit `af19365` · node rapids4 (Xeon Gold 6554S) · BlueField-3 · DOCA 3.1.0 · kernel 5.15 ·
DPU (4,4) live-verified · §1–§4 re-measured 2026-07-18 on the build with the batching knobs +
lane-inbox data-race fix (0-fail on loopback/preload + the ablation) · §5–§6 frozen from commit
`4beb0be`.
