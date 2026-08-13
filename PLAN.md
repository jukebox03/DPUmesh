# DPUmesh + Linkerd implementation plan

This file is the implementation checklist for work that remains after the
single-worker persistent runtime and session-close fixes. Complete the items in
order.

## Current implementation baseline

The following behavior is the starting point and is not a TODO:

- DPUmesh creates and pins the ARM data-worker threads in
  `doca/dpu_worker.c`.
- Each worker calls blocking `l7_worker_run(worker_id, worker_state)` when the
  Linkerd backend owns the runtime.
- `linkerd/rust/src/lib.rs` creates a Tokio `current_thread` runtime on that
  worker and runs `dmesh_doca::runtime::run` until the worker stops.
- DPUmesh owns DOCA progress, DPA/DMA rings, conntrack, staging custody,
  routing and egress. The Linkerd archive is built without `own-datapath`.
- One selected worker, `DPUMESH_L7_LINKERD_WORKER=0` by default, owns Linkerd
  session state. Other workers decline L7 sessions.
- The adapter accepts opaque and full-stream modes, isolates one outbound stack
  and backend channel per session, and delegates backend selection to DPUmesh.
- Closing either request or reply direction aborts both `DmeshIo` endpoints,
  returns every outstanding extent once, removes all session indexes and
  permits the service address to be opened again.
- The Host transport protocol and `DmeshRuntime` gRPC threading model do not
  depend on the Linkerd runtime.

## Invariants for every change

- [x] A connection and every object reachable from it have one owning ARM
  worker for their entire lifetime.
- [x] No Linkerd task reads a staging segment after
  `dmesh_l7_release(worker_id, conn, pos, len)` returns its custody.
- [x] Each accepted staging extent is released exactly once, in handoff order.
- [x] A close is idempotent from request FIN, reply FIN, poison, worker stop and
  late registration paths.
- [x] No connection handle, slot or service address can bind a late event to a
  newer connection generation.
- [x] Output is ordered. A partially accepted send restores only its unaccepted
  suffix at the front of the same connection queue.
- [x] An ARM worker never blocks on another ARM worker. Cross-worker work uses a
  bounded queue and an eventfd wake.
- [x] Host descriptors, reverse-ring records and gRPC transport APIs remain
  unchanged unless a separate protocol-version task explicitly changes them.
- [x] A benchmark point is valid only when `fail=0`, `drops=0`, `reorder=0`, no
  L7 fallback counter increases, and every started request completes.

## P0 — freeze session lifecycle correctness

### P0.1 Rust and C tests

- [x] Keep the endpoint abort test in
  `linkerd/port/linkerd2-proxy/linkerd/doca/src/io.rs`:
  `abort_discards_buffers_and_closes_both_halves`.
- [x] Keep adapter tests in `linkerd/rust/src/lib.rs` for:
  - reply close removes the whole paired session;
  - both stack-facing endpoints observe EOF and reject later writes;
  - unread request and reply extents are released once;
  - the same service opens again after close;
  - a registration arriving after close is immediately aborted.
- [x] Add an idempotence table test. For each sequence below, assert empty
  `sessions`, `by_conn`, `pending`, `order`, an empty backend registry entry and
  exactly one release per extent:
  - `eof(request), close(request), close(request)`;
  - `eof(reply), close(reply), close(request)`;
  - `close(request), close(reply)`;
  - `detach_worker` after either close sequence;
  - terminal `dmesh_l7_send` failure followed by C close.
- [x] Extend `tests/l7_abi_contract_test.c` only if the C ABI changes. A Rust
  internal session token does not require a C ABI change.
- [x] Run:

  ```sh
  make test
  cargo +1.90.0 test --manifest-path linkerd/rust/Cargo.toml
  cargo +1.90.0 test --manifest-path \
    linkerd/port/linkerd2-proxy/Cargo.toml -p dmesh-doca \
    --no-default-features
  ```

### P0.2 Reconnect and churn validation

- [x] Deploy the exact working tree; do not reuse an older static archive:

  ```sh
  L7_BACKEND=linkerd \
  DPUMESH_L7_OPAQUE_SVC=11 \
  DPUMESH_ARM_WORKERS=1 \
  LINKERD_BACKEND_ADDR=10.96.0.11:9092 \
  BENCH_DEPLOY_SCOPE=core \
  ./bench/bench.sh deploy
  ```

