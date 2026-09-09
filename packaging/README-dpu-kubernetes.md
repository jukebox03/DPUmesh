# DPU runtime on Kubernetes

This profile runs one DPU ARM runtime per explicitly paired host/DPU node. Host
`dpumeshd` and its per-workload brokers remain OS processes. Only brokers open
DOCA resources and progress the host PE. Applications retain the existing IPC ABI.

## Installation

1. Back up existing DPU kubelet configuration and runtime/configuration files.
   `prepare-dpu-node.sh` installs a kubelet/kubeadm matching the cluster; it does
   not assume the standalone kubelet can be reused. Join using a short-lived
   kubeadm token supplied through a root-only configuration, then delete it.
2. Configure a working CNI on the DPU management interface. A cluster CNI that
   hardcodes the host NIC name needs a separate DPU configuration. Taint the DPU
   `dpumesh.io/dpu=true:NoSchedule`; leave application placement on host nodes.
3. Keep the existing DPU node key, signed topology/membership feed receivers and
   their trust configuration under `/etc/dpumesh`. Controller/feed availability
   and existing peer authentication remain required for their existing roles.
4. Provision infrastructure TLS credentials: DPU server certificate with its
   configured DNS SAN and serverAuth, host client certificate with clientAuth and
   URI `spiffe://dpumesh.io/node/HOST_NODE`, plus the issuer trust roots. The
   DPU requires that exact host URI; the host validates the configured DPU name.
   Kubernetes membership alone supplies neither credential nor authentication.
5. Provision a dedicated Kubernetes read-only client certificate to host
   dpumeshd, subject `dpumeshd:HOST_NODE`. Replace HOST_NODE in
   `local-reader-rbac.yaml` and apply. Keep keys root-only. Configure the
   direct options in `dpumeshd.env.example`, including kubelet PodResources
   socket and `DPUMESH_LOCAL_SERVER_NAME`. Broker/application receive none of
   these credentials. Certificate issuance/rotation uses operator PKI; this
   profile does not introduce an enrollment controller.
6. Build ARM sources using `bench/bench.sh build`. On the DPU, run
   `packaging/build-dpu-image.sh PROJECT_ROOT IMAGE`. Push to a reachable registry
   or import `docker save IMAGE` with `ctr -n k8s.io images import -`.
7. Set the required chart values (DPU node, served host node, cluster, PCI,
   representor, image), existing feed/peer environment, and local TLS paths.
   Stop any runtime running outside Kubernetes before installing:

   ```sh
   helm upgrade --install dpumesh-runtime packaging/helm/dpumesh-runtime \
     -n dpumesh-system --create-namespace -f site-values.yaml
   kubectl -n dpumesh-system wait --for=condition=Ready pod \
     -l app.kubernetes.io/instance=dpumesh-runtime --timeout=120s
   ```

Host and DPU must select the same `registrationMode`. `grant` keeps the
controller-signed path and needs the matching host configuration; `direct` uses
the paired control session. There is no automatic fallback between them: the
broker accepts only the challenge version of the configured mode.

## Updates, diagnosis, rollback

The DaemonSet uses OnDelete: applying new values or a new image does not
replace the running Pod, so delete it deliberately once the values are in
place. The 40-second grace period covers the runtime's bounded DMA drain and
context cleanup. A hostPath flock admits one runtime to the device; a runtime
started outside Kubernetes must use the same lock path.

Check node Ready/DiskPressure, runtime Ready, runtime logs, host
`journalctl -u dpumeshd`, and application restart counts. Runtime readiness
requires hardware initialization and a recent control-loop heartbeat, not a
first application connection. Direct registration verifies caller evidence,
Pod/container/Service and kubelet channel allocation; Pod Ready is not an
identity requirement. Endpoint readiness still controls routing.

Control loss closes admission, retires registrations, and terminates affected
brokers. Their applications must reconnect/restart; seamless stream migration
is not provided. A slot with unproven cleanup stays quarantined. After a long
outage, inspect cleanup status before restarting the host daemon to reconcile
allocations. API outage rejects fresh registration while established identities
are retained until reconciliation/control loss; this is not an immediate
revocation guarantee during API failure.

For rollback, stop the DPU DaemonSet before starting a runtime outside
Kubernetes, restore the matching host registration mode, binaries and
configuration, then restart workloads. Drive the lifecycle with deployment
commands rather than `bench/bench.sh restart`, which does not know about the
DaemonSet. Image and standalone binary must match when comparing performance.

## What this profile assumes

Joining the DPU to the cluster changes how the runtime is delivered. It supplies
neither device access nor identity, and these constraints hold whether or not
the runtime runs as a Pod.

- The DPU kubelet must not be newer than the cluster's API server. Pick a
  supported pair before joining; a join token does not make a skewed kubelet
  work.
- The DPU is its own Kubernetes node with its own name, and that name is not the
  host node this runtime serves. `DPUMESH_NODE_NAME` is the served host node,
  and the mapping between the two is operator-owned; no scheduler derives it
  from the physical PCIe pairing.
- kubelet must reach the API server over a path that does not depend on
  DPUmesh, or the node cannot boot into the cluster that is supposed to manage
  it.
- `hostNetwork` with `ClusterFirstWithHostNet` preserves the NIC, representor
  and peer addresses, but it does not by itself make the DNS server or the
  Service network reachable from the DPU. Verify both. A peer RDMA address is
  never a Pod IP or a ClusterIP: control-plane TCP reachability is not evidence
  that the RDMA path works.
- The DPU node taint keeps ordinary workloads off the node. It is a scheduling
  control, not a security boundary, and a Pod rescheduled onto a different DPU
  cannot stand in for the original host's PCIe channel.
- Node identity and installed feeds live on the DPU host filesystem. Moving them
  to `emptyDir` changes the node key every time the Pod is recreated; pointing
  the runtime's writable paths at a read-only Secret volume breaks start-up.
- A credential issued to the DPU runtime authenticates the runtime, never an
  application. Workload identity reaches the DPU only through registration, and
  policy is watched for the host workload rather than for this Pod.

## Hardware profile limits

The initial chart is privileged, hostNetwork, uses hostPath configuration, and
has no CPU quota. It isolates infrastructure from application Pods, but is not a
least-privilege container profile. Admission to the DPU node/configuration must
remain operator controlled. It does not automatically deploy device plugins,
feed receivers, controller, PKI rotation, or node-specific CNI.

Set `DPUMESH_ARM_CPU_LIST` to worker cores followed by the main core for a
controlled performance comparison. Without it, IRQ-based placement can differ
between native and container environments. Reserve equivalent cores in a
production scheduler profile before interpreting interference as Pod overhead.

## Host broker shutdown

The systemd unit uses `KillMode=mixed` so dpumeshd stops its own brokers first.
Each broker has a 5-second remote quiesce bound and a 5-second Comch drain
bound; the supervisor gives it 15 seconds before killing its owned cgroup, then
waits for confirmed exit. The unit's 25-second stop timeout is the backstop.
`KillMode=control-group` would signal the wrapper processes first and cut that
sequence short, so the unit requires `mixed`.

The daemon keeps each wrapper's creating thread alive until its broker is
reaped, because Linux parent-death signals follow the creating thread.

A reaped wrapper is not evidence for slot reuse: the host worker cgroup must be
empty and the paired DPU must confirm cleanup. Cleanup queries continue across
Kubernetes restart backoff, so a long DPU outage delays reuse without stranding
the slot. Applications still reconnect or restart after the DPU returns.
