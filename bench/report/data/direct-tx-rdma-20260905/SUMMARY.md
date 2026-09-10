> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Linkerd direct TX and the shared RDMA lease — deployment and comparison

The two implementations were applied in the requested order, built on the
BlueField itself, deployed and compared. **Implementation, deployment and
comparison are complete, but the Linkerd performance acceptance and the real
two-DPU RDMA gate are OPEN.** The intermediate copy is gone, yet 64 KiB gRPC at
peak load became 4.25% slower. This result is not recorded as a performance
improvement, and the corresponding completion checkbox in PLAN.md was left open.

## Implementation and verification

1. **Linkerd TX**: the TX Vec in the production `DmeshIo` and the driver's
   Vec-to-arena copy were removed. The caller slice is copied once into the
   registered arena and committed in the same `poll_write` call.
   `poll_write_vectored` gathers the header and body slices offered together into
   one arena reservation. No caller bytes or pointers are retained on Pending.
   A failed reserve is distinguished from a bad handle, and the
   reserve/commit/cancel, owner-thread, from-the-start backend/Origin/remote
   route, quota/wake, and FIN/abort/slot-reuse contracts were all verified. The
   64 KiB / 4-attempt endpoint limit and the 256 KiB / 64-session worker limit are
   unchanged.
2. **Shared RDMA base**: `peer_wire_ops` gained optional TX reserve/commit/cancel
   and RX acquire/release groups that validate the connection epoch, the slot
   generation and a direction token. TX is reused after the SEND CQ, RX after
   release and repost. The existing copy API now runs on top of the lease. No TLS
   or BIO type is exposed, so TLS and a future IPsec producer/consumer can reuse
   it. **Wiring TLS to the BIO (§11.20 B) and the IPsec implementation are not in
   this shared base**, so this does not mean the production copy in today's TLS
   is already gone.

Verification passed: `make test DPUMESHD_PYTHON=/opt/dpumesh/venv/bin/python`, 40
adapter Rust tests, 30 `dmesh-doca` Rust tests, gRPC release CTest 4/4 and Clang
ASAN+UBSAN CTest 4/4. The mock-provider trials, which include the real RDMA C
implementation, covered exhaustion, CQ delay, stale and cross-direction release,
a full RX FIFO with held slots, post faults, wrap, and QP-before-MR teardown
order, and passed under ASAN+UBSAN and on the BlueField. In the real wire suite
TCP passed and RDMA is skipped for environmental reasons.

A closing gRPC deny-policy check matched 65 failed requests against a
process-global inbound denied of 65, and removing the policy restored fail=0. The
global denied value, which is exposed redundantly on all eight admin endpoints,
was not summed.

## Comparison conditions

The same BlueField-3, N/K/A = 32/8/8, Linkerd on all workers, rustc 1.90.0
release LTO with jemalloc, C `-O2 -g` debugoptimized. The same two
Restricted/Device Plugin workloads and Service were used. The client was pinned
to host CPUs 4–9 and the server to 10–17 **from process start**. This is a system
comparison at that CPU placement, not a DPU-only maximum free of host
bottlenecks.

Each of 64 B / 1 / 8 / 64 KiB ran matched-rate and closed-loop, median of three
5 s runs per condition. Matched rates were 20k / 20k / 10k / 2k RPC/s; closed
loop was 8 threads at concurrency 8, i.e. 64 outstanding. A separate first warm
run was excluded. The DPU compiler and the diagnostic debugger were not run
during timed measurement. All 96 final samples were kept: gRPC 24/24 good in both
arms, opaque 20/24 good in both.

CPU/RPC sums the utime+stime of `dmesh-w0..7` and divides by scheduled RPCs, so
it includes each call's warmup and teardown CPU. Control and admin CPU is
excluded. The comparison CSV also carries p50/p99, absolute CPU and the min/max
of the three runs. Differences from three short runs are not extrapolated into a
universal speedup or statistical significance.

## gRPC results

Peak load:

| Payload | RPC/s before → after | Change | p99 µs before → after | DPU CPU/RPC change |
|---|---:|---:|---:|---:|
| 64 B | 35,523 → 35,784 | +0.73% | 3,456 → 2,688 | +0.18% |
| 1 KiB | 34,877 → 34,765 | −0.32% | 3,316 → 2,734 | +3.38% |
| 8 KiB | 21,291 → 21,898 | +2.85% | 4,586 → 5,137 | +2.44% |
| 64 KiB | 12,795 → 12,251 | −4.25% | 7,470 → 8,119 | +4.72% |

