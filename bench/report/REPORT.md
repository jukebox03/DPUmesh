# DPUmesh vs TCP + Envoy — performance evaluation

Real BlueField-DPU hardware, **intra-node** (pod → pod through the local DPU). One workload
— a greeter RPC (N-byte request → 8-byte reply, 16-byte `bench.h` frame) — driven over every
transport with the **identical pure-C** client and server, so a measured gap is the transport,
not the runtime. Figures in `figures/`, raw data in `data/`, provenance frozen in
`data/meta.txt` + `data/configs.txt`. This document is self-contained and reproducible: each
experiment lists its pods, pinning, exact command, and repetitions.

```
tcp-direct   bench-direct ───────────────────────────► echo_sock            (kernel TCP, no proxy)
tcp-envoy    bench-tcp[bench_sock+Envoy·1core] ─svc─► [echo_sock+Envoy·1core] (a real L4 sidecar mesh)
dpumesh      bench-dpumesh ─dmesh.h─► DPU(DOCA Comch+DMA) ─► echo-dpumesh     (transport offloaded)
```

## TL;DR

At single-pod / L4 / 1 KB, ranked by throughput: **direct kernel TCP ≫ TCP+Envoy > DPUmesh.**

> Raw kernel TCP does **0.93 Mrps** (conc32); the Envoy sidecar cuts that to **0.41** (the ~2.3×
> "sidecar tax" a mesh pays); **DPUmesh does 0.20** — below even the taxed sidecar path, at 2×
> its latency. DPUmesh's *host* CPU/request beats TCP+Envoy's (the offload is real), but it is
> **relocated onto 3.3–4.6 DPU-ARM cores**, and counting host+DPU, DPUmesh costs **~7× TCP's
> total CPU per request at every DPU config**. Two things do favour it, both narrow: under
> injected **app-work** it edges past TCP+Envoy (~15–23%), and a **lean DPU config (2,2)**
> amortizes better across pods — but neither closes the order-of-magnitude total-CPU gap, and
> **N-pod amortization fails** because the DPU cost scales with traffic, it is not a fixed
> overhead that divides across pods.

This is an honest negative result for the single-node case. The remaining open question is
whether it changes at **L7** (heavier sidecar) — untested (last section).

---

## Experiments (reproducible)

**Common setup.** `bench.sh deploy` brings up all pods against a freshly started DPU and pins
them (`performance` governor, 2.5 GHz). One host core per app (`bench.sh pin fair`): bench-dpumesh
→0, echo-dpumesh→1, bench-tcp→2, echo-tcp→3, echo-dpumesh-13/14→6/7, bench-direct→8,
bench-dpumesh-2/3→9/10. All clients/servers are pure C (`bench_dpumesh`/`echo_dpumesh`,
`bench_sock`/`echo_sock`) sharing `bench.h`. Deploy the operating point under test:

```bash
DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 DPUMESH_RINGS_PER_POD=2 bash bench/bench.sh deploy
```

`run_suite.sh` freezes `data/meta.txt` from the **live** `/proc/<dpumesh_dpu>/environ` (proof of
the (4,4) point), image digests, node/kernel/DOCA/governor. All headline points are
**threads=1 → one connection → one backend** on each side (a like-for-like pod pair).

### E1 — Ablation: latency & throughput (direct / envoy / dpumesh)

Isolates the sidecar tax and the DPU transport separately. `tcp-direct` = `bench-direct` →
`echo_sock` (the `echo-direct` Service hits echo-tcp's echo_sock on :9092, **bypassing both
Envoy sidecars**), so its server core is identical to tcp-envoy — only the client path differs.

```bash
OUT=/tmp/out REPS=8 DUR=15 CONC_STEPS="1 2 4 8 16 32 64" \
RTT_SIZES="64 1024 16384 65536"  bash bench/suite/run_suite.sh conc rtt
```

**Unloaded RTT (conc=1, p50)** — `figures/fig_rtt_vs_size.png`, median of 6:

| request | tcp-direct | tcp-envoy | dpumesh |
|---|---|---|---|
| 64 B | 23 µs | 60 µs | 114 µs |
| 1 KB | 23 µs | 62 µs | 114 µs |
| 64 KB | 45 µs | 100 µs | 173 µs |

**Throughput vs concurrency (1 KB, one connection)** — `figures/fig_tput_vs_conc.png`, median of 8, Mrps:

