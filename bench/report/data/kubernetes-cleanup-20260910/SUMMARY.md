# Kubernetes deployment and code cleanup record — 2026-09-10

Host `rapids4` and DPU `rapids4-dpu` were placed in the same Kubernetes v1.34.11
cluster. The controller was kept, and registration was consolidated into a single
path that carries the host-verified identity over the paired-DPU TLS session. The
existing lifetime relationship between admin and broker was kept, so **restarting
admin also terminates the brokers and their connections.**

## Final placement and privileges

| Component | Placement | Privileges |
|---|---|---|
| Application | 2 host Pods in `test-bench` | UID 65532, no capabilities, no Kubernetes token |
| DPU runtime | DPU DaemonSet Pod in `dpumesh-system` | privileged, hostNetwork, device and configuration hostPath |
| Feed receiver | sidecar of the DPU runtime Pod | UID 65532, no token, writes only the feeds directory |
| DPUmesh controller | Deployment Pod on the designated node `rapids4` | unprivileged, read-only Kubernetes RBAC |
| Node admin | `dpumeshd.service` on the host | root, systemd capability/device/CPU/memory limits |
| Per-pod broker | host process created by admin | root supervisor wrapper plus a UID 65532 data worker |

The wrapper waits for child exit in admin's manager cgroup. The data worker runs
in its own resource-limited cgroup and private namespaces with `CapEff=0`,
`NoNewPrivs=1` and seccomp applied. There is no separate supervisor service or
Pod. A live RBAC query confirmed that the host admin's dedicated API certificate
can read Pods but can neither create Pods nor read Secrets.

The final state is 1/1 for both the controller and application Deployments, 1/1
for the runtime DaemonSet and 2/2 for the runtime Pod, all Ready. The DaemonSet
`UP-TO-DATE` count is also 1. The existing `jet1` stays Ready.

- [Live deployment state and RBAC](deployment-state.log)
- [Pod placement, images and securityContext](placement-and-privileges.json)
- [Live broker PID, UID, capabilities and cgroup](broker-privileges.log)

## Code removed and kept

Controller grant issuance, signing and verification, the grant/manager
registration branch, the grant replay cache, the membership feed and its
consumers, and the related configuration, build entries and tests were removed.
The `workload_grant` module was reduced to the canonical identity and feed
verification still in use and renamed `workload_identity`. The host Pod
deployment design and the standalone feed-receiver systemd unit were also
removed.

The controller's topology and Service-target publication, node registration and
workload-scope relay were kept because the current path uses them. Admission
drain, generation fencing and broker quiesce/cleanup were kept as lifetime
management features in actual use.

The internal registration protocol is version 5, with a 1433-byte identity, a
1497-byte request and a 36-byte response. It is not compatible with the old grant
protocol, so host, admin, broker and DPU must be replaced together. Application
IPC version 3 and `libdpumesh.so.5` were kept.

## Linkerd live verification

The existing Linkerd `edge-26.8.4` control plane was used. All 8 embedded Linkerd
workers on the DPU returned HTTP 200 on `/ready`, with 1 identity certificate
issuance success and 0 failures. The issued identity is
`dpu.dpumesh-system.serviceaccount.identity.linkerd.cluster.local`.

Verification ran on the final runtime Pod in this order.

1. The native echo Service was configured as a Linkerd opaque target and a live
   request succeeded.
2. The `accessPolicy` of the Linkerd `Server` was changed to `deny`. New requests
   were blocked with `rcnt=0` and `fail=1`, and the DPU's
   `dmesh_control_events_total{kind="inbound",reason="denied"}` rose from 0 to 1.
3. After restoring `all-unauthenticated`, live traffic was rechecked:
   `rcnt=16120`, `fail=0`, `drops=0`, `worker_fail=0`.
   The final inbound admitted counter is 4.

The control event counters on the 8 admin endpoints expose a shared value and are
therefore not summed. Actual transmission was also confirmed through the
per-worker `dmesh_tx_accepted_bytes_total`. This is a short functional smoke, not
a performance comparison.

- [Final traffic result](final-smoke.log)
- [Live policy deny result](policy-deny.log)
- [Metrics before the deny](policy-before.log), [metrics after the deny](policy-after-deny.log)
- [Readiness, identity and policy check across all workers](linkerd-verification.log)
- [Full metrics for the final worker 0](linkerd-final-4191.log)

The native Pod required the `linkerd.io/control-plane-ns` label and the
`config.linkerd.io/skip-inbound-ports` annotation. The label enables policy
observation; the annotation stops the stock destination from advertising an
in-Pod proxy/TLS listener that does not exist. DPU-side policy was confirmed
separately by the deny experiment above. The CN and DNS SAN of the Linkerd CSR
were also corrected to match the real identity.

The `diagnostic-*` files are diagnostic records taken before the configuration
was corrected. With the label missing the policy did not apply, and with the
annotation missing the endpoint lookup failed. Those failure responses were not
used as evidence of policy blocking.

The scope of this deployment verification is native opaque traffic on the same
host and dynamic inbound policy. It does not claim to have verified HTTP or gRPC
workloads, peer traffic between two hosts, or workload mTLS.

## Build, checks and documentation

The following checks passed.

- `make -j8 bench test-hostfree test test-local-registration`:
  [full result](tests.log). Ran against the cryptography and grpc dependencies of
  an isolated Python environment.
- Python F401/F821/F841 lint: [result](python-lint.log).
- Syntax of changed shell scripts, Helm lint, Kubernetes server dry-run,
  `git diff --check` and the relative document link check:
  [result](final-checks.log).

README, CONTROL/API/DATA/GRPC, PLAN, and the deployment, bench, CI and example
documents together with the control figures were aligned with the code. The
research proposal was updated to distinguish implementation status and to correct
present-tense descriptions of the removed grant path. Earlier measurement
documents keep their measured values and state the revision they apply to.

During deployment the DPU kubelet was aligned with the current cluster version
and joined, and the standalone runtime and feed-receiver processes were cleaned
up. DiskPressure was resolved by clearing regenerable build, image and apt
caches. Failed earlier bench Pods, the join bootstrap token and temporary files
were removed. Configuration backups are kept in `/var/backups/dpumesh-*` on the
DPU.

Current deployment guide: [packaging README](../../../../packaging/README-dpu-kubernetes.md).
Final design: [CONTROL](../../../../design/CONTROL.md).
