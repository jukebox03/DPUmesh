# Current Evaluation Deployment

This deployment contract corresponds to [REPORT.md](REPORT.md).

## Topology

```sh
DPUMESH_DPA_THREADS=16
DPUMESH_RINGS_PER_POD=8
DPUMESH_ARM_WORKERS=2
DPUMESH_PROXY_L7_SVC=
DPUMESH_LOG_LEVEL=40
```

The resulting topology is `N/K/A/L=16/8/2/2`. Two polling ARM workers own eight
forward/reverse rings and two RX landing stripes. Service 11 uses
connection-pinned L4.

## Platform

| Item | Value |
|---|---|
| Host | `rapids4`, Intel Xeon Gold 6554S |
| Host kernel | Linux 5.15.0-185 |
| DOCA runtime | 3.1.0-091000 |
| Host governor | performance, approximately 2.5 GHz |
| Pinning | `fair`; one core per primary application |

## Deployment

```sh
DPUMESH_DPA_THREADS=16 \
DPUMESH_RINGS_PER_POD=8 \
DPUMESH_ARM_WORKERS=2 \
DPUMESH_PROXY_L7_SVC= \
DPUMESH_LOG_LEVEL=40 \
./bench/bench.sh deploy
```

`deploy` builds both sides, imports images, restarts the DPU process, starts pods
in registration order, waits for data readiness, and applies CPU pinning.

## Measurement

```sh
./bench/bench.sh point dpumesh 8192 8 32 10 1000 2
./bench/bench.sh armbalance 8192 8 32 10 2 /tmp/dpumesh-arm-final-k8a2.csv
make -j4 test
```

Retained points include topology, pinning, payload sizes, concurrency, duration,
failure count, latency percentiles, and DPU ARM utilization.