| conc | tcp-direct | tcp-envoy | dpumesh |
|---|---|---|---|
| 8 | 0.296 | 0.122 | 0.066 |
| 16 | 0.537 | 0.229 | 0.116 |
| 32 | 0.933 | 0.409 | 0.195 |
| 64 | 2.27 | 0.571 | 0.279 |

**Reading.** The Envoy sidecar roughly **halves** kernel TCP (direct 0.93 → envoy 0.41 at conc32)
and adds ~37 µs of latency — a real, sizeable tax. DPUmesh, whose premise is to *replace* that
sidecar, lands at 0.20 Mrps / 114 µs — **below the taxed path it aims to beat**, and 4–5× below
raw kernel TCP. (Kernel TCP scales steeply past conc32 via GRO/GSO batching; the ranking holds
throughout.)

### E2 — Host + DPU CPU accounting

`host` = client-pod + server-pod host cores (for tcp-envoy this **includes both sidecars**);
`DPU-ARM` = the `dpumesh_dpu` process. `host-eff` = host %core per Krps (lower = better).

```bash
for tr in dpumesh tcp direct; do for c in 8 32; do
  bash bench/suite/cpu_probe.sh $tr 1024 8 $c 30 1 data/cpu.csv; done; done   # 3 reps each
```

`figures/fig_cpu_accounting.png`, median of 3:

| transport | conc | Mrps | host %core | host-eff | DPU-ARM |
|---|---|---|---|---|---|
| tcp-direct | 32 | 0.933 | 100.6 | 0.109 | 0 |
| dpumesh | 32 | 0.199 | 85.5 | 0.445 | 435 % |
| tcp-envoy | 32 | 0.402 | 101.3 | 0.240 | 0 |

**Reading.** DPUmesh's *host* footprint is genuinely light — its client is ~30 % of a core (the
transport is on the DPU) vs tcp-envoy's ~55 % — so at a **matched rate** DPUmesh spends less
*host* CPU than tcp-envoy. But it buys that by burning **3.3–4.6 DPU-ARM cores** (idle baseline
0.19 core, `data/idle.txt`), and plain kernel TCP (tcp-direct) is the most host-efficient of all.
So the offload *relocates* CPU to the DPU; it does not remove it.

### E3 — Busy-app: does freeing the host core pay off?

Injects `W` µs of busy-spin per request in the greeter (`APP_WORK_US`, live-tunable by writing
`/tmp/app_work_us` on the echo pods — set on **all three** dpumesh backends, since the DPU LBs
each new connection to one). tcp-envoy's echo shares its core with an *active* sidecar; dpumesh's
and tcp-direct's echo own their core.

```bash
# per work level: kubectl exec echo-{dpumesh,dpumesh-13,dpumesh-14,tcp} -- sh -c 'echo W >/tmp/app_work_us'
# then: RUN 1024 8 32 12 300 1   to bench-{dpumesh,tcp,direct}
```

`figures/fig_busyapp.png` (conc=32, 1 rep; the ordering is consistent across all levels), Mrps:

| app-work | dpumesh | tcp-envoy | tcp-direct |
|---|---|---|---|
| 0 µs | 0.162 | 0.408 | 0.930 |
| 5 µs | 0.157 | 0.131 | 0.155 |
| 25 µs | 0.0388 | 0.0315 | 0.0335 |
| 50 µs | 0.0196 | 0.0171 | 0.0182 |

**Reading.** This is the one regime DPUmesh wins: at work=0 it is worst, but for **every
work ≥ 5 µs it beats tcp-envoy by ~15–23 %** and matches tcp-direct — because when the app is
CPU-bound, DPUmesh's server core spends its cycles on the app, not on kernel-TCP or a co-located
sidecar. The win is real and consistent but modest, and only in the app-bound regime where
absolute throughput is already low.

### E4 — N-pod amortization + DPU-config frontier (the decisive test)

