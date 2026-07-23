# DPUmesh Naming and Identity

DPUmesh separates application names from data-plane addresses. Applications use
configured logical Service names; Kubernetes Service names are one common
deployment convention, not a transport requirement. A process-local registry
translates names and optional socket destinations to compact service ids, while
the DPU assigns ephemeral backend slots at registration.

## 1. One registry, two facades

```text
ClusterIP:port    service-name    service-id
10.96.23.17:9091  echo-dpumesh    13
```

The native API resolves `service-name` in `dmesh_create_qp()`. The preload facade
resolves the IPv4 `ClusterIP:port` passed to `connect()` and falls back to kernel
TCP when no row matches. Both paths use `src/dmesh_resolve.c`; neither public
surface accepts an integer service id.

The registry path is `$DPUMESH_CONFIG`, defaulting to
`/etc/dpumesh/registry`. Blank lines, comments, and malformed rows are ignored.
`0.0.0.0:0` is a name-only entry usable by the native facade. The table is
loaded once under a mutex and then read without locks. It does not reload; the
complete file must exist before first resolution.

## 2. Local identity

`$DPUMESH_SERVICE` names the Service implemented by the current process. Unset
means client-only. A matching name is translated to the internal id carried in
`POD_REGISTER`. A nonempty unknown name is logged and leaves the process
client-only rather than inventing an identity.

The host never chooses its pod id. The DPU assigns a live slot, returns
`POD_ASSIGNED`, and associates a monotonically increasing DMA generation with
that slot. Replayed registration on the same Comch connection returns the same
assignment. The id is observable through `dmesh_pod_id()` only after the later
readiness barrier completes.

`$DPUMESH_PORT` is used only by the preload listener facade. Native servers and
the gRPC PassiveListener receive inbound QPs as `DMESH_EVENT_CONN_REQ` events;
they do not bind a numeric transport port through the native API.

## 3. Routing meaning

A QP names a service, not a backend. In default L4 passthrough, its first data
causes the DPU to select one ready backend and the resulting byte stream remains
pinned. An optional per-service codec can delimit frames and select an upstream
for each request frame. Backend identities and upstream ports remain internal;
the public QP continues to expose one response byte stream without a stream id.

The DPU derives a service's current backend set from live registered pod slots.
A pod participates only after `POD_INIT_RESULT(READY)` and is removed from
routing as soon as unregister or disconnect clears its live state. VMs,
bare-metal processes, and pods can join or leave a configured Service without
rewriting the registry. Backend loss terminates pinned L4 streams; new
connections select from the current live set.

## 4. gRPC authority is separate

The C++ client target is a configured Service name passed to each QP creation
attempt. It is not an IP address or gRPC resolver URI.

HTTP/2 `:authority`, TLS SNI, and certificate identity remain
application-layer values. An explicit `GRPC_ARG_DEFAULT_AUTHORITY` is preserved;
an absent value defaults to the target.

The per-channel EventEngine creates a targeted QP for each `Connect`. The server
uses the experimental `PassiveListener` endpoint-injection API.

The repository implements endpoint injection, not a global
`dpumesh:///service` resolver. Generated protobuf code, stubs, handlers, and RPC
methods are unchanged; only client/server bootstrap chooses the DPUmesh runtime.

## 5. Control-plane boundary

This repository includes a static registry and deployment tooling, not an
orchestrator controller. It does not implement admission identity, EndpointSlice
watching, registry reload, or workload identity. Registry consistency and
`$DPUMESH_SERVICE` injection are deployment invariants. Dynamic instances of a
configured Service are supported; new Service names require registry updates.
DPUmesh readiness is established by its initialization barrier.
