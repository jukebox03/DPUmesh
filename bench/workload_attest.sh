#!/bin/bash
# Provision the root-only registration keyring and deploy its node agent.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
if [ -f "$PROJ_ROOT/.env" ]; then
    set -a
    source "$PROJ_ROOT/.env"
    set +a
fi

HOST_KEY_DIR="${DPUMESH_REGISTRATION_KEY_DIR_HOST:-/etc/dpumesh/registration.keys}"
DPU_KEY_DIR="${DPUMESH_REGISTRATION_KEY_DIR_DPU:-/etc/dpumesh/registration.keys}"
DEFAULT_KEY_ID="${DPUMESH_REGISTRATION_KEY_ID:-node-hmac-v1}"
ISSUER="${DPUMESH_REGISTRATION_ISSUER:-dpumesh-node-agent}"
NS="${NS:-test-bench}"

valid_key_id() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]$ ]]
}

need_host() { : "${HOST_PASS:?HOST_PASS is required}"; }
need_dpu() {
    need_host
    : "${DPU_HOST:?DPU_HOST is required}"
    : "${DPU_PASS:?DPU_PASS is required}"
}

install_host_key() {
    local key_id="$1" temporary
    valid_key_id "$key_id" || { echo "invalid registration key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S install -d -o root -g root -m 0700 "$HOST_KEY_DIR"
    if echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/$key_id.key" 2>/dev/null; then
        echo "$HOST_PASS" | sudo -S chown root:root "$HOST_KEY_DIR/$key_id.key"
        echo "$HOST_PASS" | sudo -S chmod 0400 "$HOST_KEY_DIR/$key_id.key"
        return
    fi
    temporary=$(mktemp)
    openssl rand 32 > "$temporary"
    chmod 0600 "$temporary"
    echo "$HOST_PASS" | sudo -S install -o root -g root -m 0400 \
        "$temporary" "$HOST_KEY_DIR/$key_id.key"
    rm -f "$temporary"
}

set_active() {
    local key_id="$1" temporary
    valid_key_id "$key_id" || { echo "invalid registration key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/$key_id.key"
    temporary=$(mktemp)
    printf '%s\n' "$key_id" > "$temporary"
    chmod 0600 "$temporary"
    echo "$HOST_PASS" | sudo -S install -o root -g root -m 0400 \
        "$temporary" "$HOST_KEY_DIR/active"
    rm -f "$temporary"
}

install_dpu_key() {
    local key_id="$1"
    valid_key_id "$key_id" || { echo "invalid registration key id: $key_id" >&2; exit 2; }
    echo "$HOST_PASS" | sudo -S cat "$HOST_KEY_DIR/$key_id.key" 2>/dev/null |
        ssh -o ConnectTimeout=8 "$DPU_HOST" \
            "umask 077; cat > /tmp/dpumesh-registration.key.in; \
             echo '$DPU_PASS' | sudo -S install -d -o root -g root -m 0700 '$DPU_KEY_DIR'; \
             echo '$DPU_PASS' | sudo -S install -o root -g root -m 0400 \
                 /tmp/dpumesh-registration.key.in '$DPU_KEY_DIR/$key_id.key'; \
             rm -f /tmp/dpumesh-registration.key.in"
}

sync_dpu_keyring() {
    local filename key_id
    while IFS= read -r filename; do
        key_id=${filename%.key}
        install_dpu_key "$key_id"
    done < <(echo "$HOST_PASS" | sudo -S find "$HOST_KEY_DIR" -maxdepth 1 \
        -type f -name '*.key' -printf '%f\n' 2>/dev/null | LC_ALL=C sort)
}

prepare() {
    need_dpu
    echo "$HOST_PASS" | sudo -S install -d -o root -g root -m 0700 "$HOST_KEY_DIR"
    if echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/active" 2>/dev/null; then
        active=$(echo "$HOST_PASS" | sudo -S cat "$HOST_KEY_DIR/active" 2>/dev/null)
        valid_key_id "$active" || {
            echo "invalid active registration key id: $active" >&2
            exit 1
        }
        # Reuse the active keyring. In particular, do not resurrect the
        # bootstrap key after it has been pruned at the end of a rotation.
        echo "$HOST_PASS" | sudo -S test -f "$HOST_KEY_DIR/$active.key"
        echo "$HOST_PASS" | sudo -S chown root:root "$HOST_KEY_DIR/$active.key"
        echo "$HOST_PASS" | sudo -S chmod 0400 "$HOST_KEY_DIR/$active.key"
    else
        install_host_key "$DEFAULT_KEY_ID"
        set_active "$DEFAULT_KEY_ID"
    fi
    sync_dpu_keyring
    echo "trusted registration keyring ready: host=$HOST_KEY_DIR dpu=$DPU_KEY_DIR"
}

deploy_agent() {
    need_host
    : "${IMG_WORKLOAD_AGENT:?IMG_WORKLOAD_AGENT is required}"
    command -v envsubst >/dev/null 2>&1 || {
        echo "envsubst not found (apt install gettext-base)" >&2
        exit 1
    }
    # Retire the former Host systemd fixture before the DaemonSet owns the
    # shared hostPath socket.
    echo "$HOST_PASS" | sudo -S systemctl stop dpumesh-workload-attest.service \
        >/dev/null 2>&1 || true
    export NS IMG_WORKLOAD_AGENT DPUMESH_REGISTRATION_ISSUER="$ISSUER"
    DPUMESH_REGISTRY_YAML=$(sed 's/^/    /' "$BENCH_DIR/k8s/registry")
    export DPUMESH_REGISTRY_YAML
    envsubst < "$BENCH_DIR/k8s/workload-agent.yaml" | kubectl apply -f -
    kubectl rollout status daemonset/dpumesh-node-agent -n "$NS" --timeout=120s
    kubectl auth can-i list pods -n "$NS" \
        --as="system:serviceaccount:$NS:dpumesh-node-agent" | grep -qx yes
    [ "$(kubectl auth can-i list pods --all-namespaces \
        --as="system:serviceaccount:$NS:dpumesh-node-agent" 2>/dev/null || true)" = no ]
    echo "trusted workload node agent ready in namespace $NS"
}

case "${1:-status}" in
    prepare)
        prepare
        ;;
    deploy)
        deploy_agent
        ;;
    rotate-stage)
        need_dpu
        key_id="${2:?usage: $0 rotate-stage KEY_ID}"
        install_host_key "$key_id"
        install_dpu_key "$key_id"
        echo "key $key_id staged on Host and DPU; restart DPU before activating it"
        ;;
    activate)
        need_host
        key_id="${2:?usage: $0 activate KEY_ID}"
        set_active "$key_id"
        echo "node agent now signs with $key_id"
        ;;
    prune)
        need_dpu
        key_id="${2:?usage: $0 prune KEY_ID}"
        valid_key_id "$key_id" || { echo "invalid registration key id: $key_id" >&2; exit 2; }
        active=$(echo "$HOST_PASS" | sudo -S cat "$HOST_KEY_DIR/active" 2>/dev/null)
        [ "$active" != "$key_id" ] || {
            echo "refusing to prune active registration key $key_id" >&2
            exit 1
        }
        echo "$HOST_PASS" | sudo -S rm -f "$HOST_KEY_DIR/$key_id.key"
        ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S rm -f '$DPU_KEY_DIR/$key_id.key'"
        echo "pruned inactive registration key $key_id from Host and DPU"
        ;;
    stop)
        kubectl delete daemonset/dpumesh-node-agent -n "$NS" --ignore-not-found=true
        ;;
    status)
        kubectl get daemonset,pod -n "$NS" -l app=dpumesh-node-agent -o wide
        ;;
    *)
        echo "usage: $0 prepare|deploy|rotate-stage KEY_ID|activate KEY_ID|prune KEY_ID|stop|status" >&2
        exit 2
        ;;
esac
