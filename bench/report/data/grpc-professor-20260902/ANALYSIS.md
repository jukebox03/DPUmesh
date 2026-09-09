# DPUmesh gRPC correctness, performance and bottlenecks

One node, one BlueField, `N/K/A=32/8/8`, nine host cores each for client and
server. Every frame size is the logical frame of the request and of the response
each (64 B / 1 KiB / 8 KiB, with protobuf bodies of 48/1,008/8,176 B). The
procedure and commands are in [`EXPERIMENT.md`](EXPERIMENT.md), the raw data in
`*-raw.csv`, [`knee-followup-raw/`](knee-followup-raw/) and
[`lowload/`](lowload/), and the derived values in the `derived-*.csv` that
`derive.py` produces.

## Conclusions

1. **Every correctness gate passed.** §1.
2. **There are two capacities, depending on the definition.** By delivery
   (achieved ≥ 0.99 x offered, zero errors) they are **90k** at 64 B, **75k** at
   1 KiB and **29.75k** RPC/s at 8 KiB. Under a p99 ≤ 5 ms criterion they are
   **80k / 70k / 20k**. The p99 at the delivery points is 24 ms / 10 ms / 318 ms,
   so a deployment with a latency target must use the second definition. §2.
3. **The low-load DPU cost is a fixed cost of processing one event.** Measuring a
   single worker's PMU directly, one 64 B request costs 467k instructions, 4.9k
   cache misses and IPC 0.30 — **747 µs** — at 100 RPS, against 173k, 1.7k and
   0.54 — **154 µs** — at the knee (6,250 RPS per worker). One request is handled
   by roughly eight runtime loop passes and one or two wakes; as load rises a
   single pass serves several requests and the fixed cost is divided. Spin, timer
   gating, session rebuilding and the host coalescer were all excluded by
   measurement. So eight cores appearing full from 40k RPS is not a defect but an
   **implementation with a large per-event cost**, and the knee is set by the
   worker count. §3.
4. **The single-request latency floor is 0.94 ms (closed loop, 1 in flight) and
   1.6 ms in a 100 RPS open loop.** Of that, DPU worker on-CPU is about 450 µs
   forward and 320 µs reverse, and the rest is the wake chains on both host sides.
   The host tail coalescer is not the cause (E2). At the same 10k RPS a stock
   Linkerd sidecar is 403 µs against DPUmesh's 617 µs. §2, §3.
5. **The comparison must be read per core.** In a closed loop with the same
   application, DPUmesh reaches 0.39x direct TCP at 64 B and 4.3x the Linkerd
   sidecar. But Linkerd runs one core per sidecar
   (`LINKERD2_PROXY_CORES=1`) while DPUmesh uses eight Arm cores, so throughput
   per proxy core is level: 13.3k against 12.5k. At 10k RPS, saving 1.36 host
   cores costs 4.11 Arm cores. §6.
6. **DPUmesh loses the most as the payload grows.** From 64 B to 8 KiB, direct
   falls to 0.75x, Linkerd to 0.61x and DPUmesh to 0.32x. The Arm cost is 14.6 ns
   per payload byte, tens of times a memcpy. §4.
7. **Overload behaviour is loss, not saturation.** Past the peak, throughput
   falls, and there are RPC failures and drops, a single worker stalling, and
   degradation in an 11-hour-old deployment. §5.
8. **The worker count sets the capacity.** A = 4/6/8/12 gives 40k/70k/80k/130k,
   i.e. 10.0–11.7k RPC/s per worker. §7.

## 1. Correctness

| Gate | Result |
|---|---:|
| host transport/ABI/fault/analyzer (`make test-hostfree`) | PASS |
| real DPU lane and SG-DMA queue contract | PASS |
| release cHTTP2 adapter CTest | 4/4 |
| Clang ASAN+UBSAN cHTTP2 CTest | 4/4 |
| embedded Rust tests | 38/38 |
| real DPU shutdown and slot reuse | opened = closed 22/22 |
| gRPC policy and routing surfaces | 19/19 ([`policy-stages.csv`](policy-stages.csv)) |
| final Pod restarts / live tasks | 0 / 0 |

