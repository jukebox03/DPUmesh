# DPUmesh evaluation suite — staged plan

The reconstruction of `bench/` from a set of single-shot engineering runs into a
staged, repeatable evaluation that a top-tier reviewer can trust. It keeps the
existing HW-validated harness (`bench.sh deploy`, the DPU transport, the k8s pods)
and adds, on top, three things that were missing: a **matched-language baseline**,
an **open-loop offered-load model**, and a **statistics + figure pipeline**
(repetitions → median + 95% CI → plots).

Everything here shares one CSV schema and one analyzer:

| script | where it runs | what it does |
|---|---|---|
| `suite/run_suite.sh`  | against `bench.sh deploy` on the DPU host | the full staged run across all live transports (rtt/conc/bw/curve) |
| `suite/cpu_probe.sh`  | on the DPU host node | host + DPU-ARM CPU accounting for one load point |
| `suite/analyze.py`    | anywhere | reps → median + 95% bootstrap CI, tidy → summary |
| `suite/plot.py`       | anywhere | summary → figures (renders only the points present) |

---

## Transports (the comparison matrix)

The whole point of the matched C client (`bench_sock.c`) + C server (`echo_sock.c`)
is that ONE workload rides every layer, so a measured gap is attributable:

| transport id | client | server | path | isolates |
|---|---|---|---|---|
| `tcp-direct`      | bench_sock (C) | echo_sock (C) | kernel TCP, no proxy | baseline TCP |
| `tcp-envoy`       | bench_sock (C) | echo_sock (C) | kernel TCP + 2× Envoy sidecar | the sidecar tax |
| `dpumesh-preload` | bench_sock (C) | echo_sock (C) | same socket calls, LD_PRELOAD → DPU | DPU transport under an UNMODIFIED app |
| `dpumesh-native`  | bench_dpumesh (C) | echo_dpumesh (C) | native verbs-shaped API on the DPU | the native API's ceiling |

`tcp-envoy`, `dpumesh-native`, and **`tcp-direct`** (bench-direct → echo_sock, no sidecar) are
LIVE. **`dpumesh-preload` is blocked**: `libdmesh_preload.so` does not hook `epoll`, and
`bench_sock` is epoll-driven, so the POSIX-shim path can't be measured with the matched client
(a shim feature gap, not a result — see `../report/REPORT.md`). The former Go client
(`bench_tcp.go`) was removed after it quantified the confound behind the retracted "2× DPUmesh".

---

## Research questions → stages → figures

| RQ | claim to prove | stage(s) | figure |
|---|---|---|---|
| RQ1 | the verbs-shaped API's abstraction cost is small | `conc`, `apicost` (native/copy/preload) | fig_tput_vs_conc, fig_lang_confound |
| RQ2 | small RPC *and* large transfer are both efficient | `rtt`, `bw`, `curve` | fig_rtt_vs_size, fig_lat_throughput, fig_goodput_vs_size |
| RQ3 | DPU offload returns host cores to the app | `cpu`, `appwork` | fig_cpu_accounting, fig_appwork |
| RQ4 | service-addressed routing scales & stays stable | `scale`, `route`, `fault` | fig_scale, fig_route_lb, fig_fault_recovery |
| RQ5 | the benefit reaches a real application | `dag`, `kv` | fig_service_dag, fig_ycsb |

---

## Phases (do them in order; each is a self-contained run)

### P0 — the six results that convince an advisor (mostly harness-only)
Everything here is measurable with what exists today plus the two manifest additions.

1. **`rtt`** — unloaded RTT vs size, conc=1, sizes 64B…64KB, all transports. → *fig_rtt_vs_size*
2. **`conc`** — closed-loop throughput vs concurrency window, 1KB, all transports. → *fig_tput_vs_conc*
3. **`curve`** — OPEN-loop latency vs offered load, 64B & 1KB, const + Poisson. THE figure. → *fig_lat_throughput*
4. **`bw`** — goodput vs size to 8MB, closed loop. → *fig_goodput_vs_size*
5. **`apicost`** — native vs native-copy vs preload vs direct, + `DMESH_SEND_MORE` batch {1,2,4,8,16,32}. → ablation table
6. **`cpu` + `appwork`** — host CPU + DPU ARM CPU under a fixed core budget, with calibrated app-work {0,5,25,50 µs} injected in the greeter. → *fig_cpu_accounting*, *fig_appwork*

### P1 — before submission
- **`scale`** — QP/conn {1,8,64,512,4K}, backend pods {1,2,4,8}, active services {1,8,32,128}, DPU ingest/egress {1/1,2/2,4/4}. Main config fixed at INGEST_SHARDS=2, ARM_EGRESS_THREADS=2; 1/1 is an ablation only.
- **`route`** — codec per-message LB vs no-codec pinning: backend distribution, Jain fairness, reorder %, multi-stream reassembly. Compare frame-aware routing to a *host software proxy speaking the same frame protocol*, NOT to Envoy L4 tcp_proxy (different function — see caveat).
- **`fault`** — RX-credit stall, slow-backend injection, backend kill, conn churn {0,10,100,1000/s}, elephant+mouse, multi-tenant, 30–60 min soak. Fixed fault timeline, repeated.

