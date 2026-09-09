# DPU runtime on Kubernetes, direct registration — deployment record

The BlueField Arm OS joined the existing cluster as the node `rapids4-dpu` and
`dpumesh_dpu` ran as the `dpumesh-system/dpumesh-runtime` DaemonSet. Host
`dpumeshd` and its per-workload brokers stayed OS processes. Registration used
the paired host–DPU control session instead of a controller-signed grant, and
real Comch and DMA traffic was confirmed on that path.

This record covers local registration and placement only. The controller keeps
its signed topology and feed roles, and DPU-to-DPU node-key-pinned TLS is
unchanged. It is not evidence for cross-node workload mTLS, two-DPU hardware,
L7 performance, or a long soak. The contract is `design/CONTROL.md`.

## Rig

| Item | Value |
|---|---|
| Host / DPU | rapids4 / BlueField-3 Arm64, 16 cores, ~31 GiB |
| Kubernetes | API server 1.31.14; DPU kubelet aligned to 1.31.14 |
| DOCA | 3.1.0105 |
| DPU node / PodCIDR | rapids4-dpu / 10.244.1.0/24 |
| Management path | host 192.168.100.1 ↔ DPU 192.168.100.2 |
| Host PCI / DPU PCI / representor | 94:00.0 / 03:00.0 / 94:00.0 |
| Data plane | N/K/A = 32/8/8, L4 raw-byte forwarding, L7 off |
| CPU pinning | workers 13,3,4,1,6,2,12,5; main 15 |
| Runtime profile | privileged infrastructure Pod, hostNetwork, hostPath config, no CPU quota |

## What the deployment does

- The host keeps its `SO_PEERCRED`/cgroup/container evidence and additionally
  checks the Pod, its Service, and the kubelet PodResources channel allocation
  directly.
- The broker reports the DPU-issued 32-byte connection ID to `dpumeshd` over its
  retained private launch socket. The application can reach neither that socket
  nor any infrastructure credential.
- REGISTER, UNREGISTER and STATUS travel over a host–DPU TLS 1.3 session. The
  DPU checks the client certificate's exact host URI SAN; the host checks the
  DPU server name.
- A Comch `WORKLOAD_ASSERT` is refused in direct mode. The explicit `grant` mode
  remains for migration and rollback, with no automatic fallback.
- The existing 1545-byte metadata encoding is reused with a zero signature. It
  is neither a controller-issued grant nor a newly minted workload credential.
- The broker owns the DOCA device, Comch, memory registration and the host PE.
  The application IPC ABI is unchanged, so existing images were used as-is.
- Pod Ready was removed from registration eligibility. Endpoint readiness still
  governs routing.
- Session loss closes admission and retires registrations and brokers. A slot is
  not reused before its previous connection's cleanup is confirmed. A runtime
  lock, a readiness file, SIGTERM drain, and DPA/Comch/DMA teardown were added.

## Environment changes this required

The DPU's standalone kubelet configuration was backed up and replaced with a
cluster kubelet override. Host Flannel pins `eno1`, so the amd64 original was
left alone and a separate Flannel DaemonSet using `tmfifo_net0` was placed on
the DPU; the original update strategy was changed to OnDelete to avoid
restarting host networking. Following the DPU log exhausted the inotify instance
limit (128); it was raised to 1024 and persisted through sysctl.

The DPU had roughly 9–11 GiB free, which triggered DiskPressure under the
default eviction and image-GC thresholds. nodefs/imagefs available 5% and image
GC high/low 95/90 were applied to the DPU kubelet only (memory 100 MiB and inode
thresholds kept). This did not add free space, so image retention still needs
managing. DPU `br_netfilter` and forwarding sysctls were applied.

The host API credential is a `dpumeshd:rapids4` read-only client limited to
get/list/watch on Pods and Services. RBAC cannot scope a list by node, so the
trusted daemon applies the node filter. The existing DPU static node key and
signed-feed trust were preserved, and no automatic PKI issuance controller was
introduced.

## What was checked

- Full host `make test` plus authentication and framing tests against the
  production local TLS server.
- Refusal of a certificate with a different host URI, of an old session, of a
  conflicting retry, of a wrong node/container/Service, of a deleted Pod, and of
  a kubelet slot mismatch. An unready Pod is accepted as eligible.
- ARM meson/DOCA build, Helm lint and server-side validation, and a running
  DaemonSet.
- A grant-mode Pod smoke test, then direct-mode REGISTER with Comch and DMA.
- Broker SIGKILL under the bench: the same Pod re-registered under a new
  container and generation, and traffic recovered.
- Two runs reported `worker_fail=1` — the first native 8 KiB baseline and one
  pinned native run. Failures were not folded into the throughput samples. The
  cause was found later; see `../registration-fault-investigation-20260908/`.

## Limits observed

During an API outage new registrations are refused while established workload
identities are retained; reconciliation resumes when the API returns, so this is
not an immediate revocation guarantee. Control loss and a DPU restart break
existing streams and require the application to reconnect or restart.

