> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Fixed-overhead TX (lean) against arena batching (batch) — hardware A/B

## Verdict

The lean build, which removes the fixed cost per write and per drain pass, runs
gRPC at 2.9–4.4% lower CPU/RPC and 3.1–4.4% higher capacity than the batch build
on the same BlueField, at all four sizes. The RPC/s ranges over three repeats are
disjoint at every size. At matched input rate CPU/RPC is also 0.8–5.0% lower with
both p50 and p99 lower. Publication size is identical in the two arms, so the
difference comes from the fixed cost and not from batching.

Opaque TCP shows −0.7% and −3.0% capacity RPC/s with widely overlapping ranges,
and −2.0% and −0.2% matched CPU/RPC. No improvement or regression is claimed for
opaque.

This receipt answers the question left open after direct TX: "the copy went away
but CPU went up". Against the Vec (before) arm of the morning campaign on
2026-09-06, batch was +2% CPU/RPC at 64 KiB; lean turns that into −3.8%. That
comparison spans campaigns on different days and therefore includes rig
variation.

## What lean changed

The C side is unchanged; only two Rust crates moved. The writer-side quota
(epoch, remaining, attempts, `TxBudget`, grant, the 1 ms gate) is deleted and the
chunk itself is the backpressure. The writer lives inside the endpoint as a
`Box<dyn TxWriter>` and the C call happens under the endpoint lock, cutting five
lock pairs per write to one. The waker is cloned only on the Pending path and the
owner check is a thread_local. A per-worker `DriverSignal` plus a per-endpoint
dirty bit let the driver skip idle endpoints without locking, and the prometheus
refresh moved to the 1 ms maintenance pass. `poll_shutdown` returns immediately
and the driver preserves FIN ordering. The `tx_budget_wait` metric is deleted.

## Clean capacity

Median of three 8 s runs. CPU is the summed utime+stime of the eight Arm workers
divided by scheduled RPCs.

| Protocol | Payload | RPC/s batch→lean | CPU µs/RPC batch→lean | p50 µs | p99 µs |
|---|---:|---:|---:|---:|---:|
| gRPC | 64 B | 35,154→36,694 (+4.38%) | 183.01→174.92 (−4.42%) | 1,822→1,768 | 3,083→2,553 |
| gRPC | 1 KiB | 33,790→34,828 (+3.07%) | 190.14→183.61 (−3.43%) | 1,878→1,829 | 4,156→3,233 |
| gRPC | 8 KiB | 20,960→21,619 (+3.14%) | 314.08→305.08 (−2.87%) | 3,027→2,944 | 4,777→4,407 |
| gRPC | 64 KiB | 12,679→13,120 (+3.48%) | 613.24→590.00 (−3.79%) | 5,003→4,870 | 7,549→7,253 |
| opaque | 64 B | 135,303→134,353 (−0.70%) | 23.71→23.35 (−1.51%) | 441→443 | 1,103→1,116 |
| opaque | 1 KiB | 120,658→117,052 (−2.99%) | 25.57→25.98 (+1.60%) | 483→502 | 1,178→1,177 |

RPC/s ranges over the three runs separate everywhere for gRPC: 64 B
34,872–35,627 against 36,665–36,937, 1 KiB 33,536–34,227 against 34,638–35,227,
8 KiB 20,774–21,059 against 21,580–21,621, 64 KiB 12,645–12,713 against
13,013–13,169. Opaque overlaps: 64 B 134,772–135,901 against 131,532–139,349,
1 KiB 118,888–125,753 against 116,617–123,579.

Total worker CPU at 64 KiB is the same, 7.78 cores for batch and 7.74 for lean:
the same CPU handled 3.5% more RPCs.

## Matched rate

gRPC 20k RPC/s at 64 B and 1 KiB, 10k at 8 KiB, 2k at 64 KiB. Opaque 20k RPC/s.

| Protocol | Payload | CPU µs/RPC batch→lean | p50 µs | p99 µs |
|---|---:|---:|---:|---:|
| gRPC | 64 B | 333.00→330.06 (−0.88%) | 998→963 | 2,003→1,941 |
| gRPC | 1 KiB | 334.75→332.06 (−0.80%) | 1,040→1,003 | 2,093→2,040 |
| gRPC | 8 KiB | 558.38→541.38 (−3.04%) | 1,704→1,695 | 2,319→2,264 |
| gRPC | 64 KiB | 1,016.25→965.00 (−5.04%) | 1,459→1,437 | 2,588→2,553 |
| opaque | 64 B | 79.31→77.75 (−1.97%) | 263→259 | 490→483 |
| opaque | 1 KiB | 80.81→80.69 (−0.15%) | 265→251 | 491→476 |

## Publication shape is unchanged

A separate 12 s run counted only the successful returns of
`dmesh_l7_tx_batch_flush` with a uprobe.

| Protocol | Payload | Mean publication bytes batch→lean | Publications/MiB batch→lean |
|---|---:|---:|---:|
| gRPC | 64 B | 320.5→321.9 | 3,272→3,257 |
| gRPC | 1 KiB | 2,879.5→2,872.7 | 364→365 |
| gRPC | 8 KiB | 7,006.9→6,999.4 | 149.7→149.8 |
| gRPC | 64 KiB | 9,490.8→9,508.4 | 110.5→110.3 |
| opaque | 64 B | 266.1→266.2 | 3,941→3,939 |
| opaque | 1 KiB | 3,449.0→3,451.8 | 304.0→303.8 |

