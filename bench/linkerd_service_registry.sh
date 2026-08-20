#!/bin/bash
# The Service-target feed the embedded L7 adapter consumes.
#
# The node agent publishes it: it derives the feed from the topology generation
# it holds — the generation already names each Service's ClusterIP and its
# ready endpoints, so there is one source of truth and no second set of
# Kubernetes reads — signs it with the feed keyring, and delivers it over the
# same hop as every other feed. This script is the operator's view of that:
# which Services the agent is told to name, and what the DPU ends up holding.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ -f "$PROJECT_ROOT/.env" ]; then
    set -a
    source "$PROJECT_ROOT/.env"
    set +a
fi

NS="${NS:-test-bench}"
SERVICES="${DPUMESH_L7_SERVICES:-$NS/echo-dpumesh}"
FEED_ROOT="${DPUMESH_FEED_ROOT_DPU:-/etc/dpumesh}"
TARGETS_DPU="${DPUMESH_L7_SERVICE_TARGETS_FILE:-$FEED_ROOT/service-targets.v1}"

# The agent takes one --l7-service argument per Service. Rendering them here
# keeps the Service list in one place for the manifest and for this view.
service_args() {
    local key
    for key in ${SERVICES//,/ }; do
        [[ "$key" =~ ^[a-z0-9.-]+/[a-z0-9-]+$ ]] || {
            echo "invalid DPUmesh Service (want namespace/name): $key" >&2
            exit 2
        }
        printf '        - --l7-service=%s\n' "$key"
    done
}

case "${1:-show}" in
    service-args)
        service_args
        ;;
    validate)
        service_args >/dev/null
        echo "Service list is well formed: $SERVICES"
        ;;
    show)
        : "${DPU_HOST:?DPU_HOST is required}"
        ssh "$DPU_HOST" "cat '$TARGETS_DPU'"
        ;;
    *)
        echo "Usage: $0 service-args|validate|show" >&2
        exit 2
        ;;
esac
