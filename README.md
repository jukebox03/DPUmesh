# DPUmesh

DPUmesh is a BlueField service mesh built with DOCA Comch, DPA, DMA, and an
embedded Linkerd proxy. Applications address a Kubernetes Service; the DPU owns
policy, discovery, backend selection, connection state, and Host↔DPU transfer.
Each Service is deployed as an opaque byte stream or on Linkerd's
protocol-aware HTTP/1, HTTP/2 or gRPC path, independently of the surface its
Pods use. Endpoints on the node are reached by DMA, and endpoints on another
node by the authenticated peer channel. That channel's transport is built and
its TCP carrier runs in the host test. The current hardware deployment leaves
the peer carrier unset, and no deployment here has had a second node, so the
cross-node path has never run: a Service whose only replicas are elsewhere has
no route yet.

This repository is a research prototype. The design documents below define its
transport, proxy, control-plane, and API contracts.

## Architecture

A sending application writes into memory the transport registered with the DPU
and publishes a descriptor. The DPU reads those bytes, chooses the backend, and
writes them straight into the receiving application's registered memory. No copy
travels through the host kernel, and no proxy process runs beside either pod.

```text
           host                        BlueField DPU                      host
 +-----------------------+       +----------------------+       +-----------------------+
 | client app or gRPC    |       | DPA execution unit   |       | backend app or gRPC   |
 | libdpumesh.so.5       |       | ARM data worker      |       | libdpumesh.so.5       |
 | registered memory     |       | staging and routing  |       | registered memory     |
 +-----------------------+       +----------------------+       +-----------------------+

 1. forward ring   the client publishes a descriptor for the bytes it wrote
 2. DPA + ARM      the DPU stages those bytes and selects the backend
 3. DMA            a scatter-gather DMA writes them into the backend's memory
 4. reverse ring   completions report delivered bytes and return send capacity
```

### Terms

The rest of this repository uses these names. The first three are borrowed from
RDMA verbs, because the native API has the same shape.

| Term | Meaning |
|---|---|
| channel | one process's transport: its DPU registration, its registered memory, and its rings |
| QP | one full-duplex byte stream on that channel — the equivalent of a TCP connection |
| EQ (event queue) | what one application thread polls for the events of the QPs it owns |
| forward ring | host→DPU descriptor queue; a pod has `K` of them |
| reverse ring | DPU→host completion queue: bytes delivered, and send capacity returned |
| DPA | the BlueField data-path accelerator; its execution units (EUs) drain forward rings |
| ARM data worker | a DPU CPU thread that owns routing, DMA and reverse publication for its connections |
| staging | DPU memory holding arrived bytes until the DPU has finished sending them on |
| `N` / `K` / `A` / `L` | DPA execution units / forward rings per pod / ARM data workers / receive-side landing stripes |

