> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Faults found after the registration change — causes and fixes

Seven defects surfaced once the DaemonSet and direct registration were deployed
(`../k8s-registration-20260908/`). None was a registration-protocol flaw; all
were transmit-tail, teardown or supervision defects that the new deployment
exercised harder.

## 1. Drain timeout: the last fragment was lost by the timer

The `worker_fail=1` seen in the native baseline **reproduced under the Pod too**:
before the fix, the tenth repeat of 8 KiB / concurrency 32 / 5 s logged
`drain timeout: 1 requests still outstanding` in the application. It is
therefore neither native-only nor Pod overhead. `pending` is the outstanding
count at the measurement boundary and is not the number left after a failure.

A debugger attached to the application captured the state right after the drain
timeout and before abort/close; the sixth attempt reproduced it.

| Observation | Value / meaning |
|---|---|
| `outstanding` | 1 request |
| `tx_active`, `tx_done` | 0, 8208 — the application posted the whole frame |
| `tx_c - tx_s` | 608 bytes the library had not published |
| `su_head == su_tail` | every published send unit was acknowledged |
| `tx_error`, `tx_gate` | 0, 0 — no terminal TX error, no gate held |
| `tx_armed_count` | 1 — a tail exists for the timer to serve |
| `tx_earliest_ns`, `tx_deadline_ns`, `tx_due_hint` | all 0 |

[debugger state](bandwidth-gdb.txt), [application log](bandwidth-before-app.log).

While the ACK thread arms a tail, the owner thread can clear the previous tail's
cached deadline: the armed bit remains while the cache reads 0. The old
`eq_tx_armed_wait_ns()` returned "nothing waiting" (-1) whenever the cache was 0,
so the timer never woke the EQ and the final posted fragment was never sent.
Raising the last request's response timeout would not have fixed this.

**Fix.** Return due (0) when the cache is 0 but the armed count is not, and let
the EQ owner rescan the real armed bits and per-port state before publishing. The
wire protocol, the coalescing delay and the measurement success criteria are
unchanged; the existing bitmap and count are reused, with no new timer or lock.
`test_armed_tail_without_cached_deadline()` reproduces the captured state; it
failed before the fix and passes with the whole TX policy suite after it.

## 2. Broker SIGSEGV: using the task pool of an IDLE Comch

The fault PC in the kernel log resolved inside the DOCA 3.1.0105 shared library
at `priv_doca_task_pool_allocate_task+8`, faulting on `mov 0x24(%rdi),%eax`, so
the task-pool pointer was NULL. The wrapper's exit 139 was a real SIGSEGV, not a
normal close code.

The broker detected the Comch stop, left its event loop, and called
`request_remote_pod_quiesce()` from cleanup. That function sent `POD_UNREGISTER`
first and checked Comch state afterwards, and the shared `client_send_msg()` had
no RUNNING check either, so allocation could run after the transition to IDLE had
freed the pool.

**Fix.** `client_send_msg()` checks RUNNING before allocating and returns
`DOCA_ERROR_CONNECTION_ABORTED` once the connection is gone, rechecking after
each PE progress made while the send pool is full. No PE progress happens between
the check and the allocation, and the broker owns that PE alone, so no
check/use race with another PE thread is created. `comch_send_state_test` covers
IDLE, STOPPING, and a RUNNING→IDLE transition during a pool wait, plus the
existing error-return and slot-return behaviour while RUNNING.

## 3. Teardown retransmissions accumulated on an unresponsive peer

Killing the DPU process does not make Comch notice the break immediately, and
`request_remote_pod_quiesce()` enqueued a new UNREGISTER every 100 ms meanwhile.
Instrumentation showed **37/37** SDK in-flight and self-owned tasks per broker at
cleanup entry, still **17/17** in the failing broker five seconds later. This was
not a leak of unfreed tasks but a pile of incomplete retransmissions that could
not drain inside the bound. ([before](cleanup-diag-host.log))

**Fix.** Do not enqueue the next UNREGISTER before the previous send completes;
after it completes, retry on the existing interval while the QUIESCED ACK is
missing. The 5 s quiesce and 5 s Comch drain bounds are unchanged. After the fix
the first two SIGKILL runs entered cleanup with **0/0** outstanding, both brokers
exited 0, and no IDLE timeout or IN_USE occurred.
([after](cleanup-fixed-first-host.log)). `broker_quiesce_test` checks that a peer
which looks RUNNING but never completes a send limits the outstanding send to
one, and that a completed send with no ACK is retried.

## 4. Reaping the wrapper was treated as the broker exiting

