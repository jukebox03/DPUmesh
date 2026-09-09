# gRPC evaluation procedure

This document alone must be enough to reproduce every number in
[`FINAL.md`](FINAL.md) (the correctness and performance summary) and
[`ANALYSIS.md`](ANALYSIS.md) (the bottleneck analysis). Each arm is four steps —
deploy, pin, sweep, aggregate — and `derive.py` and `plot.py` do the aggregation.

## Questions and verdicts

| Question | Verdict value | Basis |
|---|---|---|
| is it correct | every gate PASS | C |
| what is the capacity | by delivery criterion and by p99 ≤ 5 ms, per payload | P1–P3, `derive.py` |
| what does DPU CPU follow | worker core against in-flight, Arm µs/RPC against rate | P4–P6, `derive.py` |
| how big is it | absolute and per proxy core against direct TCP and Linkerd | R1–R3 |
| what is still unknown | pass/fail of E1–E4 | E |

## Fixed conditions

- One Kubernetes node, one BlueField-3, no cross-node traffic. The DPU process is
  `dpumesh_dpu`, its data worker threads are `dmesh-w0..A-1`, and the affinity
  receipt is [`dpu-cpu-affinity.txt`](dpu-cpu-affinity.txt).
- Geometry derives from the single `DPUMESH_THROUGHPUT_WORKERS=W`: A = K = W, N is
  the largest multiple of W not above 32, and L7 runs on all workers. The default
  W = 8 gives `N/K/A = 32/8/8`.
- Host: client Pod on CPUs 18–26 and server Pod on CPUs 27–35 (the NUMA node of
  PCI 94:00.0), `bench.sh pin grpcmax`, performance governor at 2.5 GHz (set by
  deploy).
- Generator `bench_grpc`: 8 threads, 8 channels (one channel is one QP and one
  worker), 8 reactors, open loop with constant arrival, 10 s, 1,000 warmup
  requests (the retained receipts do not record warmup, so `grpc_worker_scale.sh`'s
  value is fixed as the convention). Latency is the client histogram's
  scheduled-arrival-to-completion. In closed loop, total in-flight is the
  per-worker window times the thread count.
- Frames of 64/1,024/8,192 B are the logical frame of the request and of the
  response each; the body is frame − 16.
- CPU: on the host, the usage delta of the recursive Pod cgroup (application plus
  broker, or application plus sidecar), over a window opened 2.5 s after the start
  and lasting 6 s. On the DPU, the `dmesh-w*` thread tick delta over the same
  window plus the process total. If the eight-worker sum at 8 KiB appears to
  exceed 8.0, the excess is the main and helper threads' share.
- `clean` means all three repeats had achieved/offered ≥ 0.99 and zero failures,
  drops, pending, worker failures, credit loss, EQ exhaustion and Pod restarts.
  `mixed` means clean and failing repeats coexist at the same rate in independent
  repeats. `bad` means it failed from the first repeat. The rate axis stops at the
  first overload (`STOP_ON_OVERLOAD=1`), and a full deploy is redone for every
  payload change.
- One campaign at a time (`/tmp/dpumesh-bench.lock`).

## Common procedure

```sh
cd ~/DPUmesh
export DPUMESH_THROUGHPUT_WORKERS=8 BENCH_REACTORS=8 BENCH_NUMA_POLICY=local
BENCH_DEPLOY_SCOPE=grpc bash bench/bench.sh deploy        # DPU + Pods, always a full deploy
bash bench/bench.sh pin grpcmax                           # 9+9 host CPU pin
# common open-loop env
export CHANNELS=8 THREADS=8 REPS=3 DUR=10 WARMUP=1000 REACTORS_TAG=8 PIN_PROFILE=grpcmax STOP_ON_OVERLOAD=1
```

The open-loop sweep is `bench/suite/grpc_conns_sweep.sh` and the closed-loop sweep
is `bench/suite/grpc_closed_sweep.sh`. Both leave `$OUT/points.csv` (raw
per-repeat) and `$OUT/sweep.log`; the `*-raw.csv` files here concatenate those
`points.csv`, and `*-summary.csv` is the median of three per rate.

## Experiment matrix

