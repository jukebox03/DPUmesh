# Host-only Tests

`make test` runs the native transport, preload adapter, benchmark scheduler, and
analysis contracts without Kubernetes or BlueField hardware.

| Test | Coverage |
|---|---|
| `native_api_contract_test.c` | public allocation, post, the open-transmit-call pairing, library-owned batching, async TX error, event, and RX-credit contracts |
| `native_control_state_test.c` | registration, unregister replay, and slot cleanup |
| `native_tx_batch_policy_test.c` | idle/immediate and busy/deadline tail publication, deadlines that never move once stamped, arming a tail retained when the stream falls quiet, the transmit gate excluding the deadline pass with retention surviving it, waiting for submitted data to leave the DPU before FIN, timer wakes without submitting, armed-bit release on close, deferred TX error, TX units, block ordering, landing geometry, and credit sharding |
| `native_writable_test.c` | TX-ready arm/recheck, shared-pool readiness, EQ notification suppression, and cursor rollback |
| `preload_api_contract_test.c` | POSIX blocking, `POLLOUT`, EQ drain serialization, library-owned TX batching/error signalling, RX ordering, FIN, close, and fd lifetime |
| `l4_pin_policy_test.c` | connection pinning and backend loss |
| `benchmark_result_contract_test.c` | rejects zero-progress, failed-request and failed-worker benchmark points instead of labelling them `OK` |
| `lb_policy_test.c` | ready-backend filtering and service round robin |
| `proxy_lane_queue_test.c` | per-destination queue order, ordered retry, receive-stripe geometry, and DMA progress |
| `worker_mpsc_queue_test.c` | the multi-producer queue one worker uses to hand work to another |
| `topology_test.c` | how a port maps to its forward ring, accelerator unit and ARM worker |
| `ring_counter_test.c` | descriptor generation, wraparound, and admission |
| `l7_abi_contract_test.c` | L7 adapter flow/verdict layout, mode and decline constants, and the connection handle both sides form |
| `analyze_saturation_test.py` | saturation, CPU slopes, knee stability, and generator headroom |
| `summarize_l4_test.py` | collector metadata and selected-matrix repetition counts |
| `generator_selftest_test.sh` | native and POSIX transport-free arrival schedulers |
| `l4_collector_contract_test.sh` | collector matrix and optional RPS limits |
| `dma_fault_scope_test.sh` | worker DMA-context recovery |
| `abi_contract_test.sh` | SONAME, public symbols, and preload runtime linkage |

```sh
make -j4 test
```

Executables are written under `build/test`. Hardware validation uses the
programs in [bench/validators](../bench/validators/README.md).

The deployed gRPC lifecycle gate is `./bench/bench.sh grpcshutdown`. It stops a
client process with a live HTTP/2 channel, requires balanced Linkerd
session/task metrics and no new RX-mmap reclaim error, re-registers the recycled
pod slot, and finishes with a four-channel smoke point.
