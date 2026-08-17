#!/bin/bash
# Publish a monotonically versioned Service-id -> ClusterIP:port feed to the
# DPU. Each update is an atomic rename; the embedded proxy rejects rollback and
# target withdrawal affects new sessions immediately.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ -f "$PROJECT_ROOT/.env" ]; then
    set -a
    source "$PROJECT_ROOT/.env"
    set +a
fi

NS="${NS:-test-bench}"
REGISTRY="${DPUMESH_ATTEST_REGISTRY:-$SCRIPT_DIR/k8s/registry}"
SERVICE_IDS="${DPUMESH_L7_SERVICE_IDS:-11}"
INTERVAL="${DPUMESH_SERVICE_REGISTRY_INTERVAL:-10}"
UNIT="dpumesh-linkerd-service-registry.service"

remote_path() {
    if [ -n "${DPUMESH_L7_SERVICE_TARGETS_FILE:-}" ]; then
        printf '%s\n' "$DPUMESH_L7_SERVICE_TARGETS_FILE"
        return
    fi
    local remote_home
    remote_home=$(ssh "$DPU_HOST" 'printf %s "$HOME"')
    printf '%s/l7build/service-targets.v1\n' "$remote_home"
}

service_name() {
    local service_id="$1"
    awk -v wanted="$service_id" '
        /^[[:space:]]*#/ || NF == 0 { next }
        $3 == wanted { print $2; found++ }
        END { if (found != 1) exit 1 }
    ' "$REGISTRY"
}

sync_once() {
    : "${DPU_HOST:?DPU_HOST is required}"
    local target temporary version service_id name cluster_ip port endpoint_port endpoint_ip
    target=$(remote_path)
    temporary=$(mktemp)
    version=$(date +%s%N)
    printf 'version=%s\n' "$version" > "$temporary"
    for service_id in ${SERVICE_IDS//,/ }; do
        [[ "$service_id" =~ ^[0-9]+$ ]] || {
            echo "invalid DPUmesh Service id: $service_id" >&2
            rm -f "$temporary"
            exit 2
        }
        name=$(service_name "$service_id") || {
            echo "Service id $service_id is not unique in $REGISTRY" >&2
            rm -f "$temporary"
            exit 1
        }
        cluster_ip=$(kubectl get service "$name" -n "$NS" -o jsonpath='{.spec.clusterIP}')
        port=$(kubectl get service "$name" -n "$NS" -o jsonpath='{.spec.ports[0].port}')
        if [ -z "$cluster_ip" ] || [ "$cluster_ip" = None ] || [ -z "$port" ]; then
            echo "Service $NS/$name has no ClusterIP:port" >&2
            rm -f "$temporary"
            exit 1
        fi
        printf '%s=%s:%s\n' "$service_id" "$cluster_ip" "$port" >> "$temporary"
        endpoint_port=$(kubectl get endpoints "$name" -n "$NS" \
            -o jsonpath='{.subsets[0].ports[0].port}')
        while IFS= read -r endpoint_ip; do
            [ -n "$endpoint_ip" ] || continue
            printf 'endpoint=%s,%s:%s\n' \
                "$service_id" "$endpoint_ip" "$endpoint_port" >> "$temporary"
        done < <(kubectl get endpoints "$name" -n "$NS" \
            -o jsonpath='{range .subsets[*].addresses[*]}{.ip}{"\n"}{end}')
    done
    ssh "$DPU_HOST" "mkdir -p '${target%/*}'"
    rsync -az --chmod=F600 "$temporary" "$DPU_HOST:$target.new"
    ssh "$DPU_HOST" "mv '$target.new' '$target'"
    rm -f "$temporary"
    echo "published Service target generation $version to $DPU_HOST:$target"
}

withdraw_all() {
    : "${DPU_HOST:?DPU_HOST is required}"
    local target temporary version
    target=$(remote_path)
    temporary=$(mktemp)
    version=$(date +%s%N)
    printf 'version=%s\n' "$version" > "$temporary"
    ssh "$DPU_HOST" "mkdir -p '${target%/*}'"
    rsync -az --chmod=F600 "$temporary" "$DPU_HOST:$target.new"
    ssh "$DPU_HOST" "mv '$target.new' '$target'"
    rm -f "$temporary"
    echo "withdrew all Service targets at generation $version"
}

watch() {
    while true; do
        if ! sync_once; then
            echo "Service target update failed; retrying in ${INTERVAL}s" >&2
        fi
        sleep "$INTERVAL"
    done
}

start() {
    sync_once
    systemctl --user stop "$UNIT" >/dev/null 2>&1 || true
    systemctl --user reset-failed "$UNIT" >/dev/null 2>&1 || true
    systemd-run --user --collect --unit="${UNIT%.service}" \
        --property=Restart=always --property=RestartSec=2s "$0" watch
    systemctl --user is-active --quiet "$UNIT"
}

case "${1:-}" in
    sync) sync_once ;;
    withdraw) withdraw_all ;;
    watch) watch ;;
    start) start ;;
    stop) systemctl --user stop "$UNIT" ;;
    status) systemctl --user status "$UNIT" --no-pager ;;
    show) ssh "$DPU_HOST" "cat '$(remote_path)'" ;;
    *) echo "Usage: $0 sync|withdraw|start|stop|status|show" >&2; exit 2 ;;
esac
