# Current Evaluation Deployment

This is the deployment contract for [REPORT.md](REPORT.md).

## DPU topology

```sh
DPUMESH_DPA_THREADS=16
DPUMESH_RINGS_PER_POD=8
DPUMESH_ARM_WORKERS=8
DPUMESH_PROXY_L7_SVC=16
DPUMESH_LOG_LEVEL=40
```

This selects `N/K/A=16/8/8`. Each ARM data worker owns one DPA consumer PE, a
private connection table, conntrack state, parser/routing state, and one SG-DMA
engine. Worker-owned reverse rings carry `REV_DONE` and `TX_ACK`. The main
thread owns lifecycle control and reverse-ring doorbells. Service 16 uses the
framed L7 codec; other services use connection-pinned L4.

## Host

- Host: `rapids4`, Intel Xeon Gold 6554S, Linux 5.15.0-185.
- DOCA runtime: 3.1.0-091000.
- Host governor: performance, approximately 2.5 GHz.
- `fair` pinning assigns one core to each primary application.
- `hw`, `hw3`, and `hw6` assign additional cores to DPUmesh applications.

## Deployment

```sh
DPUMESH_DPA_THREADS=16 \
DPUMESH_RINGS_PER_POD=8 \
DPUMESH_ARM_WORKERS=8 \
DPUMESH_PROXY_L7_SVC=16 \
DPUMESH_LOG_LEVEL=40 \
./bench/bench.sh deploy
```

`deploy` builds host and DPU artifacts, imports images, restarts the DPU process,
starts all pods, waits for registration readiness, and applies `fair` pinning.

## Verification

```sh
./bench/bench.sh status
./bench/bench.sh dpulog 500
./bench/bench.sh armbalance 8192 8 32 15 16 /tmp/arm-balance.csv
make test
```

Retained measurements record the live DPU environment, N/K/A topology, host
pinning, connection count, per-connection concurrency, failure counters, and
per-thread ARM utilization.
