**Direct TX batching and CPU diagnosis**

The retained before and after binaries were redeployed and compared with no
behavioural change. **The batching loss is confirmed, but the 64 KiB regression
cannot be explained by quota waiting or by a simple rise in instruction count.**
Arena batching remains a candidate worth validating, and this diagnosis is not
proof that it resolves the whole regression.

![gRPC diagnosis comparison](diagnosis.png)

**Measurement scope and how to read it**

114 samples were retained: 72 clean, 12 profile, 24 trace, 6 PMU.

- gRPC: 64 B / 1 / 8 / 64 KiB, capacity and matched, three clean 5 s runs each.
- opaque TCP: only 64 B and 1 KiB, where a valid comparison was possible, under
  the same clean conditions. The 8 and 64 KiB drain failures of the earlier
  report were neither fixed nor re-verified here.
- profile: a separate 16 s capacity run per size, with the 12 s after the start
  region recorded on the eight `dmesh-w0..7` threads via
  `perf record -F 99 -e cycles --call-graph fp`.
- trace: the middle 8 s of a separate 12 s run, instrumenting the Rust scalar and
  vectored write entries and the C commit entry as uprobe histograms. Both
  capacity and matched were collected.
- PMU: three separate 12 s runs at gRPC 64 KiB, with `perf stat` over the middle
  8 s. This extra check ran after → before. Every PMU event ran at 100%.
- The main matrix ran before → after on the same BlueField, N/K/A = 32/8/8, with
  client CPUs 4–9 and server CPUs 10–17 applied from process start. Nothing was
  rebuilt or recompiled.
- The CPU seconds below are the summed `utime+stime` of the eight workers, with
  the real `_SC_CLK_TCK=100` recorded. As in the earlier report, clean CPU/RPC
  divides by scheduled RPCs and includes warmup, teardown and the time around the
  instrumentation calls. It is neither system-wide CPU nor pure payload
  processing time.
- Tracing changes throughput and scheduling, so the traced runs' throughput and
  p99 were not mixed into the clean performance results. A matched trace was
  added to check that the batching change holds at identical input load. Because
  the histogram triggers are started and stopped per event in sequence, a
  boundary difference of a few dozen events between write and commit is possible.
  Nonempty commits are used, and the entry count is not a count of destination
  ACKs.
- The median of three and the overall min/max are retained. A small share
  difference in a single profile is not read as statistical significance or as an
  exact cost change for a function. The wait counters are counts; waiting time
  was not measured.

The running binaries' SHA256 match the earlier report.

```text
before 4273f0148b78346c83a2922db86d5a6a981fa8217fdd3cf148eb980faa76b62e
after  2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e
```

**gRPC throughput and CPU**

| Payload | RPC/s before → after | Change | Total CPU seconds | CPU µs/RPC | CPU/RPC change |
|---|---|---|---|---|---|
| 64 B | 36,505 → 36,657 | +0.42% | 33.71 → 33.90 | 174.65 → 174.24 | −0.24% |
| 1 KiB | 35,145 → 35,324 | +0.51% | 33.68 → 35.04 | 181.23 → 187.36 | +3.39% |
| 8 KiB | 21,942 → 22,756 | +3.71% | 34.54 → 36.74 | 297.14 → 304.64 | +2.52% |
| 64 KiB | 13,378 → 12,682 | −5.20% | 40.92 → 40.85 | 577.61 → 607.91 | +5.25% |

At 64 KiB the total CPU time is essentially unchanged while throughput fell. At
1 KiB throughput is similar yet total CPU time rose. At 8 KiB a throughput gain
and a higher per-RPC cost appear together. The small throughput difference at
64 B is not called an improvement, because the repeat ranges overlap.

At identical input load:

| Payload | RPC/s before → after | CPU/RPC change | p99 µs before → after |
|---|---|---|---|
| 64 B | 19,998 → 19,999 | +0.21% | 2,022 → 2,040 |
| 1 KiB | 19,999 → 19,999 | +1.13% | 1,980 → 2,061 |
| 8 KiB | 9,999 → 9,999 | +1.56% | 2,250 → 2,294 |
| 64 KiB | 1,999 → 1,999 | +1.03% | 2,610 → 2,540 |

