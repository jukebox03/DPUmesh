# DPUmesh Topology and Load Sweep

Measured on 2026-07-27 KST with an 8 KiB request, 8 B reply, three round-robin
backends, and the host CPU governor fixed at approximately 2.5 GHz. The 44-row
initial sweep is in [data/sweep_2026-07-27.csv](data/sweep_2026-07-27.csv).

## Results

### Topology and host threads

The initial sweep used `N=16`, `conc=32`, and one host core per pod. Values are
goodput in Gb/s.

| A/K | 1 thread | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| 1/2 | 10.53 | 14.08 | 15.17 | 16.63 |
| 2/2 | 10.80 | 19.58 | 19.61 | 21.39 |
| 2/4 | 11.07 | 20.53 | 26.20 | 31.96 |
| 4/4 | 11.08 | 20.81 | 29.92 | 38.44 |
| 8/8 | 11.13 | 16.75 | 27.62 | **41.20** |

Eight-thread resource cost:

| A/K | Goodput | Host cores/Mrps | DPU ARM | ARM % per Gb/s |
|---|---:|---:|---:|---:|
| 1/2 | 16.63 Gb/s | 0.484 | 140% | 8.43 |
| 2/2 | 21.39 | 0.470 | 250% | 11.71 |
| 2/4 | 31.96 | 0.303 | 245% | **7.67** |
| 4/4 | 38.44 | 0.272 | 428% | 11.13 |
| 8/8 | 41.20 | **0.251** | 667% | 16.18 |

### Concurrency

`N/K/A=16/4/4`, two host threads:

| conc | DPUmesh | p50 | p99 | Host | DPU ARM | TCP | TCP p50 | TCP p99 | TCP host |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0.68 Gb/s | 175 µs | 460 µs | 31.6% | 103.1% | 2.15 Gb/s | 59 µs | 72 µs | 185.9% |
| 2 | 1.27 | 177 | 464 | 40.1% | 109.5% | 4.15 | 61 | 84 | 187.5% |
| 4 | 2.66 | 177 | 413 | 51.0% | 135.9% | 7.25 | 70 | 106 | 186.6% |
| 8 | 5.84 | 178 | 349 | 73.7% | 201.7% | 9.79 | 96 | 180 | 181.7% |
| 16 | 11.21 | 181 | 253 | 103.1% | 240.4% | 12.65 | 139 | 273 | 173.1% |
| 32 | 17.86 | 237 | 387 | 106.8% | 249.0% | 16.93 | 238 | 384 | 191.7% |

### Transport and pinning

`conc=32`; DPUmesh uses `N/K/A=16/4/4`.

| threads | DPUmesh | TCP | DPUmesh p50 | TCP p50 | DPUmesh cores/Mrps | TCP cores/Mrps |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 11.08 Gb/s | 10.25 Gb/s | 187 µs | 200 µs | 0.392 | 0.751 |
| 2 | 20.81 | 16.93 | 191 | 238 | 0.373 | 0.742 |
| 4 | 29.92 | 14.03 | 265 | 595 | 0.333 | 0.799 |
| 8 | 38.44 | 15.26 | 421 | 1,088 | 0.272 | 0.810 |

Giving each pod two host cores (`hw`) instead of one (`fair`) did not improve
throughput consistently:

| threads | DPUmesh fair | DPUmesh hw | TCP fair | TCP hw |
|---:|---:|---:|---:|---:|
| 1 | 11.08 | 10.30 | 10.25 | 10.40 |
| 2 | 20.81 | 20.16 | 16.93 | 16.63 |
| 4 | 29.92 | 31.53 | 14.03 | 15.69 |
| 8 | 38.44 | 36.94 | 15.26 | 15.30 |

### Backend asymmetry and NUMA controls

Three-run single-thread means at `A/K=8/8`, `conc=32`:

| DPA topology | backend 0 | backend 1 | backend 2 |
|---|---:|---:|---:|
| `N=8` | 8.58 Gb/s | 8.38 Gb/s | 8.48 Gb/s |
| `N=16` | 11.51 | **6.89** | 11.42 |
| `N=32` | 11.52 | 11.51 | 11.76 |

At N32, three ten-second eight-thread runs with CPU and memory on NUMA node 1
averaged 40.42 Gb/s (40.31–40.51), 169.2% host CPU, and 651.7% DPU ARM CPU.

Matched placement controls:

| Host placement | Runs | Mean | Range |
|---|---:|---:|---:|
| CPU and memory on node 1 | 3 | 40.42 Gb/s | 40.31–40.51 |
| Original core 0 and mixed memory | 5 | 38.54 | 36.63–39.83 |
| CPU on node 1, existing mixed pages | 3 | 36.21 | 36.14–36.27 |

A final N32/node-1 redeploy passed all correctness checks but still exposed two
saturated states in five-second samples:

| State | Runs | Mean | Range | `grow_waits` |
|---|---:|---:|---:|---:|
| Wait-free | 3 | 39.33 Gb/s | 39.29–39.39 | 0 |
| TX backpressure | 2 | 36.27 | 36.15–36.40 | 336,903–341,433 |

The same redeploy produced TCP 10.44 Gb/s and passed preload 5,000/0, loopback
10,000/0, and verbs 10,000/0.

## Analysis

One connection per host thread limits usable parallelism. At one thread, all
initial topologies deliver 10.5–11.1 Gb/s. Extra rings help independently of ARM
workers: `A/K=2/4` reaches 26.20 Gb/s at four threads versus 19.61 for `2/2`.
`8/8` gives the highest throughput and host efficiency, while `2/4` gives the
lowest DPU ARM cost per Gb/s.

TCP with Envoy saturates near two host cores and does not scale after two
threads. At eight threads, DPUmesh is 2.5 times faster and uses about one third
of the host CPU per request, at the cost of 428% DPU ARM for `A/K=4/4`.

The N16 backend-1 slowdown is a DPA EU-group collision, not incorrect host or
ARM core pinning. With `K=A=8`, each pod selects an eight-EU group from its
`pod_id`. At N16, backend pod 1 and client pod 3 both select EU 8–15, so request
and reply rings contend on the same group. N8 makes every pod share one group;
N32 gives the first four pods distinct groups and removes the backend-specific
penalty.

NUMA is independent of that collision. The BlueField host PCI function
`94:00.0` is on node 1, but automatic NUMA balancing did not move registered DMA
pages: one unbound backend retained 233 MiB on node 0. Full node-1 binding did
not change the N16 per-backend pattern, but improved the matched N32 saturated
mean by 4.9%. It is therefore useful for locality, but the final run shows that
it does not by itself eliminate TX-pool backpressure or all throughput variance.

Single-point differences need repetition: the same `A/K=4/4`, two-thread,
`conc=32` point measured 20.81 and 17.86 Gb/s in separate sweep blocks.

## Reproduction

The test host was `rapids4` (Intel Xeon Gold 6554S, Linux 5.15.0-186) with DOCA
3.1.0-091000. Configure `.env`, inspect the complete campaign, then run it:

```sh
OUT=bench/report/data/sweep-final \
  ./bench/suite/sweep_final.sh --dry-run
OUT=bench/report/data/sweep-final \
  ./bench/suite/sweep_final.sh
```

Defaults are three 20-second repetitions per performance point, three backend
triplets per N, and five NUMA repetitions. The script validates each live
topology, alternates measurement order, records `grow_waits`, and produces
`results.csv`, `backends.csv`, and their summaries. Reusing the same `OUT`
resumes by skipping completed run IDs. It finishes by restoring
`N/K/A=32/8/8` with PCI-local NUMA placement and running the correctness
validators.