The host exposes three integration surfaces — the native
`<dpumesh/dmesh.h>` API, a POSIX socket shim for libc binaries, and a gRPC C++
integration. [Using it from an application](#using-it-from-an-application) is
where to start; the rest of this section is what sits underneath all three.

The shared libdpumesh send core batches all three surfaces. `dmesh_alloc()`
reserves registered bytes and `dmesh_post_send()` commits them into one ordered
stream: complete transport units submit at once, and a partial tail is published
at a bounded deadline unless `dmesh_flush()` forces it earlier. The size of that
unit and its timing are internal, not application tuning parameters, and the
preload and gRPC layers keep no batch queue or timer of their own.

Every QP is one full-duplex byte stream into the DPU-hosted Linkerd proxy and
does not expose backend or proxy-session ids through native events. An opaque
Service enters Linkerd as a byte stream and a protocol-aware one as HTTP/1,
HTTP/2 or gRPC. Linkerd's selected backend is reached through DMA into that
Pod's registered memory, or across the peer channel when the generation places
it on another node.

Backpressure is nonblocking. `dmesh_alloc()` returning `NULL/EAGAIN` arms that QP
itself, and returned capacity produces one `DMESH_EVENT_TX_READY` on its EQ:
applications park the named write and retry it from the event. Readiness is a
one-shot hint, not a reservation. [design/API.md](design/API.md) is the contract.

## Lifecycle

Channel creation returns only after a replayable two-phase barrier:

```text
POD_REGISTER → POD_ASSIGNED → memory and ring import → all DPA RING_ADD_ACKs
             → POD_INIT_RESULT(READY, L)
```

A node agent's signed grant precedes every `POD_REGISTER`, and the DPU admits
only the Service that grant authorizes.
[design/CONTROL.md](design/CONTROL.md) is the contract.

The host retries registration while either assignment or readiness is pending;
the DPU treats identical registration as idempotent. Missing DPA add ACKs are
also retried. Graceful destruction similarly retries `POD_UNREGISTER` until the
DPU has removed every ring, finished every DMA that reads the pod's memory,
destroyed the mappings it imported, and replied `POD_QUIESCED`.

Those retries are phase-local. A ready channel does not periodically send
registration heartbeats, and unregister traffic starts only when channel
destruction begins. The steady-state data plane uses the imported rings and
reverse DMA path.

Each registration of a pod slot carries a generation number, so a DMA completion
that arrives late cannot be attributed to the slot's next occupant. A connection
is fenced the same way at its own scale: its close is acknowledged only once the
DPU has retired the proxy session, and its port leaves the pool until then, so a
new stream cannot arrive in the one it replaced. A DMA fault restarts that
worker's DMA engine without unpublishing healthy pods, and a current-generation
payload batch is retried once, in order. Removing a pod and
tearing its mappings down remains the control connection's decision.

## Repository

```text
include/dpumesh/       public C API
src/                   host core, native facade, resolver, preload facade
doca/                  BlueField ARM process and DPA kernel
controller/            cluster controller and the admission webhook
integrations/grpc/     gRPC C++ runtime, reactor, tests, benchmark
linkerd/               DPU-side L7 layer: adapter ABI, consumers, port submodule
bench/                 deployment, workloads, validators, measurement records
tests/                 fast host-only ABI and state-machine regression tests
design/                current API, data-plane, control-plane and gRPC whitepapers
```

## Using it from an application

An application addresses a Kubernetes Service by name and never names a backend.
Three surfaces reach the transport; which one to use follows from what the
application already is.

| The application is | Surface | Source change |
|---|---|---|
| an existing libc TCP binary | `libdmesh_preload.so` | none |
| new, or already event-loop shaped | `<dpumesh/dmesh.h>` | writes against the native API |
| gRPC C++ | `integrations/grpc` | channel and server bootstrap only |

All three register the same way, under the same signed grant, and share the same
send core. Mixing them in one process is not a supported arrangement: one
process holds one channel.

### No source change: the preload shim

The shim interposes the POSIX socket calls and routes the connections whose
destination resolves to a meshed Service; every other descriptor stays
kernel-backed. It hands the application a real kernel file descriptor, so
`epoll`, `poll` and `select` see ordinary readiness and an event loop needs no
change.

```sh
# a server: take over the port it listens on
LD_PRELOAD=/usr/local/lib/libdmesh_preload.so \
DPUMESH_SERVICE=echo-dpumesh DPUMESH_PORT=9095 ./my-server

# a client: connect() to the Service as usual, by DNS or ClusterIP
LD_PRELOAD=/usr/local/lib/libdmesh_preload.so ./my-client
```

`DPUMESH_SERVICE` names the Service this process serves; a client-only process
leaves it unset. `DPUMESH_PORT` is the listening port to take over, and its
absence means the process is not a server.

The shim reaches a binary only through `dlsym(RTLD_NEXT, …)`. A static link, or
a runtime that issues syscalls without going through libc — every Go program —
cannot be interposed, and those workloads need the native API instead.

### The native API

One channel per process, one event queue per polling thread, one QP per
byte stream. A QP is the equivalent of a TCP connection; the DPU chooses which
backend of the Service it lands on.

```c
#include <dpumesh/dmesh.h>

dmesh_channel_t *ch = dmesh_create_channel();              /* registers as $DPUMESH_SERVICE */
dmesh_eq_t      *eq = dmesh_create_eq(ch);                 /* one per polling thread */
dmesh_qp_t      *qp = dmesh_create_qp(eq, "echo-dpumesh"); /* a Service name, not an address */
```

Sending reserves registered bytes, fills them, and commits. The reservation is
the transport's own memory, so the committed bytes are never copied through the
kernel:

```c
void *tx = dmesh_alloc(qp, len);
if (tx == NULL) {
    /* EAGAIN: the window is full. Park this write and retry it when
     * DMESH_EVENT_TX_READY arrives for this QP. Readiness is a one-shot
     * hint, not a reservation. */
} else {
    memcpy(tx, payload, len);
    dmesh_post_send(qp, tx, len);   /* ownership transfers on success */
}
```

Receiving reads in place out of the RX mapping and returns the credit:

```c
dmesh_event_t ev[32];
int n = dmesh_poll_eq(eq, ev, 32);
for (int i = 0; i < n; i++) {
    switch (ev[i].type) {
    case DMESH_EVENT_RECV:                       /* ev[i].buf points into the RX mapping */
        handle(ev[i].qp, ev[i].buf, ev[i].len);
        dmesh_release_rx_buffer(ch, &ev[i]);     /* until this, the buffer is yours */
        break;
    case DMESH_EVENT_CONN_REQ:  accept_conn(ev[i].qp);      break;  /* server side */
    case DMESH_EVENT_RECV_FIN:  peer_closed(ev[i].qp);      break;
    case DMESH_EVENT_TX_READY:  retry_parked(ev[i].qp);     break;
    case DMESH_EVENT_TX_ERROR:  fail_conn(ev[i].qp);        break;
    }
}
```

A server does not listen. It creates a channel under its own
`DPUMESH_SERVICE` and takes the QP that arrives with `DMESH_EVENT_CONN_REQ`;
`ev.qp->user_data` is where per-connection context belongs.

To fold this into an existing event loop, `dmesh_eq_fd()` gives an eventfd to
add to `epoll` — drain it on wake, then poll until empty — and
`dmesh_eq_next_deadline_ns()` bounds the loop's own timeout so a buffered
transmit tail is not left waiting. Batching itself is library-owned:
`dmesh_post_send()` submits complete units immediately and publishes a partial
tail at a bounded deadline, and `dmesh_flush()` forces it earlier.
[design/API.md](design/API.md) is the full contract.

Build against the installed header and link `libdpumesh.so.5`:

```sh
cc app.c -I/path/to/include -ldpumesh -o app
```

### gRPC C++

Generated stubs, services, RPC semantics, metadata, deadlines and credentials
are unchanged. Only bootstrap differs: the channel target is a Service name
rather than an address, and the server takes connections from a
`PassiveListener` instead of binding a port.

```cpp
auto runtime = dpumesh::grpc::DmeshRuntime::Create(
    dpumesh::grpc::MakeNativeDmeshApiOps());

// client
auto channel = dpumesh::grpc::CreateDmeshChannel(
    *runtime, "echo-dpumesh", grpc::InsecureChannelCredentials(), args);
auto stub = Echo::NewStub(*channel);

// server
grpc::ServerBuilder builder;
builder.RegisterService(&service);
std::unique_ptr<grpc::experimental::PassiveListener> listener;
builder.experimental().AddPassiveListener(credentials, listener);
auto server = builder.BuildAndStart();
auto attachment = dpumesh::grpc::AttachDmeshGrpcServer(*runtime, listener.get());
```

Create one runtime per process and share it across every channel and the server
attachment. Link `grpc_dpumesh`; [design/GRPC.md](design/GRPC.md) covers the
build, the runtime options and the connection lifecycle.

### Making a workload meshed

One annotation, on the Namespace or the Pod template:

```yaml
metadata:
  annotations:
    dpumesh.io/inject: "enabled"
  labels:
    dpumesh-service: echo-dpumesh      # this Pod's Service; a client-only Pod has none
```

An admission webhook turns that into the access the transport needs: the DOCA
device, the transport library and the node agent's socket as mounts;
`DPUMESH_PCI_ADDR`, `DPUMESH_RINGS_PER_POD`, `DPUMESH_ATTEST_SOCKET` and
`DPUMESH_SERVICE` as environment, with `LD_PRELOAD` naming the shim for a
workload that is not linked against the native API; and the two Linkerd markers
that make the workload sidecarless. It also requires a node labelled `dpumesh.io/dpu=true`
and refuses the Pod when the cluster has none, because a Pod holding part of the
patch fails later and elsewhere.

The second Linkerd marker is `config.linkerd.io/skip-inbound-ports` on the data
ports. It is part of the data path, not a tuning choice: it keeps the
destination controller advertising the backend as unmeshed, and without it the
controller offers an endpoint that expects a second proxy, so sessions end
before carrying a byte. The webhook applies it and the control-plane label
together or applies neither.

A Pod that declines the shim — one written against the native API — annotates
`dpumesh.io/preload: disabled`. [bench/k8s/injected.yaml](bench/k8s/injected.yaml)
is a workload carrying nothing else; [bench/k8s/pods.yaml](bench/k8s/pods.yaml)
is the same access written by hand. The complete configuration surface — every
variable, flag and file, per consumer, including the two an author may set by
hand — is [design/CONTROL.md §5.5](design/CONTROL.md).

### What to expect from the mesh

- A Service's Pods must be on a node running `dpumesh_dpu`. That node serves the
  smaller of `MAX_PODS` (127, the wire ceiling) and its forward-ring supply —
  execution units times eight rings, divided by the K rings each Pod spans: on
  this BlueField 32 Pods at the benchmark's `K = 8`, 127 at `K = 2`.
- The deployment assigns each Service a protocol treatment, and the surface its
  Pods use does not decide it: an opaque Service is a byte stream, and a
  protocol-aware one takes Linkerd's HTTP/1, HTTP/2 or gRPC path. Policy,
  discovery and balancing run either way; per-request routing needs the
  protocol-aware path.
- On the protocol-aware path a backend is chosen per request, so one client
  channel spreads across a Service's endpoints. An opaque stream carries no
  message boundaries and stays on the backend it was pinned to.
- An `HTTPRoute` or `GRPCRoute` may reorder, filter, reject or redirect
  requests, including into another Service — weighted `backendRefs` are the
  ordinary canary shape. What guards the dial is the endpoint's liveness, and
  the inbound policy that grades the stream is the destination Pod's own.
- Traffic between Pods on one node is plaintext inside registered DMA mappings
  the workload cannot address. There is no per-hop proxy TLS. Confidentiality
  between nodes is the peer channel's mutually authenticated TLS 1.3 session,
  which is implemented but has never carried traffic between two nodes — a
  property of the code, not yet of a deployment.

Building the library and bringing up a cluster are covered in
[bench/README.md](bench/README.md).

## Documentation

Four design documents define the contracts, one report states what the
deployment measures, and one plan holds what is still open.

**Design**

| Document | Defines |
|---|---|
| [design/API.md](design/API.md) | the native `<dpumesh/dmesh.h>` contract: lifecycle, batching, EQ, errors |
| [design/DATA.md](design/DATA.md) | the data plane: host/DPA/ARM custody, rings, replay barriers, and the DPU-side Linkerd runtime that rides on them |
| [design/CONTROL.md](design/CONTROL.md) | the control plane at both scopes: naming, trusted registration, signed feeds, the Linkerd control plane, the cluster controller and the peer channel |
| [design/GRPC.md](design/GRPC.md) | the gRPC adapter: how it maps chttp2 onto the transport, how an application bootstraps against it, and its workloads |

**Measurement and work**

| Document | Covers |
|---|---|
| [bench/README.md](bench/README.md) | deployment, the experiment commands, the measurement rules, and the host-only and hardware validation gates |
| [bench/report/REPORT.md](bench/report/REPORT.md) | what the deployment measures: policy, routing, balancing, latency, throughput and cost |
| [PLAN.md](PLAN.md) | open function, defect and cost items, and what the campaigns established |
| [ci/README.md](ci/README.md) | which checks run where, and what each one protects |
