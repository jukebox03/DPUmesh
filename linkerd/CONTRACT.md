# DPUmesh–Linkerd Interface Contract

This document describes the interface implemented by the current DPUmesh and
embedded Linkerd build. The normative definitions are
`linkerd/include/dmesh_l7.h`, `doca/dpu_worker.c`, `doca/dpu_proxy.c` and
`linkerd/rust/src/lib.rs`.

## Component boundary

DPUmesh owns all transport and hardware state. The Linkerd static library owns
the per-worker Rust runtime, `DmeshIo` endpoints and outbound proxy tasks.

| DPUmesh | Linkerd static library |
|---|---|
| DOCA device and progress engines | Tokio `current_thread` runtime |
| DPA and DMA rings | persistent async driver |
| ARM worker threads and affinity | `DmeshIo` endpoint pairs |
| connection routing and conntrack | Linkerd outbound stack |
| staging custody and sender credits | adapter session state |
| egress arena and SG-DMA | control-plane clients |

The Linkerd build uses `dmesh-doca` with `default-features = false`. Its C
datapath, DOCA initialization and standalone `Driver` are not linked into
`libdmesh_l7.a`.

## Thread contract

Each DPUmesh data-worker thread calls:

```c
int l7_worker_run(int worker_id, void *driver);
```

`driver` remains valid until the worker stop flag is set. The call is blocking
and returns after the persistent runtime exits. Adapter state is thread-local;
all connection calls for a worker execute on that same worker thread.

`DPUMESH_L7_LINKERD_WORKER`, default `0`, selects which workers own Linkerd
session state: a worker id names one, and `all` names every ARM data worker. A
worker without session state runs the DPUmesh runtime backend and returns
`DMESH_L7_DECLINE_NOT_ATTACHED` for Linkerd session opens. DPUmesh validates the
value against the ARM worker count at initialization.

With one selected worker, the completion of every L7-carrying request service is
routed to it. With `all`, no worker declines, so requests keep the port policy
and spread. A reply is routed by its upstream port either way, which is
allocated in its owner's residue class. Traffic for services the L7 layer does
not carry always keeps the port policy.

Under `all`, each worker holds a complete proxy: its own control-plane clients,
session slots, backend registry and metrics registry. Worker `n` serves its
admin endpoint at the configured port plus `n`, the other listeners already
being ephemeral.

## Session identity

A session is named by a token, internal to the Rust side and not part of the C
ABI:

```rust
pub struct SessionToken { worker: u16, slot: u32, generation: u32 }
```

A slot is reused; its generation is not. Registrations, lifecycle events, task
ownership and backend-registry keys all carry the token, so an event from a
closed session cannot bind to the session now holding its slot. A slot whose
generation space is exhausted is retired rather than issued again.

The acceptor owns one task per session. `ConnClosed` and `ConnError` cancel
exactly the task their token names and are awaited outside event dispatch; when
the event stream or the drain signal ends, every task is cancelled and joined
before the acceptor returns.

## Persistent runtime backend

DPUmesh implements the backend consumed by `dmesh_doca::runtime::run`.

| Entry point | Contract |
|---|---|
| `dmesh_l7_driver_notification_fds` | return completion, optional DMA and wake fds |
| `dmesh_l7_driver_arm` | arm progress-engine notifications and mark the worker parked |
| `dmesh_l7_driver_drain` | perform bounded DPUmesh progress and return `Idle`, `Pending` or `Progressed` |
| `dmesh_l7_driver_clear_notifications` | clear PE notifications, drain the wake fd and mark the worker active |
| `dmesh_l7_driver_maintenance` | run the 1 ms worker maintenance action |
| `dmesh_l7_driver_stopped` | expose the DPUmesh worker stop state |
| `dmesh_l7_driver_ready` | publish successful worker initialization |
| `dmesh_l7_driver_failed` | publish failed worker initialization |

The runtime drains before arming and drains again after arming. It waits on the
backend fds, Linkerd output wakers and the maintenance deadline. It supplies a
per-queue drain budget of 64.

