# Deployment — environment, config knobs, and topology

Everything needed to reproduce the measured environment. `bench/bench.sh deploy` is the
ONE bring-up path: it builds the host library, the DPU firmware, the container images,
applies the k8s manifest, starts the DPU process, brings up every pod in order, and pins
them to cores. There is deliberately no per-pod start / DPU-only restart (either
desynchronizes host↔DPU registration).

## Reproduce this evaluation

```bash
# from repo root, with .env present (see below)
DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 DPUMESH_RINGS_PER_POD=2 bash bench/bench.sh deploy

# then, against the live deploy:
OUT=/tmp/out bash bench/suite/run_suite.sh conc rtt bw    # ablation + headline (3 transports)
bash bench/suite/cpu_probe.sh dpumesh|tcp|direct 1024 8 32 30 1 data/cpu.csv   # host+DPU CPU
bash bench/suite/npod.sh 4-4 32 20 3 data/npod.csv        # N-pod amortization
```

That reproduces the **(4,4) operating point**. The DPU-config frontier repeats the deploy +
`npod.sh` at `2 2` and `1 1`. The **busy-app** sweep sets `/tmp/app_work_us` on the echo pods at
runtime (`kubectl exec … -- sh -c 'echo <µs> >/tmp/app_work_us'`; the echo re-reads it every
0.5 s), so it needs no redeploy. The three knobs above are the only ones set explicitly.

## Provenance is captured live, not asserted

`run_suite.sh` freezes `data/meta.txt` at the start of every run by reading the **actual
running system**, not this shell's environment:

- the DPU operating point from the live `/proc/<dpumesh_dpu>/environ` (proves (4,4)),
  plus the process uptime and binary path;
- the **container-image digests** loaded in containerd (the exact host binaries running);
- node, kernel, kubectl version, namespace, and the run params (`DUR/WARMUP/REPS`).

This is the fix for a prior report that *claimed* (4,4) but recorded only the invoking
shell's (unset) env. If `.env` is absent the DPU config is explicitly marked *not
captured* rather than silently omitted.

## DPU config knobs (data-plane operating point)

Read live from the running process at `/proc/<dpumesh_dpu-pid>/environ`.

| knob | value here | meaning |
|---|---|---|
| `DPUMESH_INGEST_SHARDS` | **4** | parallel ingest (parse/route) shards on the ARM |
| `DPUMESH_ARM_EGRESS_THREADS` | **4** | parallel egress (send) threads on the ARM |
| `DPUMESH_RINGS_PER_POD` | **2** | DPA forward rings per meshed pod |
| `DPUMESH_SHARD_SHARED` | *(unset → 0)* | share-nothing shards (each shard owns its state) |
| `DPUMESH_PROXY` | *(unset)* | passthrough — L4 byte stream, conn pinned to one backend (no per-message codec) |
| `DPUMESH_INGEST_REAP` | *(unset)* | DPA-reap not split off the main funnel |
| `DPUMESH_LOG_LEVEL` | 40 | DOCA log level (warn) |

DPU launch (observed in `meta.txt`): `./dpumesh_dpu <dpu_pci> -l 40`.

## Host / manifest values (set by `bench.sh apply_manifest`)

| var | value | applies to | note |
|---|---|---|---|
| `ASYNC_THREADS` | 4 | DPUmesh client async poll threads | |
| `BENCH_PIPELINE` | 8 | DPUmesh client pipeline depth | |
| `BENCH_COALESCE` | 0 | client TX coalescing off | |
| `DPUMESH_ARENA_SLOTS` | 512 | RX arena slots per pod | |
| `ECHO_THREADS` | 3 | *(legacy)* | **not consumed by the current C servers** — see below |

**Server concurrency model (important for fairness).** `ECHO_THREADS` is a leftover
manifest var; neither greeter server reads it. `echo_dpumesh` is a **single CQ / epoll
event loop** (transport offloaded to the DPU). `echo_sock` is **thread-per-connection**
(one thread per accepted socket). The load generators hold one long-lived connection per
client thread, so at the headline **threads=1** each server serves exactly one connection
on one host core — a like-for-like 1-backend-vs-1-backend comparison.

## `.env` (repo root, required for deploy/pin — NOT committed)

Keys only (values are secrets / site-specific):
`DPU_HOST`, `HOST_PASS`, `DPU_PASS`, `HOST_PCI`, `DPU_PCI`.
Benchmark/validator RUNs need only `kubectl` + `nc`; deploy/pin and the provenance freeze
need `.env`.

## Pod topology (`bench/k8s/pods.yaml`, namespace `test-bench`)

```
DPUmesh path (transport on the DPU, app owns its whole core):
  bench-dpumesh ─ dmesh.h ─►  DPU (DOCA Comch + DMA)  ─►  echo-dpumesh
     (+ bench-dpumesh-2/-3 clients & echo-dpumesh-13/-14 backends: the N-pod amortization sweep)

TCP+Envoy path (matched-C client; app shares its core with an Envoy sidecar):
  bench-tcp ─► sidecar1(Envoy) ─ k8s svc ─► sidecar2(Envoy) ─► echo-tcp[echo_sock]

TCP-direct path (ablation — isolates the sidecar tax; reuses echo-tcp's echo_sock):
  bench-direct ─ k8s svc echo-direct:9092 ─► echo-tcp[echo_sock]   (no Envoy)
```

Both clients and both servers are **pure C** (`bench_sock`/`echo_sock`,
`bench_dpumesh`/`echo_dpumesh`) sharing the same 16-byte wire frame (`bench.h`) — the
transport is the only difference. The former Go baseline was removed (it was the source of
the retracted "2× DPUmesh" result). `echo-dpumesh-13/-14` are extra backends of the same
service; a single-connection (threads=1) run pins to exactly one of them, so they engage
only in the multi-thread and fair-core sweeps.

Validator pods (feature correctness, not perf): `loopback-dpumesh`, `verbs-dpumesh`,
`stream-dpumesh`, `preload-dpumesh`.

## CPU pinning (`bench.sh pin fair` — the apples-to-apples 1-core comparison)

`performance` governor, fixed 2.5 GHz on cores 0–7. One host core per app:

| pod | core(s) |
|---|---|
| bench-dpumesh (client) | 0 |
| echo-dpumesh (greeter) | 1 |
| bench-tcp (client + sidecar1) | 2 |
| echo-tcp (greeter + sidecar2) | 3 |
| echo-dpumesh-13 / -14 (extra backends) | 6 / 7 |
| bench-direct (tcp-direct client, no sidecar) | 8 |
| bench-dpumesh-2 / -3 (N-pod clients) | 9 / 10 |
| validators | 4,5 |

The DPUmesh app gets a full core because its transport runs on the DPU; the TCP app shares
its single core with its Envoy sidecar — that contention is the sidecar tax the CPU
accounting measures. The `fair_cores.sh` sweep re-pins both clients to a symmetric B-core
budget (cores 8–15) and restores `fair` when done.

## Images (containerd `k8s.io` namespace)

`bench/{bench-dpumesh,echo-dpumesh,bench-tcp,echo-tcp,loopback-dpumesh,stream-dpumesh,verbs-dpumesh,preload-dpumesh}:latest`
+ `envoyproxy/envoy:v1.30-latest`. The `bench-tcp`/`echo-tcp` images build from
`docker/bench_sock.Dockerfile` / `docker/echo_sock.Dockerfile` (pure C). Exact digests for
each run are frozen in `data/meta.txt`.
