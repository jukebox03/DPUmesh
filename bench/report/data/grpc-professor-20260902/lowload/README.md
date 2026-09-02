# Low-load diagnosis receipts (2026-09-02, N/K/A=32/8/8, 64 B)

Why one open request costs a DPU worker 0.7 ms of CPU and 1.6 ms of latency at
100 RPS when the same request costs 87 µs and 0.6 ms at the knee. All runs use
the professor-report geometry and pins; `baseline` is the committed source,
`tail50us` is the E2 build with `TX_TAIL_DELAY_NS` 500 → 50 µs (reverted).

| file | what it holds |
|---|---|
| `open-loop-raw.csv` | single-channel 100–2,000 RPS on both builds and 8-channel 500/1k/10k on the tail50us build; per-worker CPU columns show one worker carrying the channel |
| `closed-single-inflight-raw.csv` | closed loop, one generator thread, 1/2/4 requests in flight in total: the warm single-request floor is 0.93–0.95 ms |
| `pmu-per-rpc.csv` / `pmu-raw.txt` | `perf stat -t <busy worker>` cycles/instructions/cache-misses/task-clock/syscalls per RPC at 100 RPS (1 ch), 10k and 50k (8 ch) |
| `drain-passes.txt` | `DPUMESH_PERF_STATS=1` runtime-loop counters: drain passes per RPC and their Progressed/Pending/Idle split |
| `syscalls-500rps.txt` | `perf trace -s` on the busy worker: 29 `epoll_pwait`, 12 `read`, 7 `write` per RPC |
| `wakes-500rps.txt` | context switches and syscalls per RPC of the busy worker against an idle worker |
| `e5-ab.csv` / `e5-open-loop-raw.csv` / `e5-closed-raw.csv` | E5 first build (Rust drain before the C engine drain; wake-eventfd read only when a tick was posted) against baseline: median of 3 per condition |
| `e5-probes-100rps.txt` | uprobe counts per RPC on the e5-1 build: hyper server-connection poll 2.0, h2 client-connection poll 4.0, drain 21, wake 1.2 |
| `sched-100rps-clusters.txt` | `sched:sched_switch` trace at 100 RPS: each request is two continuous on-CPU runs (forward ≈450 µs, reverse ≈320 µs) separated by the server-side gap |

Commands are in [`../EXPERIMENT.md`](../EXPERIMENT.md#e-사전-등록-실험).
