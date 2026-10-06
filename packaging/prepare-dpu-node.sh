#!/bin/bash
# Run as root on a DPU, then use the matching kubeadm with a private JoinConfiguration.
#
# kubelet and kubeadm come from DPUMESH_K8S_BIN_DIR when set (an offline DPU:
# copy them there first), else from the installed binaries when they already
# report KUBERNETES_VERSION (BlueField images ship kubelet and kubeadm), else
# from dl.k8s.io.
set -euo pipefail
version=${1:?usage: prepare-dpu-node.sh KUBERNETES_VERSION}
[[ "$version" =~ ^v1\.[0-9]+\.[0-9]+$ ]] || exit 2
[ "$(id -u)" = 0 ] && [ "$(uname -m)" = aarch64 ]
backup="/var/backups/dpumesh-kubelet-$(date -u +%Y%m%dT%H%M%SZ)"
install -d -m 0700 "$backup"
cp -a /var/lib/kubelet/config.yaml "$backup/"
systemctl cat kubelet > "$backup/kubelet.service.txt"
# The standalone kubelet's static Pods and any CNI configuration it had.
for path in /etc/kubelet.d /etc/kubernetes /etc/cni/net.d; do
    [ ! -e "$path" ] || cp -a "$path" "$backup/$(echo "${path#/}" | tr / -)"
done
sysctl net.ipv4.ip_forward > "$backup/sysctl-before.txt"

binary_version() { # binary path -> its Kubernetes version, or nothing
    case "$(basename "$1")" in
        kubelet) "$1" --version 2>/dev/null | awk '{print $2}' ;;
        kubeadm) "$1" version -o short 2>/dev/null ;;
    esac
}
install -d /opt/dpumesh/kubernetes/bin /etc/systemd/system/kubelet.service.d
for binary in kubelet kubeadm; do
    target="/opt/dpumesh/kubernetes/bin/$binary"
    installed=$(command -v "$binary" || true)
    if [ -n "${DPUMESH_K8S_BIN_DIR:-}" ]; then
        source="$DPUMESH_K8S_BIN_DIR/$binary"
        [ "$(binary_version "$source")" = "$version" ] || {
            echo "$source is not $binary $version" >&2
            exit 1
        }
        install -m 0755 "$source" "$target"
    elif [ -n "$installed" ] && [ "$(binary_version "$installed")" = "$version" ]; then
        install -m 0755 "$installed" "$target"
    else
        curl --fail --location --retry 3 "https://dl.k8s.io/release/$version/bin/linux/arm64/$binary" -o "$target.new"
        expected=$(curl --fail --silent --location "https://dl.k8s.io/release/$version/bin/linux/arm64/$binary.sha256")
        printf '%s  %s\n' "$expected" "$target.new" | sha256sum -c -
        chmod 0755 "$target.new"
        mv "$target.new" "$target"
    fi
done

# Kubernetes networking prerequisites. BlueField images leave IP forwarding off,
# which kubeadm's preflight rejects, and do not load br_netfilter. Dedicated
# files keep them easy to remove.
printf 'br_netfilter\n' > /etc/modules-load.d/99-dpumesh-k8s.conf
modprobe br_netfilter
printf 'net.ipv4.ip_forward = 1\nnet.bridge.bridge-nf-call-iptables = 1\nnet.bridge.bridge-nf-call-ip6tables = 1\n' \
    > /etc/sysctl.d/99-dpumesh-k8s.conf
sysctl -q -p /etc/sysctl.d/99-dpumesh-k8s.conf

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
ignore=
if [ -n "$(ls -A /etc/kubernetes/manifests 2>/dev/null)" ]; then
    # BlueField keeps a .kubelet-keep marker there, which preflight's
    # DirAvailable check rejects.
    ignore=" --ignore-preflight-errors=DirAvailable--etc-kubernetes-manifests"
fi
echo "Join with /opt/dpumesh/kubernetes/bin/kubeadm join --config PRIVATE_CONFIG$ignore"
echo "After the join kubelet reads static Pods from /etc/kubernetes/manifests, not /etc/kubelet.d."
echo "An offline DPU needs the cluster's pause, kube-proxy and CNI images imported into containerd"
echo "(namespace k8s.io) before its node turns Ready; see packaging/README-dpu-kubernetes.md."