## Connection ABI

### Flow identity

```c
struct dmesh_l7_flow {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    int32_t  src_pod;
    int32_t  dst_service;
    int32_t  peer_pod;
    uint8_t  mode;
    uint8_t  is_reply;
    char     workload[384];
};
```

Addresses use host byte order. `workload` is NUL-terminated. The C and Rust
layouts are checked by unit and ABI tests.

The connection handle is:

```text
((uint64_t)(uint8_t)pod << 16) | port
```

It is an identifier, not a pointer.

### DPUmesh calls Linkerd

| Entry point | Result |
|---|---|
| `l7_conn_open` | `0` accepts; a negative decline code selects L4 fallback, or refusal under required registration |
| `l7_conn_segment` | accepted prefix in `[0, len]`; negative closes the adapter session |
| `l7_conn_eof` | closes the input half for the named direction |
| `l7_conn_close` | drops the session and releases all held extents |
| `l7_resolve` | decision verdict; the Linkerd consumer currently declines |
| `l7_report` | terminal accounting for decision mode |
| `l7_control_event` | one control-plane admission outcome by kind and reason |

`l7_conn_segment` may retain the accepted prefix after the call. The staging
memory remains valid until the matching release.

### Linkerd calls DPUmesh

| Entry point | Result |
|---|---|
| `dmesh_l7_backends` | live backend pod identifiers for a service |
| `dmesh_l7_send` | accepted output prefix; `0` is retryable backpressure |
| `dmesh_l7_tx_reserve` | writable egress-arena region or `NULL` |
| `dmesh_l7_tx_commit` | publishes a reservation or returns it unused |
| `dmesh_l7_release` | returns staging custody and sender credit |
| `dmesh_l7_verify_feed` | signed prefix of an authoritative feed document, or `-1` |

`DMESH_L7_BACKEND_ANY` delegates backend selection to the DPUmesh balancer.
`DMESH_L7_ORIGIN` sends bytes to the source of the request connection.

Output takes the reservation path: the adapter copies queued bytes straight from
the endpoint into the chunk `dmesh_l7_tx_reserve` lends and publishes it with
`dmesh_l7_tx_commit`. A commit of `0` cancels the reservation and leaves the
bytes queued; a negative result closes the session. `DMESH_L7_TX_RESERVE=0`
selects the `dmesh_l7_send` path, which copies through a temporary buffer, and
exists so the two can be compared on hardware.

## Backend channels

A session publishes the endpoint that provides its service into its worker's
registry, keyed by

```rust
struct BackendKey { worker: u16, service: SocketAddr, session: SessionToken }
```

The registry is owned by the worker, not global: the adapter, the acceptor and
the outbound connector of one worker share one instance, and no lock is shared
between workers. `publish` refuses a duplicate live key. Exact-key `take`
answers `NotPublished`, `AlreadyTaken` or `Stale`; the outbound connector uses
`take_session`, because discovery may replace the original synthetic service
address with a concrete endpoint address. A close evicts its own key before the
next generation publishes.

The normal Linkerd stack continues to key discovery, protocol, balancer and
transport caches by destination. DMesh does not change those stock keys.
Instead, the DMA acceptor builds one complete outbound stack per
`SessionToken`; its connector is bound to that token and takes only the channel
owned by that token. The discovery-selected address is retained for routing
metadata and diagnostics, but cannot substitute another session's DMA channel.
All caches and reconnect state in that stack are therefore session-local. Two
sessions to the same service use distinct backend channels, and closing
generation N drops its stack and removes only its registry key before a reused
slot publishes generation N+1.

This is the session-isolated model. Sharing one backend HTTP/2 transport across
several frontend sessions would still require a DPU-side transport identity,
an independent upstream-port lifetime and response routing by Linkerd stream;
that is a separate ABI design.

The cache boundary is the same for detected HTTP/2 and opaque traffic:

