# Benchmark Deployment

This document defines the environment represented by retained benchmark results.
`bench/bench.sh deploy` is the reproducible fresh bring-up path: it builds both
sides, synchronizes DPU sources, creates and imports images, starts a fresh DPU
process, starts pods in registration order, waits for DPUmesh data readiness, and
pins processes to CPUs.

## Required local configuration

Create an uncommitted `.env` at the repository root with these site-specific
keys:

```text
DPU_HOST     SSH destination for the BlueField ARM
HOST_PASS    sudo password on the Kubernetes host
DPU_PASS     sudo password on the BlueField ARM
HOST_PCI     host-side DOCA PCI address
DPU_PCI      DPU-side DOCA PCI address
```

Deploy and CPU pinning require these values. Read-only benchmark execution needs
only working `kubectl` access and the live deployment. Never commit `.env`.

## Bring-up

```sh
DPUMESH_INGEST_SHARDS=4 \
DPUMESH_ARM_EGRESS_THREADS=4 \
DPUMESH_RINGS_PER_POD=2 \
DPUMESH_LOG_LEVEL=40 \
./bench/bench.sh deploy
```

The command stops the prior DPU process and recreates the complete registration
state. A DPU-only restart invalidates all host channels and requires a complete
pod restart. A controlled host-image A/B may retain the DPU process only when the
old pod exits through `POD_QUIESCED` and the replacement reaches a fresh
`POD_INIT_RESULT(READY)`; record that exception explicitly. Kubernetes `Ready`
alone is never sufficient.

## DPU operating point

The live DPU process is authoritative. Capture both
`/proc/<dpumesh_dpu-pid>/environ` and its command line, not the invoking shell.
The environment holds the shard/ring settings, while warning log level appears
as `-l 40` in the command line. A bare `deploy` uses the implementation defaults
of one ingest shard and one egress worker, so omitting the explicit `(4,4)`
variables changes the measured system.

| Setting | Reported L4 headline value | Meaning |
|---|---:|---|
| `DPUMESH_INGEST_SHARDS` | 4 | ARM ingest/routing shards |
| `DPUMESH_ARM_EGRESS_THREADS` | 4 | ARM SG-DMA egress workers |
| `DPUMESH_RINGS_PER_POD` | 2 | Forward rings per pod |
| `DPUMESH_SHARD_SHARED` | unset/0 | Share-nothing shard routing |
| `DPUMESH_PROXY` | unset | L4 passthrough |
| `DPUMESH_INGEST_REAP` | unset | Inline default unless shards require reapers |
| process argument `-l` | 40 | Warning-level DOCA logging |

N DPA EUs are auto-detected after DPA startup and clamped to eight unless
`DPUMESH_DPA_THREADS` is explicitly set. Host and DPU must use the same K. The
2026-07 hardware reported more EUs than the cap, so the effective automatic N was
eight in the gRPC lifecycle runs.

The 4/4 ARM configuration is the historical L4 headline operating point, not a
universal recommendation. The report keeps 1/1, 2/2, and 4/4 results separate.

## Host manifest values

The current host programs consume these rendered transport values:

| Variable | Default | Consumer |
|---|---:|---|
| `DPUMESH_RINGS_PER_POD` | 2 | Host native core; must match DPU K |
| `DMESH_PRELOAD_DEBUG` | 0 | POSIX preload diagnostics |

The manifest still renders `ASYNC_THREADS`, `BENCH_PIPELINE`,
`BENCH_COALESCE`, `ECHO_THREADS`, and `DPUMESH_ARENA_SLOTS` for compatibility
with older images. The current C benchmark, echo, and validator programs do not
read them. Workload concurrency, threads, reconnect cadence, validator window,
and pipeline are command arguments. ABI 2 batching is mandatory and has no
environment toggle.

`echo_dpumesh` is a single-CQ event loop. `echo_sock` uses a thread per accepted
socket. At the headline one-client-thread point, each server handles one
long-lived connection on one assigned application core.

## Kubernetes topology

All resources run in namespace `test-bench`.

```text
DPUmesh:
  bench-dpumesh ─ native API ─ BlueField ─ backend TCP ─ echo-dpumesh
       ├─ bench-dpumesh-2/-3                  ├─ echo-dpumesh-13/-14

TCP + Envoy:
  bench-tcp/bench_sock ─ Envoy tcp_proxy ─ Service ─ Envoy ─ echo-tcp/echo_sock

Direct TCP:
  bench-direct/bench_sock ─ echo-direct Service ─ echo-tcp/echo_sock

Validators:
  loopback-dpumesh, verbs-dpumesh, stream-dpumesh, preload-dpumesh
```

The TCP paths use the same pure-C `bench_sock`/`echo_sock` wire workload. The
native path uses the equivalent DPUmesh benchmark semantics. Extra DPUmesh
clients and backends are idle in the basic single-pair measurement and are used
only by explicit multi-pod or multi-connection runs.

The gRPC benchmark reuses a DPUmesh client pod and an independently named DPUmesh
server registration so it does not conflict with the resident L4 echo service.
For its TCP comparator, the same gRPC binary listens on an unoccupied pod TCP port
and the client targets that server pod IP.

## CPU pinning

`./bench/bench.sh pin fair` assigns one application core per primary pod:

| Pod | Core(s) |
|---|---|
| `bench-dpumesh` | 0 |
| `echo-dpumesh` | 1 |
| `bench-tcp` including sidecar | 2 |
| `echo-tcp` including sidecar | 3 |
| validators | 4,5 |
| `echo-dpumesh-13`, `echo-dpumesh-14` | 6, 7 |
| `bench-direct` | 8 |
| `bench-dpumesh-2`, `bench-dpumesh-3` | 9, 10 |

When available, the harness selects the performance governor and fixes cores 0–7
at 2.5 GHz. Failure to apply the frequency setting is nonfatal and must be noted
in provenance. `hw`, `hw3`, and `hw6` grant additional cores only to the DPUmesh
side and therefore are ceiling probes, not fair TCP comparisons.

## Image and run provenance

The deploy imports repository images into the containerd `k8s.io` namespace and
uses `envoyproxy/envoy:v1.30-latest`. A retained performance run must record:

- git commit and dirty-tree state;
- built binary hash and container image digests;
- node, kernel, Kubernetes context, and pod placement;
- DPU binary path, PID/uptime, launch environment, N/K, and log level;
- benchmark payload, concurrency, warmup, duration, repetitions, and pin profile.

`bench/suite/run_suite.sh` captures live metadata in its output directory. If it
cannot read DPU state, the result is marked uncaptured rather than inferring it
from shell variables.

After an image rollout, point commands select a Running pod with no deletion
timestamp. This prevents a terminating old replica from being chosen merely
because it appears first in the Kubernetes item list.

## Inspection and cleanup

```sh
./bench/bench.sh status
./bench/bench.sh logs
./bench/bench.sh dpulog 500
./bench/bench.sh dpucpu
./bench/bench.sh cleanup
```

Performance runs use log level 40. Higher-volume diagnostics change CPU and I/O
behavior and are not comparable to warning-level results.
