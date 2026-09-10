# DPUmesh deployment

Host and DPU join the same Kubernetes cluster as separate nodes. This is the
single supported placement:

| Component | Runs as |
|---|---|
| Application | unprivileged Pod on a host node |
| DPU runtime | privileged DaemonSet Pod on each paired DPU node |
| DPUmesh controller | unprivileged Deployment on a designated node |
| Node admin (dpumeshd) | root systemd service on each host |
| Per-pod broker | child process on the application's host |
| Feed receiver | unprivileged sidecar in the DPU runtime Pod |

Node admin contains the Device Plugin, kernel evidence reader, read-only API
client, feed relay, scope tunnel and broker supervisor. Its service and broker
parent-death behavior are retained: admin replacement closes brokers and
connections. Supervisor separation is not implemented.

## Prerequisites

Use a working kubeconfig for the intended cluster. Both nodes must be Ready,
with a working CNI and enough filesystem space to avoid DiskPressure. The
existing Linkerd control plane supplies identity, destination and policy APIs.
DOCA, the paired PCI functions, containerd, Docker, Helm, OpenSSL, Python, SSH and
the build toolchains must be installed.

For a DPU joining a different cluster, back up its kubelet/CNI configuration,
stop the previous runtime, install a kubelet/kubeadm matching the cluster with
[prepare-dpu-node.sh](prepare-dpu-node.sh), and join using a short-lived bootstrap
token. The script prepares binaries; it does not reset or automatically join
the node. Remove bootstrap files and tokens after a successful join.

Configure [.env.example](../.env.example) as .env. Explicitly set the host and
DPU node names for each pair. A DPU is tainted dpumesh.io/dpu=true:NoSchedule.
Applications are scheduled on the host. Hardware pairing is not load balanced.

## Build and deploy

From the host:

```sh
export KUBECONFIG=/path/to/current/kubeconfig
export DPUMESH_NODE_NAME=rapids4
export DPUMESH_DPU_NODE_NAME=rapids4-dpu
export IMG_DPU=bench/dpumesh-dpu:YOUR_VERSION
export IMG_CONTROLLER_NATIVE=bench/dpumesh-controller:YOUR_VERSION
export IMG_ECHO_NATIVE=bench/echo-dpumesh:YOUR_VERSION
export IMG_BENCH_NATIVE=bench/bench-dpumesh:YOUR_VERSION
bench/native_deploy.sh all
```

The command builds native/controller/application images and the ARM/DPA runtime,
imports images into each node's containerd, provisions controller trust,
installs the host systemd service and read-only Kubernetes client certificate,
deploys the DPU Helm release, and starts the two application Pods. The supplied
host budget is two channels with eight rings each, CPUs 0–2 and a 3 GiB kubelet
system memory reservation. Choose resource budgets deliberately before
increasing channel count.

[provision-host-reader.sh](provision-host-reader.sh) uses the Kubernetes CSR API
and binds dpumeshd:HOST_NODE to read-only Pods/Services access. It requires
operator CSR approval rights. Credentials stay under /etc/dpumesh/kube.
[deploy-dpu.sh](deploy-dpu.sh) configures the paired DPU certificate, copies
Linkerd trust roots, and deploys the chart using [runtime-values.py](runtime-values.py).

The host verifies the DPU server certificate name; the DPU accepts only the
configured host URI spiffe://dpumesh.io/node/HOST_NODE. Cluster membership
does not replace this authentication. Registration uses one TLS control path;
there is no grant keyring or registration mode setting. Update host, broker and
DPU together when changing the internal protocol (currently version 5,
1433-byte identity, 1497-byte request). The application IPC remains version 3.

## Linkerd

The runtime embeds Linkerd workers; it does not inject a proxy into application
Pods. The chart generates a P-256 identity key and CSR in memory-backed storage
with matching CommonName and DNS SAN (the short default ServiceAccount is dpu)
and projects a token with audience identity.l5d.io into the runtime container.
The feed sidecar and applications receive no token. The controller's API token
is separately restricted by RBAC.

The supplied native echo Service uses Linkerd opaque processing. Service
protocol selection is independent of deployment placement. Select HTTP/gRPC
Services with DPUMESH_L7_SVC; DPUMESH_L7_OPAQUE_SVC selects byte-stream
processing. Both lists use namespace/Service keys from the signed topology.
Service targets are refreshed through the signed Service-target feed.
The native Pod manifests declare port 9092 and a Linkerd Server selects the echo
Pod with opaque protocol and all-unauthenticated access. Change its accessPolicy
to deny to verify policy enforcement, then restore it. Port declarations are
required for Linkerd Server selection.

Verify requests and DPU worker metrics, not merely Running Pods or the Linkerd
control plane's health. Worker 0 serves admin port 4191; all-worker placement
increments the port for each worker. Access these infrastructure ports only
from trusted management networks.

## Operations and failure behavior

```sh
systemctl status dpumeshd
journalctl -u dpumeshd
kubectl -n test-bench get pods -o wide
kubectl -n dpumesh-system get pods -o wide
bench/bench.sh dpulog 60
bench/native_deploy.sh smoke
bench/bench.sh restart
```

The DPU DaemonSet uses OnDelete; restart explicitly deletes the Pod and waits
for its replacement. Runtime and feed receiver share that Pod. The runtime
owns one hardware lock and readiness file on the DPU host filesystem.
Certificates, feed verification keys and the peer node key persist under
/etc/dpumesh; the feed sidecar writes only its feeds mount.

dpumeshd.service uses KillMode=mixed. Admin stops and reaps brokers, with bounded
quiesce/drain and a final cgroup kill. Each broker owns private namespaces,
drops to uid/gid 65532 after trusted device bootstrap, clears capabilities and
denies exec. Its root supervisor wrapper remains in the admin's manager cgroup
to wait for that child; it does not run the data path or outlive the admin.
A slot is reusable only after its worker cgroup is empty and the
DPU confirms cleanup. Unexpected transport loss terminates the application;
Kubernetes restarts it and registration starts anew.

The DPU runtime is privileged and uses hostNetwork/hostPath for hardware. The
node admin is a privileged host service with a systemd capability/device bound.
Application, controller and feed-receiver containers are unprivileged.
Privileged bootstrap and infrastructure permissions are not granted to apps.

## Reference

[CONTROL](../design/CONTROL.md) defines authority and lifecycle.
[API](../design/API.md) defines the application contract.
Kubernetes documents [CSR issuance](https://kubernetes.io/docs/tasks/tls/managing-tls-in-a-cluster/)
and Linkerd documents [proxy configuration](https://linkerd.io/2-edge/reference/proxy-configuration/).

## Native endpoint policy metadata

The Pod label `linkerd.io/control-plane-ns: linkerd` enables policy-controller
observation. The `config.linkerd.io/skip-inbound-ports` annotation names the native
application port so stock destination discovery does not advertise a nonexistent
in-Pod proxy or TLS listener. DPU-side Server policy is still enforced; it is
verified by the deny/restore traffic test. Keep proxy injection disabled.