### P2 — full evaluation
- **`dag`** — controlled service DAG (chain {1,2,4,8}, fan-out {1,2,4,8}, hop work {0,5,20 µs}).
- **`kv`** — RocksDB/Memcached, YCSB A/B/C, uniform + Zipf 0.9.
- power / energy (RPC/J) — only if board-level instrumentation is credible; otherwise omit rather than hand-wave.

---

## Measurement protocol (applied to every point)

- **Freeze**: `run_suite.sh` freezes `meta.txt` from the **live system**, not the invoking
  shell — the DPU operating point read from `/proc/<dpumesh_dpu>/environ` (proves the (4,4)
  config), **all** container-image digests, node/kernel, kubectl, commit + dirty count, and
  the run params. Still to add for a paper: DOCA/firmware version, CPU model string, and a
  per-core governor snapshot. *(This project's Mrps are config-specific — mixing configs
  silently fabricates deltas.)*
- **Reps**: ≥5 per point (≥10 for the key latency points). Report median + 95% bootstrap CI.
  This run used 8 (conc) / 6 (rtt) / 4 (bw) / 3 (cpu) — tighten the headline to ≥10 for submission.
- **Order**: ABBA / alternate transport order per rep, to spread thermal & time-order bias.
- **Warmup**: excluded by completion count today; a fixed-time warmup is the intended refinement.
- **Tail validity**: the OK line reports `overflow` (samples past the 1.05 s histogram
  ceiling). Any nonzero value means a reported tail percentile may be pinned to max — the
  analyzer surfaces it so such points are read with care (matters only under deep overload).
- **Honesty of scope**: all results are **intra-node** (pod ↔ pod through the *local* DPU).
  Figures and abstract must say so; these are not general inter-host service-mesh numbers.

---

## Framing the API (wording that survives review)

- Not "an RDMA API" but: *a verbs-shaped, service-addressed, two-sided messaging API
  over pre-registered DMA buffers and completion queues* — there is no one-sided
  READ/WRITE/atomic (dmesh.h states this outright).
- "Zero-copy" means the **application↔transport boundary** (TX `dmesh_alloc`, RX mmap),
  not end-to-end: the DPU still DMAs the bytes.

## Fairness caveat (do not overclaim)
DPU frame-codec per-message load balancing is **not** functionally equivalent to Envoy
L4 `tcp_proxy` (connection-level). Compare like function to like: L4 passthrough vs
`tcp_proxy`; frame-aware routing vs a host proxy speaking the same frame protocol; and
only compare to Envoy HCM once an HTTP/gRPC codec exists.

---

## Deploy TODO to light up the two missing transports
1. `tcp-direct`: a Service that points bench_sock straight at echo_sock's port,
   bypassing the sidecars (add `BENCH_TARGET=echo-sock:PORT` variant pod).
2. `dpumesh-preload`: a pod running `echo_sock` and `bench_sock` under
   `LD_PRELOAD=libdmesh_preload.so` (the shim already exists; validators/preload_dpumesh
   is the pattern). Then `curve`/`rtt`/`conc` cover all four transports with one client.

---

## What has actually been run
Live BlueField-DPU, matched-C, intra-node, fair 1-core. **A-phase** (4,4): 3-transport ablation
(dpumesh-native / tcp-envoy / tcp-direct) `conc`(8 reps)/`rtt`(6)/`bw`(4) + host+DPU CPU (3 reps)
+ idle baseline. **Batching phase** (4,4): matched-batching ablation (`batch_ablation.sh`, 6 reps)
+ DPU-ARM cost (`batch_cpu.sh`). **B-phase**: busy-app sweep (`APP_WORK_US`) + N-pod amortization
(`npod.sh`, N=1..3) across the **(4,4)/(2,2)/(1,1)** DPU-config frontier. Results
(`../report/REPORT.md`, reproducible):

- **throughput/latency** — direct kernel TCP ≫ TCP+Envoy > DPUmesh (conc32: 0.93 / 0.42 / 0.19
  Mrps). The Envoy sidecar tax is ~2.3×, but DPUmesh sits *below* even the taxed path.
- **batching-fairness** — the throughput columns are TCP-batched vs DPUmesh-per-RPC. Matched
  coalescing (`batch_ablation.sh`) lifts DPUmesh +32%/+77% (conc 32/64) and restores its latency,
  but it stays below Envoy (0.61×/0.83×). The win is super-additive: only coalescing *both* legs helps.
- **CPU** — DPUmesh returns *client* cycles, but total host CPU/req is *worse* than Envoy (host-eff
  0.44 vs 0.25); the transport is relocated onto 3.3–4.6 DPU cores; host+DPU total is **7.5–10.9×**
  TCP. ARM cost is per-RPC, not core-bound — batching halves ARM µs/RPC (`batch_cpu.sh`).
- **amortization FAILS** — DPU cost scales with traffic (not a fixed overhead), so sharing it
  across pods doesn't divide it; (2,2) amortizes best but stays ~10× TCP; (1,1) is
  throughput-capped and does not scale.
- **busy-app** — the one DPUmesh win: under app-work ≥5 µs it edges tcp-envoy by ~15–23%.

Still open: **L7** (heavier sidecar — the likeliest lever), inter-host, and the preload ablation
(needs shim epoll support). The busy-app & N-pod sweeps are 1-rep — tighten to ≥10 before
publication. The open-loop `curve` stage remains excluded (single-epoll generator-limited).
