# Dead-code pass — hardware confirmation

Scope: removal of unreferenced code and duplicated wire values, plus three
control-flow simplifications. No functional change was intended.

Removed: `dmesh_local_control_connected`, `DMESH_LOCAL_BUSY`,
`require_ready_endpoint` and the unused `endpoint_slices` parameter,
five unused module constants and four unused imports on the host.
Simplified: `cleanup_comch_object` error flow, the duplicated
outstanding-registration predicate, the shutdown drain loop, and the
registration-mode branch in `init_control_path`.

Confirmations on the rig:

- host `make test`, `make test-hostfree`, `make test-local-registration` pass.
- Full ARM rebuild at `warning_level=2` produces no warning.
- Direct registration accepted on both slots after each rollout.
- Workload restart returns its slot: generation 4 registered ~4 s after the
  previous broker exited, with no stranded `CLEANUP_WAIT`.
- Runtime Pod deletion logs `runtime hardware cleanup completed` and exits
  through `exit()`; its teardown log is byte-comparable in shape to the
  preceding build, including the same 16 DPA send-error lines.
- Data path after each event: `OK pong`, `fail=0`, `drops=0`, `worker_fail=0`.
- Latency and bandwidth land inside the preceding build's band
  (`performance.txt`).

Files: `host.log`, `runtime.log`, `graceful-runtime-exit.log`,
`final-apps.txt`, `final-runtime.txt`, `performance.txt`.