Raw output [`correctness.txt`](correctness.txt); acceptance criteria
[`design/GRPC.md`](../../../../design/GRPC.md#verification-contract).

## 2. Capacity and latency

![Offered against achieved](graphs/01_offered_achieved.png)

![p50 from 500 RPS](graphs/02_p50_latency.png)

![p99 by payload](graphs/03_p99_latency.png)

| Frame | Delivery capacity | p99 there | p99 ≤ 5 ms capacity | p99 there | 10k RPS p50 |
|---:|---:|---:|---:|---:|---:|
| 64 B | 90k | 24.2 ms | 80k | 4.97 ms | 611 µs |
| 1 KiB | 75k | 10.3 ms | 70k | 4.73 ms | 625 µs |
| 8 KiB | 29.75k | 318 ms | 20k | 2.29 ms | 1,552 µs |

The delivery criterion is the highest offered rate that is 3/3 clean in one
deployment and has no mixed or bad fresh-redeploy repeat at or below it. 64 B was
clean up to 100k in one campaign, but fresh redeployments produced mixed at 92k
and bad at 98k and 99k, so only 90k is granted
([`knee-followup-summary.csv`](knee-followup-summary.csv),
[`derived-capacity.csv`](derived-capacity.csv)).

The latency floor moves against load. 64 B p50 is 983/988/739/643/611 µs at
500/1k/2.5k/5k/10k RPS, and 8 KiB p50 falls from 1,556 µs at 10k to 1,239 µs at
15k before rising. Measuring the floor itself gives ([`lowload/`](lowload/)):

| Condition | p50 | Note |
|---|---:|---|
| closed loop, 1 in flight total (1 thread) | 936 µs | a warm single round trip |
| closed loop, 2 / 4 in flight total | 1,494 / 2,064 µs | serialized on one worker, about 550 µs per request |
| open loop, 1 channel, 100 RPS | 1,611 µs | 10 ms idle between requests |
| open loop, 1 channel, 1,000 RPS | 985 µs | 1 ms between requests |
| host `TX_TAIL_DELAY_NS` 500→50 µs build, 100 RPS | 1,605 µs | unchanged; 10k RPS got worse, 611→1,456 µs |

A same-node DMA round trip is tens of microseconds, so this floor is the software
path on both sides, not the hardware. At 100 RPS the DPU worker spends about
450 µs forward and 320 µs reverse of on-CPU time on one request (§3), and the
remaining ~800 µs is the wake chains and applications on the client and server
hosts. The host tail coalescer plays no part in the floor, and reducing it makes
mid-load worse.

## 3. What the low-load DPU cost really is

![CPU against load](graphs/05_cpu_attribution.png)

![CPU against in-flight](graphs/09_inflight_cpu.png)

In the campaign data, worker core tracks the **number of open requests**, not
requests per second: 0.69 core while one request is in flight, 0.67–0.74 from 500
RPS through closed concurrency 8
([`derived-inflight-cpu.csv`](derived-inflight-cpu.csv)). The reason was measured
directly with one channel bound to one worker.

![Per-RPC PMU](graphs/11_per_rpc_pmu.png)

| RPS per worker | Condition | p50 | cycles/RPC | instr/RPC | cache miss/RPC | IPC | Arm µs/RPC | syscall/RPC | wake/RPC |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 1 channel, 100 RPS | 1,611 µs | 1.56 M | 467 k | 4,926 | 0.30 | 747 | 47 | 1.2 |
| 1,250 | 8 channels, 10k RPS | 617 µs | 1.07 M | 348 k | 4,080 | 0.33 | 520 | 46 | 1.9 |
| 6,250 | 8 channels, 50k RPS | 1,565 µs | 0.32 M | 173 k | 1,672 | 0.54 | 154 | 8 | 0.1 |

`perf stat -t` measured only the worker thread holding the channel, so the
maintenance ticks of idle workers are excluded
([`lowload/pmu-per-rpc.csv`](lowload/pmu-per-rpc.csv)). One request costs 2.7x the
instructions and 2.9x the cache misses of the knee at half the IPC, hence 4.9x the
cycles.

What that cost is:

- **About eight loop passes per request.** The `DPUMESH_PERF_STATS=1` counters
  show 7.4–8.1 drain passes per request at 100–1,000 RPS, half of them Idle passes
  that found nothing ([`lowload/drain-passes.txt`](lowload/drain-passes.txt)). One
  pass runs through DOCA progress, engine pump and emit, ack release, registration
  collection, both pumps of every session, the hyper/H2 connection poll,
  notification arm and clear, and `select` re-registration, so a single idle tick
  on a worker with an open session is 30–70 µs.
- **47 syscalls per request.** `epoll_pwait` 29, `read` 12 (8 of them EAGAIN) and
  `write` 7, with 1.2 context switches
  ([`lowload/syscalls-500rps.txt`](lowload/syscalls-500rps.txt),
  [`lowload/wakes-500rps.txt`](lowload/wakes-500rps.txt)). This is not a spin that
  never sleeps; it is the cost of the tokio scheduler visiting the I/O driver on
  every yield after waking.
- **One request is two continuous on-CPU blocks.** In the `sched_switch` trace, a
  100 RPS request shows about 450 µs of forward execution, a 200–550 µs gap on the
  server side, and about 320 µs of reverse execution; over 202 requests the median
  is 686 µs on-CPU within a 1,265 µs span
  ([`lowload/sched-100rps-clusters.txt`](lowload/sched-100rps-clusters.txt)).
  There is no interval spent spinning for an event.
- **Six task polls per request, each expensive.** Uprobes on the running binary
  count 2.0 hyper server connection polls, 4.0 h2 client connection polls and 21
  drains per request
  ([`lowload/e5-probes-100rps.txt`](lowload/e5-probes-100rps.txt)). The inclusive
  profile puts tasks at 64% and the loop at 30%, so one poll is about 80 µs from
  cold. The cost is not excess polls but the width of the L7 stack each poll
  traverses.
- **Hypotheses excluded.** Idle with no session is 0.043 core and an idle worker's
  tick is 8.7 µs (not a spin). The DPU has no cpufreq or cpuidle, so the clock is
  a steady 2.05–2.09 GHz (not DVFS). Session and stack metrics grew by 0 over
  5,000 requests (not session rebuilding). Cutting the host tail delay 500→50 µs
  did not change the 100 RPS p50 (not the coalescer). The generator honours
  arrival times with `nanosleep` (not a measurement artifact).

`perf stat` at 64 B 50k: 7.591 cores, 321.6k cycles/RPC, 181.2k instructions/RPC,
IPC 0.56. The exclusive profile is `memcpy` 3.41%, atomics 3.35% + 2.16%, syscalls
2.11% and HPACK encode 1.80%, with the top thirteen summing to 20%. At 100 RPS,
with only the channel worker awake, the shape is the same. The cost is not in one
function but spread across the long path repeated for every event.

![DPU perf](graphs/07_perf.png)

Conclusion: the low-load CPU and the latency floor share one cause — **the width
of the L7 stack traversed cold on every event**. The loop side (pass count,
syscalls) is 30% of the total, so reducing only that is worth single-digit
percent (E5 first pass: −2 to −6%), and the other 64% is the six connection polls
per request and the linkerd service stack itself. The levers for that are in §8
E5.

## 4. Payload scaling

![Payload scaling](graphs/10_payload_scaling.png)

| Frame | Knee Arm µs/RPC | Increase over 64 B | Per byte |
|---:|---:|---:|---:|
| 64 B | 79 | — | — |
| 1 KiB | 104 | +25 µs | 12.8 ns |
| 8 KiB | 317 | +237 µs | 14.6 ns |

29.75k RPC/s at 8 KiB is 3.9 Gbit/s in both directions combined. Eight Arm cores
saturating at that bandwidth is not explained by copying (0.1–0.3 ns per byte) but
by the fixed cost per delivery unit — H2 frames, DMA descriptors, the flow-control
window and a second copy ([`derived-knee-cost.csv`](derived-knee-cost.csv)).

## 5. Overload and repeatability

![Closed loop](graphs/04_inflight.png)

- Closed-loop throughput falls past the peak: 64 B 118.9k (2,048) → 102.1k
  (8,192), 1 KiB 88.7k (1,024) → 77.4k (8,192). A saturated server should stay
  flat.
- Overload appears as errors rather than backpressure: 408 drops at 1 KiB 80k;
  7,765 failures and 64 credit losses at 8 KiB 30k; 767 failures per repeat at
  8 KiB with 2,048 in flight
  ([`saturation-rejected.csv`](saturation-rejected.csv)).
- In the second repeat of 64 B 92k on a fresh deployment, worker 5 alone stalled
  at 0.15 core with 73,587 schedule drops. The other seven workers were fine.
- An 11-hour-old deployment gave ratio 0.9878 and p99 904 ms at 80k, while after
  redeploying the same point was 3/3 clean with p99 4.97 ms
  ([`stability-observation.csv`](stability-observation.csv)).
- Eight workers with 24 channels (3 sessions per worker) delivered only 43.7k of
  80k with a p50 of 5.5 s. Twelve workers with 24 channels was fine
  ([`session-scaling-summary.csv`](session-scaling-summary.csv)).

## 6. Three transports for the same application

![Comparison](graphs/06_comparison.png)

| Frame | direct TCP | DPUmesh (vs direct) | Linkerd (vs direct) | per proxy core, DPUmesh / Linkerd |
|---:|---:|---:|---:|---:|
| 64 B | 272.4k | 106.8k (0.39) | 25.0k (0.09) | 13.3k / 12.5k |
| 1 KiB | 259.2k | 89.1k (0.34) | 19.3k (0.07) | 11.1k / 9.7k |
| 8 KiB | 204.6k | 34.3k (0.17) | 15.2k (0.07) | 4.3k / 7.6k |

Closed loop, 1,024 in flight total, 10 s, median of three. Proxy cores are the
configured values (8 DPUmesh Arm workers; Linkerd 1 core x 2 sidecars). Per core
the two meshes are in the same class, and at 8 KiB Linkerd is ahead
([`derived-comparison.csv`](derived-comparison.csv)).

| Frame | Linkerd host cores | DPUmesh host cores | Saved | Arm consumed | p50 Linkerd / DPUmesh |
|---:|---:|---:|---:|---:|---:|
| 64 B | 3.24 | 1.87 | 1.36 | 4.11 | 403 / 611 µs |
| 1 KiB | 3.52 | 1.90 | 1.62 | 4.21 | 433 / 625 µs |
| 8 KiB | 3.80 | 2.38 | 1.42 | 5.24 | 514 / 1,552 µs |

At 10k RPS achieved, with the host measured as the recursive Pod cgroup of
application plus broker or application plus sidecar. Saving one host core costs
2.6–3.7 Arm cores and p50 is 1.4–3.0x slower
([`derived-exchange-10k.csv`](derived-exchange-10k.csv)). Of the 520 µs/RPC of Arm
at 10k RPS, everything above the knee cost of 154 µs is the per-event fixed cost
of §3, so reducing that fixed cost moves both the exchange ratio and the latency
inversion.

## 7. Worker scaling

![Worker scaling](graphs/08_worker_scaling.png)

| Workers | N/K/A | Highest 3/3 clean | First bad | Per worker | Knee worker CPU |
|---:|---|---:|---:|---:|---:|
| 4 | 32/4/4 | 40k | 50k | 10.0k | 3.97/4 |
| 6 | 30/6/6 | 70k | 80k | 11.7k | 5.95/6 |
| 8 | 32/8/8 | 80k | 90k repeatedly failed | 10.0k | 7.79/8 |
| 12 | 24/12/12 | 130k | 140k | 10.8k | 11.37/12 |

With `threads=channels=workers`, so one session per worker. At 130k the twelve
workers sit at 97.4–98.9% utilization, i.e. balanced. The first A=6 deployment
failed with a broker READY reset and passed on redeployment
([`worker-scale-deploy-retry.txt`](worker-scale-deploy-retry.txt)).

## 8. The experiments that decided the verdicts, and what is left

Conditions and pass criteria are in
[`EXPERIMENT.md`](EXPERIMENT.md#e-pre-registered-experiments).

| ID | Question | Result |
|---|---|---|
| E1 | why worker CPU follows the number of open requests | **complete.** 747 µs/RPC (≥ 400) at 1 channel and 100 RPS, but a per-event fixed cost rather than a spin (§3) |
| E2 | where the 0.6–1 ms latency floor lives | **complete, rejected.** With the 50 µs `TX_TAIL_DELAY_NS` build, 100 RPS p50 was 1,605 µs (unchanged) and 10k RPS 1,456 µs (worse); the floor is the DPU event path and the host wake chains |
| E3 | a core-matched comparison | not measured: a 4-core Linkerd sidecar and direct TCP p50 at 10k RPS |
| E4 | a single worker stalling and degradation | not measured: five fresh deployments x 90/92k, and a 24-hour 80k probe |
| E5 | cutting the fixed cost per event | **first pass complete.** A build that puts the Rust drain ahead of the C engine drain (so published bytes are submitted to DMA in the same pass) and reads the wake eventfd only when a tick was posted. Worker CPU −2 to −6% (1 channel at 100/500/1k RPS: 695→680, 641→616, 617→601 µs/RPC; 8 channels at 10k: 411→406), syscalls/RPC 47→32, latency unchanged (closed loop 936→930 µs), 64 B 90k 3/3 clean, `grpcshutdown` and policy 19/19 passing ([`lowload/e5-ab.csv`](lowload/e5-ab.csv), [`policy-route-20260902-184855/`](../policy-route-20260902-184855/)). The pre-registered target (≤ 200 µs) is unreachable by loop tidying |

What the first E5 pass showed is that loop tidying is capped at a few percent. The
remaining levers are cutting the six connection polls per request (h2 client 4→2),
a short bounded spin after an event to avoid the wake chain and cold re-entry, and
reducing the depth of the linkerd service stack — all three on the L7 stack rather
than the loop. This report's bottleneck verdicts are that the knee is set by the
worker count (§7), and that the low-load CPU and latency are the L7 stack
traversed cold on every event (§3).

## Reproduction

```sh
python3 bench/report/data/grpc-professor-20260902/derive.py   # derived-*.csv
python3 bench/report/data/grpc-professor-20260902/plot.py     # graphs/*.{svg,png}
bash bench/suite/grpc_correctness.sh all                       # §1
```

The deploy, pin and sweep commands for each measurement arm are in the rows of
[`EXPERIMENT.md`](EXPERIMENT.md), and the low-load diagnosis commands of §3 are in
E1 and E2 of the same document. The external scope is a single node, one
BlueField, and A = 4/6/8/12; cross-node traffic and 24-hour stability were not
measured.