This is evidence that "CPU rose because throughput rose" cannot account for all
of it. The small CPU differences at low load do, however, include maintenance and
measurement-boundary effects.

**Batching: fewer writes can still mean more commits**

| Payload | Capacity mean commit B | Capacity commit/MiB ratio | Matched mean commit B | Matched commit/MiB ratio |
|---|---|---|---|---|
| 64 B | 320 → 319 | 1.002x | 157 → 159 | 0.989x |
| 1 KiB | 2,855 → 906 | 3.151x | 1,504 → 821 | 1.832x |
| 8 KiB | 6,999 → 3,269 | 2.141x | 4,194 → 3,268 | 1.283x |
| 64 KiB | 9,552 → 8,584 | 1.113x | 10,271 → 8,956 | 1.147x |

Ratios are after/before. 64 B is essentially unchanged, but at 1, 8 and 64 KiB
there are more commits per byte transferred. In the 1 KiB capacity trace, before
recorded 1,099,882 scalar write entries while after recorded 626,405 vectored
write entries — yet commits went 189,802 → 585,065. Vectored support cut the
caller's write count while losing the coalescing Vec provided between polls, and
the two effects point in opposite directions.

Neither 64 KiB arm produced a single 65,536 B commit in this steady-state trace.
What changed is how much before merged its roughly 16 KiB DATA with small control
output, so explaining the regression as "the existing 64 KiB commits disappeared"
is not appropriate. The earlier 128 startup debugger samples are not extrapolated
to the steady-state distribution.

**Quota matters at 1 KiB**

| Payload | after capacity wait median / 5 s | Three-run min–max | after matched wait median / 5 s |
|---|---|---|---|
| 64 B | 0 | 0–0 | 0 |
| 1 KiB | 30,133 | 29,553–30,961 | 130 |
| 8 KiB | 2 | 2–2 | 0 |
| 64 KiB | 9 | 0–9 | 0 |

Zero to nine waits at 64 KiB capacity cannot explain a 5.2% throughput
regression. The many waits and wakes at 1 KiB deserve their own investigation.
before has no equivalent direct-writer counter, so its waits are not shown as 0.

In clean capacity, DMA local completions per RPC are 0.736→0.734 at 64 B,
0.747→0.748 at 1 KiB, 2.337→2.383 at 8 KiB and 17.376→17.408 at 64 KiB. The
commit growth rate therefore must not be reinterpreted as a growth in physical
DMA completions.

**What the CPU profile and PMU do and do not show**

Below are exclusive self shares of all worker cycle samples. The copy and atomic
helper rows sum every layer and are not TX-adapter-only costs.

| Payload | memcpy/memmove share | AArch64 atomic helper share | DmeshIo write self share |
|---|---|---|---|
| 64 B | 3.41% → 2.73% | 9.56% → 10.82% | 0.14% → 0.22% |
| 1 KiB | 3.78% → 3.40% | 9.18% → 10.36% | 0.09% → 0.16% |
| 8 KiB | 4.25% → 4.18% | 8.74% → 8.80% | 0.08% → 0.29% |
| 64 KiB | 6.73% → 5.99% | 10.32% → 10.06% | 0.13% → 0.40% |

The copy share at 64 KiB fell 6.73%→5.99%. Write self went 0.13%→0.40% and
`h2::codec::framed_write::FramedWrite::flush` 0.37%→1.04%. These numbers show
that a copy reduction and a change in other processing paths coexist; they are
not a decomposition of the mutex, Arc, quota and publication costs in
nanoseconds. Inlined code and helper costs exist too, so the whole writer cost is
not computed from self shares alone.

The separate 64 KiB PMU comparison:

| Metric | before | after | Change |
|---|---|---|---|
| worker CPU cores | 7.727 | 7.733 | +0.07% |
| cycles/RPC (estimated) | 1,239,976 | 1,302,964 | +5.08% |
| instructions/RPC (estimated) | 566,028 | 562,294 | −0.66% |
| IPC | 0.4565 | 0.4315 | −5.46% |
| generic cache-misses/RPC (estimated) | 11,294 | 10,228 | −9.44% |

