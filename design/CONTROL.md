# DPUmesh Naming, Identity, and Control Plane

DPUmesh maps application Service names to compact transport identifiers. Pod
membership is established through the Host↔DPU registration protocol. The
embedded Linkerd consumer uses its own destination, identity and policy clients.

## Registry

The registry contains one mapping per configured Service:

```text
ClusterIP:port    service-name        service-id
10.96.23.17:9091  echo-dpumesh       13
0.0.0.0:0         echo-grpc-dpumesh  20
```

The native API resolves `service-name` in `dmesh_create_qp`. The preload facade
resolves the IPv4 destination passed to `connect` and uses kernel TCP when the
destination is absent. Both use `src/dmesh_resolve.c`.

The registry path is `$DPUMESH_CONFIG`, default
`/etc/dpumesh/registry`. It is loaded once per process. `0.0.0.0:0` defines a
name-only entry.

## Local identity and membership

`$DPUMESH_SERVICE` names the Service provided by the process. An unset or
unknown value creates a client-only channel.

```text
Host Pod                 trusted Host agent                    DPU
  │                               │                              │
  │◀──────────────────────── REG_CHALLENGE(nonce) ───────────────│
  │── nonce + requested service ─▶│                              │
  │                               │── SO_PEERCRED/cgroup ─┐      │
  │                               │◀─ K8s Pod/Service ────┘      │
  │◀─ signed immutable claims ────│                              │
  │───────────────────────── WORKLOAD_GRANT ────────────────────▶│
  │───────────────────────── POD_REGISTER(service_id) ──────────▶│
  │◀──────────────────────── POD_ASSIGNED(pod_id, stripes) ──────│
  │───────────────────────── MMAP_EXPORT × regions ─────────────▶│
  │◀──────────────────────── POD_INIT_RESULT(READY) ─────────────│
```

With `DPUMESH_TRUSTED_REGISTRATION=required`, the DPU creates a fresh 32-byte
nonce for each Comch connection. The root-owned agent identifies the Unix-socket
peer with `SO_PEERCRED`, resolves its host cgroup to the authoritative
Kubernetes Pod, verifies that the Pod labels select the requested Service, and
returns a canonical HMAC-SHA256 grant. It includes issuer/key id, issue/expiry,
grant id, nonce, Pod UID, namespace, Pod name, ServiceAccount, node and Service
id. The application can relay the grant but cannot change a claim.

The DPU verifies and consumes the grant once, rejects a Service mismatch, and
constructs the Linkerd workload JSON from the signed namespace and Pod. A grant
cannot move to another connection or survive reconnect/DPU restart because its
nonce is different. `DPUMESH_WORKLOAD` and `POD_IDENTITY` remain only in the
explicit `off`/development mode; required mode rejects them and never falls
back. The Pod enters backend selection after `POD_INIT_RESULT(READY)`.

Unregister or Comch disconnect removes the pod from selection. Each ARM worker
first closes the connection/L7 state it owns. After every producer has joined
that barrier, egress owners drain their lanes and run one further PE progress
pass to fence DOCA's post-callback buffer release. Only then does the control
thread destroy imported mappings and return `POD_QUIESCED`.

## Routing

A public QP names a Service. Backend pod and upstream port remain internal.

In L4 mode, the first data selects one ready backend and the connection remains
pinned. A Service assigned to an L7 payload mode is presented to the L7 adapter,
which publishes output through DPUmesh. `DMESH_L7_BACKEND_ANY` selects a backend
with the DPUmesh per-Service round-robin cursor.

The current backend set contains ready pods registered on the same DPUmesh node.

## gRPC authority

A gRPC client target is the DPUmesh Service name supplied to each connection
attempt. HTTP/2 `:authority`, TLS SNI and certificate identity remain gRPC
values. `GRPC_ARG_DEFAULT_AUTHORITY` is preserved; otherwise the target is used.

The C++ integration creates a targeted QP for each EventEngine `Connect`. The
server receives QPs through the experimental `PassiveListener` endpoint
injection API. Protobuf messages, stubs and handlers are unchanged.

