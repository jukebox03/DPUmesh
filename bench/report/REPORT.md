# DPUmesh L4 Evaluation

Four L4 paths carry the same request/response workload under one client core and
one server core each: Envoy as a plaintext TCP proxy, Envoy with mutual TLS on
the inter-pod leg, DPUmesh through the POSIX preload shim, and DPUmesh through
its native API. Two measurements describe them — the host CPU each consumes at a
shared offered load, and the throughput each sustains once a host core saturates.

## Host CPU at matched load

![Host cores at matched load](figures/01_host_cpu_by_load.png)

Host cores consumed by the client and server cores together, at loads every
configuration serves cleanly:

| Frame | Offered/s | Envoy permissive | Envoy strict | DPUmesh preload | DPUmesh native |
|---|---:|---:|---:|---:|---:|
| 64 B | 435,986 | 1.784 | 1.933 | 1.737 | **0.968** |
| 64 B | 871,973 | 1.893 | 1.962 | 1.544 | **1.003** |
| 64 B | 1,307,959 | 1.662 | 1.900 | 1.747 | **1.051** |
| 1 KiB | 88,741 | 1.976 | 1.975 | 1.414 | **0.835** |
| 1 KiB | 177,483 | 1.970 | 1.975 | 1.670 | **0.922** |
| 1 KiB | 266,224 | 1.967 | 1.871 | 1.740 | **0.896** |
| 8 KiB | 13,853 | 1.043 | 1.171 | 0.273 | **0.175** |
| 8 KiB | 27,707 | 1.893 | 1.955 | 0.563 | **0.342** |
| 8 KiB | 41,560 | 1.956 | 1.976 | 0.860 | **0.515** |

Both Envoy paths sit against the two-core budget from the lowest load measured
and stay there, so the remaining budget cannot absorb more traffic. Native serves
the same loads on 0.18–1.05 core, leaving 1.0–1.8 core free.

The gap at any single load is not a fixed efficiency ratio. Envoy's host cost per
message falls steeply with load — 75.3 µs at 13.9 k, 68.3 µs at 27.7 k, 47.1 µs
at 41.6 k, 12.5 µs at its 157 k ceiling — while native holds 12.4 µs across the
whole range. The ratio at a matched load therefore measures how far Envoy is from
its own efficient operating point, and it closes as load rises.

## Throughput at host-core saturation

![Sustained throughput at host-core saturation](figures/02_fixed_budget_throughput.png)

The highest offered rate each configuration serves cleanly, with the endpoint
cores observed there:

| Frame | Configuration | Sustained RPC/s | vs permissive | Client | Server | p99 |
|---|---|---:|---:|---:|---:|---:|
| 64 B | Envoy permissive | 2,059,736 | 1.00× | 0.991 | 0.666 | 1.29 ms |
| 64 B | Envoy strict | 2,048,601 | 0.99× | 0.990 | 0.686 | 1.58 ms |
| 64 B | DPUmesh preload | 5,097,004 | 2.47× | 0.991 | 0.743 | 0.49 ms |
| 64 B | DPUmesh native | **8,553,806** | **4.15×** | 0.991 | 0.721 | 1.51 ms |
| 1 KiB | Envoy permissive | 756,875 | 1.00× | 0.990 | 0.888 | 1.74 ms |
| 1 KiB | Envoy strict | 441,916 | 0.58× | 0.989 | 0.964 | 2.70 ms |
| 1 KiB | DPUmesh preload | 550,010 | 0.73× | 0.990 | 0.777 | 0.36 ms |
| 1 KiB | DPUmesh native | **1,109,843** | **1.47×** | 0.984 | 0.381 | 1.45 ms |
| 8 KiB | Envoy permissive | 157,050 | 1.00× | 0.987 | 0.981 | 8.09 ms |
| 8 KiB | Envoy strict | 70,027 | 0.45× | 0.986 | 0.978 | 6.91 ms |
| 8 KiB | DPUmesh preload | 144,990 | 0.92× | 0.991 | 0.958 | 0.39 ms |
| 8 KiB | DPUmesh native | **169,999** | **1.08×** | 0.944 | 0.438 | 0.75 ms |

Every configuration reaches its ceiling with the client core at 0.94–0.99, so
each number is a host-core capacity under the fixed budget. Native leads at every
frame size and by the widest margin at 64 B, where per-message overhead dominates
and Envoy spends a full core on 2.06 M messages while native spends the same core
on 8.55 M.

The two Envoy paths are indistinguishable at 64 B — 2.06 M against 2.05 M —
because the sidecar's per-connection TCP work, not the cipher, sets the limit
there. They separate as frames grow and encryption cost follows the byte count:
strict falls to 0.58× of permissive at 1 KiB and 0.45× at 8 KiB.

## Why the advantage narrows with frame size

Host cost per message at each configuration's own ceiling:

