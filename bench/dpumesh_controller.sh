#!/bin/bash
# Provision the controller signing key and deploy the dpumesh-controller Pod.
# The generation reaches a DPU through that node's agent and through nothing
# else: the controller serves it on the cluster network and the agent's
# delivery loop installs it.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
if [ -f "$PROJ_ROOT/.env" ]; then
    set -a
    source "$PROJ_ROOT/.env"
    set +a
fi

NS="${NS:-test-bench}"
# Private signing key: host-only, root-only. The DPU holds public keys only.
HOST_KEY_DIR="${DPUMESH_CONTROLLER_KEY_DIR_HOST:-/etc/dpumesh/controller.keys}"
DPU_KEY_DIR="${DPUMESH_CONTROLLER_KEY_DIR_DPU:-/etc/dpumesh/controller.pub.keys}"
DEFAULT_KEY_ID="${DPUMESH_CONTROLLER_KEY_ID:-controller-v1}"
REGISTRATION_KEY_DIR="${DPUMESH_REGISTRATION_KEY_DIR_HOST:-/etc/dpumesh/registration.keys}"
TOPOLOGY_HOST="${DPUMESH_TOPOLOGY_FILE_HOST:-/run/dpumesh/topology.v1}"
TOPOLOGY_DPU="${DPUMESH_TOPOLOGY_FILE:-/etc/dpumesh/topology.v1}"
# The DPU-to-DPU transport address this node publishes.
NODE_RDMA_ADDR="${DPUMESH_NODE_RDMA_ADDR:-192.168.100.2:4791}"

valid_key_id() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]$ ]]
}

need_host() { : "${HOST_PASS:?HOST_PASS is required}"; }
need_dpu() {
    need_host
    : "${DPU_HOST:?DPU_HOST is required}"
    : "${DPU_PASS:?DPU_PASS is required}"
}

# stdin: 32-byte raw Ed25519 seed; stdout: 64 hex chars of the raw public key.
derive_public_hex() {
    { printf '302e020100300506032b657004220420' | xxd -r -p; cat; } |
        openssl pkey -inform DER -pubout -outform DER 2>/dev/null |
        tail -c 32 | xxd -p -c 64
}

