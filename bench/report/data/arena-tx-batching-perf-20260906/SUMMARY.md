# Arena TX batching hardware A/B

## Verdict

Arena batching worked on the real gRPC path. Against direct TX the successful
publication size grew from 906 B to 2,873 B at 1 KiB and from 3,268 B to 7,018 B
at 8 KiB, and publications per MiB fell by 3.17x and 2.15x respectively. The
median TX budget wait at 1 KiB capacity fell from 46,021 to 0.

CPU/RPC fell 1.0–3.6% at all four gRPC capacity sizes. At the size that matters
most, 64 KiB, capacity rose 12,505→12,905 RPC/s (+3.20%), CPU/RPC fell
619.07→599.50 µs (−3.16%), and p99 fell 8,009→7,610 µs (−4.98%). Total worker CPU
time was essentially unchanged, 64.17→64.24 s: the same CPU handled more RPCs.

Overall performance acceptance is nevertheless **OPEN**. gRPC 8 KiB capacity fell
22,436→21,949 RPC/s (−2.17%) and the ranges over three repeats do not overlap
(direct 22,196–22,499, batch 21,698–22,071). The 1 KiB capacity p99 median rose
18.42%, but the ranges overlap widely and the matched-rate change is +0.65%, so
these samples alone do not establish a stable tail regression. HTTP and policy
hardware correctness was outside this campaign's scope.

The results therefore support the following.

- The reservation and publication bookkeeping incurred on every write was one
  cause of the CPU regression. Batching cut that count and CPU/RPC fell at every
  measured size.
- Counting state-management instructions alone does not explain it. In the 64 KiB
  PMU, instructions/RPC rose 1.77% while cycles/RPC fell 3.16% and IPC rose
  5.12%: the new path executes slightly more instructions yet uses cycles better.
- Which stall or synchronization cost actually fell cannot be separated from
  these generic PMU events. Cache-misses/RPC was essentially unchanged at −0.35%.

## Clean capacity results

Each value is the median of three uninstrumented 8 s runs. Parentheses give
batch's change against direct. CPU is the summed utime+stime of the eight Arm
workers divided by scheduled RPCs.

| Protocol | Payload | direct→batch RPC/s | direct→batch CPU µs/RPC | direct→batch p99 µs |
|---|---:|---:|---:|---:|
| gRPC | 64 B | 36,134→36,163 (+0.08%) | 178.09→176.30 (−1.00%) | 2,676→2,699 (+0.86%) |
| gRPC | 1 KiB | 34,104→34,632 (+1.55%) | 190.55→183.65 (−3.62%) | 3,752→4,443 (+18.42%) |
| gRPC | 8 KiB | 22,436→21,949 (−2.17%) | 309.02→300.71 (−2.69%) | 5,636→4,240 (−24.77%) |
| gRPC | 64 KiB | 12,505→12,905 (+3.20%) | 619.07→599.50 (−3.16%) | 8,009→7,610 (−4.98%) |
| opaque | 64 B | 133,844→142,393 (+6.39%) | 23.96→22.52 (−6.00%) | 1,112→1,099 (−1.17%) |
| opaque | 1 KiB | 122,685→128,964 (+5.12%) | 24.98→24.30 (−2.72%) | 1,176→1,135 (−3.49%) |

Opaque's mean publication size was effectively identical — 266.17→266.29 B at
64 B and 3,451.22→3,450.52 B at 1 KiB — so its throughput gain is not explained
by denser batches. Running the arms in the opposite order from gRPC reduces a
simple campaign-order effect, but attributing the cause properly needs a separate
profile.

## Matched-rate results

gRPC was pinned at 20k RPC/s for 64 B and 1 KiB, 10k for 8 KiB and 2k for 64 KiB.
Opaque was pinned at 20k RPC/s for both sizes.

| Protocol | Payload | CPU/RPC change | p99 change |
|---|---:|---:|---:|
| gRPC | 64 B | −0.48% | +3.56% |
| gRPC | 1 KiB | −0.43% | +0.65% |
| gRPC | 8 KiB | −1.29% | −1.53% |
| gRPC | 64 KiB | −2.16% | +0.81% |
| opaque | 64 B | −5.83% | −0.41% |
| opaque | 1 KiB | −5.22% | −4.01% |

CPU/RPC fell in all six conditions at fixed load as well. The gRPC matched-rate
p99 changes were mostly small and mixed in direction.

## Publication shape

The uprobes counted only successful returns, in 12 s runs kept separate from the
clean performance runs. The tracing runs' throughput and latency were not used in
the performance tables. Every histogram reported 0 drops.