| Frame | Envoy permissive | DPUmesh native | Ratio |
|---|---:|---:|---:|
| 64 B | 962 ns | 232 ns | 4.15× |
| 1 KiB | 2,616 ns | 1,774 ns | 1.47× |
| 8 KiB | 12,530 ns | 12,400 ns | 1.08× |

What DPUmesh removes from the host — the sidecar hop, the socket calls, the
stack traversal — is per-message work of roughly fixed size. What remains is
per-byte work the application performs on its own payload, and that grows with
the frame. At 8 KiB both paths converge on about 1.3–1.5 GB/s per client core,
so the fixed saving is a small share of a much larger total.

Envoy's own cost per message is strongly load-dependent, because a byte-stream
socket returns whatever has queued since the last read and a busier connection
places more messages in each read and each event. Nothing configures this:
`TCP_NODELAY` is set on both endpoints, and socket buffers and Envoy buffer
limits are left at their defaults. Native is close to flat across the same range.

## Library batching validation

The current native and preload paths were deployed and measured on 2026-08-04
with the report topology, `l4` pin profile, offered rates, and frame sizes. Each
current value below is the median of three runs.

| Path | Frame | Reference achieved/s | Current achieved/s | Delta | Reference p99 | Current p99 | Current p99 range |
|---|---:|---:|---:|---:|---:|---:|---:|
| Native | 64 B | 8,553,806 | 8,541,252 | -0.15% | 1,506 µs | 1,929 µs | 1,572–6,720 µs |
| Native | 1 KiB | 1,109,843 | 1,109,990 | +0.01% | 1,454 µs | 710 µs | 544–912 µs |
| Native | 8 KiB | 169,999 | 169,860 | -0.08% | 753 µs | 834 µs | 691–3,190 µs |
| Preload | 64 B | 5,097,004 | 5,076,405 | -0.40% | 488 µs | 618 µs | 611–623 µs |
| Preload | 1 KiB | 550,010 | 549,998 | -0.002% | 363 µs | 398 µs | 377–406 µs |
| Preload | 8 KiB | 144,990 | 144,996 | +0.004% | 386 µs | 397 µs | 397–397 µs |

Achieved throughput remains within 0.5% of the reference in every row. All runs
had `fail=0` and `reorder=0`, and every p99 remained below the report's 10 ms
limit. The admission-drop limit of 0.1% was exceeded by native 64 B repetition
2, native 8 KiB repetition 3, and all three preload 64 B repetitions. Thus the
throughput and latency comparison shows no material regression, while the exact
all-repetitions clean criterion is not satisfied at those offered-rate points.

Run-level histogram output is stored in
[`data/batching-20260804/latency_runs.csv`](data/batching-20260804/latency_runs.csv).
It contains p50, p95, p99, p99.9, and p99.99. The reference
[`measurements.csv`](data/l4-final-20260729/measurements.csv) contains p99 but no
per-request samples or other latency percentiles, so only p99 can be compared
directly.

## Contract

| Axis | Value |
|---|---|
| Configurations | Envoy permissive TCP, Envoy strict mTLS, DPUmesh preload, DPUmesh native |
| Frames | symmetric request/response: 64 B, 1 KiB, 8 KiB |
| Frame format | 16 B benchmark header plus 48 B, 1008 B or 8176 B body |
| Load | constant-rate open loop; 8 persistent connections; 10 s per rate |
| Host budget | one exclusive client core and one exclusive server core per configuration |
| Core placement | cores 18–25, NUMA node 1, SMT disabled, 2.5 GHz performance governor |
| Backend | exactly one ready server per configuration |
| DPU | `N/K/A=32/8/8`; L7 disabled |
| Platform | `rapids4`, Intel Xeon Gold 6554S, Linux 5.15.0-186-generic |
| Software | Envoy `v1.30-latest`; swap disabled |

Envoy permissive, Envoy strict and DPUmesh preload run byte-identical
`bench_sock` and `echo_sock`. Native runs `bench_dpumesh` and `echo_dpumesh`.
Envoy strict authenticates and encrypts the inter-pod leg; DPUmesh does not, and
the two are not security-equivalent.

A rate counts as clean when achieved/offered ≥ 0.98, admission drops ≤ 0.1% of
scheduled, and p99 ≤ 10 ms. The endpoint core budget is validated before
measurement: each configuration holds two exclusive cores, eight in total.

DPUmesh spends DPU ARM cores that Envoy does not. The comparison above is of
host CPU under a fixed host budget.

## Reproduction

Create `.env` with `HOST_PASS`, `DPU_HOST` and `DPU_PASS`, then from the
repository root:

```sh
sudo swapoff -a

env DPUMESH_DPA_THREADS=32 DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=8 \
    DPUMESH_PROXY_L7_SVC= BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=l4 \
    ./bench/bench.sh deploy

./bench/suite/l4_proxy_data.sh --no-deploy --preflight-only --out /tmp/preflight
python3 bench/suite/plot_final.py measurements.csv bench/report/figures
```

The preflight validates the deployment, pinning and core budget and records the
per-configuration endpoint cores that the measurement reads.
