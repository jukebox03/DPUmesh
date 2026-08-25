# O3 — is the backend registry lock ever contended? (2026-08-25)

## Question

Per-request backend selection made `Backends::take_session` run once per
(session, endpoint) instead of once per session, and PLAN O3 gated any
worker-local-state work on a measurement: nothing counted contention on the
registry's `parking_lot::Mutex`, so count it before concluding anything.

## Instrument

A temporary pair of process-global counters in the `Backends::registry()`
funnel — every lock acquisition, and every acquisition whose `try_lock` found
the mutex held — exported as `dmesh_backend_lock_acquisitions_total` /
`dmesh_backend_lock_contended_total` on the workers' admin endpoints. Every
`Backends` method passes through `registry()`, so the count is complete. The
counters were removed after this reading; the build under test was tree
`9d450b5` plus only that instrumentation.

## Readings

| point | acquisitions | contended | load |
|---|---:|---:|---|
| after deploy smoke gate | 11 | 0 | ~53 K requests, 3 arms |
| after session churn | 9,271 | 0 | native, reconnect every 60 completions: 205,332 requests, **3,081 sessions** in 25 s |
| after alternating gRPC | 9,291 | 0 | 50/50 weighted route, 292,247 requests in 25 s |

- The churn delta is 9,260 acquisitions for 3,081 sessions — almost exactly
  three per session (publish, take, remove), and ~0 per request across 205 K
  requests.
- The alternating-backend run added 292 K requests and **20** acquisitions:
  per-request selection reaches the registry only when a session meets a new
  endpoint; the per-unit admission question is answered by `px_conn_admitted`'s
  cache on the DPA/proxy side and never touches this lock.
- All eight admin endpoints report identical values (the pair is
  process-global), and `contended` never left zero.

## Verdict

**Not material.** Across ~550 K requests and 3,081 session builds the mutex
was taken 9,280 times and was never once found held. The lock is a
session-lifecycle cost of ~3 acquisitions per session, invisible next to the
0.4–0.5 ms a session costs in total (O1's subject), and no per-request path
touches it. The conditional half of O3 — a Tokio `LocalSet` + `Rc<RefCell>`
specialization — is unjustified, and the item closes.

If a future change makes take frequency per-request rather than per-endpoint,
this instrument is three small hunks: two counters in
`linkerd/doca/src/metrics.rs` and a `try_lock` fallback in
`Backends::registry()`.
