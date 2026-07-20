# DPUmesh Naming and Identity

DPUmesh separates application names from data-plane addresses. Applications use
Kubernetes Service names; a process-local registry translates those names and
socket destinations to compact service ids, while the DPU assigns ephemeral pod
ids at registration.

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
the gRPC PassiveListener receive inbound QPs as `DMESH_WC_CONN_REQ` completions;
they do not bind a numeric transport port through the native API.

## 3. Routing meaning

A QP names a service, not a backend. In default L4 passthrough, its first data
causes the DPU to select one ready backend and the resulting byte stream remains
pinned. Optional per-service codecs may route distinct messages to distinct
upstreams; in that case `wc.stream` distinguishes reply streams.

The DPU derives a service's current backend set from live registered pod slots.
A pod participates only after `POD_INIT_RESULT(READY)` and is removed from
routing as soon as unregister or disconnect clears its live state.

## 4. gRPC authority is separate

The C++ adapter passes an explicit DPUmesh Service name when creating a channel
or listener. That value chooses the transport destination. HTTP/2 `:authority`,
TLS SNI, and certificate identity remain application/security-layer values;
DPUmesh neither rewrites nor terminates them.

The repository implements endpoint injection, not a global
`dpumesh:///service` resolver. Generated protobuf code, stubs, handlers, and RPC
methods are unchanged; only client/server bootstrap chooses the DPUmesh runtime.

## 5. Control-plane boundary

This repository includes a static registry and benchmark deployment machinery,
not a Kubernetes controller. It does not implement admission-time identity,
EndpointSlice watching, live registry reload, or cryptographic workload
identity. Registry consistency and `$DPUMESH_SERVICE` injection are deployment
invariants. Kubernetes readiness and DPUmesh data-path readiness are distinct;
only the DPUmesh initialization barrier proves that the registered DMA path is
usable.
