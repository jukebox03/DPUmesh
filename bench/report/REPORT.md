# Current DPUmesh Evaluation

Measurements were collected on 2026-07-27 KST with the deployment in
[DEPLOY.md](DEPLOY.md).

## Implementation

The evaluated topology is `N/K/A/L=16/8/2/2`.

```text
K forward rings → N DPA EUs → A polling ARM workers
                                      │
                                      ├─ connection and routing state
                                      ├─ shared per-worker SG-DMA context
                                      └─ L RX landing stripes → K reverse rings
```

RX landing geometry is independent of ring count. The 64 MiB host RX mapping is
split into `L=A` stripes. Each stripe aggregates `K/L` sharded credit counters.
Data workers use run-to-completion polling; the main control thread remains
event-driven.

A shared DMA-context fault restarts the worker context without changing pod
readiness. A current-generation payload batch receives one ordered retry.
Control-path disconnect removes the pod and releases its imported mappings.

## 8 KiB request / 8 B reply

The client used concurrency 32, two threads, a 1,000-request warmup, and a
10-second measurement. Each row is one run; all runs completed with zero
failures and zero reorder.

| Run | Throughput | p50 | p99 |
|---:|---:|---:|---:|
| 1 | 13.9482 Gb/s | 300 µs | 539 µs |
| 2 | 13.9340 Gb/s | 301 µs | 532 µs |
| 3 | 13.1310 Gb/s | 308 µs | 535 µs |
| Median | **13.9340 Gb/s** | **301 µs** | **535 µs** |

## Performance comparison

The retained comparison uses the same 8 KiB request, 8 B reply, and two client
threads.

| Topology | Throughput | p99 | Throughput change |
|---|---:|---:|---:|
| `K=2, A=2` reference | 14.15 Gb/s | 492 µs | — |
| `K=8, A=8` reference | 10.04 Gb/s | 671 µs | -29.0% vs. K=2 |
| `K=8, A=2, L=2` current | 13.934 Gb/s | 535 µs | -1.5% vs. K=2 |

The current topology improves throughput by 38.8% and reduces p99 by 20.3%
relative to the `K=8, A=8` reference. Relative to the `K=2, A=2` reference,
throughput is 1.5% lower and p99 is 8.7% higher.

## DPU ARM

The `armbalance` run produced 14.059 Gb/s, p50 300 µs, and p99 535 µs.

| Thread set | CPU |
|---|---:|
| Main | 41.9% |
| Worker 0 | 105.3% |
| Worker 1 | 105.2% |
| Total process | 252.8% |
| Worker coefficient of variation | 0.1% |

The polling cost is bounded by `A=2`; increasing `K` does not create additional
ARM workers. With no benchmark traffic, each polling worker uses approximately
one ARM core and the main thread is idle, for an approximately two-core floor.

## Memory and synchronization

| Resource | Current value |
|---|---:|
| Host TX mapping per pod | 64 MiB |
| Host RX mapping per pod | 64 MiB |
| RX allocation for landing stripes | partitioned, not replicated |
| Credit scratch cell | 64 B; up to eight counters |
| New landing/credit locks | none |

Landing selection, credit sharding, and counter aggregation add no mutex to the
data path. The existing shared object-pool lock is unchanged.

## Validation

- `make -j4 test`: all native, queue, topology, fault-scope, and ABI tests pass.
- BlueField build: 14 DPU C objects compiled successfully.
- Deployment: all DPUmesh pods reached data-ready with `K=8, L=2`.
- Backend deletion during a 12-second transfer: `rcnt=582,484`, `fail=0`.
- Replacement backend: data-ready as pod slot 0 with `K=8, L=2`.
- DPU log: no poison, fault, or failed-DMA record in the retained run.

## Constraints

- One connection uses one ARM worker.
- `L=A`, `K % A=0`, `N % A=0`, and `K≤N`.
- L4 keeps one backend per connection.
- Framed L7 may select a backend per frame.
- Polling ARM cost is proportional to `A`, independent of offered load.
