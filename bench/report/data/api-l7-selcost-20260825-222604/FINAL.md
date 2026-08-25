# O4 — what per-request backend selection costs the data path (2026-08-25)

## Question

Per-request selection added work that runs per segment and per unit:
`l7_conn_segment` names a segment's direction, `px_conn_admitted` scans a
four-entry verdict cache, `struct px_conn` carries that cache. PLAN O4 asks for
its price at two points — a single-backend opaque stream, where the additions
are pure cost, and a protocol-aware stream alternating backends, where the
cache is the saving.

## Build and method

- Tree `9d450b5`, the deploy left by the F8 campaign (DPU data path PID
  3545257). Nothing in the tree changes the data path relative to the published
  `1518aae` receipts: the only datapath-adjacent commit since is the
  fail-closed shim (`92a5d6d`), which is host-shim and webhook code.
- **The verdict cache is in both builds.** `px_conn_admitted` and
  `l7_conn_segment` are present at `1518aae` (5 occurrences in
  `doca/dpu_proxy.c`), so the 2026-08-21 receipts already carry the additions
  and no pre-addition receipt exists on these arms. What this campaign can
  price is the absolute cost of each scenario and the same-build delta between
  them; the additions' own delta against a build without them would need that
  build, which was never measured on these arms.
- `bench/suite/api_l7_cost.sh`, `REQ=1024 CONC=32 REPS=3 PIN=1`; closed loop at
  `THREADS=1` and open loop at 8 K rps `THREADS=4` — the same shapes as
  `api-l7-20260821/cost-closed` and `cost-open8k`.
- Alternating backends: the lb campaign's 50/50 weighted `HTTPRoute`
  (`bench/k8s/policy/httproute-weighted.yaml`) across `echo-grpc-dpumesh` and
  `echo-grpc-alt`, applied only for the `-alt` runs. Harness additions for this
  campaign: `api_l7_cost.sh` charges `echo-grpc-alt` to the gRPC arm's server
  side, and `bench.sh` pins it (cores 12–17 under the `grpc` profile).
- Split proof (`split_*.txt`, cumulative outbound `request_total` by
  destination Pod): across both `-alt` runs the primary served +303,603 and the
  alternate +304,179 — 50.0/50.0. The alternate had served nothing before the
  route was applied.

## Result — ARM µs/request

| scenario | mode | ARM µs/req (3 reps) | Mrps |
|---|---|---|---|
| preload, single-backend opaque | open 8K | 96.5 – 99.1 | 0.008 |
| native, 3-backend opaque | open 8K | 101.7 – 108.5 | 0.008 |
| gRPC, single backend | open 8K | 481.2 – 487.3 | 0.008 |
| gRPC, alternating 50/50 | open 8K | 489.1 – 495.8 | 0.008 |
| preload, single-backend opaque | closed | 13.7 – 14.0 | 0.026 – 0.028 |
| native, 3-backend opaque | closed | 11.2 – 11.4 | 0.037 – 0.042 |
| gRPC, single backend | closed | 443.6 – 467.2 | 0.00227 – 0.00239 |
| gRPC, alternating 50/50 | closed | 491.0 – 513.3 | 0.00206 – 0.00216 |

- **At a matched 8 K rate, alternating backends is free on the ARM.** The
  single-vs-alternating gap is +1.6% (481.6 → 489.3 median), inside this rig's
  several-percent run-to-run spread. With the cache in place, a session that
  spreads its requests over two Services costs the data path what one backend
  costs.
- **The closed-loop gap is an operating point, not a per-request cost.** At its
  own knee the alternating arm completes 9% less and reads +10% µs/req, with
  p50 up ~7% (14.1–15.5 ms vs 13.4–14.1 ms): the route hop and the second
  session channel add latency, and a concurrency-32 closed loop turns latency
  into throughput. The matched-rate rows are the cost comparison.
- **The single-backend opaque price carries no visible scan cost.** Preload
  open-8K reproduces the published `1518aae` receipt (96.5–99.1 now vs 99.6
  then), native likewise (101.7–108.5 vs 102.7), gRPC slightly better (481–487
  vs 496, the D1 benchmark-server replacement sits between the builds). Since
  both builds carry the four-entry scan, this is reproduction, not an ablation
  — but nothing about the price moved.
- Host-side context: the alternating arm's server host cost rises (0.81–0.95 →
  1.03–1.15 cores) because two gRPC server processes each run their own
  runtime at half the load — an application property, not a mesh one.

## Files

- `cost-closed/`, `cost-open8k/` — three single-backend arms.
- `cost-closed-alt/`, `cost-open8k-alt/` — gRPC under the 50/50 route.
- `split_before.txt`, `split_mid.txt`, `split_after.txt` — per-Pod cumulative
  request counts before the route, after the closed run, after the open run.
