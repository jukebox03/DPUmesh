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
Host                                      DPU
  │── POD_IDENTITY(workload) ─────────────▶│
  │── POD_REGISTER(service_id) ───────────▶│
  │◀─ POD_ASSIGNED(pod_id, stripes) ───────│
  │── MMAP_EXPORT × regions ──────────────▶│
  │◀─ POD_INIT_RESULT(READY) ──────────────│
```

The DPU assigns the pod id and binds the workload, Service, imported mappings
and DMA generation to the registration connection. The pod enters backend
selection after `POD_INIT_RESULT(READY)`.

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

`POD_REGISTER` is a fixed 12-byte message. Forward and reverse descriptors use
fixed-width fields and compile-time layout assertions. Host and DPU endpoints
are little-endian.

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
`LINKERD2_PROXY_*` environment variables. The benchmark deployment defaults to
the port's mock services on the DPU:

```text
mock-destination  127.0.0.1:8089
mock-identity     127.0.0.1:8088
mock-policy       127.0.0.1:8087
```

With `LINKERD_MOCK_CONTROL_PLANE=0`, deployment instead requires
`LINKERD_DST_ADDR`, `LINKERD_POLICY_ADDR`, `LINKERD_IDENTITY_ADDR`, the identity
directory and trust anchors. Missing external configuration fails deployment;
the mock processes are test fixtures and are never an automatic fallback.

`LINKERD_BACKEND_ADDR` maps the selected Service to
`10.96.0.<service-id>:9092`. The adapter supplies each connection's workload and
synthetic source/destination addresses to the Linkerd outbound stack.

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
- backend membership is node-local and self-registered;
- service and pod identifiers occupy the signed one-byte wire space;
- benchmark deployments default to mock destination, identity and policy
  services; external control-plane addresses and identity material are
  deploy-time supported, while renewal and failure/update coverage remain P6;
- Linkerd sessions run on one selected ARM worker, or on every worker when
  `DPUMESH_L7_LINKERD_WORKER=all`;
- concurrent sessions to one service address are isolated, each owning its own
  outbound stack and backend channel;
- declined L7 sessions use counted L4 fallback.
