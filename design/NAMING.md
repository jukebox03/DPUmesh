# DPUmesh Naming and Identity

This document describes the name-resolution and identity behavior implemented in
the 2026-07-19 working tree. Applications name Kubernetes Services. Numeric
service ids and DPU-assigned pod ids remain below the public API.

## 1. Contract

```text
application name or socket address
              │
              ▼
      immutable registry row
ClusterIP:port  service-name  service-id
              │
              ▼
       DPU routing identity
```

The same registry drives both public facades:

- Native callers pass a Service name to `dmesh_create_qp()`.
- The preload facade looks up the destination `ClusterIP:port` supplied to
  `connect()`. A missing row means ordinary kernel TCP rather than DPUmesh.

The integer service id is produced inside `src/dmesh_resolve.c`. It does not
appear in application code, public API arguments, or Kubernetes workload YAML.

## 2. Registry

The resolver reads `$DPUMESH_CONFIG`, or `/etc/dpumesh/registry` when the variable
is unset. Each valid row has this form:

```text
10.96.23.17:9091 echo-dpumesh 13
0.0.0.0:0       name-only-service 27
```

`0.0.0.0` creates a name-only row that native clients can use but the socket
facade cannot match. Blank, comment, malformed, and non-IPv4 rows are skipped.
The fixed table holds at most 256 entries, names are at most 63 characters plus
the terminator, and service ids are expected to fit the data-plane service range.

The file is loaded once, on first resolution, under a mutex. Subsequent reads are
lock-free and the process never reloads the file. An absent file produces a
warning and an immutable empty table; adding the file later does not repair that
process. A deployment must therefore mount the complete registry before process
startup.

## 3. Local identity

`$DPUMESH_SERVICE` identifies the Service implemented by the current process.

- Unset or empty means intentionally client-only.
- A matching registry name yields the internal service id sent during channel
  registration.
- A nonempty name absent from the table emits a warning and starts client-only;
  the process is unreachable as a server.

The application never selects its pod id. The DPU reserves a free slot, returns
it in `POD_ASSIGNED`, and associates a generation with that slot. The channel is
usable only after the later `POD_INIT_RESULT(READY)`.

`$DPUMESH_PORT` supplies the meshed listening port used by the preload facade.
The native API receives inbound QPs through `DMESH_WC_CONN_REQ` and does not need
a numeric listen call. The C++ gRPC server similarly attaches native inbound QPs
to a gRPC `PassiveListener`.

## 4. Native path

```c
dmesh_channel_t *ch = dmesh_create_channel();
dmesh_cq_t *cq = dmesh_create_cq(ch);
dmesh_qp_t *qp = dmesh_create_qp(cq, "echo-dpumesh");
```

The call resolves the name at point of use. A missing name returns
`NULL/ENOENT`. QP construction itself is local; the first data and DPU routing
establish the remote stream. In default L4 mode that stream remains pinned to the
chosen backend for its lifetime.

## 5. POSIX preload path

An unmodified application may continue to use DNS and a ClusterIP:

```c
getaddrinfo("echo-dpumesh", "9091", ...);
connect(fd, cluster_ip, ...);
```

The preload facade compares the resolved IPv4 address and port with the same
registry. A match creates a DPUmesh QP; no match delegates to the original libc
socket path. `listen($DPUMESH_PORT)` represents this process's meshed listener,
and inbound QPs are returned through the interposed `accept()` behavior.

This interception is suitable only where networking flows through the
interposed libc ABI. It is not a reliable Go integration mechanism because the Go
runtime may issue network syscalls without libc.

## 6. gRPC names and authority

The C++ adapter currently passes an explicit DPUmesh Service name to
`ConnectDmeshGrpcChannel()`. That name selects the QP destination. gRPC's HTTP/2
`:authority`, TLS SNI, and certificate identity are independent L7/security
values; DPUmesh does not rewrite or terminate them.

The implemented endpoint-injection path does not install a global
`dpumesh:///service` resolver or a complete hybrid EventEngine. Therefore an
application changes only its channel/listener bootstrap, while generated protobuf
types, stubs, handlers, and RPC methods remain unchanged.

## 7. Kubernetes state in this repository

The benchmark harness generates or mounts registry content before starting the
pods and injects `$DPUMESH_SERVICE` into server pods. There is no controller,
admission webhook, live registry reload, or Kubernetes Endpoint readiness gate in
this repository. Consequently:

- registry/service-id consistency is an operator and benchmark-deploy invariant;
- a process does not observe registry changes after its first lookup;
- Kubernetes readiness alone does not prove that DPU DMA initialization reached
  `POD_INIT_RESULT(READY)`;
- workload identity is the injected Service name, not a cryptographic identity.

These are factual boundaries of the current implementation and must be included
when interpreting deployment or failure results.
