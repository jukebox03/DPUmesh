#!/bin/bash
# Deploy the host-network, TLS-pass-through gateway used by the DPU to reach
# stock Linkerd control-plane Services. Endpoint resolution stays in Kubernetes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
if [ -f "$PROJECT_ROOT/.env" ]; then
    set -a
    source "$PROJECT_ROOT/.env"
    set +a
fi

LINKERD_CONTROL_NAMESPACE="${LINKERD_CONTROL_NAMESPACE:-linkerd}"
LINKERD_GATEWAY_BIND="${LINKERD_GATEWAY_BIND:-192.168.100.1}"
LINKERD_GATEWAY_DST_PORT="${LINKERD_GATEWAY_DST_PORT:-28086}"
LINKERD_GATEWAY_POLICY_PORT="${LINKERD_GATEWAY_POLICY_PORT:-28087}"
LINKERD_GATEWAY_IDENTITY_PORT="${LINKERD_GATEWAY_IDENTITY_PORT:-28088}"
IMG_LINKERD_GATEWAY="${IMG_LINKERD_GATEWAY:-bench/dpumesh-linkerd-cp-gateway:latest}"
K8S_NODE_NAME="${K8S_NODE_NAME:-}"

start() {
    command -v envsubst >/dev/null 2>&1 || {
        echo "envsubst not found (apt install gettext-base)" >&2
        exit 1
    }
    if [ -z "$K8S_NODE_NAME" ]; then
        K8S_NODE_NAME=$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}')
    fi
    [ -n "$K8S_NODE_NAME" ] || { echo "no Kubernetes node selected" >&2; exit 1; }

    export LINKERD_CONTROL_NAMESPACE LINKERD_GATEWAY_BIND
    export LINKERD_GATEWAY_DST_PORT LINKERD_GATEWAY_POLICY_PORT
    export LINKERD_GATEWAY_IDENTITY_PORT IMG_LINKERD_GATEWAY K8S_NODE_NAME
    envsubst < "$SCRIPT_DIR/k8s/linkerd-cp-gateway.yaml" | kubectl apply -f -
    # The image is rebuilt under one tag, so an unchanged Pod spec would keep
    # the previous relay binary running behind a successful apply.
    kubectl rollout restart daemonset/dpumesh-linkerd-cp-gateway \
        -n "$LINKERD_CONTROL_NAMESPACE"
    kubectl rollout status daemonset/dpumesh-linkerd-cp-gateway \
        -n "$LINKERD_CONTROL_NAMESPACE" --timeout=120s
    kubectl auth can-i get services -n "$LINKERD_CONTROL_NAMESPACE" \
        --as="system:serviceaccount:$LINKERD_CONTROL_NAMESPACE:dpumesh-linkerd-cp-gateway" |
        grep -qx yes
    [ "$(kubectl auth can-i get services --all-namespaces \
        --as="system:serviceaccount:$LINKERD_CONTROL_NAMESPACE:dpumesh-linkerd-cp-gateway" \
        2>/dev/null || true)" = no ]
    echo "Linkerd control-plane gateway ready at $LINKERD_GATEWAY_BIND:{$LINKERD_GATEWAY_DST_PORT,$LINKERD_GATEWAY_POLICY_PORT,$LINKERD_GATEWAY_IDENTITY_PORT}"
}

stop() {
    kubectl delete daemonset/dpumesh-linkerd-cp-gateway \
        -n "$LINKERD_CONTROL_NAMESPACE" --ignore-not-found=true
}

status() {
    kubectl get daemonset,pod -n "$LINKERD_CONTROL_NAMESPACE" \
        -l app=dpumesh-linkerd-cp-gateway -o wide
}

case "${1:-}" in
    start) start ;;
    stop) stop ;;
    status) status ;;
    *) echo "Usage: $0 start|stop|status" >&2; exit 2 ;;
esac
