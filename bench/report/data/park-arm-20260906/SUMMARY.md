> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Driver park/arm cost — lean to lean2 to lean3

## Verdict

lean3, which cuts the syscalls around the driver loop's park and arm, runs gRPC
at 1.3–3.0% lower CPU/RPC at capacity and 1.7–3.3% lower at matched rate than
lean on the same BlueField. The CPU/RPC ranges are disjoint in all eight
conditions. Capacity RPC/s gains only +0.5 to +1.9%, and the 64 B and 8 KiB
ranges overlap. On opaque TCP the lean2 to lean3 step is larger, −9.7 to −12.0%
CPU/RPC: with so little CPU per request, the saved syscalls show through
directly.

lean2 alone — dropping the epoll in the driver yield and reading the wake
eventfd once — barely moved epoll, 8.30 to 8.18 per RPC. The caller of
`park_yield` was not our yield but the `defer` of an h2/hyper task that had spent
its tokio coop budget of 128 operations. What lean3 actually removed is the tokio
timer waker write (0.6 per RPC) and the epoll(0) inside the PE clear (1.0 per
RPC).

## Arms

| Arm | Change | SHA256 |
|---|---|---|
| lean | the fixed-overhead removal build from the preceding campaign | `1577c3cc…e270` |
| lean2 | runtime loop yield replaced by a self-wake (intended to avoid `park_yield`), wake eventfd read once | `1f4507c9…5fe9` |
| lean3 | plus one pinned maintenance `Sleep` that is only `reset`, `clear_notifications(fired)` clearing only the PEs that fired, and fatal-signal/`atexit` traces in `dpu_main.c` | `d511d0c0…b543` |

ABI: `dmesh_l7_driver_clear_notifications(void *, unsigned fired)` and
`DMESH_L7_NOTIFY_{COMPLETION,DMA,WAKE}` were added. The C data path is unchanged.

## Clean capacity (median of three)

| Protocol | Payload | RPC/s lean / lean2 / lean3 | CPU µs/RPC lean / lean2 / lean3 | lean3 vs lean | p99 µs lean→lean3 |
|---|---:|---:|---:|---:|---:|
| gRPC | 64 B | 35,724 / 36,276 / 36,121 | 179.14 / 176.72 / 175.15 | −2.22% | 2,725→2,662 |
| gRPC | 1 KiB | 34,518 / 34,930 / 34,833 | 186.21 / 183.54 / 181.69 | −2.43% | 3,452→3,102 |
| gRPC | 8 KiB | 21,363 / 21,608 / 21,465 | 307.93 / 303.87 / 304.05 | −1.26% | 4,852→4,584 |
| gRPC | 64 KiB | 12,961 / 13,167 / 13,213 | 596.62 / 586.91 / 578.53 | −3.03% | 7,458→7,740 |
| opaque | 64 B | — / 141,854 / 141,604 | — / 21.95 / 19.83 | lean3 vs lean2 −9.68% | 1,088→1,107 |
| opaque | 1 KiB | — / 125,647 / 131,095 | — / 24.59 / 21.64 | lean3 vs lean2 −12.01% | 1,149→1,120 |

CPU/RPC ranges across the three runs: 64 B 178.3–179.6 against 174.1–175.2,
1 KiB 186.2–186.6 against 180.6–181.9, 8 KiB 307.6–308.2 against 303.3–305.2,
64 KiB 594.9–597.7 against 578.1–578.6. Only the 1 KiB and 64 KiB RPC/s ranges
separate. Opaque 1 KiB capacity separates as well — lean2 122,169–131,259 against
lean3 130,805–134,756 — while 64 B overlaps.

## Matched rate

| Protocol | Payload | CPU µs/RPC lean / lean2 / lean3 | lean3 vs lean | p50 µs lean→lean3 | p99 µs lean→lean3 |
|---|---:|---:|---:|---:|---:|
| gRPC | 64 B | 331.38 / 331.06 / 325.62 | −1.74% | 963→969 | 1,986→1,965 |
| gRPC | 1 KiB | 333.31 / 332.19 / 327.56 | −1.73% | 1,003→1,000 | 2,007→2,011 |
| gRPC | 8 KiB | 549.12 / 547.62 / 534.62 | −2.64% | 1,695→1,687 | 2,314→2,249 |
| gRPC | 64 KiB | 975.62 / 974.38 / 943.12 | −3.33% | 1,437→1,411 | 2,532→2,532 |
| opaque | 64 B | — / 73.69 / 66.50 | lean3 vs lean2 −9.75% | 249→259 | 489→484 |
| opaque | 1 KiB | — / 76.06 / 68.56 | lean3 vs lean2 −9.86% | 257→247 | 483→489 |

## Where the syscalls went

`perf trace -s` counted six of the twelve seconds of a run across all eight
workers (per RPC, gRPC).

| Payload | Syscall | lean | lean2 | lean3 |
|---|---|---:|---:|---:|
| 64 B | epoll_pwait | 8.30 | 8.18 | 7.01 |
| 64 B | read (EAGAIN) | 2.37 (1.04) | 2.33 (1.03) | 2.73 (1.21) |
| 64 B | write | 1.20 | 1.18 | 0.68 |
| 64 KiB | epoll_pwait | 22.46 | 22.55 | 21.96 |
| 64 KiB | read (EAGAIN) | 1.51 (0.68) | 1.52 (0.68) | 1.65 (0.73) |
| 64 KiB | write | 1.17 | 1.16 | 0.92 |

Call paths (`perf record -e syscalls:*` with callchains, 64 B):