## Identifier spaces

| Identifier | Representation |
|---|---|
| pod id | nonnegative `int8_t` on the wire |
| service id | nonnegative `int8_t` on the wire |
| port | `uint16_t` |
| sequence | per-connection `uint16_t` |
| internal routing fields | `int32_t` |

`POD_REGISTER` is a fixed 12-byte message. The v1 grant is a 1090-byte canonical
message whose numeric fields are explicit little-endian bytes and whose text is
NUL-terminated and zero-padded. Forward and reverse descriptors use fixed-width
fields and compile-time layout assertions. Host and DPU endpoints are
little-endian.

## Control channels

DPUmesh uses two control inputs:

| Input | State supplied |
|---|---|
| static Host registry | name, socket destination, service id |
| DOCA Comch | pod registration, mappings, readiness, teardown, doorbells |

The static registry does not reload. Dynamic instances of an existing Service
join and leave through Comch registration.

## Linkerd control plane

The Linkerd static library creates destination, identity and policy clients from
`LINKERD2_PROXY_*` environment variables. The complete current/target protocol
is defined in [LINKERD_CONTROL.md](LINKERD_CONTROL.md). Deployment requires the
stock Linkerd control plane, its three management-link gateway addresses,
provisioned identity material and a monotonically versioned Service target
feed. Missing configuration fails preflight; no mock control-plane path exists.

Internally, the selected Service remains keyed by
`10.96.0.<service-id>:9092`. The versioned controller feed maps service ids to
real Kubernetes `ClusterIP:port` discovery targets. The adapter atomically
applies only newer generations and rejects new protected sessions when a target
is withdrawn. Each session's Policy Watch uses the workload carried by its
attested registration rather than the process-wide fallback.

Control connections authenticate distinct service identities:
`LINKERD_IDENTITY_NAME`, `LINKERD_DST_NAME` and `LINKERD_POLICY_NAME`. Deployment
waits for each embedded proxy's `/ready` endpoint, which is released only after
the first identity certificate has been installed. Because the DPU has no route
to the Kubernetes Service/Pod CIDRs on the current testbed, the configured
addresses point to an mTLS-pass-through gateway on the Host management link.

The Linkerd consumer handles opaque and protocol-aware payload modes. Its
decision-mode entry point returns a decline, which DPUmesh counts before using
the L4 path.

## Node boundary

The current data plane is node-local. A backend is reachable through the Comch
attachment and imported Host mappings registered on the same DPU. DPUmesh does
not create DPU-to-DPU links or register mappings from another node.

## Thread placement

| Thread | Current work |
|---|---|
| Host application | API calls and QP operations |
| Host PE progress | reverse-ring drain and EQ readiness |
| Host tail timer | retained partial-send deadlines |
| ARM main | registration, teardown and Host doorbells |
| ARM data worker × A | DPA completions, routing, SG-DMA and reverse publication |
| DPA EU × N | forward-ring drain and completion metadata |

In a Linkerd build, each ARM data-worker thread hosts a Tokio `current_thread`
runtime and persistent driver. Linkerd session state belongs to the configured
worker, or to every worker under `DPUMESH_L7_LINKERD_WORKER=all`. The ARM main
thread remains the Comch control and doorbell owner.

## Current bounds

- configured Service names come from one static registry;
- backend membership is node-local; required mode admits only node-agent-signed
  Pod/Service membership, while development mode remains self-reported;
- service and pod identifiers occupy the signed one-byte wire space;
- deployments require Linkerd destination, identity and policy services;
  gateway addresses, TLS service names, dynamic per-service discovery targets
  and DPU identity material are supervised. The stock certificate loop renews
  certificates and reloads its token source while the identity agent rotates
  the audience-bound token atomically;
- Linkerd sessions run on one selected ARM worker, or on every worker when
  `DPUMESH_L7_LINKERD_WORKER=all`;
- concurrent sessions to one service address are isolated, each owning its own
  outbound stack and backend channel;
- declined L7 sessions use counted L4 fallback only when the explicit
  protected-service fail-closed switch is disabled.