```text
DmeshTarget(SessionToken)
  -> per-session Outbound::mk
       -> discovery cache (private to the stack)
       -> protocol cache (private to the stack)
       -> logical/concrete endpoint caches (private to the stack)
       -> reconnect service (private to the stack)
       -> DmeshOrTcp(session)
            -> Backends::take_session(session)
```

Two targets with the same `OrigDstAddr` never enter the same cache instance.
The registry tests publish two same-service keys and take them in the opposite
order, and also take a session whose discovery endpoint differs from its
original service address. Publication order and endpoint rewriting therefore
cannot select another session's channel.

A service DPUmesh has provided is never dialed over TCP. When its channel is
missing the connector fails the connection and counts it, because a stream on
the TCP path would run without the policy the mesh applied.

## Modes and decline codes

| Value | Mode |
|---|---|
| `1` | decision |
| `2` | opaque stream |
| `3` | protocol-aware stream |

The Linkerd consumer accepts modes 2 and 3.

| Code | Meaning |
|---|---|
| `-1` | adapter error |
| `-2` | no Linkerd state on this worker |
| `-3` | unsupported mode |
| `-4` | the session already holds a reply direction, or its backend key is live |
| `-5` | reply has no matching request session |

DPUmesh counts each decline by cause and reports the totals in the DPU log
whenever they move. `DPUMESH_L7_FAIL_CLOSED=1` refuses a declined connection
instead of forwarding it; the default forwards it through the L4 path, which is
auditable but carries no policy.

## Custody and ordering

An accepted arrival segment is owned by the adapter until
`dmesh_l7_release(worker_id, conn, pos, len)`. Releases are in handoff order and
cover each accepted byte once. DPUmesh keeps the source staging mapping and
sender slot valid during this interval.

`dmesh_l7_send` accepts a prefix. The adapter restores an unaccepted suffix to
the front of its tx queue. Output for each connection is published in order.
Closing either transport direction closes the paired session. Teardown aborts
both endpoints, discards their queued input and output, releases all outstanding
input, then removes session and backend-registry state. A late endpoint
registration for a closed session is aborted.

On pod disconnect, session close is executed by the ARM worker that owns the
connection, not by the control thread. All workers publish producer quiescence
before egress lanes may become reclaimable. A second PE-progress fence after
lane quiescence covers DOCA task buffer references released after completion
callbacks return; `POD_QUIESCED` cannot precede that fence.

## Host transport ABI

The L7 runtime does not alter the Host↔DPU protocol.

| Control message | Direction | Function |
|---|---|---|
| `REG_CHALLENGE` | DPU→Host | fresh connection nonce; required-mode bit |
| `WORKLOAD_GRANT` | Host→DPU | node-agent-authorized Pod and Service claims |
| `POD_IDENTITY` | Host→DPU | development-only reported workload |
| `POD_REGISTER` | Host→DPU | pod and service registration |
| `POD_ASSIGNED` | DPU→Host | assigned pod and landing stripes |
| `MMAP_EXPORT` | Host→DPU | ring and buffer mappings |
| `POD_INIT_RESULT` | DPU→Host | data-path readiness |
| `POD_UNREGISTER` | Host→DPU | stop admission and begin quiescence |
| `POD_QUIESCED` | DPU→Host | remote mapping references are gone |
| `REV_DOORBELL` | DPU→Host | reverse-ring wake |

The forward descriptor is 64 bytes. The reverse completion entry is 32 bytes
and carries `DONE` and `TX_ACK` records. Published fields and structure offsets
are guarded by static assertions.

## Build contract

The Linkerd archive must:

- export `l7_worker_run`, the connection API, `l7_resolve` and `l7_report`;
- leave every `dmesh_l7_driver_*` symbol undefined for DPUmesh to provide;
- contain no undefined `dmesh_doca_*` datapath symbol;
- build with the pinned Rust toolchain and lock files.

Meson links the archive with `-Dl7_backend=linkerd` and defines
`DMESH_L7_RUNTIME_OWNER`. The null consumer uses the C attach/step/detach loop
and does not define this macro.

