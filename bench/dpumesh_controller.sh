#!/bin/bash
# Provision controller trust, the DPU feed receiver, and the Kubernetes controller.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
if [ -f "$PROJ_ROOT/.env" ]; then
    set -a
    source "$PROJ_ROOT/.env"
    set +a
fi

NS="${NS:-test-bench}"
HOST_CONTROLLER_KEYS="${DPUMESH_CONTROLLER_KEY_DIR_HOST:-/etc/dpumesh/controller.keys}"
DPU_CONTROLLER_KEYS="${DPUMESH_CONTROLLER_KEY_DIR_DPU:-/etc/dpumesh/controller.pub.keys}"
HOST_GRANT_KEYS="${DPUMESH_REGISTRATION_KEY_DIR_HOST:-/etc/dpumesh/registration.keys}"
DPU_GRANT_KEYS="${DPUMESH_REGISTRATION_KEY_DIR_DPU:-/etc/dpumesh/registration.keys}"
HOST_FEED_KEYS="${DPUMESH_FEED_KEY_DIR_HOST:-/etc/dpumesh/feed.keys}"
DPU_FEED_KEYS="${DPUMESH_FEED_KEY_DIR_DPU:-/etc/dpumesh/feed.keys}"
CONTROLLER_KEY_ID="${DPUMESH_CONTROLLER_KEY_ID:-controller-v1}"
GRANT_KEY_ID="${DPUMESH_REGISTRATION_KEY_ID:-node-ed25519-v1}"
FEED_KEY_ID="${DPUMESH_FEED_KEY_ID:-feed-hmac-v1}"
NODE_RDMA_ADDR="${DPUMESH_NODE_RDMA_ADDR:-${DPUMESH_PEER_BIND:-192.168.100.2}:${DPUMESH_PEER_PORT:-47900}}"
NODES_FILE="${DPUMESH_NODES_FILE:-}"
CLUSTER_ID="${DPUMESH_CLUSTER_ID:-dpumesh-test}"
PKI_DIR="${DPUMESH_PKI_DIR:-$PROJ_ROOT/build/pki}"
FEED_USER="${DPUMESH_FEED_USER:-dpumesh-feed}"
FEED_BIND="${DPUMESH_DPU_FEED_HOST:-192.168.100.2}"
FEED_PORT="${DPUMESH_DPU_FEED_PORT:-4788}"

valid_key_id() { [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]$ ]]; }
need_host() { : "${HOST_PASS:?HOST_PASS is required}"; }
need_dpu() { need_host; : "${DPU_HOST:?DPU_HOST is required}" "${DPU_PASS:?DPU_PASS is required}"; }
sudo_host() { printf '%s\n' "$HOST_PASS" | sudo -S -p '' "$@"; }

derive_public_hex() {
    { printf '302e020100300506032b657004220420' | xxd -r -p; cat; } |
        openssl pkey -inform DER -pubout -outform DER 2>/dev/null |
        tail -c 32 | xxd -p -c 64
}

ensure_host_key() {
    local directory="$1" requested="$2" active temporary
    valid_key_id "$requested" || { echo "invalid key id: $requested" >&2; exit 2; }
    sudo_host install -d -o root -g root -m 0700 "$directory"
    if sudo_host test -s "$directory/active"; then
        active=$(sudo_host cat "$directory/active" | tr -d '[:space:]')
        valid_key_id "$active" && sudo_host test -s "$directory/$active.key" || {
            echo "invalid active key in $directory" >&2
            exit 1
        }
    else
        active="$requested"
        temporary=$(mktemp)
        openssl rand 32 >"$temporary"
        chmod 0600 "$temporary"
        sudo_host install -o root -g root -m 0400 "$temporary" "$directory/$active.key"
        printf '%s\n' "$active" >"$temporary"
        sudo_host install -o root -g root -m 0400 "$temporary" "$directory/active"
        rm -f "$temporary"
    fi
    printf '%s\n' "$active"
}

