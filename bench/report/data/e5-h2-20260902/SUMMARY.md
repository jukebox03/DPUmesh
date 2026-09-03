# E5 — two of the four h2 client connection polls per request (2026-09-02)

Same rig and geometry as [`grpc-professor-20260902/`](../grpc-professor-20260902/EXPERIMENT.md)
(`N/K/A = 32/8/8`, Host 9+9 CPUs, `pin grpcmax`, 64 B frames). Baseline is the
tree's e5-1 build re-measured the same day; the arm under test differs only in
the vendored `h2` at `linkerd/port/linkerd2-proxy/h2/`.

## What a request costs the worker loop

Uprobes on the busy worker thread, one channel at 100 RPS, 10 s windows, idle
maintenance ticks subtracted (`probe-counts-*.txt`, order in
`probe-timeline-base-100rps.txt`):

| per request | e5-1 | patched h2 |
|---|---:|---:|
| input segments (`l7_conn_segment`) | 2.0 | 2.0 |
| publications (`dmesh_l7_tx_commit`) | 2.0 | 2.0 |
| hyper server connection polls | 2.0 | 2.0 |
| h2 client connection polls | **4.0** | **2.0** |
| runtime drains | 21.1 | 20.7 |
| arm / select / clear cycles | 3.5 | 3.3 |
| syscalls | 36.7 | 36.3 |
| context switches | 1.2 | 1.2 |

The timeline names the four client polls. (A) `send_request` queues the
HEADERS; hyper then polls the body pipe eagerly, but `h2` assigns no send
capacity to a stream still in `pending_open`, so the pipe parks and the
connection is polled once for HEADERS and (B) once more, after it opened the
stream and assigned capacity, for DATA+END_STREAM — one publication either way,
because both writes land before the runtime drains. (C) is the response
segment. (D) is `drop_stream_ref`: when the last handle to a closed stream
drops, `h2` wakes the connection so it "can close properly", and the connection
polls for nothing.

## The change

Two edits in the vendored `h2` 0.4.15, each a few lines:

- `send_headers` opens a locally initiated stream on the caller's thread when
  nothing waits in `pending_open` and the concurrency limit allows, exactly as
  `pop_pending_open` would on the connection task. The eager body pipe then
  reserves capacity and the headers and body leave in one connection poll.
- `drop_stream_ref` wakes the connection for a closed, unreferenced stream only
  when the connection may close: no other handle remains, or a GOAWAY was sent
  or received (a `going_away` flag on `Send` and `Recv`).

## A/B (`ab-summary.txt`, medians; raw in `ab-*-raw.csv`)

| point | e5-1 p50 / µs·RPC⁻¹ | patched p50 / µs·RPC⁻¹ | Δ µs/RPC |
|---|---:|---:|---:|
| 1 ch 100 RPS | 1,639 / 680 | 1,596 / 687 | +1.0 % |
| 1 ch 500 RPS | 1,140 / 617 | 1,134 / 603 | −2.2 % |
| 1 ch 1,000 RPS | 990 / 612 | 974 / 597 | −2.5 % |
| 8 ch 10 k RPS | 612 / 410 | 609 / 405 | −1.2 % |
| 8 ch 90 k RPS | 2,881 / 87 (1/1 clean) | 2,419 / 87 (3/3 clean) | −0.2 % |
| closed, 1 in flight | 939 / 666 | 906 / 627 | −5.8 % |

Zero failures, drops or restarts on every point. The two removed polls are
worth 2–6 % of the per-request worker time below 1 k RPS per worker and
nothing at the knee, which is what the timeline predicted: a connection poll
is 15–50 µs there, and the request's time goes to the stack between the polls
(request in → outbound send ≈ 230 µs, response in → server write ≈ 120 µs,
probe-inflated) and to the ~21 runtime passes.

## Retention gates

- 64 B 90 k: 3/3 clean, p99 no worse than the baseline's single run.
- `bash bench/suite/grpc_correctness.sh hardware` on the patched deploy: real-DPU
  shutdown, quiescence and slot reuse passed, the gRPC policy and routing
  surfaces 19/19 ([`policy-route-20260902-233507/`](../policy-route-20260902-233507/stages.csv)),
  zero container restarts, sessions opened equal to closed with nothing
  active, pending or orphaned (`correctness.txt`).

## Method

```sh
BENCH_DEPLOY_SCOPE=grpc DPUMESH_THROUGHPUT_WORKERS=8 BENCH_REACTORS=8 BENCH_NUMA_POLICY=local bash bench/bench.sh deploy
bash bench/bench.sh pin grpcmax
PIN_PROFILE=grpcmax REACTORS_TAG=8 DUR=10 STOP_ON_OVERLOAD=1 \
  CHANNELS=1 THREADS=1 FRAME=64 RATES="100 500 1000" REPS=2 WARMUP=100 bash bench/suite/grpc_conns_sweep.sh --out c1
  CHANNELS=8 THREADS=8 FRAME=64 RATES="10000" REPS=2 WARMUP=1000 bash bench/suite/grpc_conns_sweep.sh --out c8
  THREADS=1 REPS=2 WARMUP=100 bash bench/suite/grpc_closed_sweep.sh --out closed --configs grpc-dpumesh --frames 64 --concs 1
  CHANNELS=8 THREADS=8 FRAME=64 RATES="90000" REPS=3 WARMUP=1000 bash bench/suite/grpc_conns_sweep.sh --out knee
```

Probe counts: `perf probe -x dpumesh_dpu -a name=0x<addr>` with addresses from
`nm` of the running binary (the C entry points by name resolve only by
address too), `perf stat -e 'probe_dpumesh_dpu:*' -t <busy worker tid> --
sleep 10` inside a 30 s `OPEN 48 48 1 30 100 100 const 1` run; the idle
window on the same thread gives the maintenance baseline to subtract. Probes
are removed before any CPU or latency point is taken.
