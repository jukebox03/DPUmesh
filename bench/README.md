# DPUmesh deployment and validation

This directory is the deployment surface for the supported architecture:

```text
native API ─┐
LD_PRELOAD ─┼─ Host↔DPU DMA ─ embedded Linkerd proxy ─ DMA/RDMA ─ backend
gRPC ───────┘
```

Native, preload, and gRPC workloads are application adapters over the same
DPU-hosted Linkerd mesh.

## Start here

There is one operator entry point: `./bench/bench.sh`. The rest of the tree is
grouped by role:

| Path | Purpose |
|---|---|
| `examples/` | minimal native API program; read this first |
| `apps/` | benchmark clients and long-lived echo servers |
| `k8s/` | controller, node-agent, workload and policy manifests |
| `system/` | one-time Kubernetes node prerequisites |
| `docker/` | images used by those manifests |
| `validators/` | API and transport correctness probes |
| `suite/` | repeatable benchmark and policy campaigns |
| `report/` | measured results, never runtime input |

The top-level controller, attestation, identity and feed files implement the
trusted deployment plumbing called by `bench.sh`; applications do not copy or
invoke them. For a first application, read
[`examples/hello_dpumesh.c`](examples/hello_dpumesh.c), then
[`apps/echo_dpumesh.c`](apps/echo_dpumesh.c) for a backpressured multi-connection
server. `apps/bench_dpumesh.c` is deliberately a load generator, not a minimal
API tutorial.

## One-shot deployment

Create the repository-root `.env` with the rig's connection settings, then:

```sh
BENCH_DEPLOY_SCOPE=all ./bench/bench.sh deploy
```

The command deploys `N/K/A/L=32/8/8/8`. Each Pod has one ring and one RX
landing stripe per ARM worker, and all eight workers host an embedded Linkerd
runtime. Port affinity keeps each connection, proxy session, DMA engine, and
reverse-ring producer on one worker. The harness labels its Kubernetes node
`dpumesh.io/dpu=true`, which is the admission webhook's scheduling contract.

Everything the command provisions or exports is the configuration surface
defined in [design/CONTROL.md §5.5](../design/CONTROL.md):
`workload_attest.sh` places the §5.5.4 key material and feed receiver,
`dpumesh_controller.sh` provisions and deploys the §5.5.3 daemons, and the DPU
launcher exports the §5.5.1 environment. `.env` holds only the rig — machines,
access, and the geometry this rig runs — so the harness is a worked example of
that section, not a second definition of it.

The command performs the complete lifecycle in order:

1. provisions the signed registration, topology, policy, identity, and service
   target feeds;
2. builds the Host library, preload shim, native workloads, and gRPC adapter;
3. builds the embedded Linkerd static library and the DPU binary;
4. starts the controller, node agent, admission webhook, DPU process, and API workloads;
5. pins the workloads to the BlueField-local NUMA node;
6. runs a real request through native, preload, and gRPC and fails deployment if
   any adapter does not traverse the embedded Linkerd path.

Narrow diagnostic scopes are `native`, `preload`, and `grpc`; each deploys the
corresponding application adapter over the same mesh.

## What a deployment needs

The DPU binary always links the embedded Linkerd adapter, so the deployment
brings up its control plane too:

- the pinned proxy fork, `git submodule update --init linkerd/port/linkerd2-proxy`;
- Linkerd destination, policy and identity services, reached through the node
  agent's authenticated management-link relay;
- DPU identity material and trust anchors;
- signed membership, topology, workload-scope and Service-target feeds — the
  Service-target feed is what presents real ClusterIPs and ready endpoint
  addresses to Linkerd;
- `DPUMESH_L7_FAIL_CLOSED=1`.

## Two-node RDMA configuration

The operator owns one node file. Each row is:

```text
<k8s-node> <dpu-rdma-ip>:<base-port> <agent-key-id> <agent-public-key> <dpu-public-key>
```

Do not copy a registration private key between nodes. Bootstrap each node, then
run this locally on that node and collect the two output rows into one file:

```sh
DPUMESH_NODE_NAME=jet1 \
DPUMESH_NODE_RDMA_ADDR=10.77.0.1:47900 \
  ./bench/dpumesh_controller.sh node-record
```

Use the equivalent command on `rapids4` with its own RDMA address, save the
rows as (for example) `/etc/dpumesh/nodes`, and set on the administrator host:

```sh
DPUMESH_NODES_FILE=/etc/dpumesh/nodes
DPUMESH_PEER_TRANSPORT=rdma
```

`dpumesh_controller.sh deploy` puts that file in one ConfigMap. The controller
and every node-agent DaemonSet Pod mount that exact ConfigMap; an agent selects
its row from `spec.nodeName` and may report only the DPU public key. It cannot
change the operator's address or agent identity.

The address must belong to the RDMA device used by that node's DPU. Both DPUs
must use the same `DPUMESH_ARM_WORKERS` and base port; worker `w` listens on
`base-port + w`. Before deployment, verify carrier/link state on both ends with
`rdma link show`, `ibv_devinfo`, and an RDMA-CM ping such as `rping`. These
checks require the physical port, switch/VLAN/PFC or RoCE routing, and DPU
ownership to have already been configured.

Steps 2 and 3 of the lifecycle above are also available on their own, for
rebuilding after a source change without redeploying:

