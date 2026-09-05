# DPUmesh deployment and validation

This directory contains the runnable deployment, application examples,
benchmark programs and hardware validation surface for DPUmesh. The supplied
one-node profile uses the native L4 data path:

```text
Kubernetes controller                 Kubernetes workload Pods
(read-only cluster objects)           (unprivileged, no token)
          │                                      │
          │ mTLS                         allocated Unix socket
          ▼                                      ▼
 host dpumeshd.service ── broker children ── Host↔DPU DMA ── DPU routing
          │                                      │
          └──── signed feed delivery ─────► BlueField Arm OS services
```

The DPU and `dpumeshd` are system services outside Kubernetes. Kubernetes runs
the controller and workloads. The DPU binary also contains the optional Linkerd
adapter; it starts only when an L7 Service list is configured.

## Directory map

| Path | Purpose |
|---|---|
| `native_deploy.sh` | complete one-node build, deployment and smoke test |
| `bench.sh` | DPU build/restart, geometry, diagnostics and native measurements |
| `dpumesh_controller.sh` | keys, node PKI, feed receiver and controller deployment |
| `examples/` | native C and gRPC C++ application examples |
| `apps/` | native, POSIX, HTTP/1 and gRPC workload programs |
| `k8s/` | controller and native hardware manifests |
| `docker/` | workload and controller images |
| `system/` | Kubernetes host prerequisites |
| `validators/` | native, POSIX and verbs correctness programs |
| `suite/deployed_geometry.sh` | live DPU geometry reader |
| `report/` | measurement evidence; never runtime input |

The native application contract is [`design/API.md`](../design/API.md), the
transport is [`design/DATA.md`](../design/DATA.md), and control/security is
[`design/CONTROL.md`](../design/CONTROL.md).

## Host prerequisites

Install the kernel settings in [`system/README.md`](system/README.md), disable
swap and provide a working Kubernetes node, containerd, Docker, DOCA SDK, SSH
to the paired BlueField Arm OS, and `kubectl`, `envsubst`, `rsync`, `nc`, `rg`
and OpenSSL. The host service uses cgroup v2 and requires systemd delegation of
`cpu`, `memory` and `pids`.

Copy [`.env.example`](../.env.example) to `.env` and set the rig values:

```text
DPU_HOST       SSH destination for the BlueField Arm OS
DPU_PASS       DPU sudo password used by the local harness
HOST_PASS      host sudo password used by the local harness
DPU_PCI        "-p <device> -r <representor>" for dpumesh_dpu
HOST_PCI       host-side DOCA PCI function
```

`.env` is optional for commands that do not touch the rig. Set
`DPUMESH_ENV_FILE=/absolute/path` to choose another file; an explicitly selected
file must be a readable regular file. Password variables are a test-rig
interface, not a service configuration or production secret-delivery pattern.
Progress is written to stderr and machine-readable command results to stdout.

## One-node deployment

Run the complete path with:

```bash
./bench/native_deploy.sh all
```

`all` performs these gates in order:

1. builds `libdpumesh.so.5`, `dmesh_broker`, native applications and the
   controller/workload images;
2. imports the images into the local containerd namespace;
3. creates separate topology, WorkloadGrant and feed keys plus node mTLS PKI;
4. installs and starts `dpumesh-feed-receiver.service` on the DPU;
5. configures kubelet system reservation and installs `dpumeshd.service`;
6. builds and starts `dpumesh_dpu` on the BlueField Arm OS;
7. applies the Restricted controller and workload manifests;
8. waits for both native workloads and requires a real DPU request to return
   `OK`, `fail=0` and `drops=0`.

The profile is fixed at `N/K/A/L=32/8/8/8`, two Device Plugin slots, reserved
host CPUs `0-2`, 3 GiB kubelet system memory and per-broker limits of 0.5 CPU,
768 MiB `memory.high`, 1 GiB `memory.max` and 64 PIDs.

Narrow operations reuse the same workflow:

```bash
./bench/native_deploy.sh build
./bench/native_deploy.sh deploy
./bench/native_deploy.sh smoke
./bench/native_deploy.sh status
```

`build` builds/imports images and the DPU binary. `deploy` provisions trust,
installs services and applies workloads. `smoke` performs readiness and real
byte-path checks against the deployed objects. `status` prints Kubernetes
Deployment state and the complete host service status.

## Workload contract

