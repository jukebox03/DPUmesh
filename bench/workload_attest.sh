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
MEMBERSHIP_HOST="${DPUMESH_MEMBERSHIP_FILE_HOST:-/run/dpumesh/membership.v1}"
MEMBERSHIP_DPU="${DPUMESH_MEMBERSHIP_FILE:-/etc/dpumesh/membership.v1}"
MEMBERSHIP_INTERVAL="${DPUMESH_MEMBERSHIP_PUSH_INTERVAL:-5}"
MEMBERSHIP_UNIT="dpumesh-membership.service"

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
    export NS IMG_WORKLOAD_AGENT DPUMESH_REGISTRATION_ISSUER="$ISSUER"
    DPUMESH_REGISTRY_YAML=$(sed 's/^/    /' "$BENCH_DIR/k8s/registry")
    export DPUMESH_REGISTRY_YAML
    envsubst < "$BENCH_DIR/k8s/workload-agent.yaml" | kubectl apply -f -
    # The image is rebuilt under one tag, so an unchanged Pod spec would keep
    # the previous agent binary running behind a successful apply.
    kubectl rollout restart daemonset/dpumesh-node-agent -n "$NS"
    kubectl rollout status daemonset/dpumesh-node-agent -n "$NS" --timeout=120s
    kubectl auth can-i list pods -n "$NS" \
        --as="system:serviceaccount:$NS:dpumesh-node-agent" | grep -qx yes
    [ "$(kubectl auth can-i list pods --all-namespaces \
        --as="system:serviceaccount:$NS:dpumesh-node-agent" 2>/dev/null || true)" = no ]
    echo "trusted workload node agent ready in namespace $NS"
}

# Move one membership generation to the DPU. The node agent owns the content;
# this only carries it, and installs it root-only because it is an input the
# verifier acts on. An unchanged generation is not reinstalled, so the consumer
# does not re-read a document it already holds.
membership_sync() {
    need_dpu
    [ -s "$MEMBERSHIP_HOST" ] || { echo "no membership document at $MEMBERSHIP_HOST" >&2; return 1; }
    local digest
    digest=$(sha256sum "$MEMBERSHIP_HOST" | cut -d' ' -f1)
    [ "$digest" != "${MEMBERSHIP_LAST_DIGEST:-}" ] || return 0
    local stage="/tmp/dpumesh-membership.v1.in"
    rsync -az --chmod=F600 -e "ssh -o ConnectTimeout=8" \
        "$MEMBERSHIP_HOST" "$DPU_HOST:$stage"
    ssh -o ConnectTimeout=8 "$DPU_HOST" \
        "echo '$DPU_PASS' | sudo -S install -d -o root -g root -m 0755 '${MEMBERSHIP_DPU%/*}' && \
         echo '$DPU_PASS' | sudo -S install -o root -g root -m 0644 '$stage' '$MEMBERSHIP_DPU.new' && \
         echo '$DPU_PASS' | sudo -S mv '$MEMBERSHIP_DPU.new' '$MEMBERSHIP_DPU' && \
         rm -f '$stage'"
    MEMBERSHIP_LAST_DIGEST="$digest"
    echo "installed membership generation $(sed -n 's/^version=//p' "$MEMBERSHIP_HOST") at $DPU_HOST:$MEMBERSHIP_DPU"
}

membership_watch() {
    while true; do
        membership_sync || echo "membership push failed; retrying in ${MEMBERSHIP_INTERVAL}s" >&2
        sleep "$MEMBERSHIP_INTERVAL"
    done
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
    membership-sync)
        membership_sync
        ;;
    membership-watch)
        membership_watch
        ;;
    membership-start)
        need_dpu
        systemctl --user stop "$MEMBERSHIP_UNIT" >/dev/null 2>&1 || true
        systemctl --user reset-failed "$MEMBERSHIP_UNIT" >/dev/null 2>&1 || true
        systemd-run --user --collect --unit="${MEMBERSHIP_UNIT%.service}" \
            --property=Restart=always --property=RestartSec=2s "$0" membership-watch
        systemctl --user is-active --quiet "$MEMBERSHIP_UNIT"
        echo "membership publication running (interval=${MEMBERSHIP_INTERVAL}s)"
        ;;
    membership-stop)
        systemctl --user stop "$MEMBERSHIP_UNIT" >/dev/null 2>&1 || true
        ;;
    membership-show)
        : "${DPU_HOST:?DPU_HOST is required}"
        ssh "$DPU_HOST" "cat '$MEMBERSHIP_DPU'"
        ;;
    stop)
        systemctl --user stop "$MEMBERSHIP_UNIT" >/dev/null 2>&1 || true
        kubectl delete daemonset/dpumesh-node-agent -n "$NS" --ignore-not-found=true
        ;;
    status)
        kubectl get daemonset,pod -n "$NS" -l app=dpumesh-node-agent -o wide
        ;;
    *)
        echo "usage: $0 prepare|deploy|rotate-stage KEY_ID|activate KEY_ID|prune KEY_ID|" >&2
        echo "       membership-start|membership-stop|membership-sync|membership-show|" >&2
        echo "       stop|status" >&2
        exit 2
        ;;
esac