The supervisor sent SIGKILL to the wrapper alone after five seconds, while the
broker has up to ten seconds of cleanup, so a healthy cleanup could be killed.
Worse, `setresgid`/`setresuid` clears the parent-death signal armed right after
fork — [documented Linux behaviour](https://www.man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html).
The old logs show the real broker still printing cleanup errors after the wrapper
was reaped.

**Fix.**

- Re-arm `PDEATHSIG=SIGKILL` after the credential change. Confirmed to survive a
  real root→uid 65532 transition: [parentdeath-after.txt](parentdeath-after.txt).
- Give the supervisor a 15 s grace and, past it, use the real worker's
  `cgroup.kill`. That call only queues SIGKILL and returns, so wait for the
  watcher to confirm the actual exit. An intermediate deployment that skipped
  this wait let systemd restart the delegated unit while a broker still occupied
  it, failing once with `219/CGROUP` before restarting itself; that race is fixed.
- Do not release tracking or the slot while the worker cgroup is still
  `populated`, even after the wrapper is gone. This fence also covers a parent
  death between the credential change and the PDEATHSIG re-arm.
- Parent-death signals follow the creating **thread**, so a slot's launch thread
  must not return during daemon shutdown until the watcher has confirmed the
  broker was reaped; otherwise the wrapper dies before the grace regardless of
  the systemd settings.
- `KillMode=mixed` and `TimeoutStopSec=25s` let `dpumeshd` clean up its brokers
  first. The previous `control-group` signalled the wrappers on service stop.

## 5. A restart backoff longer than 30 s locked the slot permanently

On the second SIGKILL of the first cleanup fix, the application did not recover
for 108 s even though the DPU had returned. The broker exited normally, but
`_watch()` gave up confirming DPU cleanup after 30 s and the slot stayed in
`CLEANUP_WAIT`. ([reproduction](cleanup-fixed-first.json))

**Fix.** Keep confirming until the DPU returns. A slot reopens only when the host
cgroup is empty and DPU STATUS proves cleanup finished; the slot is never reused
speculatively while the DPU is away. The wait lives in the individual watcher and
does not block the daemon. `dpumeshd_test` gained the wrapper-only fence, the
forced cgroup reclaim, and a cleanup retry beyond the old 30 s window.

## 6. Cleanup failure led to dependent destroys and a success exit

Failing to reach IDLE still went on to destroy the client, PE and device and to
NULL their pointers, cascading IN_USE errors. The failed object and its
dependents are now retained, and the broker exits with a failure code without
first releasing exported memory; process teardown and the host and DPU cleanup
fences perform the final reclamation. `comch_cleanup_test` checks that dependents
survive both a timeout and a destroy failure.

## 7. Stopping a context inside an error callback caused an SDK use-after-free

All seven hardware fault scenarios of the cleanup fixes recovered communication,
but one broker ended with `corrupted size vs. prev_size` and exit 139 during
consecutive SIGKILLs, so that run was not treated as a clean regression pass.
([scenarios](cleanup-first-seven-restarts.json), [kernel](heap-corruption-kernel.txt))

An ASan build did not reproduce it. `hw_comch_peer_loss` was then written to use
only the Comch client — no application, exported memory, or workload
registration — in the same PE `PROGRESS_ALL` mode as the real broker, killing the
DPU and retrying sends while awaiting completion to force CONNECTION_ABORTED.
Valgrind, which also instruments the SDK's own memory accesses, found the **same
invalid write on 4/4 connections**:

- the error callback freed the task and then called `doca_ctx_stop()`;
- that call released the SDK's internal 1,320-byte connection object;
- the same `doca_pe_progress()` the callback returned into wrote one byte at
  offset 1,312 of the freed object, with address and allocation/free/write
  stacks all recorded.

([before](peer-loss-event-vg-0.txt)) The installed DOCA 3.1.0105 samples contain
the stop-inside-callback pattern, but it is not safe on this real error path.
ASan neither instruments the SDK's internal writes nor keeps the same allocator
layout, so its silence could not be claimed as a fix.

**Fix.** The callback only returns the payload and task and records
`client_send_failed`. Further sends are refused on that flag, and the broker
stops the context from cleanup after PE progress has returned. No authentication
protocol or IPC message was added.

Re-running the same Valgrind reproduction gave **4/4 exit 0 with 0 memory-access
errors** ([after](peer-loss-fixed-vg-0.txt)). Both comparison runs used
`--undef-value-errors=no` to exclude false positives on DMA-filled buffers;
invalid read/write/free checking was kept, and this result does not claim a leak
check passed. The automatic regression test now fails if a context is stopped
inside an error callback.

## Final hardware confirmation

The final images are `cleanup-v2-20260908` for both host applications and the
DPU. Host `dpumeshd` Python, the broker and library, and the systemd shutdown
settings were updated together. Performance after the fixes is in
[cleanup-v2-performance.json](cleanup-v2-performance.json): latency64 p50 132 µs
across three repeats, bandwidth8192 p50 268–273 µs at 6.46–6.92 Gbps, `fail=0`
throughout. A later dead-code pass re-confirmed the same band
(`../deadcode-cleanup-20260908/`).