Scaling applications up immediately after a host daemon restart produced a
transient `UnexpectedAdmissionError`: kubelet still saw the device as unhealthy
and refused the allocation. The ReplicaSet's replacement Pod ran normally, but a
later Recreate rollout was blocked by the leftover Failed Pod until it was
deleted. Operationally, start workloads after host control, feed and device
health have returned.
([raw](admission-after-host-restart.json))

The initial infrastructure Pod is privileged and uses hostPath. A least-privilege
device profile, automatic certificate renewal, and a production resource
reservation were not completed here.

## Performance comparison

`measure_registration.py`: 5 s per case, warmup 1000, three repeats, 8 B reply.
Latency is 64 B / concurrency 1 / 1 thread; bandwidth is 8192 B / 32 / 1; rate is
32 B / 32 per thread / 8 threads (256 outstanding). Gbps is the benchmark's own
request+reply framing rate, not PCIe wire utilization.

Native and Pod ran the same v3 binary and CPU list. The final v4 adds cached
staging mmap return on shutdown and a shutdown failure-reporting fix, so the
comparison and the final confirmation are reported separately.

| Metric (median of good samples, range) | Native direct | Pod direct |
|---|---:|---:|
| 64 B p50 (µs) | 132 (132–132) | 132 (132–141) |
| 64 B p99 (µs) | 438 (367–457) | 409 (360–419) |
| 8 KiB throughput (Gbps) | 6.416 (6.365–6.466), 2/3 good | 6.691 (6.359–6.733), 3/3 good |
| 8-thread small request (Mreq/s) | 0.561 (0.558–0.567) | 0.619 (0.617–0.620) |
| Good / attempted | 8/9 | 9/9 |

[native](pinned-native/samples.json), [pod](pinned-pod/samples.json),
[CPU placement](pod-affinity.txt).

The one native bandwidth failure (`fail=1, worker_fail=1, pending=23`) is kept
separately; stability is not claimed from the good samples alone. This short
sequential measurement shows no consistent Pod regression, and the higher Pod
numbers are not evidence that containerization is faster: a long ABBA comparison
controlling for temperature, IRQ, background load, run order and repeat count
was not run, and CPU/RSS efficiency is not measured here. Pre-pinning runs are
kept in the raw directory and were not used as container overhead, because
native and Pod could select different cores from their own IRQ observations.

## Shutdown and fault recovery

A graceful stop closes admission, runs the per-workload DEL_ACK and Arm DMA
barriers, joins the workers, then releases DPA producers/consumers, message
queues, completions, threads, the DMA engine, the slot-local staging mmaps held
for reuse, and finally the PE and device, in that order, following the SDK's
context stop/IDLE contract and its destroy dependencies.

An early shutdown implementation left the runtime-wide message queue and the
slot-local staging mmaps behind and produced IN_USE. v4 fixed both, and a Pod
deletion with a live workload logged `runtime hardware cleanup completed` and
exited normally. ([log](graceful-runtime-exit.log))

Not every shutdown log is error-free: error callbacks for cancelled Comch and
DPA tasks are recorded, and across repeated restarts one host broker ended with
`exit=139`. The host confirmed cleanup status on a new session and issued a new
generation, and traffic recovered; the root cause of that broker exit was found
later (see the fault investigation). ([host log](graceful-host.log))

The second repeated-restart trial did not recover inside the first 54 s polling
window. Application CrashLoopBackOff accumulated and broker re-registration was
seen at roughly 60 s; later requests succeeded. Relative polling times are not
summed into an end-to-end recovery figure. ([samples](graceful-recovery.json))

For the SIGKILL trial a fresh application Pod was created first to avoid
accumulated backoff, then the DPU runtime process was killed. Kubernetes
restarted the container in the same Pod and an error-free request succeeded at
about **26.15 s**; this includes 2 s polling and a 1 s successful request and is
not a service recovery SLA.
([samples](sigkill-recovery.json), [host log](sigkill-host.log))

These restarts are results for this driver and firmware combination, not a proof
that DMA is fenced under an arbitrary device fault. A hardware cleanup timeout is
reported as an abnormal exit so the host never reuses a slot without evidence.

## Final state of this campaign

Helm release `dpumesh-runtime` in namespace `dpumesh-system`, image
`bench/dpumesh-dpu:kubernetes-v4`. The systemd runtime kept for the native
comparison was stopped. Host `dpumeshd`, the broker binary and the library were
installed from the same source. Both application deployments run one replica and
the trial readiness probes were removed.

[nodes](final-nodes.txt), [runtime](final-runtime.txt), [apps](final-apps.txt),
[binary hashes](final-binary.txt), [manifest](deployed-daemonset.yaml).

Backups are kept in `/var/backups/dpumesh-k8s-20260908` on both host and DPU and
in the DPU kubelet backup directory. No credential or private key is included in
this record. The bootstrap token used for the join was revoked, and the host join
command and the DPU's temporary join configuration were deleted.

Each case was re-run once on the final v4 deployment, all with
fail/drop/worker_fail/overflow = 0: 64 B p50 **132 µs**, p99 **433 µs**;
8 KiB **6.582 Gbps**; 8-thread small request **0.557 Mreq/s**. Run-to-run
variation exists between the comparison and this confirmation, so the v3 table is
not a fixed performance guarantee for the final deployment.
([samples](final-v4-pod/samples.json))