## Control plane and identity

Destination, policy and identity addresses, the identity directory, the token
file and the trust anchors are startup configuration
(`LINKERD_DST_ADDR`, `LINKERD_POLICY_ADDR`, `LINKERD_IDENTITY_ADDR`,
`LINKERD_IDENTITY_DIR`, `LINKERD_TRUST_ANCHORS`). A deployed Linkerd control
plane is mandatory. Missing addresses, identity material or a versioned Service
target feed fail preflight; there is no in-process or benchmark mock fallback.

The workload in `struct dmesh_l7_flow` is bound to the Pod registration and a
payload never supplies it. In required mode, a root-owned Host agent resolves
the Unix peer PID/cgroup to Kubernetes Pod and Service objects and signs a
connection-nonce-bound grant. The DPU selects the key by the signed key id,
verifies the HMAC, lifetime, issuer, nonce and exact Service id, then constructs
the workload from signed namespace/Pod claims and retains the signed Pod UID
with the registration. A Pod whose (Pod UID, Service) pair leaves the
authoritative node membership generation has that exact registration closed.
Both feeds are signed by the registration keyring; the adapter verifies the
Service target feed through `dmesh_l7_verify_feed` and parses only the signed
prefix, so it holds no key material of its own.
`DPUMESH_WORKLOAD` is accepted only in development mode.
Each DMesh outbound Policy Watch uses the flow value, not the process-wide
Linkerd fallback.

While identity is unavailable the proxy does not serve: sessions are opened,
their outbound connections fail, and each failure is counted. Nothing is
forwarded without the policy the service selected.

## Transport security

Node-local DMesh traffic is plaintext. A session's bytes travel Pod registration
memory, PCIe and DPU memory; what separates one Pod from another is the DPU's
per-Pod mapping and routing, not a wire an attacker could reach. The source
workload used for policy comes from the registration path. Required mode makes
it node-agent-attested, connection-bound authorization; development mode is
only reported attribution and must not protect a Service. Node-to-node mTLS
terminates on the DPU and is a separate milestone.

This is a discovery contract, not a proxy code path. `push_tcp_endpoint` layers
`tls::Client` and `TaggedTransport` above the connector, so an endpoint whose
metadata carries `tls_identity` or a tagged transport port is given a real
handshake and a transport header **over its `DmeshIo`**. A destination service
serving node-local backends must return neither for them. Returning them changes
the shape of the data path rather than its configuration — per-byte cryptography
on the ARM cores, and a header written into a stream whose far end is a pod, not
a proxy — and invalidates every performance figure collected without it. When
node-to-node arrives, the choice belongs to a per-service mode
(`intra-plaintext`, `intra-mtls`, `inter-mtls`), never to a global constant.

## Observability

The proxy's own metrics registry carries the `dmesh` counters: sessions opened,
closed and active, registrations pending and orphaned, endpoints aborted, tasks
live and cancelled, backend take errors and target mismatches, per-session stack
build phases, and retired slots. Refused sessions are counted by cause as
`dmesh_sessions_declined_total{reason}`, and registration, membership,
revocation and admission outcomes as `dmesh_control_events_total{kind,reason}`.
The control events are decided on the Comch control thread rather than on a
worker, so they are process-global and every worker's admin endpoint reports the
same values. After traffic quiesces, active sessions, pending registrations and
live tasks are zero.

## Current behavior

- outbound Linkerd proxying;
- one selected Linkerd worker, which every L7 request is routed to, or a proxy
  on every worker with requests kept on the port policy;
- concurrent session-isolated connections to the same service address;
- opaque and protocol-aware stream modes;
- DPUmesh backend selection through `DMESH_L7_BACKEND_ANY`;
- deployed Linkerd control-plane configuration through the host-network gateway;
- plaintext node-local transport, with identity granted at pod registration;
- L4 fallback with per-cause counters in development mode; required
  registration selects fail-closed refusal.