| ID | Purpose | Command (on top of the common env) | Output |
|---|---|---|---|
| C | correctness | `bash bench/suite/grpc_correctness.sh all` | [`correctness.txt`](correctness.txt), [`policy-stages.csv`](policy-stages.csv) |
| P1 | 64 B open-loop capacity | `FRAME=64 RATES="40000 50000 60000 70000 80000 85000 90000 92500 95000 97500 100000 102500 105000" OUT=/tmp/p1 bash bench/suite/grpc_conns_sweep.sh` | [`open-raw.csv`](open-raw.csv) |
| P1r | 64 B fresh redeploy repeats | one per deployment: `RATES="90000 92000 94000 96000 98000"`, `RATES="98000 98500"`, `RATES="99000 101000 102000"` | [`knee-followup-raw/fresh-64-*`](knee-followup-raw/) |
| P2 | 1 KiB | `FRAME=1024 RATES="30000 40000 50000 60000 65000 70000 75000 80000"`; after redeploy `RATES="75250 75500 75750"`, `RATES="76000 77000 78000 79000"` | open-raw, `fresh-1k-*` |
| P3 | 8 KiB | `FRAME=8192 RATES="10000 15000 20000 25000 30000"`; after redeploy `"25250 25500 25750 26000"`, `"27000 28000 29000"`, `"29250 29500 29750"`, `"30000"` twice | open-raw, `fresh-8k-*` |
| P4 | closed loop | `THREADS=8 REPS=3 DUR=10 bash bench/suite/grpc_closed_sweep.sh --configs grpc-dpumesh --frames "64 1024 8192" --concs "1 2 4 8 16 32 64 128 256 512 1024" --out /tmp/p4` (`--concs` is the per-worker window; total in-flight is x8) | [`concurrency-raw.csv`](concurrency-raw.csv), [`closed-raw.csv`](closed-raw.csv); 8 KiB windows ≥ 256 failed and went to [`saturation-rejected.csv`](saturation-rejected.csv) |
| P5 | low-load CPU and latency | `FRAME=64 RATES="500 1000 2500 5000 10000"`; `FRAME=1024 RATES=10000`; `FRAME=8192 RATES=10000` | [`dpu-low-load-summary.csv`](dpu-low-load-summary.csv), [`mesh-cpu-raw.csv`](mesh-cpu-raw.csv) |
| P6 | DPU profile | during a 64 B 50k open loop, on the DPU: `pid=$(pgrep -x dpumesh_dpu); perf stat -p $pid -- sleep 8; perf record -F 49 -e cycles --call-graph fp -p $pid -- sleep 8; perf report --no-children`; idle is the same `perf stat` with no session | [`perf-stat.csv`](perf-stat.csv), [`perf-self.csv`](perf-self.csv) |
| P7 | worker count | `for w in 4 6 8 12; do WORKERS=$w OUT=/tmp/p7-a$w bash bench/suite/grpc_worker_scale.sh; done` (`threads=channels=workers`, geometry 32/4/4, 30/6/6, 32/8/8, 24/12/12) | [`worker-scale-raw.csv`](worker-scale-raw.csv) |
| P8 | session fan-in | on the W=8 deployment `CHANNELS=24 THREADS=24 FRAME=64 RATES=80000`; the same command on a W=12 deployment | [`session-scaling-summary.csv`](session-scaling-summary.csv) |
| R1 | Linkerd sidecar closed loop | Pods from [`bench/k8s/grpc-linkerd-pods.yaml`](../../../k8s/grpc-linkerd-pods.yaml) (`linkerd.io/inject: enabled`, `skip-inbound-ports: $CTRL_PORT`, `BENCH_TRANSPORT=tcp`, `BENCH_TARGET=echo-grpc-linkerd:9091`), brought up by deploy. `bash bench/suite/grpc_closed_sweep.sh --configs grpc-linkerd --frames "64 1024 8192" --concs 128 --out /tmp/r1` | [`mesh-closed-raw.csv`](mesh-closed-raw.csv), [`linkerd-receipt.txt`](linkerd-receipt.txt) |
| R2 | matched 10k RPS host CPU | `CLIENT_APP=bench-grpc-linkerd SERVER_APP=echo-grpc-linkerd RATES=10000` for `FRAME=64/1024/8192`; the DPUmesh side is P5's 10k point | [`mesh-cpu-raw.csv`](mesh-cpu-raw.csv) |
| R3 | direct TCP (no mesh) | the same two Pods with the sidecar removed (`linkerd.io/inject: disabled`), `BENCH_TRANSPORT=tcp BENCH_TARGET=echo-grpc-linkerd:9091`, and the same closed sweep as R1. Acceptance: the DPU process must stay ≤ 0.05 core (the DPU is not on the path) | the `direct-tcp` rows of [`closed-raw.csv`](closed-raw.csv) |
| L | low-load diagnosis (E1, E2) | the E1 and E2 commands below | [`lowload/`](lowload/) |

The Linkerd proxy is `edge-26.8.1` with `LINKERD2_PROXY_CORES=1` (the stock
install value), measured after identity and mTLS are confirmed. R1–R3 use the
same client and server binaries and frames as the DPUmesh arm.

## Derived metrics (`derive.py`)

