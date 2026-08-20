# Linkerd on DPUmesh

This directory contains the DPU-side Linkerd consumer linked into
`dpumesh_dpu`. DPUmesh owns the Host transport and DPU data plane. The Linkerd
port runs its outbound stack over `DmeshIo` endpoints supplied through the L7
adapter ABI.

## Layout

```text
linkerd/
  include/dmesh_l7.h   C ABI
  shim/l7_null.c       reference consumer
  rust/                libdmesh_l7.a adapter
  port/
    linkerd2-proxy/    ported proxy submodule
      linkerd/doca/    DmeshIo, session registry, runtime backend contract
```

The execution model and the interface contract are documented in
[`design/DATA.md`](../design/DATA.md).

## Runtime

Each pinned DPUmesh ARM data worker calls `l7_worker_run`. The static library
creates a Tokio `current_thread` runtime on that thread and runs one persistent
driver over:

- the DPUmesh completion progress engine;
- the DPUmesh SG-DMA progress engine;
- the cross-worker wake fd;
- Linkerd output wakers;
- a 1 ms maintenance deadline.

`DPUMESH_L7_LINKERD_WORKER`, default `0`, selects which workers hold Linkerd
sessions: a worker id names one, `all` names every ARM data worker. Under `all`
each worker builds its own proxy and serves its admin endpoint at the configured
port plus its worker id.

## Ownership

| Component | Owner |
|---|---|
| Host API and Host↔DPU protocol | DPUmesh |
| DOCA, DPA, rings, progress engines and SG-DMA | DPUmesh |
| ARM worker threads and affinity | DPUmesh |
| current-thread runtime and persistent driver | Linkerd port |
| `DmeshIo` and outbound Linkerd stack | Linkerd port |
| adapter ABI | `include/dmesh_l7.h` |

Either transport direction closing removes the paired adapter session. Both
endpoints are aborted before unread staging custody is returned, and a late
endpoint registration is aborted.

## Build and deploy

The DPU build uses the pinned Rust toolchain and lock files.

```sh
L7_BACKEND=linkerd ./bench/bench.sh linkerdbuild
L7_BACKEND=linkerd ./bench/bench.sh build
```

`linkerdbuild` builds `libdmesh_l7.a` on the DPU. Its symbol and deployed
control-plane preflight checks the adapter/runtime boundary, identity material
and versioned Service target feed before Meson links the DPU binary.

Native opaque-stream deployment for the echo Service. A Service is named by
`namespace/name` everywhere; the node-local compact id is the DPU's own
interning of the topology generation and never appears in configuration:

```sh
L7_BACKEND=linkerd \
DPUMESH_L7_OPAQUE_SVC=test-bench/echo-dpumesh \
DPUMESH_ARM_WORKERS=1 \
DPUMESH_L7_FAIL_CLOSED=1 \
BENCH_DEPLOY_SCOPE=core \
./bench/bench.sh deploy
```

gRPC deployment for the gRPC echo Service:

```sh
L7_BACKEND=linkerd \
DPUMESH_L7_OPAQUE_SVC=test-bench/echo-grpc-linkerd-opaque \
DPUMESH_ARM_WORKERS=1 \
DPUMESH_L7_FAIL_CLOSED=1 \
BENCH_DEPLOY_SCOPE=grpc \
./bench/bench.sh deploy
```

Both deployments run a single-connection request/response validation after pod
registration. With a numeric worker selection, requests for a service the L7
layer carries are routed to that worker whatever the ARM worker count is. With
`all`, requests retain the ordinary port policy and can spread across workers.

## Configuration