install_private_key() {
    local key_id="$1" temporary
    valid_key_id "$key_id" || { echo "invalid controller key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S mkdir -p "$HOST_KEY_DIR"
    echo "$HOST_PASS" | sudo -S chown root:root "$HOST_KEY_DIR"
    echo "$HOST_PASS" | sudo -S chmod 0700 "$HOST_KEY_DIR"
    if echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/$key_id.key" 2>/dev/null; then
        return
    fi
    temporary=$(mktemp)
    openssl rand 32 > "$temporary"
    chmod 0600 "$temporary"
    echo "$HOST_PASS" | sudo -S install -o root -g root -m 0400 \
        "$temporary" "$HOST_KEY_DIR/$key_id.key"
    rm -f "$temporary"
}

install_dpu_pubkey() {
    local key_id="$1" pub_hex
    valid_key_id "$key_id" || { echo "invalid controller key id: $key_id" >&2; exit 2; }
    pub_hex=$(echo "$HOST_PASS" | sudo -S cat "$HOST_KEY_DIR/$key_id.key" 2>/dev/null |
        derive_public_hex)
    [ ${#pub_hex} -eq 64 ] || {
        echo "cannot derive the controller public key of $key_id" >&2
        exit 1
    }
    printf '%s\n' "$pub_hex" |
        ssh -o ConnectTimeout=8 "$DPU_HOST" \
            "umask 077; cat > /tmp/dpumesh-controller.key.in; \
             echo '$DPU_PASS' | sudo -S mkdir -p '$DPU_KEY_DIR'; \
             echo '$DPU_PASS' | sudo -S chown root:root '$DPU_KEY_DIR'; \
             echo '$DPU_PASS' | sudo -S chmod 0700 '$DPU_KEY_DIR'; \
             echo '$DPU_PASS' | sudo -S install -o root -g root -m 0400 \
                 /tmp/dpumesh-controller.key.in '$DPU_KEY_DIR/$key_id.key'; \
             rm -f /tmp/dpumesh-controller.key.in"
}

set_active() {
    local key_id="$1" temporary
    valid_key_id "$key_id" || { echo "invalid controller key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/$key_id.key"
    temporary=$(mktemp)
    printf '%s\n' "$key_id" > "$temporary"
    chmod 0600 "$temporary"
    echo "$HOST_PASS" | sudo -S install -o root -g root -m 0400 \
        "$temporary" "$HOST_KEY_DIR/active"
    rm -f "$temporary"
}

prepare() {
    need_dpu
    if echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/active" 2>/dev/null; then
        active=$(echo "$HOST_PASS" | sudo -S cat "$HOST_KEY_DIR/active" 2>/dev/null)
        valid_key_id "$active" || { echo "invalid active controller key id" >&2; exit 1; }
        install_dpu_pubkey "$active"
    else
        install_private_key "$DEFAULT_KEY_ID"
        set_active "$DEFAULT_KEY_ID"
        install_dpu_pubkey "$DEFAULT_KEY_ID"
    fi
    echo "controller keyring ready: host=$HOST_KEY_DIR dpu=$DPU_KEY_DIR (public only)"
}

# The per-node identity records the controller publishes. The agent public key
# is derived from the node's active registration seed, so the generation and
# the verifier agree on the same key. The DPU static key is published all-zero
# until the node's agent reports the one its DPU generated at first boot.
nodes_config() {
    need_host
    local node_name key_id agent_pub
    node_name="${DPUMESH_NODE_NAME:-$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}')}"
    [ -n "$node_name" ] || { echo "cannot resolve the Kubernetes node name" >&2; exit 1; }
    key_id=$(echo "$HOST_PASS" | sudo -S -p '' cat "$REGISTRATION_KEY_DIR/active" 2>/dev/null | tr -d '[:space:]')
    valid_key_id "$key_id" || { echo "no active registration key in $REGISTRATION_KEY_DIR" >&2; exit 1; }
    agent_pub=$(echo "$HOST_PASS" | sudo -S -p '' cat "$REGISTRATION_KEY_DIR/$key_id.key" 2>/dev/null |
        derive_public_hex)
    [ ${#agent_pub} -eq 64 ] || { echo "cannot derive the agent public key" >&2; exit 1; }
    printf '%s %s %s %s %s\n' "$node_name" "$NODE_RDMA_ADDR" "$key_id" "$agent_pub" \
        "0000000000000000000000000000000000000000000000000000000000000000"
}

deploy() {
    need_host
    : "${IMG_CONTROLLER:?IMG_CONTROLLER is required}"
    command -v envsubst >/dev/null 2>&1 || {
        echo "envsubst not found (apt install gettext-base)" >&2
        exit 1
    }
    export NS IMG_CONTROLLER
    DPUMESH_CONTROLLER_NODES_YAML=$(nodes_config | sed 's/^/    /')
    export DPUMESH_CONTROLLER_NODES_YAML
    DPUMESH_CONTROLLER_PROTECTED_ARGS=""
    local key
    for key in ${DPUMESH_PROTECTED_SERVICES:-}; do
        DPUMESH_CONTROLLER_PROTECTED_ARGS+="        - --protected=$key"$'\n'
    done
    export DPUMESH_CONTROLLER_PROTECTED_ARGS
    envsubst < "$BENCH_DIR/k8s/controller.yaml" | kubectl apply -f -
    kubectl rollout restart deployment/dpumesh-controller -n "$NS"
    kubectl rollout status deployment/dpumesh-controller -n "$NS" --timeout=120s
    echo "dpumesh-controller ready in namespace $NS"
}

case "${1:-status}" in
    prepare)
        prepare
        ;;
    nodes-config)
        nodes_config
        ;;
    deploy)
        deploy
        ;;
    topology-published)
        # What the controller currently holds, on the node it runs on.
        cat "$TOPOLOGY_HOST"
        ;;
    topology-show)
        # What this node's DPU actually adopted, after the agent delivered it.
        : "${DPU_HOST:?DPU_HOST is required}"
        ssh "$DPU_HOST" "cat '$TOPOLOGY_DPU'"
        ;;
    stop)
        kubectl delete deployment/dpumesh-controller -n "$NS" --ignore-not-found=true
        ;;
    status)
        kubectl get deployment,pod -n "$NS" -l app=dpumesh-controller -o wide
        ;;
    *)
        echo "usage: $0 prepare|nodes-config|deploy|topology-published|topology-show|" >&2
        echo "       stop|status" >&2
        exit 2
        ;;
esac
