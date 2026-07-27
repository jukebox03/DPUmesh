# Current DPUmesh Evaluation

Measurements were collected on 2026-07-24 KST with the deployment in
[DEPLOY.md](DEPLOY.md).

## Implementation

```text
host forward ring
        │
        ▼
      DPA EU
        │
        ▼
ARM worker (connection owner)
  connection · route/LB · SG-DMA · DMA completion
        │
        ▼
host reverse ring (REV_DONE / TX_ACK)
        │
        ▼
host PE progress thread

DPU main: lifecycle control and armed-host doorbells
```

The deployed topology is `N/K/A=16/8/8`. A connection is assigned by
`port % A`. Each worker owns its connection state, DPA completion PE, SG-DMA
engine, DMA completion callbacks, and reverse-ring producers. The main thread
does not process payload completions.

Reverse entries use monotonic `publish_seq`. The host publishes one
`consumer_head` after a drain and one `arm_epoch` before blocking. A newly
observed armed epoch produces one Comch doorbell.

## 8 KiB request / 8 B reply

The client used one pinned host core. Each loaded row is the median of three
15-second samples with zero failures, drops, and reorder.

| Connections × outstanding | Throughput | p50 | p99 | Host CPU | DPU ARM | Host cores/Mrps |
|---:|---:|---:|---:|---:|---:|---:|
| 16 × 32 | 0.5024 Mrps / 32.93 Gb/s | 976 µs | 1,817 µs | 1.35 cores | 6.98 cores | 2.69 |
| 16 × 4 | 0.2071 Mrps / 13.57 Gb/s | 302 µs | 584 µs | 1.55 cores | 7.26 cores | 7.50 |
| 4 × 16 | 0.1943 Mrps / 12.73 Gb/s | 303 µs | 596 µs | 1.11 cores | 4.04 cores | 5.61 |
| 1 × 1 | 0.00525 Mrps / 0.344 Gb/s | 172 µs | 432 µs | — | — | — |

At 16 × 32, `512 / 0.5024 Mrps = 1.02 ms`. At 16 × 4,
`64 / 0.2071 Mrps = 309 µs`; at 4 × 16, `64 / 0.1943 Mrps = 329 µs`.
Loaded latency tracks the closed-loop queue depth. The single-outstanding p50 is
172 µs.

## DPU thread placement

The 512-outstanding sample used:

| Thread set | CPU |
|---|---:|
| Main, pinned to core 8 | 25.5% |
| Workers, pinned to cores 0–7 | 84.7% average |
| Worker range | 80.3–87.1% |
| Worker coefficient of variation | 2.5% |
| Total process | 7.04 cores |

The eight workers are balanced and reach higher utilization than the main
thread.

## Host CPU

The 512-outstanding point used 0.68 client cores and 0.67 server cores, summed
across the three backend processes. Host efficiency was 2.69 cores/Mrps.

With no workload, the measured client and backend processes used 0.00 host
cores. The DPU process used 0.15 ARM cores.

## Validation

The deployed topology passed:

- native loopback: 10,000/10,000, p50 230.6 µs;
- verbs facade: 10,000/10,000, p50 148.4 µs;
- framed L7 stream: 5,000/5,000, p50 231.0 µs;
- preload: 3,000/3,000 over eight connections, p50/p99 155/508 µs;
- host unit, queue, topology, ring-counter, and ABI tests.

## Constraints

- One connection uses one ARM data worker.
- Connections must cover distinct values modulo `A` to use additional workers.
- `A` divides `K` and `N`; `K ≤ N`.
- L4 keeps one backend per connection.
- Framed L7 may select a backend per frame.