```text
DPUMESH_L7_DECISION_SVC=<namespace/name list>
DPUMESH_L7_OPAQUE_SVC=<namespace/name list>
DPUMESH_L7_SVC=<namespace/name list>
DPUMESH_L7_LINKERD_WORKER=<worker id>|all
DPUMESH_L7_FAIL_CLOSED=0|1     (the default for a Service the generation
                                does not grade; a graded one carries its own)
DMESH_L7_TX_RESERVE=1|0
LINKERD_ADMIN_ADDR=127.0.0.1:4191
LINKERD_DST_ADDR / LINKERD_POLICY_ADDR / LINKERD_IDENTITY_ADDR
LINKERD_DST_NAME / LINKERD_POLICY_NAME / LINKERD_IDENTITY_NAME
LINKERD_LOCAL_NAME / LINKERD_WORKLOAD / LINKERD_DESTINATION_CONTEXT
LINKERD_IDENTITY_DIR / LINKERD_TRUST_ANCHORS
DPUMESH_L7_SERVICE_TARGETS_FILE=<signed versioned controller feed>
DPUMESH_MEMBERSHIP_FILE=<signed versioned node membership>
DPUMESH_ADMISSION_FILE=<open|drain switch the control thread polls>
```

`DPUMESH_L7_FAIL_CLOSED=1` refuses a connection the L7 layer declined instead of
forwarding it at L4. `bench/bench.sh` sets it and refuses to deploy with any
other value. `DMESH_L7_TX_RESERVE=0` selects the copy-then-send output path
instead of the egress reservation. It is a startup selection for comparing the
two, not an automatic fallback when the reservation path is out of chunks.

The destination, policy and identity addresses, identity material and the signed
Service target feed are required; preflight fails if any is missing. The feed is
verified against the registration keyring before any of it is parsed. The three
`*_NAME` values authenticate the control services and are deliberately distinct
from `LINKERD_LOCAL_NAME`, the identity certified for the DPU proxy.

Host Pods mount only `/run/dpumesh/attest.sock`. The node agent derives the
injector-shaped `{"ns":"...","pod":"..."}` workload and authorized Service
from peer cgroup and Kubernetes metadata; the application never supplies them.
Each session creates its Policy Watch with the registration-bound
workload.
`DPUMESH_L7_SERVICE_TARGETS_FILE` names the signed, monotonically versioned
Service and ready-endpoint feed. It presents real Kubernetes addresses to the
stock Linkerd control plane while the DPUmesh backend channel keeps its internal
synthetic key. An unsigned or stale generation, a target withdrawal or a
cross-Service selected endpoint fails closed. `DPUMESH_MEMBERSHIP_FILE` names
the node membership generation the verifier revokes against, and
`DPUMESH_ADMISSION_FILE` the switch that stops admitting protected sessions
without cutting the ones in flight.

[`bench/linkerd_identity.sh`](../bench/linkerd_identity.sh) creates the dedicated
`dpumesh-dpu` key/CSR, requests audience-bound Kubernetes ServiceAccount tokens,
installs root-only material atomically on the DPU and runs as a supervised
renewal agent. The proxy reloads the token file at every certificate refresh;
root/key/CSR replacement uses a drain and controlled restart.

The current DPU has no route to Kubernetes Service or Pod CIDRs. The
host-network node agent therefore carries a TCP pass-through relay
([`bench/linkerd_cp_relay.py`](../bench/linkerd_cp_relay.py)) on
management-link ports 28086–28088. Its dedicated Role can resolve only Linkerd
namespace Services and Endpoints; each new connection re-resolves the current
target. Traffic enters the control Pod's Linkerd inbound proxy and
control-plane mTLS remains end-to-end.

## Current bounds

- outbound opaque and protocol-aware streams;
- one selected Linkerd worker, which every selected L7 flow is routed to, or a
  proxy on every worker with requests kept on the port policy;
- concurrent connections to one service: each session owns a complete
  outbound stack and its connector takes the exact session backend key;
- DPUmesh-selected backend pod;
- deployed Linkerd control plane, reached through the node agent's
  endpoint-re-resolving relay;
- Linkerd output copied once, into the DPUmesh egress arena;
- signed authoritative feeds, with membership withdrawal closing the exact
  registration it names;
- declined sessions are refused rather than forwarded as plain L4.

## Submodules

Initialize the port with:

```sh
git submodule update --init linkerd/port/linkerd2-proxy
```

The DPU source sync keeps Rust build outputs on the DPU and updates the source
trees used by the next locked build.