Identical input load:

| Payload | RPC/s before → after | Change | p99 µs before → after | DPU CPU/RPC change |
|---|---:|---:|---:|---:|
| 64 B | 19,999 → 19,999 | +0.00% | 2,180 → 1,927 | +0.91% |
| 1 KiB | 19,999 → 20,000 | +0.01% | 2,045 → 2,009 | +0.75% |
| 8 KiB | 9,999 → 9,999 | +0.00% | 2,255 → 2,305 | +0.98% |
| 64 KiB | 1,999 → 1,999 | +0.00% | 2,718 → 2,617 | +0.40% |

The 64 KiB capacity ranges do not overlap either: before 12,776–12,820 RPC/s,
after 12,233–12,285 RPC/s. At the same size under matched load, p99 improves
2,718→2,617 µs (−3.72%). In short, removing the copy did not translate into a
peak-load CPU or throughput improvement.

## Opaque TCP results

Peak load:

| Payload | RPC/s before → after | Change | p99 µs before → after | DPU CPU/RPC change |
|---|---:|---:|---:|---:|
| 64 B | 138,770 → 139,689 | +0.66% | 1,090 → 1,102 | +1.52% |
| 1 KiB | 124,335 → 126,664 | +1.87% | 1,155 → 1,148 | +0.15% |
| 8 KiB | failed samples: before 1/3, after 1/3 | excluded | excluded | excluded |
| 64 KiB | failed samples: before 3/3, after 3/3 | excluded | excluded | excluded |

Identical input load:

| Payload | RPC/s before → after | Change | p99 µs before → after | DPU CPU/RPC change |
|---|---:|---:|---:|---:|
| 64 B | 19,999 → 19,999 | +0.00% | 492 → 481 | +1.07% |
| 1 KiB | 19,999 → 19,999 | +0.00% | 486 → 491 | +1.32% |
| 8 KiB | 10,000 → 10,000 | +0.00% | 1,777 → 1,788 | +2.51% |
| 64 KiB | 1,999 → 1,999 | +0.00% | 2,842 → 2,356 | +3.41% |

Drain failures occurred in 1/3 of the 8 KiB capacity runs and 3/3 of the 64 KiB
runs, in both arms. A clean capacity figure was not manufactured by keeping only
the successful samples. An earlier preliminary 512-outstanding / 64 KiB baseline
also produced DPU heap corruption, kept as
`raw/before-opaque-crash-dpu.log`. No claim is made that this optimization fixed
those pre-existing failures.

## Copies and resource return

| Final arm | accepted bytes | arena copy bytes | retry / error | budget wait / writer wake |
|---|---:|---:|---:|---:|
| gRPC | 39,823,706,798 | 39,823,706,798 | 0 / 0 | 89,944 / 89,944 |
| opaque | 190,569,151,088 | 190,569,151,088 | 0 / 0 | 2,084 / 1,042 |

The new path performs zero intermediate TX Vec copies: for B accepted bytes the
adapter copy goes **2B → B**. "Accepted" is the volume of completed C
publications and does not mean destination ACK or delivery. The opaque counters
are whole-arm values including the failed samples, and the byte equality in the
table does not hide errors.

After every final arm shut down, the session, task, registration, DMA and queue
residue across all eight workers was 0. After the final opaque arm, a debugger
checked the shared free list and every thread magazine for duplicates and cycles
and confirmed the arena at **1,024/1,024 free with 0 live**; the same 1,024/1,024
return was re-confirmed after the final gRPC diagnosis. These checks are separate
from the timed measurements. **The arena high-watermark was not collected (NA)**,
and full return at shutdown does not substitute for a peak measurement.

## Cost of the shared RDMA API

The retained previous C source and the changed source were run against the same
mock verbs, pinned to BlueField CPU 10. TX covers producer fill, publication and
the SEND CQ; RX covers acquire/copy, observing the first and last byte, and
release. 128 MiB per size, median of three after excluding a warm repeat. This is
the **API CPU cost**, excluding NIC, network and cryptography.