- epoll_pwait: in lean, 61% comes from tokio's `Driver::turn` (of which
  `park_yield` is 53% and a real `park` 8%) and 39% from inside DOCA
  (`doca_pe_clear_notification` 22%, `doca_pe_request_notification` 9%). In lean3
  the DOCA share falls from 3.2 to 2.2 per RPC, while tokio's `park_yield` stays
  at 4.4.
- write: in lean, 50% is `Sleep::poll → Handle::reregister → mio Waker::wake`
  (the `sleep_until` re-registration on every `select!`), 25% is
  `dpu_request_host_doorbell` (a DMA completion callback waking main), and 25% is
  the eventfd write inside `doca_pe_request_notification`. In lean3 the tokio
  share drops from 0.59 to 0.03.
- read: 87% is `mlx5dv_devx_get_event`. In lean this was entirely the clear path;
  in lean3 it is 60% clear and 26% `request_notification`. DOCA drains the
  backlog of a PE whose clear was skipped on the next arm, so the total read
  count does not fall. The pre-campaign belief that a second read of the wake
  eventfd caused the EAGAINs was wrong.

Driver loop counters (per RPC, capacity median): 64 B drain 8.87→8.04, idle
3.37→2.52, arm 2.17→1.77; 64 KiB arm 1.43→1.31. This is the wasted pass that the
timer re-registration used to wake, now gone.

64 KiB PMU (three separate 12 s runs, middle 8 s): cycles/RPC
1,274,785→1,230,955 (−3.4%), instructions/RPC 546,334→536,301 (−1.8%), IPC
0.429→0.435. lean3's worker core goes 7.76→6.75 and context switches per RPC
0.187→0.294, i.e. the thread genuinely parks for longer (mean epoll 7→12.7 µs).

## What is left

- tokio `park_yield` epoll(0), 4.4 per RPC at 64 B and about 20 per RPC at
  64 KiB: caused by h2/hyper tasks exhausting their coop budget, so the driver
  cannot remove it. Wrapping the H2 connection task hyper spawns in
  `tokio::task::unconstrained` removes it, but that turns off task fairness.
- Of the 1.8 arms per RPC, only 0.4–0.7 are a real thread park. The rest are
  "empty parks": `select!` returns Pending and a stack task raises a signal that
  wakes it immediately, wasting roughly one DOCA syscall per arm. One more
  self-wake yield before the arm would reduce them.
- The 0.3 writes per RPC from `dpu_request_host_doorbell → dpu_wake_main` follow
  from the main thread owning the doorbell and are out of scope here.

## Incidents

- Three lean2 64 KiB capacity runs (11.7–12.1K RPC/s with low CPU/RPC) overlapped
  a `cargo test`/`cargo check` of lean3 running on the host (rapids4). The host
  is the load generator, so those runs were quarantined as
  `raw/lean2-grpc/*.json.contaminated` and re-measured as reps 4–6.
- The lean2 runtime (the process before PID 3472082) disappeared just after the
  64 B matched run started, leaving no log, dmesg, journal or core. The batch
  runtime of the preceding campaign vanished the same way. There is no kill
  record in the sudo journal and apport did save the previous day's crash core,
  so it may not have been a signal crash. From lean3 onwards `dpu_main.c` leaves
  a fatal-signal backtrace and an `atexit` trace on stderr. It did not recur
  across seven deployments after lean3
  (`raw/lean2-grpc/dpu-log-died.txt`, `64-matched-clean-1.json.died`).
- `raw/lean2-grpc/65536-capacity-clean-6.json` carries
  `eq_budget_exhausted=1`. The sample is valid, and reps 4–6 were the ones used
  for the A/B.

## Difference from the final tree

The tree was cleaned up after measuring. The driver yield had no effect and was
reverted to `tokio::task::yield_now`, and the now-unused
`dmesh_l7_tx_{try_reserve,reserve,commit,commit_remote}`, `px_conn.l7_tx_chunk`,
`TxAttempt::Accepted` and the `tx_reserve_attempts`/`tx_writer_wakes` metrics were
removed. The rest of lean3 — the pinned maintenance timer, clearing only the PEs
that fired, the single wake-eventfd read, and the exit traces — is unchanged. No
re-measurement was made after that cleanup.

## Method and quality gates

- Same BlueField-3, N/K/A = 32/8/8, client CPUs 4–9, server CPUs 10–17,
  closed loop of 8 threads at concurrency 8, gRPC image `direct-tx`. Every
  deployment was checked by hashing `/proc/<pid>/exe`.
- All 112 samples (99 clean, 9 PMU, 4 profile) are `OK` with fail, drop,
  overflow, worker_fail and reorder 0. Syscall and callchain runs were not used
  in the performance tables.
- Before every L7 arm shut down, the arena read 1,024/1,024 free with 0 live
  chunks, confirmed ten times.
- lean 64 KiB re-measurement (reps 4–6): 13,000/12,920/12,538 RPC/s at CPU/RPC
  595.0/598.4/596.9, matching the main samples.
- Host software gates: dmesh-doca 35/35, adapter 41/41, `l7_abi_contract_test`
  PASS, `cargo check` OK.

Restoration is in [RESTORATION.md](RESTORATION.md); derived data in
[comparison3.csv](comparison3.csv), [syscalls.csv](syscalls.csv),
[pmu-summary.csv](pmu-summary.csv) and [summary.csv](summary.csv); the figure in
[comparison.png](comparison.png). The procedure is `commands.txt`, `campaign.py`,
`run_matrix.sh` and `run_matrix3.sh`.