| Metric | Definition | File |
|---|---|---|
| mean in-flight | open loop: achieved x p50; closed loop: total window | `derived-inflight-cpu.csv` |
| occupancy | worker core / in-flight, median over points where in-flight < worker count | same file, `derive.py` output |
| Arm µs/RPC | worker core / achieved x 10⁶ | same file |
| delivery capacity | the highest offered rate that is 3/3 clean and has no mixed or bad fresh repeat at or below it | `derived-capacity.csv` |
| SLO capacity | the above plus a median p99 ≤ 5 ms | same file |
| throughput per proxy core | closed-1,024 throughput / configured proxy cores (DPUmesh 8, Linkerd 2) | `derived-comparison.csv` |
| exchange ratio | (Linkerd host cores − DPUmesh host cores) against DPUmesh Arm worker cores, at 10k RPS | `derived-exchange-10k.csv` |
| payload cost | the rise in knee Arm µs/RPC over 64 B / (2 x the payload increase in bytes) | `derived-knee-cost.csv` |

## Graphs (`plot.py`)

Proportional comparisons (offered-achieved, CPU, bars) start at zero. Only
latency, concurrency and in-flight use log axes. The baseline is a solid grey
line, individual fresh repeats are open circles, and everything after the first
bad point is dashed. There is no smoothing and no clipping.

| File | Content |
|---|---|
| `00_summary` | the only figure in FINAL.md: peak RPS (closed 1,024) and 10k RPS p50 and p99, DPUmesh against Linkerd |
| `01_offered_achieved` | offered against achieved, with `y=x` |
| `02_p50_latency` | p50 from 500 RPS to overload, log-log |
| `03_p99_latency` | p99 per payload with the 5 ms SLO line |
| `04_inflight` | closed-loop total in-flight against throughput and p50 |
| `05_cpu_attribution` | achieved against client/server Pod and DPU worker cores, from 500 RPS |
| `06_comparison` | direct/DPUmesh/Linkerd throughput, per proxy core, and the 10k RPS core exchange |
| `07_perf` | exclusive profile, 50k against idle with no session |
| `08_worker_scaling` | A = 4/6/8/12 |
| `09_inflight_cpu` | worker core against in-flight (with the 0.69 baseline), Arm µs/RPC against rate |
| `10_payload_scaling` | throughput relative to 64 B, knee Arm µs/RPC |
| `11_per_rpc_pmu` | one worker's instructions, cycles, cache misses and µs per request, at 100 RPS against 10k against 50k |

## E. Pre-registered experiments

The pass criteria are fixed before measuring. E1 and E2 are complete and their
results are in ANALYSIS.md §2 and §3 and in [`lowload/`](lowload/).

**E1. Why worker CPU follows the number of open requests — complete.** On the
W=8 deployment,
`CHANNELS=1 THREADS=1 FRAME=64 RATES="100 200 500 1000 2000" REPS=3 WARMUP=100`
(`RATES=10` is excluded because 100 warmup requests consume the whole measurement
window and the result comes back empty). The pre-registered criterion was that
Arm µs/RPC ≤ 150 means event-driven and ≥ 400 means a defect; the result was
620–700, i.e. ≥ 400. The cause was established on the DPU with these three
measurements.

```sh
# 1) PMU on just the worker thread holding the channel (repeated at 100 RPS 1 ch and 10k/50k 8 ch)
pid=$(pgrep -x dpumesh_dpu); busy=$(top -bH -d1 -n2 -p $pid | awk '/ PID +USER/{n++} n==2 && /dmesh-w/ {print $1, $9}' | sort -k2 -nr | head -1 | cut -d" " -f1)
perf stat -e cycles,instructions,cache-references,cache-misses,task-clock,context-switches,raw_syscalls:sys_enter -t $busy -- sleep 10
perf trace -s -t $busy -- sleep 10                       # syscall counts by kind
# 2) runtime loop pass count: deploy with DPUMESH_PERF_STATS=1 and every 10 s the DPU log
#    (bench.sh dpulog) prints cumulative per-worker drains/progressed/pending/idle;
#    take the difference across the run and divide by the RPC count
# 3) the on-CPU shape of one request
perf record -e sched:sched_switch -a -o /tmp/sched.data -- sleep 2
perf script -i /tmp/sched.data -F time,event,trace | grep -E "dmesh-w[0-9]"   # group requests by the 4 ms gaps
```

Verdict: 467k instructions per request (173k at the knee), 4.9k cache misses
(1.7k), IPC 0.30 (0.54), 7.4–8.1 drain passes (half of them Idle), 47 syscalls,
1.2 wakes, and a continuous on-CPU span of 450 µs forward plus 320 µs reverse.
This is a fixed cost per event, not a spin. Ruled out because: an idle worker
ticks at 8.7 µs, the DPU has no cpufreq or cpuidle (a fixed 2.05–2.09 GHz),
session metrics did not grow, and the generator uses `nanosleep`.

