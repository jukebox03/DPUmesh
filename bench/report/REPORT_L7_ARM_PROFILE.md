# Linkerd DMesh ARM profile (2026-08-14)

This note records the first P4 profiling pass on the target BlueField ARM
cores. It is an attribution run, not the frozen performance baseline: each
point has one `perf stat` sample and one `perf record` sample, and there is no
equivalent full-stack x86 request path yet.

## Configuration

- Exact working tree deployed with `debugoptimized` DPU C and release Rust.
- `DPUMESH_DPA_THREADS=32`, `DPUMESH_RINGS_PER_POD=8`,
  `DPUMESH_ARM_WORKERS=4`, `DPUMESH_L7_LINKERD_WORKER=all`.
- Opaque services 11, 13 and 14; the measured point used service 11.
- 1,024-byte request, 8-byte reply, one client thread, ten measured seconds.
- `perf stat` attached to the whole `dpumesh_dpu` process for 13 seconds. The
  table subtracts an equal-duration idle process sample before dividing by
  completed requests.
- `perf record -F 199 -g --call-graph dwarf,16384` covered the same 13-second
  window. All three pre-fix profiles reported zero lost samples.

Every load point completed with `fail=0`, `drops=0`, `worker_fail=0` and
`reorder=0`.

## ARM perf stat

| concurrency | requests | Mrps | p50 us | p99 us | cycles/request | instructions/request | cache misses/request | process CPU us/request |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 48,053 | 0.004908 | 168 | 538 | 328,893 | 297,343 | 2,091.23 | 159.55 |
| 32 | 421,637 | 0.042273 | 659 | 1,322 | 18,012 | 16,237 | 132.50 | 8.66 |
| 128 | 1,672,268 | 0.167382 | 705 | 1,318 | 6,705 | 6,011 | 59.83 | 3.26 |

The concurrency-1 row includes a large fixed progress/wakeup cost per request.
Higher concurrency amortizes that work. These values must not be used as a
release comparison without repetitions, fixed affinity/frequency provenance
and the pre-change binary required by the final gate.

## Sample attribution

The most useful flat samples were:

| symbol/category | c1 | c32 | c128 |
|---|---:|---:|---:|
| `eventfd_write` | 3.18% | 2.87% | 3.76% |
| `px_worker_drain` | 2.46% | 2.29% | 3.89% |
| `__memcpy_generic` | 0.53% | 1.71% | 5.85% |
| `__aarch64_cas1_acq` | 1.75% | 0.91% | 1.28% |
| `__aarch64_cas1_rel` | 1.58% | 1.74% | 1.48% |
| `__aarch64_ldadd8_relax` | 1.07% | 0.77% | 1.42% |
| kernel `mutex_lock` (largest one symbol) | 0.52% | 0.88% | 0.60% |

Call graphs put `pump_side` and its children at about 3.18% for c32 and 4.88%
for c128. At c128, `copy_tx_into`/`memcpy` was the largest child on the output
side; `DmeshIo::poll_read` and its copy children accounted for about 4.19%.
No allocator or Tokio scheduler symbol dominated the flat profile.

The shared C pool is not hot. The deployed audit counter reached only 16
shared-list acquisitions with zero contention after the multi-worker acceptance
traffic, and no pool symbol appeared in the profiles. The existing bounded
per-thread magazines are sufficient for this workload; further pool
partitioning would add complexity without evidence.

## Compiler output around the hot atomics

The x86-64 release static library and the deployed AArch64 binary were built
from the same working tree. `objdump -drC` shows the parking-lot mutex fast path
as follows:

```text
x86-64 acquire/release fast path:
    lock cmpxchg %cl,(%rbx)

AArch64 call sites:
    bl __aarch64_cas1_acq
    bl __aarch64_cas1_rel
```

The AArch64 helpers dispatch to LSE `casab`/`caslb` when available. Their
fallbacks are exclusive loops: acquire uses `ldaxrb` + `stxrb`, release uses
`ldxrb` + `stlxrb`, and acquire-release uses `ldaxrb` + `stlxrb`. Relaxed
counters dispatch to `ldadd` or an `ldxr`/`stxr` loop. There is no explicit
`dmb` full barrier in these hot helpers.

## Profiling fix and post-fix check

The first profile revealed that the lock instrumentation itself used one
process-global relaxed `AtomicU64` increment for every endpoint lock. With four
workers this added a shared RMW/cacheline to a lock whose measured contention
was zero. The counters now use owning-worker thread-local `Cell<u64>` values.

The exact post-fix tree was redeployed under the same 32/8/4 configuration.
At c128 it again passed all correctness gates. One idle-subtracted stat sample
reported 6,609 cycles/request (6,705 before); because this is a single sample,
the 1.4% delta is directional only. More importantly, a post-fix flat profile
reduced `__aarch64_ldadd8_relax` from 1.42% to 0.26%, while per-worker logs
retained independent endpoint acquisition counts and reported zero contention.
A 12-thread, three-service acceptance point also passed at 0.137552 Mrps with
worker CPU CV 0.5%, and all four registries returned to zero active, pending,
task and orphan counts.

## Decision

- Do not change the C pool again unless a future workload makes its contention
  counter nonzero or attributes samples to it.
- Do not claim an x86/ARM performance ratio yet. P8 needs an equivalent x86
  stack-slice or full-stack harness and repeated runs.
- The next evidence-backed optimization candidates are copy reduction and
  wake/progress batching. Removing the endpoint mutex through `LocalSet` remains
  a separate P4.2 experiment; it must preserve Linkerd's type bounds and
  cancellation behavior.
