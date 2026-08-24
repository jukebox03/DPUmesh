# D1 / D3 final receipt — 2026-08-24

## Build under test

- DPU PID after the final release deploy: `1861279`.
- Client Pod: `bench-grpc-dpumesh-646d5dcc88-vs2d7`, restart count 0.
- Server Pod: `echo-grpc-dpumesh-84cbd4d65d-xjmwm`, restart count 0.
- Client container image ID: `sha256:a75a0c212b3192bd87773442bbaec9b2f7d17370845a1094d999e38535c7c248`.
- Server container image ID: `sha256:2a98241d0083a559f87765bbdb2b6843c0b50a73ec0c7f2b4037a43dd46b4694`.
- CPU profile for the measurements: `grpc` (client 18-23, server 24-29).

## D1

The synchronous gRPC benchmark server created thousands of poller threads under
the exact sustained overload: 7-9K threads and 8-9.5 GiB RSS were observed before
the SIGSEGV. ASAN then failed while reserving its shadow mapping, which made the
crash look like a corrupted function pointer. The benchmark server now uses the
gRPC callback service API and a unary reactor, so the runtime owns a bounded
executor instead of creating one synchronous waiter per overloaded call.

The exact 120-second ASAN overload held at 30 server threads, did not restart the
container, and produced no new ASAN/UBSAN report. The release build, its four
CTest cases, and the deployed embedded-Linkerd smoke gate passed. The deployment
recorded above is the release callback server, not the ASAN reproduction image.

## Performance spot check

A final open-loop sweep used 64-byte payloads, eight client threads, one repeat
per point, and the release deployment above. With eight channels, 8 K, 12 K,
16 K, 20 K, and 24 K offered rps were all fully delivered with no reported
failures or drops. At 24 K rps, p50 was 1.747 ms and p99 was 7.612 ms. At 32 K
offered rps the system entered saturation, delivering 27.634 K rps with a
155.147 ms p50. This places the clean measured operating point at 24 K rps and
the observed one-run throughput ceiling in the 27 K-rps range for eight
channels.

A single-channel control saturated one ARM worker between 2 K and 3 K rps,
while eight channels distributed work across all eight workers. The final 1 K
single-channel point measured 1.000 K rps at 1.918 ms p50, matching the earlier
0.999 K-rps baselines at 1.878-1.902 ms p50. Within the repeat-to-repeat spread
of the existing measurements, no throughput or latency regression attributable
to D1 or D3 was observed. These are short spot checks, not a replacement for a
multi-repeat capacity campaign.

Raw sweep points: [`performance-c1.csv`](performance-c1.csv) and
[`performance-c8.csv`](performance-c8.csv).

## D3

The host TX window is byte-sized (4 MiB), while the software-unacked FIFO used a
512-entry depth derived from 8 KiB DMA slots. gRPC descriptors are about 110
bytes, so overload could overwrite unacknowledged FIFO metadata long before the
byte window filled. Admission now reserves tracking entries before publishing,
the tracker refuses a full FIFO without mutation, ACK wakeup is lost-wakeup safe,
and the production depth is 2048: half of the 4096-entry forward DMA ring, leaving
the other half available for completions already in flight.

Linkerd could also close both output halves during an HTTP/2 reset storm before
the transport input reached EOF. The adapter now records input EOF per direction,
reports that case as an early session end, and the DPU applies the request's
failure state to its paired reply. This prevents a removed adapter session from
being mislabeled as rejected payload.

Final hardware sequence, starting at 2026-08-24 15:48:23 KST:

1. Baseline `OPEN 48 48 8 8 100 1000 const 1`: 0.999 K rps, p50 1.881 ms,
   `fail=0`, `drops=0`, `reorder=0`.
2. `OPEN 48 48 8 10 1000 40000 const 1`, three consecutive runs. This is far
   above sustainable capacity and produced RPC failures and multi-second tails,
   but no host DMA-ring full/stall and no process restart.
3. After all three runs: client ring stall/full lines 0, server ring stall/full
   lines 0, `l7 layer rejected a segment` lines 0, and all eight Linkerd workers
   had `ACTIVE=0`, `PENDING=0`, `TASKS=0`, `OPENED=CLOSED`.
4. Recovery `OPEN 48 48 8 8 100 1000 const 1`: 0.999 K rps, p50 1.902 ms,
   `fail=0`, `drops=0`.
5. Recovery `OPEN 48 48 8 8 300 3000 const 1`: 2.823 K measured rps,
   `fail=0`, `drops=0`, and the final rejected-segment count remained 0.

Local gates after the changes: `proxy_lane_queue_test` passed, `make
test-hostfree` passed, all 38 Rust adapter unit tests passed, `cargo fmt --check`
passed, and `git diff --check` passed.