**E2. Where the 0.6–1 ms latency floor lives — complete, rejected.**
`TX_TAIL_DELAY_NS` in `src/core/dmesh_core.c` was changed from 500000 to 50000,
followed by a full deploy and three repeats each at 100/500/1,000/2,000 RPS on one
channel and 500/1k/10k RPS on eight channels. The pre-registered criterion was
that p50 < 300 µs at 500 RPS would indicate the host coalescer. Result: 100 RPS
gave 1,605 µs (baseline 1,552–1,684) and 500 RPS 1,098–1,149 (baseline
1,146–1,254), i.e. unchanged, while eight channels at 10k RPS got worse,
611→1,456 µs. The edit was reverted. The floor is in the DPU event path of §3
(about 770 µs on-CPU per request) and in the wake chains on both host sides
(about 800 µs). The warm floor is a p50 of 936 µs at a total of 1 in flight,
measured with `THREADS=1 REPS=3 DUR=10 bash bench/suite/grpc_closed_sweep.sh
--configs grpc-dpumesh --frames 64 --concs "1 2 4"`.

**E3. A core-matched comparison.** Give both Pods
`config.linkerd.io/proxy-cpu-limit: "4"` (4 cores per sidecar) and repeat R1 and
R2. Measure direct TCP three times at `RATES=10000` so the mesh-free p50 and host
cores land in the same table. Report absolute and per-proxy-core side by side, and
attach "n times" only to the per-core values.

**E4. A single worker stalling and degradation.** Five fresh deployments x three
repeats of `RATES="90000 92000"`. If any repeat shows a ratio < 0.99, immediately
save `bash bench/bench.sh dpucpu`, the per-worker
`curl 127.0.0.1:$((4191+id))/metrics | grep ^dmesh_`, and
`bash bench/bench.sh dpulog 4000`. Separately, probe 80k 24 times at one-hour
intervals. Verdict: if 90k is 5/5 clean, keep 90k as the capacity; if a stall
reproduces, put the counter difference between the stalled and healthy workers in
ANALYSIS.md §5.

**E5. Cutting the fixed cost per event — first measurement complete.** Acceptance
has two stages. (1) Hold criteria: 64 B 90k 3/3 clean with no p99 regression,
`grpcshutdown` and policy 19/19 passing, and Arm µs/RPC and syscalls/RPC at 100
RPS on one channel below the baseline. (2) Target: ≤ 200 µs/RPC and ≤ 15
syscalls/RPC at 100 RPS, and a closed-loop p50 below 0.7 ms at a total of 1 in
flight. The target is reached by combining several strands, and each strand that
passes the hold criteria is adopted on its own.

First build: in `linkerd/rust/src/lib.rs`, `ExternalBackend::drain` now calls the
Rust `Worker::drain` before the C `dmesh_l7_driver_drain` so that published bytes
are submitted to DMA in the same pass, and the clear in `doca/dpu_worker.c` reads
the wake eventfd only when the `wake_posted` flag is set (the waker sets the flag
after its write, so a stale tick is read by the next readable pass). The procedure
is the same as E1, and deploy rebuilds the Rust on the DPU, which takes about 13
minutes. Result ([`lowload/e5-ab.csv`](lowload/e5-ab.csv)): worker CPU −2 to −6%,
syscalls/RPC 47→32, latency unchanged, no knee regression, with `grpcshutdown`
(opened = closed 78/78) and policy 19/19 passing
(`bash bench/suite/grpc_correctness.sh hardware`, receipt
[`policy-route-20260902-184855/`](../policy-route-20260902-184855/)). Hold
criteria passed, target not met.

Uprobes on the same build (`perf probe -x <bin> -a name=0x<addr>`, with addresses
from `nm` on the running binary) showed 2.0 hyper server connection polls, 4.0 h2
client connection polls and 21 drains per request
([`lowload/e5-probes-100rps.txt`](lowload/e5-probes-100rps.txt)). The inclusive
profile's 64% task share divided by six polls is about 80 µs per poll, so tidying
the loop side is worth at most a few percent. The next strands are (a) cutting h2
client polls from 4 to 2 (binding the request send and the response receive into
one pass), (b) a bounded spin after an event (100 µs, say) to avoid the wake chain
and cold re-entry, and (c) reducing the depth of the linkerd service stack — each
under the same A/B and hold criteria.

## Report format

FINAL.md runs conclusions → correctness → performance → reproduction, with
`00_summary` as its only figure. ANALYSIS.md's section order is fixed as
conclusions → correctness → capacity and latency → DPU CPU → payload → overload
and repeatability → the three-transport comparison → worker scaling → unmeasured
experiments → reproduction. Every number in every table must come from a
`*-summary.csv` or a `derived-*.csv`.