- [x] Run at least ten sequential fresh-connection points:

  ```sh
  for i in $(seq 1 10); do
    ./bench/bench.sh point dpumesh 1024 8 1 3 100 1
  done
  ```

- [x] Run one connection-churn point. The last argument reconnects each client
  after that many completions:

  ```sh
  ./bench/bench.sh point dpumesh 1024 8 8 20 200 1 1000
  ```

- [x] Run concurrent sessions and inspect DPU logs:

  ```sh
  ./bench/bench.sh point dpumesh 1024 8 32 20 200 1
  ./bench/bench.sh dpulog 500
  ```

- [x] Reject the result if the log contains a new `single-session-limit`,
  `unknown-reply`, `stale upstream`, over-release or stray-release event.
- [x] Record opened/closed/reply counts at the start and end. After traffic
  quiesces, `opened == closed`, no session remains, and the backend registry is
  empty.

## P1 — make port-side task ownership explicit

The current endpoint abort wakes the task, but `linkerd/app/src/dmesh.rs` does
not own or await the spawned task. Complete this before allowing many sessions.

### P1.1 Replace tuple lifecycle messages

- [x] In `linkerd/doca/src/api.rs`, replace anonymous `usize` slots and the
  `(usize, DmeshIoHandle)` tuple with named types:

  ```rust
  #[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
  pub struct SessionToken {
      pub worker: u16,
      pub slot: u32,
      pub generation: u32,
  }

  pub struct Registration {
      pub token: SessionToken,
      pub handle: DmeshIoHandle,
  }
  ```

- [x] Change `DmeshEvent::ConnReady`, `ConnClosed` and `ConnError` to carry
  `SessionToken`. Update both the bundled `driver.rs` and embedded adapter.
- [x] Allocate monotonically increasing generations. Never reset generation on
  slot reuse. If a `u32` wraps, retire the slot until process restart.
- [x] In `linkerd/rust/src/lib.rs`, key `pending` by `SessionToken`, not by
  `usize`. Reject and abort a registration whose token is absent or stale.

### P1.2 Own spawned connection tasks

- [x] In `linkerd/app/src/dmesh.rs`, add a task table keyed by `SessionToken`.
  Store an `AbortHandle` or `JoinHandle` for every `svc.oneshot(io)` task.
- [x] On `ConnClosed(token)` or `ConnError(token)`:
  1. abort the associated `DmeshIoHandle` through the driver/adapter;
  2. signal or abort the matching service task;
  3. remove only the exact token from the task table;
  4. await task completion outside the event-dispatch critical section.
- [x] If a task completes first, remove its own exact token. A completion from
  generation N must not remove generation N+1.
- [x] When the event stream or drain signal ends, abort all endpoints, stop all
  tasks and await their joins before `serve` returns.
- [x] Add Tokio tests with a service future that deliberately waits forever.
  Verify `ConnClosed` cancels it and a reused slot does not cancel the new task.

### P1.3 Add closure observability

- [x] Add counters for active sessions, pending registrations, orphan
  registrations, aborted endpoints and live DMesh service tasks.
- [x] Export them through the existing Linkerd metrics registry; do not add a
  second metrics server.
- [x] Treat nonzero active/pending/task counts after a churn test as a test
  failure.

## P2 — choose the multi-session transport model

The adapter now uses an explicitly owned `Backends` registry keyed by
`BackendKey` and creates a complete outbound stack per session. The whole cache
namespace, rather than each stock Linkerd cache key type, is session-isolated.

### P2.1 Required trace

- [x] Trace one HTTP/2 and one opaque connection from `DmeshTarget` through the
  outbound stack to `DmeshOrTcp::call` in
  `linkerd/app/outbound/src/tcp/connect.rs`.
- [x] Record every cache key that can reuse a backend transport. Confirm
  whether two client `DmeshTarget`s with the same `OrigDstAddr` share the same
  physical `DmeshIo`.
- [x] Add a test with two simultaneous client connections to one service. The
  test must reveal whether the second connection calls `backend::take` or
  reuses the first transport.

