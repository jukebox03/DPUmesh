# Native Contract Tests

This directory contains fast host-only regression tests for the native ABI and
internal state machines. They run without Kubernetes or BlueField deployment and
are the first validation layer before the hardware validators under `bench/`.

| Test | Contract protected |
|---|---|
| `native_api_contract_test.c` | `alloc`/`post_send` validation and automatic complete-unit submission |
| `native_control_state_test.c` | Idempotent register, replay-safe unregister, and no slot reuse while cleanup is pending |
| `native_tx_batch_policy_test.c` | Complete 8 KiB unit submission, explicit partial flush, and physical-block tail ordering |
| `native_writable_test.c` | Automatic one-shot arm/recheck, QP and shared-pool readiness, stale-hint cancellation, and reservation rollback |
| `preload_api_contract_test.c` | Public native TX usage, completion-driven blocking/nonblocking retry, honest `POLLOUT`, send timeout, RX partial/peek credit, FIN/stream validation, and fd-entry lifetime |

Run all tests from the repository root:

```sh
make test
```

Executables are generated under `build/test/`; only the source files in this
directory belong in version control. The API and control tests link selected
production sources with small deterministic stubs. The batching and writable
tests are white-box tests that include the production cursor implementation so
they can seed otherwise-private TX state without constructing DOCA hardware.

These tests detect policy and state-machine regressions, but they do not prove
DMA, DPA execution, wire transfer, FIN delivery, or remote quiescence on real
hardware. Use the programs documented in
[`bench/validators/README.md`](../bench/validators/README.md) for that layer.

`native_writable_test.c` constructs deterministic block-pool and cursor states
around the production implementation. It verifies that same-block ACK progress
does not create a false QP-window wake, a block-boundary or full-drain ACK does,
and a shared block return wakes one armed waiter. It also covers capacity returned
between failure and arm, direct-retry cancellation of a queued hint, and the rule
that a failed cross-block reserve leaves the write cursor and padding counters
unchanged. `native_api_contract_test.c` separately verifies the public
`DMESH_WC_TX_READY` shape and one-shot consumption.

`preload_api_contract_test.c` includes the production preload state machine and
replaces native calls with deterministic fakes. It verifies that `EAGAIN` causes
no timer retry, that `TX_READY` changes the app-visible fd back to writable, and
that native RX credits survive partial/peek reads and are released exactly once.
It also rejects data or a stream change after FIN and checks that dispatcher
retirement cannot free an entry still held by an interposed operation.