The per-RPC PMU values estimate the RPCs inside the window as **the whole run's
reported steady rate x 8 s**. They are not read from an RPC completion counter
over exactly that window. Raw counters, window lengths and per-repeat ranges are
kept in `pmu-samples.csv` and `pmu-summary.csv`.

Cycles/RPC rose while total instructions/RPC did not, so the 64 KiB regression
cannot be explained by extra instruction count alone. That does not rule out a
higher cost in the management code either: removed copy instructions and added
other instructions can offset, and the instruction mix, dependencies,
cache/coherence and branch behaviour can all differ. Generic cache-misses also
fell, so the simple "more cache misses made it slower" story is not supported
either. That event is not called an LLC miss without separate confirmation, and
decomposing frontend against backend stalls was out of scope here.

**A small opaque TCP comparison**

| Payload | RPC/s before → after | Throughput change | CPU/RPC change | before RPC/s range | after RPC/s range |
|---|---|---|---|---|---|
| 64 B | 144,109 → 137,123 | −4.85% | +12.50% | 143,915–145,205 | 134,190–137,348 |
| 1 KiB | 136,185 → 127,428 | −6.43% | +9.03% | 130,357–136,410 | 126,780–130,092 |

Both sizes lost throughput in this opaque comparison, so the earlier report's
small improvement did not reproduce. Commit sizes in the capacity trace were
essentially identical, so there is no basis for explaining the drop as a batching
loss. The client and server image IDs of each arm match the earlier report's
opaque images. This experiment does not separate environmental effects of the day
or the run order, so the drop is not generalized into a universal improvement or
regression magnitude. Transient large tail latencies also occurred and those
samples were not deleted. The acceptance criterion is unchanged from the earlier
report: `OK` with fail, drop, overflow and worker_fail all 0. The native
benchmark's `pending` at the cutoff is distinguished from residue after teardown.

**Priorities for the next change**

1. Validate bounded coalescing of several writes in the arena as its own
   experiment first. Rather than waiting for 64 KiB to fill, small output must
   still progress at the existing worker drain and flush boundaries. Lease
   retention, retry custody, per-endpoint routes and the flush/FIN contract have
   to be designed together.
2. Split the 1 KiB quota waits and wakes into a separate change. The 1 ms quota
   is not removed first on the grounds of fixing the 64 KiB regression.
3. If commits per MiB drop after the batching change and 64 KiB cycles/RPC
   nonetheless holds, decompose instruction mix, frontend and backend stalls, and
   cache/coherence next. This profile alone neither fixes one of them as the
   cause nor promises an improvement magnitude.
4. Investigate the management cost of the opaque write path separately from
   batching. A path whose commit shape is unchanged is not assumed to improve
   from arena coalescing alone.

**Data and reproduction**

- `samples.csv`, `summary.csv`: samples and aggregates, with clean, profile,
  trace and stat kept apart.
- `commit-histograms.csv`, `trace-summary.csv`: per-length histograms and write
  and commit counts.
- `profile-symbols.csv`, `profile-summary.csv`: full exclusive symbol shares and
  the limited classification.
- `pmu-samples.csv`, `pmu-summary.csv`: the separate hardware counter experiment.
- `raw/*/`: replies, CPU ticks, metrics before and after, perf data and reports,
  histograms, binary identification.
- `diagnose.py`, `probe.py`, `summarize.py`, `plot.py`, `write_report.py`,
  `commands.txt`: collection, aggregation, figures and report generation.
  `campaign_tail.sh` is the PMU and opaque stage.

The behaviour of the uprobes and histograms was checked against Linux's
[uprobe documentation](https://docs.kernel.org/trace/uprobetracer.html) and
[histogram documentation](https://docs.kernel.org/trace/histogram.html). The
first trial truncated the trigger so continue failed; collection was redone with
append, and that trial is not part of the performance samples. Waiting for worker
readiness during the initial deployment was also strengthened. These fixes are
confined to the diagnosis harness; the production TX implementation was not
changed.

The original native/L4 deployment was restored and finally verified; the evidence
is in [RESTORATION.md](RESTORATION.md).
