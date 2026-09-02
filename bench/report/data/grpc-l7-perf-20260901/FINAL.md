# gRPC DPU L7 hot-path and optimization receipt — 2026-09-01

## Verdict

The DPU is the current capacity limiter, but not because of an accidental busy
loop. The old low RPS contained two large, removable build-configuration costs:
the embedded Rust staticlib had lost Linkerd's jemalloc global allocator and its
workspace had lost Linkerd's release LTO. Both are fixed. At closed-loop total
concurrency 1,024, the repeated 64-byte median moves from 70,670 request/s with
glibc to 96,570 with Rust-global jemalloc, then to 104,678 with LTO. That is
+48.1% end to end. The 1 KiB LTO A/B moves from 73,932 to 85,178 request/s
(+15.2%).

The current open-loop brackets are:

| Frame | Highest throughput-clean | First bad | Clean-point median p50 | median p99 | DPU worker cores | client/server Pod cores |
|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 95,000/s | 100,000/s | 5.127 ms | 88.790 ms | 7.91/8 | 3.66/4.67 of 9 each |
| 1 KiB | 70,000/s | 75,000/s | 2.311 ms | 5.673 ms | 7.82/8 | 3.14/3.89 of 9 each |

“Throughput-clean” means achieved/offered >= 0.99 and zero failures, drops,
pending requests, worker failures, retained-credit loss, EQ-budget exhaustion
and Pod restarts. It is not a latency SLO. The 64-byte 95k point is visibly at
the queueing edge: its three p99 values are 88.790, 92.341 and 20.438 ms. The
lower 90k point has a 16.367 ms median p99. Raw repetitions are in
[`open-capacity.csv`](open-capacity.csv), and the maintained analyzer's result
is [`knees.csv`](knees.csv).

The payload result is now unambiguous. The open-loop knees differ by 25k/s
(64 B is 35.7% above 1 KiB), and closed-loop medians differ by 19.5k/s (104.7k
versus 85.2k). Small-message latency and CPU can still look close below the knee
because each RPC pays a large fixed L7 cost before its payload bytes matter.
This is consistent with, rather than evidence against, native coalescing. The
earlier batching receipt measured 4.36 64-byte RPCs per DMA batch; SG-DMA
multi-unit tasks themselves were only 0.006% of DMA batches. Delaying DMA to
inflate that latter counter has little available upside.

## What the L7 path actually does

For each long-lived protocol-aware connection, DPUmesh feeds DMA staging bytes
to `DmeshIo`, and the embedded Linkerd outbound stack terminates the client-side
HTTP/2 connection. For each RPC it then:

1. copies completed staging bytes into Hyper's read buffer;
2. parses HTTP/2 frames, HPACK-decodes headers and maintains stream/flow-control
   state;
3. matches the gRPC route, applies request/response filters and retry/timeout
   policy, load-sheds when unavailable and selects a backend;
4. records request/status/body metrics, tracing and gRPC response
   classification;
5. HPACK-encodes and schedules frames on a separate backend-side HTTP/2
   connection, then performs the corresponding response path;
6. writes into the `DmeshIo` TX vector, copies accepted output into the DPU
   egress arena and submits it to the datapath.

The two H2 endpoints cannot share compressed header blocks. HPACK dynamic tables
belong to different connections, and policy/routing requires decoded method and
headers. H2 parse/decode plus encode and stream state are therefore inherent to
this architecture. Discovery, protocol detection, stack construction and the
H2 handshake are per connection/session, not per RPC in this eight persistent
channel workload.

The DPUmesh-specific residual is not inherent. `DmeshIo::poll_write` copies into
a `Vec`; driver publication then copies from that queue into the egress arena.
RX has one staging-to-`ReadBuf` copy. The stack and driver handles share one
per-connection `Arc<parking_lot::Mutex<Inner>>`, and queue length, copy,
consume and drain-state calls take separate locks. PLAN O2 should replace the
TX vector with a direct `AsyncWrite` reservation and fuse publication state,
with partial-write, backpressure, order, cancellation and shutdown tests. The
3.63% `memcpy` self sample is an upper bound on all memcpy, not a promise that O2
will recover 3.63%; the expected 64-byte gain is single-digit and should grow
with payload size. A worker-local/non-atomic I/O specialization is plausible
after O2, but requires a deliberate `Send + Sync` ownership redesign.

Policy-layer specialization is lower priority. The steady stack contains
route/backend distribution, filters, retry extensions, concurrency/load shed,
timeouts, rescue, tap, metrics, tracing, classification and response boxing.
When a deployment has no tap, trace collector, retry or filters, constructing
explicit no-op variants may remove some wrappers and Arc operations. The
profile does not identify one of these as a dominant exclusive function, so it
needs feature-by-feature A/B rather than deleting observability on inference.

## `perf` evidence