install_dpu_key() {
    local host_directory="$1" dpu_directory="$2" key_id="$3" mode="$4" material
    if [ "$mode" = public ]; then
        material=$(sudo_host cat "$host_directory/$key_id.key" | derive_public_hex)
    else
        material=$(sudo_host xxd -p -c 64 "$host_directory/$key_id.key")
    fi
    [ ${#material} -eq 64 ] || { echo "invalid $mode key material for $key_id" >&2; exit 1; }
    printf '%s' "$material" | xxd -r -p |
        ssh -o ConnectTimeout=8 "$DPU_HOST" "
            set -e
            umask 077
            cat >/tmp/dpumesh-key.in
            echo '$DPU_PASS' | sudo -S -p '' install -d -o root -g root -m 0700 '$dpu_directory'
            echo '$DPU_PASS' | sudo -S -p '' install -o root -g root -m 0400 \
                /tmp/dpumesh-key.in '$dpu_directory/$key_id.key'
            rm -f /tmp/dpumesh-key.in
        "
}

assert_key_roles() {
    [ "$HOST_CONTROLLER_KEYS" != "$HOST_GRANT_KEYS" ] &&
        [ "$HOST_CONTROLLER_KEYS" != "$HOST_FEED_KEYS" ] &&
        [ "$HOST_GRANT_KEYS" != "$HOST_FEED_KEYS" ] || {
        echo "controller, grant, and feed key directories must be distinct" >&2
        exit 1
    }
    local a b c
    a=$(sudo_host sha256sum "$HOST_CONTROLLER_KEYS/$1.key" | cut -d' ' -f1)
    b=$(sudo_host sha256sum "$HOST_GRANT_KEYS/$2.key" | cut -d' ' -f1)
    c=$(sudo_host sha256sum "$HOST_FEED_KEYS/$3.key" | cut -d' ' -f1)
    [ "$a" != "$b" ] && [ "$a" != "$c" ] && [ "$b" != "$c" ] || {
        echo "controller, grant, and feed keys must contain distinct material" >&2
        exit 1
    }
}

install_feed_receiver() {
    scp -o ConnectTimeout=8 -q "$PROJ_ROOT/dpu/feed_receiver.py" \
        "$PROJ_ROOT/packaging/dpumesh-feed-receiver.service" "$DPU_HOST:/tmp/"
    ssh -o ConnectTimeout=8 "$DPU_HOST" "
        set -e
        echo '$DPU_PASS' | sudo -S -p '' id -u '$FEED_USER' >/dev/null 2>&1 ||
            echo '$DPU_PASS' | sudo -S -p '' useradd --system --no-create-home \
                --shell /usr/sbin/nologin '$FEED_USER'
        echo '$DPU_PASS' | sudo -S -p '' install -d -o root -g root -m 0755 /etc/dpumesh
        echo '$DPU_PASS' | sudo -S -p '' install -d -o '$FEED_USER' -g '$FEED_USER' -m 0755 /etc/dpumesh/feeds
        echo '$DPU_PASS' | sudo -S -p '' install -o root -g root -m 0555 \
            /tmp/feed_receiver.py /usr/local/bin/dpumesh-feed-receiver
        echo '$DPU_PASS' | sudo -S -p '' install -o root -g root -m 0644 \
            /tmp/dpumesh-feed-receiver.service /etc/systemd/system/dpumesh-feed-receiver.service
        printf '%s\n' \
            'DPUMESH_FEED_BIND=$FEED_BIND' \
            'DPUMESH_FEED_PORT=$FEED_PORT' >/tmp/feed-receiver.env
        echo '$DPU_PASS' | sudo -S -p '' install -o root -g root -m 0600 \
            /tmp/feed-receiver.env /etc/dpumesh/feed-receiver.env
        rm -f /tmp/feed_receiver.py /tmp/dpumesh-feed-receiver.service /tmp/feed-receiver.env
        echo '$DPU_PASS' | sudo -S -p '' systemctl daemon-reload
        echo '$DPU_PASS' | sudo -S -p '' systemctl enable dpumesh-feed-receiver.service
        echo '$DPU_PASS' | sudo -S -p '' systemctl restart dpumesh-feed-receiver.service
        echo '$DPU_PASS' | sudo -S -p '' rm -f /etc/dpumesh/membership.v1 \
            /etc/dpumesh/topology.v1 /etc/dpumesh/service-targets.v1
        echo '$DPU_PASS' | sudo -S -p '' systemctl is-active --quiet dpumesh-feed-receiver.service
    "
}

prepare() {
    need_dpu
    local controller_id grant_id feed_id
    controller_id=$(ensure_host_key "$HOST_CONTROLLER_KEYS" "$CONTROLLER_KEY_ID")
    grant_id=$(ensure_host_key "$HOST_GRANT_KEYS" "$GRANT_KEY_ID")
    feed_id=$(ensure_host_key "$HOST_FEED_KEYS" "$FEED_KEY_ID")
    assert_key_roles "$controller_id" "$grant_id" "$feed_id"
    install_dpu_key "$HOST_CONTROLLER_KEYS" "$DPU_CONTROLLER_KEYS" "$controller_id" public
    install_dpu_key "$HOST_GRANT_KEYS" "$DPU_GRANT_KEYS" "$grant_id" public
    install_dpu_key "$HOST_FEED_KEYS" "$DPU_FEED_KEYS" "$feed_id" secret
    install_feed_receiver
    echo "controller trust and DPU feed receiver are ready"
}

node_record() {
    need_host
    local node_name key_id grant_public
    node_name="${1:-${DPUMESH_NODE_NAME:-$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}')}}"
    [ -n "$node_name" ] || { echo "cannot resolve the Kubernetes node name" >&2; exit 1; }
    key_id=$(sudo_host cat "$HOST_GRANT_KEYS/active" | tr -d '[:space:]')
    valid_key_id "$key_id" || { echo "no active grant key" >&2; exit 1; }
    grant_public=$(sudo_host cat "$HOST_GRANT_KEYS/$key_id.key" | derive_public_hex)
    printf '%s %s %s %s %064d\n' "$node_name" "${2:-$NODE_RDMA_ADDR}" \
        "$key_id" "$grant_public" 0
}

nodes_config() {
    if [ -n "$NODES_FILE" ]; then
        [ -s "$NODES_FILE" ] && [ -r "$NODES_FILE" ] || {
            echo "node configuration is empty or unreadable: $NODES_FILE" >&2
            exit 1
        }
        sed '/^[[:space:]]*#/d; /^[[:space:]]*$/d' "$NODES_FILE"
    else
        node_record
    fi
}

prepare_pki() {
    local node_name="$1" service_ip="$2" serial
    mkdir -p "$PKI_DIR"
    chmod 0700 "$PKI_DIR"
    if [ ! -s "$PKI_DIR/ca.key" ] || [ ! -s "$PKI_DIR/ca.crt" ]; then
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
            -sha256 -days 3650 -nodes -subj "/CN=DPUmesh node CA" \
            -keyout "$PKI_DIR/ca.key" -out "$PKI_DIR/ca.crt" >/dev/null 2>&1
        chmod 0600 "$PKI_DIR/ca.key"
    fi
    serial=$(date +%s)
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -subj "/CN=dpumesh-controller" -keyout "$PKI_DIR/server.key" \
        -out "$PKI_DIR/server.csr" >/dev/null 2>&1
    printf 'subjectAltName=DNS:dpumesh-controller.%s.svc,DNS:dpumesh-controller.%s.svc.cluster.local,IP:%s\nextendedKeyUsage=serverAuth\n' \
        "$NS" "$NS" "$service_ip" >"$PKI_DIR/server.ext"
    openssl x509 -req -sha256 -days 365 -set_serial "$serial" \
        -in "$PKI_DIR/server.csr" -CA "$PKI_DIR/ca.crt" -CAkey "$PKI_DIR/ca.key" \
        -extfile "$PKI_DIR/server.ext" -out "$PKI_DIR/server.crt" >/dev/null 2>&1
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -subj "/CN=$node_name" -keyout "$PKI_DIR/node.key" \
        -out "$PKI_DIR/node.csr" >/dev/null 2>&1
    printf 'subjectAltName=URI:spiffe://dpumesh.io/node/%s\nextendedKeyUsage=clientAuth\n' \
        "$node_name" >"$PKI_DIR/node.ext"
    openssl x509 -req -sha256 -days 365 -set_serial "$((serial + 1))" \
        -in "$PKI_DIR/node.csr" -CA "$PKI_DIR/ca.crt" -CAkey "$PKI_DIR/ca.key" \
        -extfile "$PKI_DIR/node.ext" -out "$PKI_DIR/node.crt" >/dev/null 2>&1
    chmod 0600 "$PKI_DIR/server.key" "$PKI_DIR/node.key"
    sudo_host install -d -o root -g root -m 0700 /etc/dpumesh/tls
    sudo_host install -o root -g root -m 0400 "$PKI_DIR/node.key" /etc/dpumesh/tls/node.key
    sudo_host install -o root -g root -m 0444 "$PKI_DIR/node.crt" /etc/dpumesh/tls/node.crt
    sudo_host install -o root -g root -m 0444 "$PKI_DIR/ca.crt" /etc/dpumesh/tls/controller-ca.crt
}

apply_secrets() {
    local temporary="$1" controller_id="$2" grant_id="$3" feed_id="$4"
    sudo_host install -o "$(id -u)" -g "$(id -g)" -m 0600 \
        "$HOST_CONTROLLER_KEYS/$controller_id.key" "$temporary/controller.key"
    sudo_host install -o "$(id -u)" -g "$(id -g)" -m 0600 \
        "$HOST_GRANT_KEYS/$grant_id.key" "$temporary/grant.key"
    sudo_host install -o "$(id -u)" -g "$(id -g)" -m 0600 \
        "$HOST_FEED_KEYS/$feed_id.key" "$temporary/feed.key"
    kubectl -n "$NS" create secret generic dpumesh-controller-signing \
        --from-literal=active="$controller_id" --from-file=signing-key="$temporary/controller.key" \
        --dry-run=client -o yaml | kubectl apply -f -
    kubectl -n "$NS" create secret generic dpumesh-registration-signing \
        --from-literal=active="$grant_id" --from-file=signing-key="$temporary/grant.key" \
        --dry-run=client -o yaml | kubectl apply -f -
    kubectl -n "$NS" create secret generic dpumesh-feed-signing \
        --from-literal=active="$feed_id" --from-file=signing-key="$temporary/feed.key" \
        --dry-run=client -o yaml | kubectl apply -f -
    kubectl -n "$NS" create secret tls dpumesh-controller-tls \
        --cert="$PKI_DIR/server.crt" --key="$PKI_DIR/server.key" \
        --dry-run=client -o yaml | kubectl apply -f -
    kubectl -n "$NS" create configmap dpumesh-node-ca --from-file=ca.crt="$PKI_DIR/ca.crt" \
        --dry-run=client -o yaml | kubectl apply -f -
}

deploy() {
    need_host
    : "${IMG_CONTROLLER:?IMG_CONTROLLER is required}"
    local node_name controller_id grant_id feed_id service_ip temporary
    node_name="${DPUMESH_NODE_NAME:-$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}')}"
    controller_id=$(sudo_host cat "$HOST_CONTROLLER_KEYS/active" | tr -d '[:space:]')
    grant_id=$(sudo_host cat "$HOST_GRANT_KEYS/active" | tr -d '[:space:]')
    feed_id=$(sudo_host cat "$HOST_FEED_KEYS/active" | tr -d '[:space:]')
    kubectl -n "$NS" apply -f - >/dev/null <<EOF
apiVersion: v1
kind: Service
metadata: { name: dpumesh-controller }
spec:
  selector: { app: dpumesh-controller }
  ports: [{ name: node-mtls, port: 8443, targetPort: 8443 }]
EOF
    service_ip=$(kubectl -n "$NS" get service dpumesh-controller -o jsonpath='{.spec.clusterIP}')
    prepare_pki "$node_name" "$service_ip"
    temporary=$(mktemp -d)
    trap 'rm -rf "$temporary"' EXIT
    apply_secrets "$temporary" "$controller_id" "$grant_id" "$feed_id"
    export NS IMG_CONTROLLER
    export DPUMESH_CLUSTER_ID="$CLUSTER_ID" DPUMESH_NODE_NAME="$node_name"
    export DPUMESH_CONTROLLER_KEY_ID="$controller_id" DPUMESH_REGISTRATION_KEY_ID="$grant_id"
    export DPUMESH_FEED_KEY_ID="$feed_id"
    DPUMESH_CONTROLLER_NODES_YAML=$(nodes_config | sed 's/^/    /')
    export DPUMESH_CONTROLLER_NODES_YAML
    DPUMESH_CONTROLLER_PROTECTED_ARGS=""
    local key
    for key in ${DPUMESH_PROTECTED_SERVICES:-}; do
        DPUMESH_CONTROLLER_PROTECTED_ARGS+="        - --protected=$key"$'\n'
    done
    export DPUMESH_CONTROLLER_PROTECTED_ARGS
    envsubst <"$BENCH_DIR/k8s/controller.yaml" | kubectl apply -f -
    kubectl rollout restart deployment/dpumesh-controller -n "$NS"
    kubectl rollout status deployment/dpumesh-controller -n "$NS" --timeout=120s
    rm -rf "$temporary"
    trap - EXIT
}

case "${1:-status}" in
    prepare) prepare ;;
    deploy) deploy ;;
    node-record) node_record "${2:-}" "${3:-}" ;;
    nodes-config) nodes_config ;;
    topology-show) need_dpu; ssh "$DPU_HOST" "cat /etc/dpumesh/feeds/topology.v1" ;;
    receiver-status) need_dpu; ssh "$DPU_HOST" "systemctl status dpumesh-feed-receiver --no-pager" ;;
    status) kubectl get deployment,pod -n "$NS" -l app=dpumesh-controller -o wide ;;
    *) echo "usage: $0 prepare|deploy|node-record [node address]|nodes-config|topology-show|receiver-status|status" >&2; exit 2 ;;
esac
