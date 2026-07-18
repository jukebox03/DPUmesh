# bench/ — DPUmesh benchmark & evaluation

One workload — a greeter RPC (N-byte request → 8-byte reply, 16-byte `apps/bench.h` frame)
— driven over **DPUmesh**, a **TCP + Envoy** sidecar mesh, and **direct kernel TCP** with the
*identical* C methodology, so a measured gap is the transport, not the runtime (above conc=1 the
baselines batch and DPUmesh can too — see the matched-batching ablation). **Findings, reproducible:
[report/REPORT.md](report/REPORT.md).**

## Layout

```
bench.sh            single entry point: deploy + drive benchmarks + validators
apps/               workload sources (share apps/bench.h — wire frame + histogram)
  bench_dpumesh.c / echo_dpumesh.c   DPUmesh native-API client / server  → bench-/echo-dpumesh
  bench_sock.c     / echo_sock.c     matched-C TCP client / server        → bench-/echo-tcp
suite/              staged evaluation: run_suite.sh · cpu_probe.sh · npod.sh (N-pod amortization)
                    · batch_ablation.sh / batch_cpu.sh (matched-batching) · analyze.py
                    · plot.py / plot_batch.py · STAGES.md (the plan)
report/             deliverables: REPORT.md · DEPLOY.md · figures/ · data/
validators/         feature-correctness tests (loopback / verbs / stream / preload)
docker/  k8s/       Dockerfiles · pods.yaml manifest
RESULT.md           engineering measurement log (historical)
```

## Workload & wire frame

- **Greeter SayHello**, asymmetric: `req_size`-byte request → fixed small `reply_size`-byte
  reply (default 8). Throughput counts request bytes.
- **Wire frame** (`apps/bench.h`, little-endian 16-byte header + payload):
  `magic | seq | payload_len | aux`. The client correlates replies **by seq** — replies come
  out of order under a codec, so arrival order is not assumed.
- **A RUN is closed-loop**: each thread keeps `concurrency` requests outstanding on one
  connection; the first `warmup` completions are excluded; per-thread rates are summed and
  latencies come from a merged histogram. `bench_sock` also supports **OPEN** (open-loop,
  offered-rate; generator-limited above ~15% load — see REPORT.md caveats).
- **Routing observability**: the `OK` line's `dist=pod:count` + `reorder` show the LB
  granularity — one pod & `reorder=0` = connection pinned (no codec); even spread &
  `reorder>0` = per-message load balancing (codec `_L7_SVC`/`_FRAME_SVC`).

## Run

```bash
# 1. build host+DPU, images, DPU, pods, pin  (the ONLY bring-up path; needs .env)
DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 DPUMESH_RINGS_PER_POD=2 \
  bench/bench.sh deploy

# 2. ablation + headline: 3 transports (dpumesh-native / tcp-envoy / tcp-direct) → summary+figures+meta
OUT=/tmp/out REPS=8 DUR=15 bench/suite/run_suite.sh conc rtt bw

# 3. matched-batching fairness ablation + DPU-ARM cost (fresh (4,4) headline experiment)
bench/suite/batch_ablation.sh          # -> data/batch_ablation.csv (4-way ablation, conc sweep)
bench/suite/batch_cpu.sh               # -> data/batch_cpu.csv (ARM µs/RPC, unbatched vs batched)

# 4. host + DPU-ARM CPU  (threads=1 = one backend = fair)
bench/suite/cpu_probe.sh  dpumesh|tcp|direct  1024 8 32 30 1  data/cpu.csv

# 5. N-pod amortization (N client pods through the shared DPU); repeat after deploying each DPU config
bench/suite/npod.sh 4-4 32 20 3 data/npod.csv

# 6. busy-app: set /tmp/app_work_us on echo-dpumesh{,-13,-14}+echo-tcp via kubectl exec, then RUN
# 7. plot + read
bench/suite/plot_batch.py data report/figures                                        # batching figures
bench/suite/plot.py data/summary.csv report/figures data/cpu.csv data/busy.csv data/npod.csv
bench/report/REPORT.md
```

`bench.sh` also has `latency|bandwidth|rate|point` (quick runs) and
`loopback|verbs|stream|preload` (validators); run `bench/bench.sh` for full usage. Deploy env
and every DPU config knob are in **[report/DEPLOY.md](report/DEPLOY.md)**.
