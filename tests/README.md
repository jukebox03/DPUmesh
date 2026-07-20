# Native Contract Tests

This directory contains fast host-only regression tests for the native ABI and
internal state machines. They run without Kubernetes or BlueField deployment and
are the first validation layer before the hardware validators under `bench/`.

| Test | Contract protected |
|---|---|
| `native_api_contract_test.c` | `alloc`/`post_send` validation and automatic complete-unit submission |
| `native_control_state_test.c` | Idempotent register, replay-safe unregister, and no slot reuse while cleanup is pending |
| `native_tx_batch_policy_test.c` | Complete 8 KiB unit submission, explicit partial flush, and physical-block tail ordering |

Run all tests from the repository root:

```sh
make test
```

Executables are generated under `build/test/`; only the source files in this
directory belong in version control. The API and control tests link selected
production sources with small deterministic stubs. The batching test is a
white-box test that includes the production cursor implementation so it can seed
otherwise-private TX state without constructing DOCA hardware.

These tests detect policy and state-machine regressions, but they do not prove
DMA, DPA execution, wire transfer, FIN delivery, or remote quiescence on real
hardware. Use the programs documented in
[`bench/validators/README.md`](../bench/validators/README.md) for that layer.
Future native writable-notification tests should live here and cover arm/recheck,
QP and shared-pool reclaim, missed wakeups, close races, and idle behavior.
