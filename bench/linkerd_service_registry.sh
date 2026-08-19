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
STATE="${DPUMESH_SERVICE_REGISTRY_STATE:-$PROJECT_ROOT/build/linkerd-service-registry/version}"
KEY_DIR="${DPUMESH_REGISTRATION_KEY_DIR_HOST:-/etc/dpumesh/registration.keys}"
UNIT="dpumesh-linkerd-service-registry.service"

# The feed carries the same authority as a registration grant, so it is signed
# by the same root-only keyring. The DPU refuses a generation it cannot verify.
sign_document() {
    local file="$1" key_id mac
    : "${HOST_PASS:?HOST_PASS is required to read the registration keyring}"
    key_id=$(echo "$HOST_PASS" | sudo -S -p '' cat "$KEY_DIR/active" 2>/dev/null | tr -d '[:space:]')
    [[ "$key_id" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]{0,30}$ ]] || {
        echo "no usable active registration key id in $KEY_DIR" >&2
        return 1
    }
    mac=$(echo "$HOST_PASS" | sudo -S -p '' cat "$KEY_DIR/$key_id.key" 2>/dev/null |
        xxd -p -c 256 |
        { read -r hexkey; openssl dgst -sha256 -mac HMAC -macopt "hexkey:$hexkey" -r < "$file"; } |
        cut -d' ' -f1)
    [ ${#mac} -eq 64 ] || { echo "failed to sign the Service target feed" >&2; return 1; }
    printf 'signature=%s,%s\n' "$key_id" "$mac" >> "$file"
}

# Deploy publishes an explicit sync while the watch unit is also publishing, so
# generation stamping and installation are held under one lock: two interleaved
# renames could otherwise install the older generation last and the consumer
# would reject it.
with_publish_lock() {
    mkdir -p "${STATE%/*}"
    ( flock 9; "$@" ) 9>"${STATE%/*}/.publish.lock"
}

# The DPU rejects a generation that is not newer than the one it holds, and a
# rejected feed fails new protected sessions. The published generation is
# therefore derived from the last one this publisher wrote, not from the wall
# clock alone, so an NTP step backwards cannot wedge the consumer.
next_version() {
    local last now version temporary
    mkdir -p "${STATE%/*}"
    last=$(cat "$STATE" 2>/dev/null || true)
    [[ "$last" =~ ^[0-9]+$ ]] || last=0
    now=$(date +%s%N)
    if [ "$now" -gt "$last" ]; then version="$now"; else version=$((last + 1)); fi
    temporary=$(mktemp "${STATE%/*}/.version.XXXXXX")
    printf '%s\n' "$version" > "$temporary"
    mv "$temporary" "$STATE"
    printf '%s\n' "$version"
}

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

# Each subset carries its own port list, so an address is only ever paired with
# a port from the subset that published it.
endpoint_lines() {
    local name="$1" service_id="$2"
    kubectl get endpoints "$name" -n "$NS" -o json |
        python3 -c '
import json
import sys

service_id = sys.argv[1]
document = json.load(sys.stdin)
seen = set()
for subset in document.get("subsets") or []:
    ports = [port.get("port") for port in subset.get("ports") or [] if port.get("port")]
    for address in subset.get("addresses") or []:
        ip = address.get("ip")
        if not ip:
            continue
        for port in ports:
            entry = (ip, port)
            if entry in seen:
                continue
            seen.add(entry)
            print(f"endpoint={service_id},{ip}:{port}")
' "$service_id"
}

publish_targets() {
    : "${DPU_HOST:?DPU_HOST is required}"
    local target temporary version service_id name cluster_ip port
    target=$(remote_path)
    temporary=$(mktemp)
    version=$(next_version)
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
        endpoint_lines "$name" "$service_id" >> "$temporary"
    done
    sign_document "$temporary" || { rm -f "$temporary"; exit 1; }
    ssh "$DPU_HOST" "mkdir -p '${target%/*}'"
    rsync -az --chmod=F600 "$temporary" "$DPU_HOST:$target.new"
    ssh "$DPU_HOST" "mv '$target.new' '$target'"
    rm -f "$temporary"
    echo "published Service target generation $version to $DPU_HOST:$target"
}

publish_withdrawal() {
    : "${DPU_HOST:?DPU_HOST is required}"
    local target temporary version
    target=$(remote_path)
    temporary=$(mktemp)
    version=$(next_version)
    printf 'version=%s\n' "$version" > "$temporary"
    sign_document "$temporary" || { rm -f "$temporary"; exit 1; }
    ssh "$DPU_HOST" "mkdir -p '${target%/*}'"
    rsync -az --chmod=F600 "$temporary" "$DPU_HOST:$target.new"
    ssh "$DPU_HOST" "mv '$target.new' '$target'"
    rm -f "$temporary"
    echo "withdrew all Service targets at generation $version"
}

sync_once() { with_publish_lock publish_targets; }
withdraw_all() { with_publish_lock publish_withdrawal; }

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
    # A transient unit inherits none of the caller's environment, and the watch
    # loop republishes every INTERVAL seconds, so the feed's contents and
    # destination travel with it.
    local carried=(--setenv=NS="$NS"
                   --setenv=DPUMESH_ATTEST_REGISTRY="$REGISTRY"
                   --setenv=DPUMESH_L7_SERVICE_IDS="$SERVICE_IDS"
                   --setenv=DPUMESH_SERVICE_REGISTRY_INTERVAL="$INTERVAL"
                   --setenv=DPUMESH_SERVICE_REGISTRY_STATE="$STATE"
                   --setenv=DPUMESH_REGISTRATION_KEY_DIR_HOST="$KEY_DIR")
    [ -z "${DPUMESH_L7_SERVICE_TARGETS_FILE:-}" ] ||
        carried+=(--setenv=DPUMESH_L7_SERVICE_TARGETS_FILE="$DPUMESH_L7_SERVICE_TARGETS_FILE")
    systemd-run --user --collect --unit="${UNIT%.service}" "${carried[@]}" \
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