A workload image contains its selected adapter and `libdpumesh.so.5`. Exactly
one regular container requests and limits one channel:

```yaml
spec:
  automountServiceAccountToken: false
  securityContext:
    runAsNonRoot: true
    runAsUser: 65532
    runAsGroup: 65532
    seccompProfile: {type: RuntimeDefault}
  containers:
  - name: app
    image: registry.example/application-with-dpumesh:tag
    env:
    - {name: DPUMESH_SERVICE, value: echo}       # omit for a pure client
    - {name: DPUMESH_RINGS_PER_POD, value: "8"}
    securityContext:
      allowPrivilegeEscalation: false
      readOnlyRootFilesystem: true
      capabilities: {drop: ["ALL"]}
    resources:
      requests: {dpumesh.io/channel: 1}
      limits: {dpumesh.io/channel: 1}
```

Kubelet mounts only the assigned socket at `/run/dpumesh/channel.sock`. The Pod
receives no Kubernetes token, DPU device, PCI address, host directory, signing
key, privileged init container or added capability. The controller grants
`DPUMESH_SERVICE` only when its latest Kubernetes snapshot contains the Pod as
a ready selected endpoint of that Service.
The complete worked manifest is [`k8s/native-hw.yaml`](k8s/native-hw.yaml); the
minimal server is [`examples/k8s.yaml`](examples/k8s.yaml).

## Controller and trust provisioning

The controller helper exposes each stage independently:

```bash
./bench/dpumesh_controller.sh prepare
IMG_CONTROLLER=bench/dpumesh-controller:native \
  ./bench/dpumesh_controller.sh deploy
./bench/dpumesh_controller.sh node-record [NODE [RDMA_ADDRESS]]
./bench/dpumesh_controller.sh nodes-config
./bench/dpumesh_controller.sh topology-show
./bench/dpumesh_controller.sh receiver-status
./bench/dpumesh_controller.sh status
```

`prepare` creates or reuses three distinct root-owned keyrings, copies only
verification material needed by the DPU, and installs the unprivileged feed
receiver. `deploy` creates a TLS 1.3 server identity, a node client certificate
whose URI SAN is `spiffe://dpumesh.io/node/<node>`, Kubernetes Secrets and the
controller Deployment. The controller's ServiceAccount may only get/list Pods,
Services and EndpointSlices.

A node-file row is:

```text
<k8s-node> <dpu-rdma-ip>:<base-port> <grant-key-id> \
<grant-public-key> <dpu-public-key>
```

The operator supplies name, address and grant public key. `dpumeshd` reads the
DPU public key from the paired feed receiver and reports it over node mTLS. The
controller accepts that report only for the certificate's node and only when
name and RDMA address match the configured row.

## DPU build, geometry and measurements

`bench.sh` operates on the runtime and the deployed native benchmark pair:

```bash
./bench/bench.sh geometry
./bench/bench.sh build
./bench/bench.sh restart
./bench/bench.sh ping
./bench/bench.sh point REQ REPLY CONC DUR WARMUP THREADS [RECONNECT]
./bench/bench.sh latency
./bench/bench.sh bandwidth
./bench/bench.sh rate
./bench/bench.sh all
./bench/bench.sh dpulog [LINES]
./bench/bench.sh dpubanner
./bench/bench.sh dpucpu
```

`build` synchronizes DPU sources, builds the pinned Linkerd static library and
links `dpumesh_dpu`. `restart` starts that binary with the configured keys,
feeds and geometry. The native hardware profile sets both L7 Service lists
empty, so the linked L7 runtime is inactive.

The `point` command sends:

```text
RUN <request-bytes> <reply-bytes> <concurrency> <duration-seconds> \
    <warmup-requests> <threads> [reconnect]
```

A successful result begins with `OK` and includes throughput, latency,
`fail`, `drops` and transport statistics. `latency`, `bandwidth` and `rate`
write `latency_native.csv`, `bandwidth_native.csv` and `rate_native.csv` under
`OUT`, default `/tmp/dpumesh-bench`.

Geometry may be selected directly with `DPUMESH_DPA_THREADS=N`,
`DPUMESH_RINGS_PER_POD=K` and `DPUMESH_ARM_WORKERS=A`. For hot-service profiles,
`DPUMESH_THROUGHPUT_WORKERS=W` accepts 4, 6, 8 or 12, sets `K=A=W`, selects the
largest `N≤32` divisible by `W`, and selects all workers for L7. The host and DPU
must receive the same `K`. `bench.sh geometry` prints the resolved values;
`suite/deployed_geometry.sh` reads the live DPU banner.