### P2.2 Selected first implementation: session-isolated backend transports

Use one backend transport per DPUmesh client session first. This preserves the
current DPUmesh request/reply pairing and does not require a Host protocol
change.

- [x] Carry `SessionToken` in `DmeshTarget` into a per-session outbound-stack
  factory and bind the physical connector to that token.
- [x] Give every DMesh session a private discovery/protocol/endpoint/reconnect
  cache namespace. Do not change TCP cache keys used by the stock proxy path.
- [x] Replace the global registry key with:

  ```rust
  struct BackendKey {
      worker: u16,
      service: SocketAddr,
      session: SessionToken,
  }
  ```

- [x] Make `publish` reject duplicate live `BackendKey`s instead of silently
  replacing an endpoint.
- [x] Make `take` return a typed `NotPublished`, `AlreadyTaken` or `Stale`
  error. Do not fall through to TCP for a DMesh target when its registered
  backend is missing; that would bypass DPUmesh policy.
- [x] Closing generation N must evict its exact DMesh transport cache entry
  before generation N+1 is admitted. A normal close must not wait for the
  Linkerd reconnect backoff to discover `channel closed`.
- [x] Remove the `sessions.values().any(backend_addr)` guard only after the
  connector test proves two same-service sessions use distinct backend keys.
- [x] Add simultaneous same-service tests for open, reply attach, independent
  close and slot/generation reuse.
- [x] Add a sequential gRPC channel test that asserts the next session attaches
  its reply without a `linkerd_reconnect: ... channel closed` warning or a
  reconnect-delay interval.

### P2.3 Deferred alternative: shared backend transport

- [ ] Do not implement shared HTTP/2 backend pooling until DPUmesh has a
  backend-transport object independent of a client request connection.
- [ ] A shared design requires a new DPU-side backend transport ID, independent
  upstream-port lifetime, and response routing by Linkerd stream rather than by
  request connection. Version that ABI separately; do not overload the current
  `conn` handle.

## P3 — route all selected L7 traffic to its owner

### P3.1 Single Linkerd worker

- [x] Add one parsed `linkerd_worker_id` field to DPUmesh configuration. Validate
  it against `n_data_workers` once during initialization.
- [x] In the completion owner selection near `owner_worker` and
  `dmesh_worker_for_port` in `doca/dpu_worker.c`, route an L7 request service to
  the selected Linkerd worker before calling `l7_conn_open`.
- [x] Route its reply by conntrack's recorded L7 owner, not by upstream port
  hashing. Add `l7_worker_id` to `struct dpu_upstream` in `doca/object.h` if the
  existing owner is not recoverable.
- [x] Keep non-L7 traffic distributed by the existing port/ring policy.
- [x] Exercise `DPUMESH_ARM_WORKERS=2` and `4`; no selected L7 flow may report
  `DECLINE_NOT_ATTACHED`.

### P3.2 Multiple Linkerd workers

- [ ] Start only after P1 and P2. Remove the selected-worker gate from
  `build_worker` only after all registries and session keys include worker ID.
- [ ] Decide how Linkerd admin/listener ports are handled per worker. Either
  disable duplicate listeners for data-only worker stacks or build one shared
  control-plane component with worker-local outbound stacks.
- [ ] Keep the Tokio runtime and all session tasks on the owning worker. Do not
  use `tokio::runtime::Runtime::spawn` to move a `DmeshIo` between workers.
- [ ] Partition C conntrack, Rust session maps, backend registry entries and
  metrics by worker. Cross-worker messages may contain owned bytes or stable
  IDs, never `DmeshIo`, `DmeshIoHandle` or staging pointers.
- [ ] Accept the change only when worker CPU balance and per-worker session
  counts scale under same-service and multi-service traffic.

## P4 — remove hot locks only after profiling

### P4.1 Measure the current cost

- [x] Add counters or spans around `DmeshIo` lock acquisition in
  `linkerd/doca/src/io.rs`, the backend registry mutex in `linkerd/doca/src/lib.rs`
  and `pool_lock` in `doca/dpu_proxy.c`.