| Protocol | Payload | Mean publication bytes direct→batch | Publications/MiB reduction |
|---|---:|---:|---:|
| gRPC | 64 B | 319.58→324.78 | 1.02x |
| gRPC | 1 KiB | 905.89→2,873.07 | 3.17x |
| gRPC | 8 KiB | 3,268.49→7,017.86 | 2.15x |
| gRPC | 64 KiB | 8,550.52→9,494.58 | 1.11x |
| opaque | 64 B | 266.17→266.29 | 1.00x |
| opaque | 1 KiB | 3,451.22→3,450.52 | 1.00x |

Even at 64 KiB, full 64 KiB publications were 0 in both arms. That follows from
H2's 16 KiB DATA framing combined with control output, and it cannot be claimed
that steady state coalesces into a single 64 KiB block.

Across batch's 36 clean samples the arena copy bytes equalled the actual
publication bytes in every run, with 0 TX retries and errors; direct's 36 clean
samples also had 0 retries and errors. Batch's publication and reserve-attempt
counters matched per run, so no reservation leak or duplicate reservation was
observed.

## 64 KiB PMU

`perf stat` measured the middle 8 s of three separate 12 s capacity runs. Per-RPC
values are estimates using the whole run's reported RPS.

| Metric | direct→batch | Change |
|---|---:|---:|
| worker cores | 7.772→7.744 | −0.36% |
| cycles/RPC | 1,322,393→1,280,630 | −3.16% |
| instructions/RPC | 562,923→572,912 | +1.77% |
| IPC | 0.426→0.448 | +5.12% |
| generic cache-misses/RPC | 10,108→10,073 | −0.35% |
| context-switches/RPC | 0.186→0.193 | +3.30% |

The generic `cache-misses` event is not read as LLC misses or as a specific
memory stall, and the IPC change does not identify whether a frontend, backend or
coherence cost moved.

## Method and quality gates

- Compared binaries: direct `2200fc9207...9422e`, batch `b62194f1...36eb`.
- After the campaign, the SHA256 of five production sources was compared against
  the BlueField build staging and all matched. Full values are kept in
  `raw/source-provenance.txt`.
- Same BlueField-3, N/K/A = 32/8/8, client CPUs 4–9, server CPUs 10–17, release
  LTO with jemalloc, concurrency 8.
- gRPC ran direct then batch; opaque ran batch then direct.
- The main A/B is three repeats each of capacity and matched rate. 93 valid
  samples were kept in total: 72 clean, 6 PMU, 12 trace and the three closing
  direct 64 KiB baseline runs.
- All 93 produced a valid application result with a maximum of 0 for fail, drop,
  overflow, worker_fail and reorder. One of the closing direct 64 KiB samples
  carried `eq_budget_exhausted=1` and was not included in the main A/B.
- The closing direct 64 KiB re-measurement gave 12,434/12,695/11,049 RPC/s. The
  median 12,434 is close to the main baseline of 12,505, but the single low
  sample shows the rig's variability. Three repeats are not expressed as
  statistical significance.
- At every L7 arm shutdown the arena read 1,024/1,024 free with 0 live chunks,
  and on the arms with L7 metrics the session, task, DMA, queue, ACK and FIN
  gauges were 0.

Software gates re-run:

| Gate | Result |
|---|---|
| `make test` | PASS; grpcio was missing from the system Python, so a temporary venv built from the repository requirements was used |
| adapter Rust | 41/41 PASS |
| `dmesh-doca` | 30/30 PASS |
| gRPC release | 4/4 PASS |
| gRPC ASAN+UBSAN | 4/4 PASS |
| C proxy-lane ASAN+UBSAN | PASS |
| non-test Rust check / diff whitespace | PASS |

## Restoration

The pre-campaign workload templates and Service spec were reapplied unchanged.
Both native workloads are Ready and the Service port is 9092. The running L4 DPU
binary hash is the pre-campaign `2200fc9207...9422e`, and the native 64 B smoke
gave 180,780 RPC/s with fail, drop, overflow and worker_fail all 0. The temporary
control relay and the trace instance and events were removed. Opaque's cutoff
`pending=58–64` and the native smoke's `pending=64` are in-flight counts at the
benchmark's stop, not a residue verdict.

Derived data is in [comparison.csv](comparison.csv),
[trace-summary.csv](trace-summary.csv) and [pmu-summary.csv](pmu-summary.csv);
the full figure is [comparison.png](comparison.png). Raw samples and the
reproduction scripts are kept in `raw/`, `campaign.py`, `run_matrix.sh` and
`summarize.py`.
