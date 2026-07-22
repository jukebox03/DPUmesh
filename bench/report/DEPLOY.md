# Current Evaluation Deployment

This is the deployment contract for [REPORT.md](REPORT.md). The configuration is
the maximum-throughput operating point established by the current implementation,
not the resource-minimal default.

## DPU data plane

```text
DPUMESH_PROXY_L7_SVC=
DPUMESH_INGEST_SHARDS=4
DPUMESH_ARM_EGRESS_THREADS=4
DPUMESH_RINGS_PER_POD=2
DPUMESH_LOG_LEVEL=40
```

The live startup selected eight DPA EUs from 254 available, used four
share-nothing ingest shards with a dedicated reaper, four SG-DMA egress workers,
and two forward rings per pod. L7 services were zero; native traffic used
connection-pinned L4 passthrough. The main loop and ingest shards were
event-driven.

The running BlueField binary SHA-256 was:

```text
d2e9ea56051271814265455eddf724b46072388444ab8e9e09d8498db7ce0d77
```

## Host and paths

- Host: `rapids4`, Intel Xeon Gold 6554S, Linux 5.15.0-185.
- DOCA runtime: 3.1.0-091000; DPU log level: warning.
- Host governor: performance, approximately 2.5 GHz.
- `bench-dpumesh` and its active backend: cores 0 and 1.
- additional DPUmesh backends: cores 6 and 7.
- `bench-tcp` plus client Envoy: core 2.
- `echo-tcp` plus server Envoy: core 3.
- `bench-direct`: core 8; it targets `echo_sock` directly on core 3.

Envoy is `envoyproxy/envoy:v1.30-latest`. The measured Envoy path crosses both
sidecars. Direct TCP bypasses both sidecars but reuses the same `echo_sock`
server.

The client sidecar listens on `0.0.0.0:9091` and uses only
`envoy.filters.network.tcp_proxy`. Its `STRICT_DNS` upstream is
`echo-tcp:9091`. The server sidecar uses the same filter and forwards to
`127.0.0.1:9092`; both clusters use a 5-second connect timeout. The configuration
contains no HTTP/L7 filter, TLS, retry policy, access log, telemetry filter, or
dynamic control plane. Each sidecar accepts a downstream socket, establishes a
separate upstream socket, and moves opaque bytes between them under Envoy's
event loop and flow control. The benchmark reuses a long-lived connection, so
the retained steady-state data primarily measures forwarding rather than
connection setup.

## Measurement matrix

- RTT: request 64 B, 256 B, 1 KiB, 8 KiB, 64 KiB, 1 MiB; 8 B reply; concurrency 1.
- Scaling: 1 KiB request and reply; concurrency 1–64.
- Goodput: request 64 B, 1 KiB, 8 KiB, 64 KiB, 512 KiB, 1 MiB; 8 B reply; concurrency 32.
- CPU: request 64 B, 1 KiB, 8 KiB, 64 KiB, 1 MiB; 8 B reply; concurrency 32.
- Five 10-second repetitions per point after warmup; one thread and one connection.

The transport order rotates by repetition. A result is accepted only when all
three paths contain the same points and every failure, drop, overflow, and
reorder count is zero.

## Reproduction

```sh
DPUMESH_PROXY_L7_SVC= \
DPUMESH_INGEST_SHARDS=4 \
DPUMESH_ARM_EGRESS_THREADS=4 \
DPUMESH_RINGS_PER_POD=2 \
DPUMESH_LOG_LEVEL=40 \
./bench/bench.sh deploy

./bench/suite/current_l4.sh
```

The campaign writes only the current artifacts:

- `data/tidy.csv`: one row per main repetition.
- `data/summary.csv`: medians and 95% bootstrap intervals.
- `data/cpu.csv` and `data/cpu_summary.csv`: host and DPU ARM accounting.
- `data/meta.txt` and `data/dpu_startup.txt`: live provenance.
- `figures/`: four detailed figures and one three-panel size-comparison figure regenerated
  from the aggregate CSV files. The retained meeting figures are not part of the
  current report-generation path.
