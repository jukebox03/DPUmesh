# Kubernetes host prerequisites

The host and paired DPU join the same cluster as separate nodes. These are OS prerequisites; they do not turn node admin or brokers into Pods. Admin is installed as dpumeshd.service, while [the DPU runtime](../../packaging/README-dpu-kubernetes.md) is a DaemonSet Pod. Keep enough disk space for kubelet image and node filesystem thresholds.

Install the two versioned host settings once on every DPUmesh Kubernetes node:

```sh
sudo install -o root -g root -m 0644 \
  bench/system/90-dpumesh-kubernetes.conf /etc/sysctl.d/
sudo install -o root -g root -m 0644 \
  bench/system/dpumesh-kubernetes.conf /etc/modules-load.d/
sudo modprobe br_netfilter
sudo sysctl --system
```

Kubelet also requires swap to stay disabled. Remove or mask the node's swap
unit according to the operating system; for an Ubuntu `/swap.img` entry:

```sh
sudo swapoff --all
sudo systemctl mask swap.img.swap
```

Confirm the host before deployment:

```sh
test -z "$(swapon --show --noheadings)"
test -e /proc/sys/net/bridge/bridge-nf-call-iptables
```
