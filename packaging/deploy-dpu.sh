#!/bin/bash
# Deploy one explicitly paired DPU runtime to the existing Kubernetes cluster.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
if [ -f "${DPUMESH_ENV_FILE:-$project/.env}" ]; then
    set -a; source "${DPUMESH_ENV_FILE:-$project/.env}"; set +a
fi
: "${DPU_HOST:?}" "${DPU_PASS:?}" "${DPU_PCI:?}"
runtime_ns=${RUNTIME_NS:-dpumesh-system}
release=${DPU_RELEASE:-dpumesh-runtime}
image=${IMG_DPU:-bench/dpumesh-dpu:kubernetes}
host_node=${DPUMESH_NODE_NAME:-$(hostname)}
dpu_node=${DPUMESH_DPU_NODE_NAME:-$host_node-dpu}
address=${DPUMESH_DPU_FEED_HOST:-${DPU_HOST##*@}}
pki=${DPUMESH_PKI_DIR:-$project/build/pki}
[[ "$DPU_PCI" =~ ^-p[[:space:]]([0-9A-Fa-f:.]+)[[:space:]]+-r[[:space:]]([0-9A-Fa-f:.]+)$ ]] || exit 2
pci=${BASH_REMATCH[1]}; representor=${BASH_REMATCH[2]}
test -s "$pki/ca.key" || { echo "missing $pki/ca.key; run bench/dpumesh_controller.sh deploy first" >&2; exit 1; }
if [[ "$address" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then san="IP:$address"; else san="DNS:$address"; fi
temporary=$(mktemp -d)
remote=
cleanup() {
    rm -rf "$temporary"
    # The staging directory holds the DPU server key until it is installed.
    [ -z "$remote" ] || ssh -o BatchMode=yes "$DPU_HOST" "rm -rf '$remote'" || true
}
trap cleanup EXIT
umask 077
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -subj "/CN=$dpu_node" -keyout "$temporary/dpu-server.key" \
    -out "$temporary/dpu-server.csr" >/dev/null 2>&1
printf 'subjectAltName=DNS:%s,%s\nextendedKeyUsage=serverAuth\n' "$dpu_node" "$san" > "$temporary/server.ext"
openssl x509 -req -sha256 -days 365 -set_serial "$(date +%s)" \
    -in "$temporary/dpu-server.csr" -CA "$pki/ca.crt" -CAkey "$pki/ca.key" \
    -extfile "$temporary/server.ext" -out "$temporary/dpu-server.crt" >/dev/null 2>&1
remote=$(ssh -o BatchMode=yes "$DPU_HOST" mktemp -d)
scp -q "$temporary/dpu-server.key" "$temporary/dpu-server.crt" "$pki/ca.crt" "$DPU_HOST:$remote/"
# The retired systemd feed receiver ran as dpumesh-feed and owned /etc/dpumesh;
# the runtime trusts the keys under it, so it must belong to root again.
printf '%s\n' "$DPU_PASS" | ssh -o BatchMode=yes "$DPU_HOST" "sudo -S -p '' sh -ec 'install -d -m 0700 /etc/dpumesh/local-tls; install -m 0400 $remote/dpu-server.key /etc/dpumesh/local-tls/; install -m 0444 $remote/dpu-server.crt $remote/ca.crt /etc/dpumesh/local-tls/; chown root:root /etc/dpumesh; chmod 0755 /etc/dpumesh; systemctl disable --now dpumesh-feed-receiver.service 2>/dev/null || true'"
ssh -o BatchMode=yes "$DPU_HOST" "rm -rf '$remote'"
remote=
kubectl get namespace "$runtime_ns" >/dev/null 2>&1 || kubectl create namespace "$runtime_ns"
kubectl label namespace "$runtime_ns" pod-security.kubernetes.io/enforce=privileged --overwrite
kubectl taint node "$dpu_node" dpumesh.io/dpu=true:NoSchedule --overwrite
kubectl -n linkerd get configmap linkerd-identity-trust-roots -o json |
    python3 -c 'import json,sys; d=json.load(sys.stdin); d["metadata"]={"name":d["metadata"]["name"],"namespace":sys.argv[1]}; print(json.dumps(d))' "$runtime_ns" | kubectl apply -f -
export DMESH_DEPLOY_IMAGE="$image" DMESH_DEPLOY_HOST="$host_node" DMESH_DEPLOY_DPU="$dpu_node"
export DMESH_DEPLOY_ADDRESS="$address" DMESH_DEPLOY_PCI="$pci" DMESH_DEPLOY_REPRESENTOR="$representor"
python3 "$project/packaging/runtime-values.py" > "$temporary/values.json"
helm upgrade --install "$release" "$project/packaging/helm/dpumesh-runtime" \
    -n "$runtime_ns" -f "$temporary/values.json"
# OnDelete makes disruptive runtime restarts explicit; this command is deployment.
selector="app.kubernetes.io/instance=$release"
kubectl -n "$runtime_ns" delete pod -l "$selector" --wait=true
# DaemonSet numberReady can still count the deleted Pod; wait on its replacement.
for _ in $(seq 1 60); do
    [ -z "$(kubectl -n "$runtime_ns" get pod -l "$selector" -o name)" ] || break
    sleep 1
done
kubectl -n "$runtime_ns" wait --for=condition=Ready pod -l "$selector" --timeout=240s
