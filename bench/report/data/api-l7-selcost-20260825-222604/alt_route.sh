#!/bin/bash
# Apply/delete the 50/50 weighted HTTPRoute across echo-grpc-dpumesh and
# echo-grpc-alt, and read each Service's cumulative outbound request_total from
# the DPU admin endpoints (summed across workers, keyed by dst pod).
set -euo pipefail
PROJ=/home/jukebox/DPUmesh
set -a; source "$PROJ/.env"; set +a
export NS="${NS:-test-bench}" CTRL_PORT="${CTRL_PORT:-9092}"
export ROUTE_PATH="/grpc.testing.BenchmarkService"
export ROUTE_WEIGHT_A=50 ROUTE_WEIGHT_B=50
SSH_OPTS=(-o ServerAliveInterval=15 -o ConnectTimeout=10 -o BatchMode=yes)

admin_ports() {
    local base="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}" port; base="${base##*:}"
    for ((port = base; port < base + 8; port++)); do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
            "curl -sf --max-time 3 127.0.0.1:$port/metrics >/dev/null" 2>/dev/null &&
            echo "$port"
    done
}

dpu_metrics() {
    local port
    for port in $(admin_ports); do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
            "curl -sf --max-time 5 127.0.0.1:$port/metrics" 2>/dev/null || true
    done
}

split() {  # split <service>
    { dpu_metrics | grep -E "^request_total.*direction=\"outbound\".*$1" || true; } |
        sed 's/.*dst_pod="\([^"]*\)".*} /\1 /' |
        awk '{ t[$1] += $2 } END { for (pod in t) if (t[pod] > 0) print pod, t[pod] }' | sort
}

case "${1:-}" in
    apply)  envsubst <"$PROJ/bench/k8s/policy/httproute-weighted.yaml" | kubectl apply -f - ;;
    delete) envsubst <"$PROJ/bench/k8s/policy/httproute-weighted.yaml" | kubectl delete --ignore-not-found -f - ;;
    split)  echo "== echo-grpc-dpumesh"; split echo-grpc-dpumesh
            echo "== echo-grpc-alt";     split echo-grpc-alt ;;
    locks)  dpu_metrics | grep -E "^dmesh_backend_lock" || echo no-lock-counters ;;
    *) echo "usage: $0 apply|delete|split|locks" >&2; exit 2 ;;
esac
