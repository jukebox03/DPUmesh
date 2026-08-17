# Linkerd output path A/B (2026-08-17)

The adapter publishes a session's output one of two ways. The reservation path
asks DPUmesh for a chunk of the egress arena, copies the endpoint's queued bytes
straight into it and commits (`dmesh_l7_tx_reserve` / `dmesh_l7_tx_commit`). The
copy path drains the endpoint into a temporary buffer and hands that to
`dmesh_l7_send`, which copies it again. `DMESH_L7_TX_RESERVE` selects between
them, which is what makes the one copy between them measurable.

This note answers whether that copy is worth anything on the target ARM cores.

## Method

`bench/suite/l7_tx_ab.sh`. The setting is read when the DPU process starts, so
each arm is its own full deployment of the same tree; the arms ran one after the
other. Both deployments pinned the pods to the same cores (18, 19, 24, 25) with
the governor fixed at 2.5 GHz, so the comparison carries no placement
difference.

- `DPUMESH_DPA_THREADS=32`, `DPUMESH_RINGS_PER_POD=8`, `DPUMESH_ARM_WORKERS=4`,
  `DPUMESH_L7_LINKERD_WORKER=all`, opaque services 11, 13 and 14.
- 1,024-byte request, 8-byte reply, one client thread, ten measured seconds.
- Concurrency 1, 32 and 128; three repetitions each.
- ARM CPU is the DPU process's own tick total across all its threads, over the
  requests the measured window completed. The ticks span the connection setup
  and warmup that precede the window, so the absolute figure sits above one
  taken from the load window alone. Both arms carry the same structure.

Every point completed with `fail=0`, `drops=0`, `reorder=0` and
`worker_fail=0`. After each arm all four workers reported `opened == closed`
with zero active sessions, pending registrations, live tasks and orphaned
registrations, and every L7 fallback, over-release and stray-release counter
stayed at zero. The adapter reported `retries=0 errors=0`, so no reservation was
refused and no output was ever re-queued.

## Result

Delta is the reservation path relative to the copy path. Spread is the range
across the three repetitions as a share of the median.

| concurrency | ARM us/req, reserve | ARM us/req, copy | delta | ranges |
|---:|---:|---:|---:|---|
| 1 | 156.897 | 164.371 | **-4.5%** | 156.6–162.4 vs 159.0–169.7, overlapping |
| 32 | 11.554 | 12.086 | **-4.4%** | 11.35–11.63 vs 11.72–12.31, **disjoint** |
| 128 | 3.791 | 4.056 | **-6.5%** | 3.74–3.85 vs 3.92–4.06, **disjoint** |

| concurrency | Mrps, reserve | Mrps, copy | delta | p50 delta | p99 delta |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.005407 | 0.005208 | +3.8% | -4.6% | -1.7% |
| 32 | 0.043218 | 0.042522 | +1.6% | -0.4% | -0.8% |
| 128 | 0.163616 | 0.160995 | +1.6% | -1.8% | +0.9% |

The reservation path costs less ARM CPU per request at every concurrency, and
the sign is the same on throughput and on both latency percentiles. At
concurrency 32 and 128 the two arms' three-run ranges do not overlap at all —
the slowest reserve run beat the fastest copy run — so those rows separate the
paths rather than merely favouring one. Concurrency 1 has the same sign and the
same size, but its ranges overlap, so it agrees with the other two rather than
standing on its own.

In absolute terms the saving is 0.265 ARM microseconds per request at
concurrency 128 and 0.53 at 32 — the right order for removing one copy of a
1 KB payload together with the temporary allocation that carried it. The share
is larger at 128 because the denominator is smaller, not because the copy got
cheaper. Concurrency 1 shows 7.5 microseconds, which is far more than a 1 KB
copy can account for; its ranges overlap, so that row is not evidence of
anything beyond agreeing in sign.

## What this settles

- Keep the reservation path as the default. It is not a wash: one copy of the
  published bytes is worth about 5% of the ARM cost of a request, rising to 6.5%
  where the fixed per-request cost is amortized.
- The copy path stays as an explicitly selected compatibility and comparison
  instrument. It is not an automatic fallback: if the default reservation path
  cannot borrow an arena chunk, the bytes remain queued for a later driver
  pass. `retries=0` says no reservation was refused during these runs.
- Output copying is a real, measured cost rather than a suspected one, so the
  remaining copy is worth attacking. Linkerd still writes into the endpoint's
  `tx` queue before the adapter copies that queue into the arena. Removing the
  intermediate queue is P5.2, and this note is the evidence that justifies
  starting it.

## What this does not settle

- These are single-service, single-client-thread points. Nothing here speaks to
  many concurrent sessions; `bench/report/REPORT_L7_SESSION_COST.md` measures
  that axis.
- The absolute ARM microseconds include setup and warmup CPU and are not a
  capacity figure. Read them against each other, not against the profile note.
