#!/bin/bash
# Run as root on a DPU, then use the matching kubeadm with a private JoinConfiguration.
set -euo pipefail
version=${1:?usage: prepare-dpu-node.sh KUBERNETES_VERSION}
[[ "$version" =~ ^v1\.[0-9]+\.[0-9]+$ ]] || exit 2
[ "$(id -u)" = 0 ] && [ "$(uname -m)" = aarch64 ]
backup="/var/backups/dpumesh-kubelet-$(date -u +%Y%m%dT%H%M%SZ)"
install -d -m 0700 "$backup"
cp -a /var/lib/kubelet/config.yaml "$backup/"
systemctl cat kubelet > "$backup/kubelet.service.txt"
install -d /opt/dpumesh/kubernetes/bin /etc/systemd/system/kubelet.service.d
for binary in kubelet kubeadm; do
    target="/opt/dpumesh/kubernetes/bin/$binary"
    curl --fail --location --retry 3 "https://dl.k8s.io/release/$version/bin/linux/arm64/$binary" -o "$target.new"
    expected=$(curl --fail --silent --location "https://dl.k8s.io/release/$version/bin/linux/arm64/$binary.sha256")
    printf '%s  %s\n' "$expected" "$target.new" | sha256sum -c -
    chmod 0755 "$target.new"
    mv "$target.new" "$target"
done
systemctl stop kubelet
cat > /etc/systemd/system/kubelet.service.d/99-dpumesh-cluster.conf <<'EOF'
[Service]
Environment="KUBELET_KUBECONFIG_ARGS=--bootstrap-kubeconfig=/etc/kubernetes/bootstrap-kubelet.conf --kubeconfig=/etc/kubernetes/kubelet.conf"
Environment="KUBELET_CONFIG_ARGS=--config=/var/lib/kubelet/config.yaml"
EnvironmentFile=-/var/lib/kubelet/kubeadm-flags.env
ExecStart=
ExecStart=/opt/dpumesh/kubernetes/bin/kubelet $KUBELET_KUBECONFIG_ARGS $KUBELET_CONFIG_ARGS $KUBELET_KUBEADM_ARGS
EOF
systemctl daemon-reload
echo "Prepared $version; previous standalone configuration saved at $backup"
echo "Join with /opt/dpumesh/kubernetes/bin/kubeadm join --config PRIVATE_CONFIG"