The retained final-build profile used `perf record -F 49 -e cycles
--call-graph fp` for eight seconds during a 50k/s constant-arrival 64-byte load.
The load delivered 49,999/s, p50 1.727 ms and p99 2.733 ms with every error/loss
counter zero. It captured 3,009 samples with zero lost. Higher-overhead DWARF
call-graph collection changed throughput and was rejected as observer-affected.

Top self samples are in [`perf-self.csv`](perf-self.csv). The largest are
memcpy 3.63%, atomic primitives 2.96% + 2.32% plus smaller CAS operations,
syscall entry 2.53%, HPACK encode 1.73%, `px_worker_drain` 1.20%, H2 receive and
poll functions spread below 1%, jemalloc malloc 0.78%, route selection 0.55%,
and DOCA PE progress 0.25%. Inclusive Tokio/H2/service call-tree percentages
overlap and are not additive. Protocol detection appears as a long-lived
ancestor; it is not a steady self hotspot.

`perf stat` at the same clean 50k/s load reports 7.718 DPU CPUs, 130.83 billion
cycles, 70.64 billion instructions and IPC 0.54 in eight seconds. Over about
400k sampled RPCs that is 327k cycles, 177k instructions and 154.4 us of DPU CPU
per RPC. The low IPC and distributed samples fit pointer-heavy H2 state,
allocation, copies and atomics, not one compute loop. `px_worker_drain`, the
Rust external backend drain and DOCA progress are small exclusive terms, and
the same workers measure 0--1% at quiescence. Full counters are in
[`perf-stat.csv`](perf-stat.csv).

CPU utilization is consequently a poor standalone capacity estimator here.
The workers already use 7.718 cores at the clean, low-tail 50k profile, yet they
deliver 95k at 7.91 cores near the open-loop knee. The latter has deeper queues,
more work per wake and more RPCs per transport publication, so fixed poll,
wakeup and scheduling work amortizes while latency rises. The closed-loop
64-byte plateau similarly spends about 69.6 ARM us/request (7.29 cores / 104.7k)
versus 154.4 us/request in the 50k profile. This is the measured reason “DPU CPU
is high relative to current RPS” does not imply that a loop is burning the
difference; it does mean that buying throughput through queue depth trades tail
latency.

## Optimizations retained

The stock Linkerd executable declares jemalloc in its `main.rs`; the embedded
staticlib is instead linked into a C executable and had no Rust final artifact
to install it. A same-binary DPU `LD_PRELOAD=libjemalloc.so.2` experiment first
moved the 64-byte median from 70,670 to 102,027/s (+44.4%). The retained Rust
global allocator moves it to 96,570/s (+36.7%) while leaving C allocations on
glibc. The remaining 5.6% preload advantage is evidence that C allocation may
also be optimized, but globally interposing production C/DOCA allocation is not
accepted without a separate safety A/B. Raw results are in
[`allocator-ab.csv`](allocator-ab.csv).

The embedded workspace also had only `opt-level=3`, while the stock Linkerd
workspace uses release LTO. Enabling LTO at the same allocator moves the 64-byte
median from 96,570 to 104,678/s (+8.4%) and 1 KiB from 73,932 to 85,178/s
(+15.2%). Raw results are in [`lto-ab.csv`](lto-ab.csv).

## Host and DPU conclusion

The Host was widened from 6+6 to 9+9 CPUs in the preceding same-binary test and
gained only 3.2--4.0%. On the current 64-byte 95k clean point the two complete
Pod cgroups consume 3.66 and 4.67 of their nine allowed cores while DPU workers
consume 7.91 of eight. At 1 KiB/70k they consume 3.14, 3.89 and 7.82/8. Thread
placement across many Host CPUs was not full utilization; Host CPU is not the
first limit.

The uncomfortable part of the original concern remains true: after removing
the allocator and compiler mistakes, the eight DPU data workers still define
the knee. What the data disproves is that they are being consumed by a useless
loop or broken DMA batching. Raising only an environment variable is unsafe:
`MAX_ARM_WORKERS=8`, `MAX_DPA_RINGS=8`, `A <= K`, fixed ownership arrays, PE
lanes and the main/control-thread affinity must change together. That topology
work remains PLAN O3. The current result is nevertheless a concrete
counterexample to “low RPS is inherent L7 cost”: the same eight-worker hardware
moves from 45k to 95k open-loop 64-byte clean capacity after allocator and LTO
corrections, while correctness and lifecycle gates remain green.

## Validation

The optimized build passed `make test-hostfree`, the real DPU lane/SG-DMA queue
test, all four release cHTTP2 adapter tests, and all 38 embedded Rust tests.
`bench.sh grpcshutdown` then killed a live H2 client, observed opened=closed
14/14, reused the slot and completed the four-channel exchange with no failure
or drop. After profiling, every worker again reported opened=closed, active,
pending, task and orphan counts zero; both Pods have zero restarts. The compact
receipt is [`correctness.txt`](correctness.txt), and the maintained checklist is
[`design/GRPC.md`](../../../../design/GRPC.md#verification-contract).