- [ ] Collect ARM `perf record` and `perf stat` for concurrency 1, 32 and 128.
  Attribute samples to `Mutex::lock`, atomic instructions, allocator calls,
  `DmeshIo::poll_read`, `poll_write`, `take_tx`, `pump_side` and Tokio wakeups.
- [ ] Build the same Rust revision on x86 and ARM. Compare normalized cycles per
  request, not only requests per second.
- [ ] Save compiler output around hot atomics with `objdump -dr` or
  `cargo-asm`; classify `LDAR/STLR`, exclusive RMW loops and full barriers.

### P4.2 Worker-local I/O state

- [ ] Preferred design: run the DMesh-specific stack on a Tokio `LocalSet` and
  change only its endpoint path to `Rc<RefCell<Inner>>`. Do not add new unsafe
  `Send` or `Sync` implementations.
- [ ] Relax `Send` bounds only on the DMesh acceptor/service specialization.
  Keep stock TCP Linkerd types and multithreaded builds unchanged.
- [ ] Replace `tokio::spawn` with `spawn_local` for DMesh connection tasks.
- [ ] If Linkerd generic bounds prevent this without broad changes, stop and
  retain the mutex. Do not replace it with `UnsafeCell` plus hand-written
  `Send/Sync`.
- [ ] Make borrow scopes short: no call into C, wake, service poll or task spawn
  while `RefCell<Inner>` is mutably borrowed.
- [ ] Re-run Miri-capable unit tests on x86 and all P0 churn tests on ARM.

### P4.3 Registry lock

- [x] After P2, make the backend registry worker-local. Access it only from the
  same current-thread runtime as its connector.
- [x] Remove `OnceLock<Mutex<HashMap<...>>>`; use an explicitly owned registry
  passed into the DMesh connector and adapter. This also prevents state leaking
  across tests and worker shutdown.

### P4.4 C pool lock

- [ ] Profile before modifying `pool_lock`. If it is hot, partition arena chunk,
  unit, piece and batch free lists by `px_worker_state`/`px_engine`.
- [ ] Give each worker a bounded local cache and use the shared pool only to
  refill or return batches. Keep ownership metadata so teardown can reclaim all
  caches.
- [ ] Run `worker_mpsc_queue_test`, `proxy_lane_queue_test`, DMA fault tests and
  TSAN before performance comparison.

## P5 — remove avoidable output copies

### P5.1 Eliminate the temporary `Vec`

- [x] Add a bounded `copy_tx_into(&mut [u8]) -> usize` operation to
  `DmeshIoHandle`; do not expose `Inner`.
- [x] In `pump_side`, call `dmesh_l7_tx_reserve`, copy directly from the tx
  queue into the returned arena chunk, and call `dmesh_l7_tx_commit`.
- [x] On commit `0`, cancel the reservation with commit length `0` and leave
  the source bytes queued. On a positive prefix, consume exactly that prefix.
  On negative result, close the session.
- [x] Keep `dmesh_l7_send` as a compatibility fallback and cover the real C
  reserve/cancel/commit/close paths in `proxy_lane_queue_test`.
- [ ] Measure the fallback and reserve/commit paths on hardware.

### P5.2 Direct AsyncWrite reservation

- [ ] Attempt this only after P5.1. A direct path must provide a worker-local
  egress reservation to `DmeshIo::poll_write`, copy the stack buffer once into
  the arena and commit it without the intermediate tx queue.
- [ ] Define cancellation for task drop, shutdown and partial write. At most one
  reservation may exist per connection, matching `px_conn::l7_tx_chunk`.
- [ ] `poll_write` returns `Pending` when no chunk is available and registers a
  wake that fires when the owning worker returns a chunk.
- [ ] Accept only if the new path reduces copy bytes and CPU without increasing
  p99 latency or arena stalls.

## P6 — production control plane and policy

- [ ] Replace mock destination, identity and policy addresses with deploy-time
  configuration. Keep mock binaries as test fixtures only.
- [ ] Define the DPU workload identity source. Continue using the identity
  granted during pod registration; never trust a payload-provided identity.
- [ ] Define certificate/token provisioning and renewal on the DPU, including
  restart behavior when identity is unavailable.
