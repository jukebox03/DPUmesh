# gRPC batching, Host headroom and DPU hot-path analysis — 2026-09-01

## Verdict

There were two different facts hidden by the original 64 B and 1 KiB result.

1. The gRPC adapter called native `dmesh_flush` at every EventEngine Write
   boundary. chttp2 commonly exposes headers, DATA and trailers as separate
   writes, so this defeated libdpumesh's bounded busy-stream coalescer. Removing
   that physical flush is valid because `PostSend` has already transferred byte
   custody; an idle tail still publishes immediately, a busy tail has a 500 us
   maximum delay, and close flushes the ordered tail before FIN.
2. Even after that fix, per-RPC Linkerd/HTTP/2 work is the dominant small-message
   cost. A descriptive fit through the post-change closed-loop plateaus is
   `100.9 ARM us/request + 31.4 ns/frame-byte` (`R2=0.99995`). At 1 KiB, about
   75% of that fitted cost is the fixed term. This is why 64 B and 1 KiB remain
   closer than 1 KiB and 8 KiB.

The optimization is retained. At total concurrency 256, the three-run medians
changed as follows:

| Frame | RPS before | RPS after | Change | p50 before | p50 after | ARM us/request before | after |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 29,294 | 58,150 | +98.5% | 8.658 ms | 4.063 ms | 271.4 | 125.9 |
| 1 KiB | 27,852 | 51,031 | +83.2% | 9.131 ms | 4.754 ms | 286.0 | 147.8 |
| 8 KiB | 19,902 | 22,342 | +12.3% | 12.696 ms | 11.211 ms | 402.3 | 358.1 |

The deeper closed-loop plateaus are 71.9k, 58.4k and 22.3k request/s for 64 B,
1 KiB and 8 KiB. They are not open-loop capacities. The repeated 64 B
open-loop grid has a **45,000 request/s highest clean point**, bracketed by the
first bad point at 48,000/s. The highest point whose median p50 is at most 1 ms
remains 16,000/s (872 us p50).

After the diagnostic counters were disabled and the optimized build was
redeployed, a final independent knee bracket reproduced that verdict. At 45k,
all three repetitions delivered 44,994--45,095/s with zero failures, drops,
pending requests or internal loss counters; the medians were 3.025 ms p50,
10.017 ms p99, 2.13/9 client cores, 2.86/9 server cores and 7.69/8 DPU worker
cores. At 48k, median delivery fell to 35,514/s with 3,382 drops and 5.70 s
p99. This final bracket is in [`final-knee.csv`](final-knee.csv).

No retained point reported an RPC failure, drop, pending request, worker
failure, retained-credit loss, EQ-budget exhaustion or Pod restart. The bad
open-loop points are retained only as the right side of the knee and do report
drops or failures.

## Host-core question

The original observation confused CPU placement with CPU consumption. The gRPC
server has threads scheduled across every allowed CPU, but `pidstat` measured
about 1.34 process cores under the pre-change 64 B saturation load; the complete
server Pod cgroup (application plus broker) measured 1.35--1.86 cores in the
repeated grid, not six cores.

The client and server were nevertheless widened from 6+6 CPUs to 9+9 CPUs.
With the unchanged forced-flush binary, total-concurrency-256 throughput rose
only 3.2% at 64 B and 4.0% at 1 KiB while DPU ARM stayed at 7.95--7.98 cores.
That is scheduler headroom, not evidence of a six-core Host ceiling. After the
optimization and at the deepest 64 B point, the server uses 3.54 of its nine
CPUs while all eight DPU workers read 90--94% in the same interval. At 1 KiB
and 8 KiB the DPU reaches 7.83 and 8.00 worker cores. Host CPU is not the first
limit on this rig.

This is an eight-worker architectural ceiling, not exhaustion of every physical
ARM CPU: the BlueField reports 16 CPUs, while `MAX_ARM_WORKERS=8`,
`MAX_DPA_RINGS=8` and `A <= K` cap the current data geometry. More workers
cannot be enabled safely with an environment value alone; the ring/PE/reverse
lane ownership arrays and a separate control-thread CPU must change together.
That larger topology campaign is recorded as O3 in `PLAN.md`.

The direct 8 KiB 6-versus-9-core sequence is not retained in `host-core-ab.csv`:
the 6-core repetitions declined monotonically from 19.2k to 16.6k while DPU CPU
fell from 8.03 to 7.03 cores, so that arm was not stationary. The independent
broker baseline's stable 6-core 8 KiB plateau was 19.8k, effectively the same
as the 9-core pre-change 19.9k result.