Full 64 KiB publications are 0 in both arms. Across every clean sample the arena
copy bytes equal the accepted bytes and the publication count equals the reserve
attempt count. lean's TX retries total 26 across all 30 clean runs (a full chunk
or an arena wait), matching its 26 writer wakes. batch has 0 retries and 60
budget-wait wakes.

## Where the saving came from

64 KiB PMU (three separate 12 s runs, middle 8 s):

| Metric | batch | lean | Change |
|---|---:|---:|---:|
| cycles/RPC | 1,308,634 | 1,262,728 | −3.51% |
| instructions/RPC | 573,421 | 545,137 | −4.93% |
| IPC | 0.439 | 0.432 | −1.65% |
| cache-misses/RPC | 10,008 | 9,786 | −2.22% |
| context-switches/RPC | 0.227 | 0.195 | −14.2% |

batch's second and third PMU runs sat at 11.0K RPC/s on 6.8 cores, below the
first (12.7K on 7.8 cores) and below all three lean runs (13.0K on 7.7 cores).
The per-RPC values are not much affected by that dip, and the dip appeared only
in the batch arm, so the samples were kept as they are (`pmu-samples.csv`).
Unlike the direct TX campaign, this time the instruction count itself fell.

Profile (separate 16 s run, exclusive self % summed over the eight workers):

| Payload | Atomic helpers | `ExternalBackend::drain` self | memcpy | kernel |
|---|---:|---:|---:|---:|
| 64 B | 10.75→10.40 | — | 2.78→2.88 | 11.07→12.54 |
| 1 KiB | 10.56→9.69 | — | 2.91→3.58 | 10.93→12.04 |
| 64 KiB | 11.09→10.17 | 1.27→0.80 | 5.23→5.94 | 5.78→5.49 |

At 64 KiB `__aarch64_cas1_acq` fell 1.26→0.81, `ldadd4_acq_rel` 1.45→1.19 and
`ldadd8_rel` 1.13→0.89, and the 0.35 self of `poll_write_vectored` split into
0.23 for `poll_transmit` and 0.22 for `DirectWriter::write`. The rise in the
memcpy share is a share change, not an absolute one: everything else shrank while
the publication shape and copy bytes stayed the same.

The kernel share rose 1.5pp at the small sizes. The clean counters show why: in
lean, drain passes per RPC rise 7.9→8.9 and park/arm transitions 1.6→2.2. Passes
became cheaper, so the driver catches up with the stack more often and parks more
often. That park/arm count is the next lever.

## Rig drift check

After the main A/B, batch gRPC 64 KiB was measured three more times:
12,659/12,626/12,602 RPC/s at CPU/RPC 612.6/612.7/616.3 µs. That matches the main
batch samples (12,645–12,713 at 610.6–613.4) and does not overlap lean
(13,013–13,169 at 587.1–591.5). There is no drift within this campaign.

## Two incidents

The first `run_matrix.sh` stopped during setup: the scratch directory created
under sudo was root-owned, so the unprivileged arena-layout compile could not
write into it. Nothing had been deployed, so the rig was unchanged
(`raw/matrix-attempt1-setup-permission.log`).

The second run stopped in the warmup of the batch/opaque deployment. The first
dial arrived 150 ms after the echo Pod registered, Linkerd refused it with
"No dmesh backend channel … no live registration", and right after C poisoned
eight sessions the **batch runtime (PID 3447022) disappeared without a log**.
dmesg records no segfault or OOM, and `core_pattern` is apport, so there is no
core either (`raw/batch-opaque/dpu-log-3447022-died.txt`). That binary is
identical to the preceding campaign's batch and so is unrelated to the lean
changes, but "refused dial → poison → silent exit" is an open defect. The
reproduction condition is a first dial before the controller feed carries the new
Pod, and it did not appear on the retried deployment. `campaign.py deploy` now
waits five seconds after the rollout and retries the warmup up to four times;
five of the six deployments passed on the first warmup.

## Method and quality gates

- Binaries: batch `b62194f1…36eb` (kept on the DPU by the preceding campaign),
  lean `1577c3cc…e270` (`bench/bench.sh build`, Rust release LTO+jemalloc, C
  debugoptimized). Both were staged in `/tmp/tx-fixed-overhead-20260906/` and
  every deployment was checked by hashing `/proc/<pid>/exe`.
- Same BlueField-3, N/K/A = 32/8/8, Linkerd on all workers, client CPUs 4–9,
  server CPUs 10–17, closed loop of 8 threads at concurrency 8, gRPC image
  `direct-tx`.
- gRPC ran batch then lean; opaque ran lean then batch.
- All 99 samples (75 clean, 6 PMU, 12 trace, 6 profile) are `OK`, with a maximum
  of 0 for fail, drop, overflow, worker_fail, reorder and eq_budget_exhausted.
- Before every L7 arm shut down, the arena read 1,024/1,024 free with 0 live
  chunks, confirmed seven times, and the L7 gauges (session, task, DMA, queue,
  ACK, FIN) were 0.
- lean tree software gates: dmesh-doca 35/35, adapter 41/41, production
  `cargo check` OK.

Restoration is in [RESTORATION.md](RESTORATION.md). Derived data is in
[comparison.csv](comparison.csv), [summary.csv](summary.csv),
[trace-summary.csv](trace-summary.csv), [pmu-summary.csv](pmu-summary.csv),
[profile-summary.csv](profile-summary.csv) and
[report-tables.md](report-tables.md); the figure is
[comparison.png](comparison.png). Raw samples and the procedure are in `raw/`,
`campaign.py`, `run_matrix.sh`, `resume_matrix.sh` and `commands.txt`.
