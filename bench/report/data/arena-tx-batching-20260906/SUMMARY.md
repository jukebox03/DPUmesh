> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Linkerd arena TX batching

This receipt records the working tree's Linkerd TX ownership and its software
verification. BlueField performance and the publication distribution are in the
[hardware receipt](../arena-tx-batching-perf-20260906/SUMMARY.md).

## Transmit contract

Each `DmeshIo` endpoint owns at most one unpublished 64 KiB arena batch. Scalar
and vectored writes copy the accepted prefix straight into the C-owned arena.
Rust keeps no payload pointer, only the token, the length, the exact route and
the sealed state. The origin, local backend and remote backend endpoints hold
independent batches even when they share a request connection.

The first accepted write fixes the route. A full batch is published before the
next write; a partial batch is published by the worker drain. `poll_flush`
requests driver progress and does not force a publication per HTTP/2 frame.
`poll_shutdown` allows FIN only after DATA publication completes.

Under backpressure the sealed batch and the accepted bytes are retained and the
same token is retried on the next driver grant. The byte quota and the number of
new reservations are bounded per epoch. The endpoint lock and the quota lock are
released before any C callback. Abort and connection close cancel the unpublished
token and return the arena chunk.

## Correctness

- 20 writes of 32 B with `poll_flush` between each: 640 B, one reservation, one
  publication
- byte order preserved across scalar and vectored partial prefixes
- no re-copy of the accepted payload when a blocked flush is retried
- caller bytes not retained for a cancelled `Pending` write
- origin, local and remote endpoint batches keep independent tokens and exact
  routes
- FIN blocked before DATA publication and ordered after it
- wrong worker, handle, route and token all refused
- stale tokens isolated across abort, close and connection reuse
- partial acceptance at an arena chunk boundary, with the whole chunk returned

## Software gates

| Gate | Result |
|---|---|
| `make test` | PASS |
| Linkerd adapter Rust | 41/41 PASS |
| `dmesh-doca` | 30/30 PASS |
| non-test Rust check | PASS |
| gRPC release | 4/4 PASS |
| gRPC ASAN+UBSAN | 4/4 PASS |
| C proxy-lane ASAN+UBSAN | PASS |
| repository and submodule whitespace | PASS |

`make test` ran in a Python environment built from the repository's
`tests/requirements.txt`. Every batch correctness fixture checks accepted bytes,
copied bytes, publications, reservations, retries, errors and arena return
together.

## Observation contract

| Metric | Meaning |
|---|---|
| `tx_arena_copy_bytes` | bytes copied into an arena batch |
| `tx_accepted_bytes` | bytes handed to DMA or peer custody |
| `tx_reserve_attempts` | attempts to acquire a new batch |
| `tx_publications` | batches whose custody transferred |
| `tx_retries` | reserve or publication retries |
| `tx_budget_wait` | write polls that waited for a quota epoch |
| `tx_writer_wakes` | writers woken by a new grant |
| `tx_errors` | terminal TX errors |

In the clean hardware samples the arena copy bytes matched the accepted bytes,
with 0 retries and 0 errors. After traffic stopped the arena read 1,024/1,024
free with 0 live chunks.