## Two-node carrier configuration

Create one row per node with `dpumesh_controller.sh node-record`, combine the
rows into one operator-owned file, and set:

```bash
DPUMESH_NODES_FILE=/etc/dpumesh/nodes
DPUMESH_PEER_TRANSPORT=rdma
DPUMESH_PEER_BIND=10.77.0.1
DPUMESH_PEER_PORT=47900
DPUMESH_NODE_RDMA_ADDR=10.77.0.1:47900
```

Each node has its own WorkloadGrant private key and DPU static private key; do
not copy private keys between nodes. Both DPUs use the same Arm-worker count and
base port. Worker `w` listens at `base-port+w`. `DPUMESH_PEER_BIND` must resolve
to the RDMA device used by that DPU. Validate each physical/RoCE path with
`rdma link show`, `ibv_devinfo` and an RDMA-CM exchange before enabling the
carrier. A single-node deployment leaves `DPUMESH_PEER_TRANSPORT` unset.

## Application and adapter programs

| Surface | Programs | Role |
|---|---|---|
| native C | `hello_dpumesh`, `hello_dpumesh_server` | minimal lifecycle and byte stream |
| native benchmark | `bench_dpumesh`, `echo_dpumesh` | framing, concurrency, backpressure and metrics |
| POSIX preload | `bench_sock`, `echo_sock`, `tcp_client`, `tcp_echo` | ordinary socket applications for `libdmesh_preload.so` |
| HTTP/1 | `http1_bench`, `http1_echo` | protocol-aware POSIX workload |
| gRPC | `bench_grpc`, `echo_grpc`, `grpc_dpumesh_qps_benchmark` | EventEngine and PassiveListener integration |
| validators | `loopback_dpumesh`, `verbs_dpumesh`, `preload_runner` | transport and facade contracts |

The supplied hardware manifest deploys `bench_dpumesh` and `echo_dpumesh`.
Other binaries are buildable implementation surfaces and are packaged/deployed
by an application using the same explicit resource contract. gRPC build and use
are documented in [`design/GRPC.md`](../design/GRPC.md).

Native and POSIX programs preserve the same logical submission boundary: one
request or response enters one native post or POSIX `write`. Partial POSIX
writes retain only the unfinished suffix. The native transport alone owns
physical units, partial-tail deadlines, capacity reclamation and TX-ready
notification.

## Optional L7 configuration

Set namespace-qualified Services in `DPUMESH_L7_SVC` for HTTP/1, HTTP/2 or gRPC,
or `DPUMESH_L7_OPAQUE_SVC` for opaque byte-stream handling. A Service cannot be
in both lists. L7 requires `DPUMESH_L7_SERVICE_TARGETS_FILE`, reachable Linkerd
destination/identity/policy endpoints, the standard `LINKERD2_PROXY_*` identity
and trust inputs, and the controller-produced Service-target feed. Protected or
unknown L7 Services fail closed. Empty lists select the native L4 path and need
no Linkerd control endpoints.

## Validation and lifecycle

Use the following layers in order:

```bash
make test-hostfree
make DPUMESHD_PYTHON=build/dpumeshd-venv/bin/python test
./bench/native_deploy.sh smoke
ci/health-check.sh
```

The Make targets validate C/Python contracts, protocol layouts, topology,
WorkloadGrant signing, Device Plugin behavior, feed delivery, facades and
transport state machines. `native_deploy.sh all` installs and starts the
controller, host runtime, DPU services and workloads before requiring a live DPU
byte exchange. `ci/health-check.sh` records DPU geometry, deployed workloads and
client CPU affinity, then exercises the live native path without treating one
smoke latency as a performance series.

Lifecycle invariants are:

- `dpumeshd` advertises slots Healthy only while controller-to-DPU delivery and
  node registration succeed;
- one slot generation owns at most one connected workload and one broker;
- broker exit completes DPU unregister/quiescence before the slot is reused;
- a DPU or controller delivery failure closes admission for new allocations;
- workloads never receive authority beyond their allocated socket;
- restarting `dpumeshd` terminates its direct broker children through the
  systemd control group;
- L7 selection changes only DPU processing; no workload-side proxy is added;
- measurement output is retained only with its exact geometry, deployed object
  set and separated Pod/host-service/DPU CPU accounting.
