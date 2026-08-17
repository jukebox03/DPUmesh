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
  port/                port repository
    linkerd2-proxy/
      linkerd/doca/     DmeshIo, runtime backend API, standalone driver
  CONTRACT.md          implemented interface
```

The execution model is documented in [`design/L7.md`](../design/L7.md).

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

The embedded build disables `dmesh-doca`'s `own-datapath` feature. The
standalone port enables it by default.

Either transport direction closing removes the paired adapter session. Both
endpoints are aborted before unread staging custody is returned, and a late
endpoint registration is aborted.

## Build and deploy

The DPU build uses the pinned Rust toolchain and lock files.

```sh
L7_BACKEND=linkerd ./bench/bench.sh linkerdbuild
L7_BACKEND=linkerd ./bench/bench.sh build
```

`linkerdbuild` builds `libdmesh_l7.a` and the mock destination, identity and
policy binaries on the DPU. Its symbol preflight checks the adapter/runtime
boundary before Meson links the DPU binary.

Native opaque-stream deployment for registry service 11:

```sh
L7_BACKEND=linkerd \
DPUMESH_L7_OPAQUE_SVC=11 \
DPUMESH_ARM_WORKERS=1 \
LINKERD_BACKEND_ADDR=10.96.0.11:9092 \
BENCH_DEPLOY_SCOPE=core \
./bench/bench.sh deploy
```

gRPC deployment for registry service 20:

```sh
L7_BACKEND=linkerd \
DPUMESH_L7_OPAQUE_SVC=20 \
DPUMESH_ARM_WORKERS=1 \
LINKERD_BACKEND_ADDR=10.96.0.20:9092 \
BENCH_DEPLOY_SCOPE=grpc \
./bench/bench.sh deploy
```

Both deployments run a single-connection request/response validation after pod
registration. With a numeric worker selection, requests for a service the L7
layer carries are routed to that worker whatever the ARM worker count is. With
`all`, requests retain the ordinary port policy and can spread across workers.

## Configuration

```text
DPUMESH_L7_DECISION_SVC=<service ids>
DPUMESH_L7_OPAQUE_SVC=<service ids>
DPUMESH_L7_SVC=<service ids>
DPUMESH_L7_LINKERD_WORKER=<worker id>|all
DPUMESH_L7_FAIL_CLOSED=0|1
DMESH_L7_TX_RESERVE=1|0
LINKERD_BACKEND_ADDR=10.96.0.<service id>:9092
LINKERD_ADMIN_ADDR=127.0.0.1:4191
LINKERD_MOCK_CONTROL_PLANE=1|0
LINKERD_DST_ADDR / LINKERD_POLICY_ADDR / LINKERD_IDENTITY_ADDR
LINKERD_IDENTITY_DIR / LINKERD_TRUST_ANCHORS
```

`DPUMESH_L7_FAIL_CLOSED=1` refuses a connection the L7 layer declined instead of
forwarding it at L4. `DMESH_L7_TX_RESERVE=0` selects the copy-then-send output
path in place of the egress reservation. It is a startup compatibility and A/B
selection, not an automatic fallback when the reservation path is temporarily
out of chunks.

`LINKERD_MOCK_CONTROL_PLANE=1` starts the three mock servers before
`dpumesh_dpu` and points the proxy at them. With `0`, the destination, policy
and identity addresses and the identity material are required deploy-time
configuration and startup fails if any is missing.

## Current bounds

- outbound opaque and protocol-aware streams;
- one selected Linkerd worker, which every selected L7 flow is routed to, or a
  proxy on every worker with requests kept on the port policy;
- concurrent connections to one service: each DMesh session owns a complete
  outbound stack and its connector takes the exact session backend key;
- DPUmesh-selected backend pod;
- deploy-time control plane, with the mock servers as test fixtures;
- Linkerd output copied once, into the DPUmesh egress arena;
- L4 fallback for declined sessions, or refusal when fail-closed.

## Submodules

Initialize both repository levels with:

```sh
git submodule update --init --recursive linkerd/port
```

The DPU source sync keeps Rust build outputs on the DPU and updates the source
trees used by the next locked build.