A post-measurement policy rollout exposed a harness edge case: `bench.sh pin`
could select a stale CRI sandbox carrying the same app label. It now resolves
the current Ready Kubernetes Pod UID and selects the matching sandbox. The
final live placement was verified from each container's `/proc/1/status` as
client CPUs 18--26 and server CPUs 27--35, with each Pod's broker on the same
nine-CPU set.

## What “batching” means here

There are three batching layers, and one word for all three was misleading.

- `DmeshEndpoint` coalesces consecutive gRPC slices into one registered-memory
  reservation.
- libdpumesh now owns physical byte-stream unit coalescing across logical gRPC
  writes. This is the layer the adapter's forced flush disabled and the layer
  responsible for the large A/B improvement.
- The DPU egress engine can put several already-built `px_unit`s into one
  SG-DMA task. That last layer almost never combines units in this workload:
  multi-unit tasks were 0.006%, 0.056% and 0.083% for 64 B, 1 KiB and 8 KiB.

That does not mean no useful batching remains. In the isolated 64 B run,
1,175,830 RPCs required only 269,583 DPU egress DMA batches, or 4.36 RPCs per
batch. Linkerd output had already coalesced several RPC byte streams into each
unit; there was usually only one such unit waiting when the SG engine ran.
Adding another delay merely to raise `units_per_dma_batch` has a low measured
ceiling: post-change `perf` attributes 0.59% self time to `px_worker_drain`, and
the `libdoca_dma` DSO accounts for 0.20%. It was therefore not implemented.

## Busy-loop question

There is no idle hot loop in the ARM worker runtime. Each current-thread Tokio
runtime drains twice around notification arming, then waits on the DOCA
completion fd, DMA fd, cross-worker wake fd, internal task waker, or the 1 ms
maintenance timer. Quiescent sampling showed each `dmesh-w*` thread at 0--1%.

Under load, all workers intentionally remain runnable while progress is
continuous. That CPU is useful protocol work rather than false progress:

- the optimized 64 B `perf` call graph attributes 39.3% children time to the
  Hyper HTTP/2 server stream future and 29.6% to its service future;
- leading self-time is memcpy (4.47%), allocator/free work, and AArch64 atomics;
- `px_worker_drain` is only 0.59% self time and `libdoca_dma` only 0.20% by DSO;
- idle counters continue to advance at about the 1 ms maintenance cadence when
  traffic stops, while `progressed` and DMA counters stop.

Child percentages overlap and must not be summed; they locate the call tree,
not exclusive CPU. The key counterexample to a spin is the state transition:
0--1% per worker at quiescence, 90--94% evenly across all eight workers under
deep load, and protocol/allocator/copy samples rather than a poll loop at the
top of the profile.

## Correctness and provenance

The source base is `36d095da4222e7076958140ee462a58d219b8d8f`; the measured
worktree adds the no-forced-flush adapter change and worker-local diagnostic
counters. Geometry is `N/K/A/L=32/8/8/8`, gRPC v1.80.0, eight client/server
reactors, eight channels, fixed 2.5 GHz Host clocks and 9+9 Host CPUs.

- release CTest: 4/4 pass;
- Clang ASAN+UBSAN: 4/4 pass (`detect_leaks=0` for the host ptrace policy);
- proxy lane/SG-DMA queue test: pass;
- real-DPU deploy smoke and `grpcshutdown`: pass, `opened=closed=6`, recycled
  slot served 85,423 RPCs with failure/drop/loss counters zero;
- gRPC policy/routing surfaces: 19/19 pass, 0 fail
  ([`stages.csv`](../policy-route-grpc-20260901-185430/stages.csv));
- post-performance Pods: Running, zero restarts;
- every DPU worker: `opened=closed`, `active=pending=tasks=0`.

The maintained checklist and executable gate are in
[`design/GRPC.md`](../../../../design/GRPC.md) and
[`grpc_correctness.sh`](../../../suite/grpc_correctness.sh).

## Receipt files

- [`closed-medians.csv`](closed-medians.csv): same-placement forced-flush and
  native-coalescing medians plus deeper post-change windows.
- [`open-medians.csv`](open-medians.csv): repeated 64 B open-loop grid, including
  the 45k/48k knee bracket.
- [`batch-counters.csv`](batch-counters.csv): cumulative-counter deltas around
  three isolated 20-second runs.
- [`host-core-ab.csv`](host-core-ab.csv): unchanged-binary 6+6 versus 9+9 Host
  placement A/B for the stationary payloads.
- [`final-knee.csv`](final-knee.csv): diagnostic-off optimized deployment,
  exact 45k clean/48k overload bracket with per-worker CPU.

This is a single-node DPUmesh result. It is not a stock-TCP comparison, a fair
ARM/x86 equivalence result, or a two-node receipt.