| Size | TX copy → direct lease (ns) | RX copy → direct lease (ns) |
|---|---:|---:|
| 64 B | 25.55 → 36.34 | 35.86 → 44.23 |
| 1 KiB | 79.18 → 56.61 | 70.44 → 44.17 |
| 8 KiB | 397.58 → 164.42 | 312.01 → 44.07 |
| 64 KiB | 3,451.92 → 2,380.05 | 2,114.89 → 44.13 |

64 B got slower because of the fixed cost of token and state validation. The RX
lease figures are neither whole-payload processing nor TLS decryption cost. The
change in the compatibility copy path that today's TLS uses is published as
`after_copy_ns` in `rdma-comparison.csv`; adding the shared API alone does not
make existing TLS as fast as the table above.

Kubernetes here is the single node rapids4, the BlueField p0/p1 are DOWN, and no
address is bound to an RDMA device. **A real two-DPU RDMA before/after could not
be run.** The software gates and this CPU comparison are not offered in place of
a fabric or mTLS end-to-end receipt.

## Regression diagnosis and the remaining acceptance

The initial scalar direct implementation lowered gRPC capacity by 0.6–4.9%. A
vectored write was added so H2's header and body arrive together, with partial
slice and retry trials. The final measurement re-ran from the baseline against
that vectored binary with the CPU placement pinned. The preliminary scalar and
unpinned arms are kept under separate names and were not mixed into the final
tables.

Committing immediately removes the coalescing the old Vec provided between polls
and also moves forward the point at which quota and backpressure reach the
caller. A vectored write gathers the slices within one call but not the output of
different polls. That batching change and the per-call synchronous publication
cost are the candidate causes of the 64 KiB regression. A separate debugger
diagnosis observed the first 128 nonempty commits on the real DPU: before had
three 65,536 B commits and forty-two 16,393 B ones; after had zero 65,536 B and
forty 16,393 B, and the merged size of small control output differed as well
(`raw/{before,after}-grpc-commit-shape.txt`). Because that diagnosis is affected
by startup and by the debugger, it is not used as a steady-state distribution or
as a quantitative cost decomposition; it is evidence supporting the batching
difference. The final gate stays OPEN until further batching and progress
improvements are made and re-measured.

## Deployment and reproduction material

Final running state: the new binary was kept and both workloads were restored to
their original native image and L4 profile. L7 direct TX was verified in the
preceding real opaque and gRPC deployments, and L7 is off in the shutdown
profile. The measurement CPU pins, policies and the temporary Linkerd control
relay were cleaned up; the pre-existing controller relay was left running. After
restoration the native smoke had fail, drop, overflow and worker_fail all 0 and
both workloads were Ready. The running `/proc/<pid>/exe` SHA256 also matches the
final binary below
(`raw/restored-native-{smoke,pods,dpu}.txt`, `restore_native.sh`).

Final binary SHA256:
`2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`

before binary SHA256:
`4273f0148b78346c83a2922db86d5a6a981fa8217fdd3cf148eb980faa76b62e`

Baseline source HEAD `4e43056`. The actual before measurement ran a retained copy
of the previously deployed binary.

- `comparison.csv/json`, `metrics-summary.json`: aggregate of the final 96 samples.
- `raw/{before,after}-{grpc,opaque}-clean/`: raw replies, CPU, Pod and affinity,
  metrics, DPU logs.
- `run_points.py`, `deploy_stage.sh`, `summarize.py`, `commands.txt`: the
  measurement, redeployment and aggregation procedure.
- `rdma_primitive_bench.c`, `rdma-comparison.csv`, `raw/rdma-before/`: the shared
  API before and after.
- `raw/*tests.log`, `raw/lease-asan.log`, `raw/dpu-wire-tests.log`: verification logs.
- `arena_offsets.c`, `arena_quiescence.gdb/sh`,
  `raw/after-opaque-clean-arena.txt`: the real arena return check.
- `raw/policy-*`, `raw/rejected.txt`: the policy regression and why preliminary
  measurements were excluded.
- `raw/final-binary.txt`, `raw/vectored-dpu-build.log`: deployed binary and build
  identification.

Final change patches: `raw/final-implementation.patch`,
`raw/final-linkerd-implementation.patch`.
