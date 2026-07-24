# Current DPUmesh Evaluation

This report covers the connection-affine ARM data path measured on
2026-07-24 KST.

## Implementation

```text
connection port
      │
      ▼
forward ring (port % K)
      │
      ▼
DPA EU (EU % A == ring % A)
      │
      ▼
ARM worker (port % A)
  {connection table, parser, route/LB, SG-DMA}
      │
      ▼
main Comch emitter
```

`N`, `K`, and `A` denote DPA EUs, forward rings per pod, and ARM data workers.
`A` divides `K` and `N`, and `K ≤ N`. Each connection remains on
one ARM worker. DPU-created upstream ports preserve the same owner on replies.

The forward ring is a bounded MPSC queue. The host publishes each 64-byte
descriptor with its monotonic ticket generation. The DPA consumes consecutive
generations, drains up to 32 descriptors per ring and round, and publishes one
shared `consumer_head` after progress. A round uses one read-window invalidation
and one writeback. DMA completion submission uses `FLUSH`.

L4 pins a connection to one backend. Services in `DPUMESH_PROXY_L7_SVC` select a
ready backend for each complete frame while retaining the connection table on
its owner worker. The main thread owns Comch sends and pod lifecycle control.

## Forward DMA ring

The deployed `N/K/A=16/2/2` configuration produced these three-run medians with
four client connections:

| Request / reply | Throughput | p50 |
|---|---:|---:|
| 1 KiB / 1 KiB, concurrency 32/connection | 0.653 Mrps | 181 µs |
| 8 KiB / 8 B, concurrency 16/connection | 15.74 Gb/s | 245 µs |

## ARM and connection scaling

The ARM sweep used `N=16`, `K=4`, four persistent connections, two host client
cores, and zero failures, drops, or transport reorder.

| Request / reply | A=1 | A=2 | A=4 |
|---|---:|---:|---:|
| 1 KiB / 1 KiB, concurrency 32/connection | 0.504 Mrps | 0.619 Mrps | 0.628 Mrps |
| 8 KiB / 8 B, concurrency 16/connection | 5.59 Gb/s | 12.69 Gb/s | 16.29 Gb/s |

The connection sweep used `N/K/A=16/4/4`.

| Request / reply | 1 connection | 2 connections | 4 connections |
|---|---:|---:|---:|
| 1 KiB / 1 KiB, concurrency 32/connection | 0.182 Mrps | 0.352 Mrps | 0.628 Mrps |
| 8 KiB / 8 B, concurrency 16/connection | 5.51 Gb/s | 10.36 Gb/s | 16.29 Gb/s |

Current `N/K/A=16/2/2` 1 KiB scaling:

| Host cores | Connections | Throughput |
|---:|---:|---:|
| 1 | 1 | 0.183 Mrps |
| 1 | 2 | 0.347 Mrps |
| 1 | 4 | 0.653 Mrps |
| 2 | 4 | 0.664 Mrps |
| 2 | 8 | 1.105 Mrps |

At the 8 KiB saturation point, the main thread used 92.1% of one ARM core. The
two data workers used 94.8% and 94.6%; worker CPU CV was 0.1%.

## Load balancing

The L7 hardware check used `N/K/A=16/2/2`, one client connection, three ready
backends, and 1,548,486 completed replies. Backend counts were
516,329 / 516,329 / 516,328, with zero failure and drop. The connection remained
on one ARM owner. Application request IDs recorded 1,479,259 out-of-order
completions because independent backends complete frames independently.

## Validation

The deployed implementation passed:

- 10,000-operation 8 KiB native loopback and verbs validation;
- 3,000-operation preload validation with eight connections;
- 5,000-frame 8 KiB L7 stream validation;
- reconnect validation with about 36,000 reconnects;
- 128 KiB and 1 MiB L4 logical-frame transfers;
- host unit, ABI, preload, LB, queue, and topology tests.

## Operational constraints

- One connection uses one ARM data worker. Additional workers require
  connections whose ports cover additional values modulo `A`.
- `K` bounds per-pod ring parallelism; `N` distributes rings across pods.
- `A` divides `K` and `N`; `K` does not exceed `N`.
- Strict application reply order uses L4 connection pinning or application-level
  stream IDs and reordering.
- The main Comch emitter is shared by all ARM data workers and is saturated in
  the measured 8 KiB configuration.
- Automatic DPA selection uses up to 16 EUs; explicit configuration supports up
  to 32.

Raw rows are in
[`data/rtc_scaling_2026-07-24.csv`](data/rtc_scaling_2026-07-24.csv) and
[`data/dma_ring_2026-07-24.csv`](data/dma_ring_2026-07-24.csv). The reference
deployment is in [DEPLOY.md](DEPLOY.md).