- [ ] Implement policy failure mode explicitly: fail closed for protected
  services or emit an auditable L4 fallback. Do not silently dial TCP from a
  missing DMesh backend.
- [ ] Add control-plane disconnect, certificate rotation, destination update
  and policy deny tests.
- [ ] Implement `decision` mode only if its no-payload verdict semantics are
  needed. Keep it separate from stream termination.
- [ ] Treat inbound proxying and cross-node RDMA as separate milestones. Neither
  is required for the current outbound node-local path.

## P7 — gRPC validation

The gRPC runtime does not need a new thread model for Linkerd. Validate it as a
Host API client after each DPU-side lifecycle or protocol change.

- [x] Build and run the integration tests:

  ```sh
  make lib
  cmake -S integrations/grpc -B build/grpc \
    -DDPUMESH_GRPC_SOURCE_DIR=/path/to/grpc-v1.80.0 \
    -DDPUMESH_GRPC_ENABLE_SANITIZERS=ON \
    -DBUILD_TESTING=ON
  cmake --build build/grpc -j2
  ASAN_OPTIONS=detect_leaks=0 \
    ctest --test-dir build/grpc --output-on-failure
  ```

- [x] Repeat in a separate TSAN build with
  `-DDPUMESH_GRPC_ENABLE_TSAN=ON`.
- [x] Add an end-to-end gRPC churn case that repeatedly creates/destroys
  channels to one L7 service while keeping one `DmeshRuntime` per process.
- [x] Add concurrent unary RPCs on multiple channels to the same service. This
  is the release gate for P2 multi-session support.
- [x] Add HTTP/2 channel tests covering backend FIN, client FIN, graceful
  GOAWAY and reconnect.
- [x] Add a real-DPU process shutdown test for a long-lived HTTP/2 channel.
- [x] Verify `DmeshRuntime::stats()` bounds remain zero or explicitly expected;
  Linkerd work must not create a second Host EQ/reactor or callback executor.

The local churn and concurrent-channel cases use real gRPC channels with the
fake native QP callbacks. Real QP and DPU lifecycle coverage remains part of
P0.2 and the final hardware gate.

## P8 — ARM/x86 runtime decision

- [ ] Compare the same Linkerd feature set, control-plane fixtures, request
  sizes, concurrency, compiler version and optimization flags on ARM and x86.
- [ ] Report cycles/request, instructions/request, cache misses, migrations,
  context switches, p50/p99 and throughput. Report clock frequency and CPU
  affinity with each result.
- [ ] Run stack slices: `DmeshIo` only, Tokio runtime plus `DmeshIo`, Linkerd
  opaque stack, HTTP detection, policy, identity/mTLS, then the full stack.
- [ ] Use the slices to identify whether the widening ARM gap is caused by
  atomics, cryptography, allocator traffic, cache locality, syscalls or extra
  Linkerd layers.
- [ ] Do not propose a C++ replacement for Tokio as a fix for Linkerd's Rust
  atomics. Linkerd futures still require a Rust async executor; a C++ loop can
  only replace the external DPUmesh progress backend.
- [ ] Consider a different runtime only if profiles attribute a material share
  to Tokio scheduling itself and a prototype preserves wake, timer, I/O and
  cancellation semantics. Require at least 10% improvement in the target metric
  with all correctness gates passing.

## P9 — build and deployment validation

- [x] Keep generated PNG/PDF diagrams reproducible through the checked-in
  generator scripts. Regenerate only when the implemented model changes.
- [x] Run format checks, Rust tests, `make test` and local gRPC ASAN/TSAN tests
  after the implementation stage.
- [x] Run DPU deployment and P0 reconnect/churn validation on the target
  hardware.

## Final completion gate

- [x] Ten sequential native reconnect points pass.
- [x] Native and gRPC churn pass without timeout or leaked session/task.
- [x] Multiple same-service sessions pass after P2.
- [x] Every configured L7 flow reaches its owning worker without fallback.
- [x] Opened and closed session totals balance after quiescence.
- [x] No custody, stale-generation, over-release, stray-release, drop or reorder
  error is present.
- [ ] ARM performance is reported from valid runs and compared against the
  frozen pre-change baseline.
- [x] Current implementation documents and generated diagrams match the code.
