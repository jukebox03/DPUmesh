#!/bin/bash
# Issue a dedicated read-only API client; requires kubectl CSR approval rights.
set -euo pipefail
node=${1:?usage: provision-host-reader.sh HOST_NODE}
[[ "$node" =~ ^[a-z0-9][a-z0-9.-]*$ ]] || exit 2
project=$(cd "$(dirname "$0")/.." && pwd)
sudo_host() {
    if [ -n "${HOST_PASS:-}" ]; then
        printf '%s\n' "$HOST_PASS" | sudo -S -p '' "$@"
    else
        sudo "$@"
    fi
}
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
umask 077
kubectl config view --raw --minify --flatten -o jsonpath='{.clusters[0].cluster.certificate-authority-data}' | base64 -d > "$temporary/ca.crt"
test -s "$temporary/ca.crt"
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -subj "/CN=dpumeshd:$node" -keyout "$temporary/client.key" \
    -out "$temporary/client.csr" >/dev/null 2>&1
name="dpumeshd-$node-$(date +%s)"
request=$(base64 -w0 < "$temporary/client.csr")
kubectl apply -f - <<EOF
apiVersion: certificates.k8s.io/v1
kind: CertificateSigningRequest
metadata: {name: $name}
spec:
  request: $request
  signerName: kubernetes.io/kube-apiserver-client
  expirationSeconds: 31536000
  usages: [client auth]
EOF
kubectl certificate approve "$name"
for _ in $(seq 1 30); do
    kubectl get csr "$name" -o jsonpath='{.status.certificate}' | base64 -d > "$temporary/client.crt"
    [ ! -s "$temporary/client.crt" ] || break
    sleep 1
done
openssl verify -CAfile "$temporary/ca.crt" "$temporary/client.crt"
sed "s/HOST_NODE/$node/g" "$project/packaging/local-reader-rbac.yaml" | kubectl apply -f -
sudo_host install -d -m 0700 /etc/dpumesh/kube
sudo_host install -m 0400 "$temporary/client.key" /etc/dpumesh/kube/client.key
sudo_host install -m 0444 "$temporary/ca.crt" "$temporary/client.crt" /etc/dpumesh/kube/
kubectl delete csr "$name"