```sh
./bench/bench.sh linkerdbuild     # the embedded Linkerd static library
./bench/bench.sh build            # host library, shim, workloads, DPU binary
./bench/bench.sh grpcbuild        # the gRPC workloads
```

## Supported workloads

| API | Client/server | Deployment |
|---|---|---|
| native | `bench_dpumesh` / `echo_dpumesh` | `bench-dpumesh`, `echo-dpumesh` |
| preload | `bench_sock` / `echo_sock` under `libdmesh_preload.so` | `preload-bench`, `preload-echo` |
| gRPC | `bench_grpc` / `echo_grpc` using the DPUmesh endpoint | `bench-grpc-dpumesh`, `echo-grpc-dpumesh`, `echo-grpc-alt` |
| HTTP/1.1 | `http1_bench` / `http1_echo` under `libdmesh_preload.so` | `http1-bench`, `http1-echo` |

The loopback, verbs, and preload validator deployments exercise supported API
contracts on the same DMA mesh. Extra native echo pods validate Service
fan-out and endpoint pinning. `echo-grpc-alt` is a second Service speaking the
same protocol, which is what a route crossing Services needs on both ends.
`echo_grpc` answers `INTERNAL` on every `BENCH_FAIL_EVERY`-th call, because a
retry policy and a circuit breaker are statements about a backend that fails.

The native and preload workloads preserve the same application submission
boundary. Each client submits one logical request per native post or POSIX
`write()`, and each server submits one logical response the same way. A partial
POSIX write retains only the unfinished suffix of that frame. Complete transport
units and partial-tail deadlines are formed exclusively by the shared native
transport batching policy below both adapters.

## Validation commands

```sh
./bench/bench.sh point dpumesh 1024 8 1 5 100 1
./bench/bench.sh point preload 1024 8 1 5 100 1
./bench/bench.sh point grpc-dpumesh 1024 8 1 5 100 1
./bench/bench.sh point http1 1024 8 1 5 100 1

./bench/bench.sh loopback
./bench/bench.sh verbs
./bench/bench.sh preload
./bench/bench.sh grpcshutdown
./bench/bench.sh l7metrics
```

The standard latency, bandwidth, and rate families accept exactly one of
`dpumesh`, `preload`, or `grpc-dpumesh` and write CSVs under
`$OUT` (default `/tmp/dpumesh-bench`). `http1` is a functional driver for the
protocol-aware path's HTTP/1 branch, not a capacity instrument.

## Function campaigns

What the control plane decides is judged separately from what the transport
carries, and by two readings: what the client completed, and what the DPU's own
counters say the enforcement point decided. Traffic that stops without a
matching verdict is not a policy result, and traffic that flows without one is
not a routing result.

```sh
./bench/suite/policy_route.sh [policy|route|cross|fanout|surfaces|lb|all]
./bench/suite/inject.sh
```

| Scope | Judges |
|---|---|
| `policy` | inbound authorization by caller identity and address |
| `route` | an `HTTPRoute` on the protocol-aware Service |
| `cross` | a route into another Service, and the destination's own policy |
| `fanout` | one client channel across two backends of one Service |
| `surfaces` | timeouts, retries, matching, `GRPCRoute`, route authorization, failure accrual, HTTP/1.1 |
| `lb` | connection grain, endpoint changes, and the request grain |
| `inject.sh` | one annotation meshes a workload, its absence leaves one alone, and a webhook that cannot answer creates no Pod |

A stage whose client returns no reply is recorded as `nodata` and fails: a
missing measurement is not a refusal. Stages that fail every request in flight
restart the gRPC client before the next stage reads anything, because the client
does not survive that churn indefinitely.

## Lifecycle invariants

- `deploy` is the only supported way to bring up meshed Pods. It starts the
  DPU and all selected Pods as one registration lifecycle.
- `restart` is only for a quiescent deployment with no meshed Pod.
- DPU admission is fail-closed. Applications receive no signing keys or
  authoritative placement claims.
- Linkerd policy, destination, and identity services are reached through the
  node-agent relay. Linkerd is linked into `dpumesh_dpu`; it is never injected
  beside a workload.
- Workload templates carry Linkerd's control-plane label for policy indexing
  and skip their DMA ports for Pod-local inbound interception; the DPU remains
  the sole proxy and enforcement point.
- `DPUMESH_L7_SVC` selects HTTP/1 and HTTP/2 services and
  `DPUMESH_L7_OPAQUE_SVC` selects opaque TCP services.
- Cross-node routing uses the configured `rdma` or diagnostic `tcp` peer
  carrier. A single-node deployment needs no peer carrier; a multi-node
  deployment must provide the operator node file described above.
- Meshed Pods may declare their own access or take it from the admission
  webhook; `bench/k8s/pods.yaml` is the first and `bench/k8s/injected.yaml` the
  second.

## Useful controls

```text
BENCH_DEPLOY_SCOPE=all|native|preload|grpc
BENCH_NUMA_POLICY=local|auto
BENCH_GRPC_BUILD=release|asan
```

`bench.sh pin [fair|native|preload|grpc|grpcmax]` places the workloads on the
benchmark cores. Pinning follows the PID, so anything that recreates a Pod —
`grpcshutdown`, a campaign stage that rolls a Deployment — drops it, and the
next point measures the scheduler instead of its subject. Re-pin before
believing a number taken after one of those.

Operational commands are `status`, `logs`, `dpulog`, `dpucpu`,
`armbalance`, `rotate-identity`, `admission`, and `cleanup`.
