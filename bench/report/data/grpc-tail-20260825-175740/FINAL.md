# D2 final receipt — gRPC tail regime re-measurement, 2026-08-25

## Question

PLAN.md D2 recorded a p99 regime change above ~12.5 K rps with a flat p50 and a
~15 ms periodic stall, measured before the tree held any receipt for it. The
figures came from the synchronous benchmark server that D1 replaced on
2026-08-24 (`grpc-d1-d3-20260824/`). This campaign re-measures the claim on the
current build, with the repeat count the sporadic form requires (the earlier
1-in-3 to 1-in-12 occurrence rate is why a single quiet run proves nothing).

## Build under test

- Deploy: 2026-08-25 `bench.sh deploy` (default), DPU topology `N/K/A=32/8/8`,
  data-path PID `3252634`.
- Client Pod `bench-grpc-dpumesh-646d5dcc88-pfzst`, image
  `sha256:3a834f6a66756815db1550d3c5a20badcb05533b59f3020bd6b4c5734106cdea`.
- Server Pod `echo-grpc-dpumesh-84cbd4d65d-pjt8l`, image
  `sha256:69a2c4b69e339ac5655d84f9f679e1c4132214dc29000a954dd78e8fa905315a`.
- CPU profile `grpc` (client cores 18-23, server 24-29), applied before the
  sweep. Restart count 0 on both Pods before and after.

## Campaign

`bench/suite/grpc_conns_sweep.sh` — eight channels, 64 B frames, eight client
threads, open-loop const rate, 10 s per run:

    rates 8000 10000 12000 13000 14000 16000 20000 24000 × 5 repeats = 40 runs

All 40 completed; zero recoveries, zero crash rows, and the DPU log carries
zero `px_dma_err` / `batch failed` / `table full` / `ERR` lines across the
window.

## Judgment criteria, fixed before the data existed

`d2_judge.py` (this directory; output in `judgment.txt`):

- Sustained run: achieved/offered ≥ 0.99, `fail=0`, `drops=0`.
- Stall fingerprint: a sustained run whose p99 is ≥ 5× the rate's median while
  its p50 stays within 1.5× of the median — or p99 ≥ 50 ms under a p50 < 5 ms.
- p50 and p99 rising together is queueing, not the defect.

## Result — NOT REPRODUCED

| rate | sustained | p50 med (µs) | p99 med (µs) | p99 max (µs) | p999 max (µs) |
|---:|---:|---:|---:|---:|---:|
| 8,000 | 5/5 | 838 | 1,633 | 1,716 | 9,094 |
| 10,000 | 5/5 | 806 | 1,636 | 1,648 | 6,886 |
| 12,000 | 5/5 | 778 | 1,521 | 1,642 | 15,374 |
| 13,000 | 5/5 | 800 | 1,532 | 1,670 | 22,218 |
| 14,000 | 5/5 | 818 | 1,570 | 1,747 | 17,692 |
| 16,000 | 5/5 | 971 | 1,846 | 1,867 | 3,480 |
| 20,000 | 4/5 | 1,172 | 2,699 | 2,803 | 4,851 |
| 24,000 | 5/5 | 1,949 | 7,222 | 7,303 | 11,216 |

No sustained run shows a p99 jump against a flat p50 at any rate, including the
12-14 K band the original threshold sat in. p50 and p99 move together toward
the knee — the 24 K point costs 2.4× the 8 K p50 and 4.4× its p99 — which is
queueing shape, not stall shape.

Two observations recorded, not analyzed:

- 20 K repeat 2 under-delivered (achieved exactly 15,000, ratio 0.75) with
  clean latency (p99 5.0 ms, `fail=0`, `drops=0`). Under-delivery is excluded
  from sustained by the criteria; it is not the stall fingerprint.
- The residual tail lives at the p999 level: sporadic 9-22 ms outliers at
  rates ≤ 14 K, roughly 0.1 % of requests. The original defect was defined at
  the p99 level (1-3 % of requests parked per stall window); nothing at that
  level survives.