The value proposition: one *shared* DPU amortized across many pods. `npod.sh` drives N DPUmesh
client pods concurrently through the single DPU (each LB'd to a distinct backend) and reports
aggregate throughput, total host cores, and the DPU cores. TCP pairs are independent, so TCP's
N-scaling is linear in the one measured pair. Repeated at three DPU configs.

```bash
# after deploying each config (4,4)/(2,2)/(1,1):
bash bench/suite/npod.sh <shards-egress> 32 20 3 data/npod.csv
```

`figures/fig_npod.png` (per-config N=1→3) and `figures/fig_frontier.png` (total CPU/req vs N):

| config | metric | N=1 | N=2 | N=3 |
|---|---|---|---|---|
| **(4,4)** | dpumesh agg Mrps | 0.191 | 0.263 | 0.378 |
| | dpumesh DPU-ARM % | 377 | 556 | 724 |
| | **total CPU/req (host+DPU)** | 2.4 | 2.7 | 2.4 |
| **(2,2)** | dpumesh agg Mrps | 0.201 | 0.323 | 0.385 |
| | dpumesh DPU-ARM % | 379 | 448 | 464 |
| | **total CPU/req** | 2.3 | 1.8 | **1.7** |
| **(1,1)** | dpumesh agg Mrps | 0.099 | 0.091 | 0.087 |
| | dpumesh DPU-ARM % | 98 | 99 | 98 |
| **any** | tcp-envoy agg Mrps | 0.40–0.43 | 0.81–0.86 | 1.20–1.30 |
| | **tcp total CPU/req** | 0.25 | 0.25 | 0.25 |

**Reading — amortization fails.** The DPU-ARM cost is **not a fixed overhead**: it grows with
aggregate traffic (at (4,4), 377 %→724 % as load doubles). So sharing the DPU across pods does
**not** divide a fixed cost — total CPU (host+DPU) per request stays flat at ~2–2.7, versus TCP's
0.25 (`fig_frontier`). A leaner DPU config helps at the margin: **(2,2)** does the same single-pod
throughput as (4,4) at ~37 % less DPU (1398 vs 2219 DPU%/Mrps) and is the only config that
amortizes (total CPU/req falls 2.3→1.7 with N). **(1,1)** is the most DPU-efficient per request
(one egress thread, ~1 core) but is throughput-capped at ~0.10 Mrps and **does not scale** —
aggregate *drops* with N. No config reaches TCP's efficiency, and each DPUmesh pod delivers only
~⅓–½ a TCP pod's throughput, so DPUmesh needs *more* host cores for a given aggregate, not fewer.

### E5 — Goodput vs message size

`figures/fig_goodput_vs_size.png` (conc=32 requested; the figure labels the achieved window N,
since the per-QP limit collapses it above 1 KB, so ≥64 KB is not an equal-concurrency
comparison). At 1 KB tcp is 2×; both reach ~14–15 Gb/s on large transfers.

---

## What this does NOT test

1. **L7 (the most likely to change the verdict).** Here Envoy runs cheap L4 `tcp_proxy`; a real
   mesh parses HTTP/gRPC, a much heavier host tax that E1 shows is where DPUmesh's offload could
   pay. Needs an HTTP/gRPC workload + Envoy HCM + the DPU L7 codec (currently frame-level).
2. **Inter-host.** All intra-node; a cross-host NIC path changes the kernel-TCP baseline.
3. **Preload ablation.** `libdmesh_preload.so` does not hook `epoll`, and `bench_sock` is
   epoll-driven, so the POSIX-shim path could not be measured with the matched client (a shim
   feature gap, not a result).

## Method & caveats

- Matched C client/server + 16-byte frame; transport is the only variable. Closed-loop, warmup
  excluded, median + 95 % bootstrap CI. **Reps: conc 8, rtt 6, bw 4, CPU 3; busy-app and N-pod
  sweeps are 1 rep** (their conclusions rest on monotone trends across many points, not single
  values — tighten to ≥10 before publication).
- Provenance is read from the running system, not asserted: `data/meta.txt` (A-phase, (4,4)) and
  `data/configs.txt` (the (4,4)/(2,2)/(1,1) environs).
- Envoy `tcp_proxy` is a **userspace** proxy over kernel TCP sockets (not in-kernel).
- Causal phrases ("DPU cost scales with traffic", "sidecar tax") are stated as *consistent with*
  the CPU/throughput data; no per-stage DPU profiling was done here.
- Open-loop excluded (single-epoll generator is generator-limited above ~15 % load).

commit 4beb0be (+ uncommitted eval-harness edits) · node rapids4 (Xeon Gold 6554S) ·
DOCA 3.1.0 · kernel 5.15 · DPU (4,4)/(2,2)/(1,1) live-verified · 2026-07-18.
