#!/bin/bash
# One-node deployment for the host-resident dpumeshd architecture.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
ENV_FILE="${DPUMESH_ENV_FILE:-$PROJ_ROOT/.env}"
if [ -e "$ENV_FILE" ]; then
    [ -f "$ENV_FILE" ] && [ -r "$ENV_FILE" ] || {
        echo "configuration is not a readable regular file: $ENV_FILE" >&2
        exit 1
    }
    set -a; source "$ENV_FILE"; set +a
fi

NS="${NS:-test-bench}"
NODE_NAME="${DPUMESH_NODE_NAME:-$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}')}"
SLOTS="${DPUMESH_SLOTS:-2}"
RINGS="${DPUMESH_RINGS_PER_POD:-8}"
IMG_CONTROLLER="${IMG_CONTROLLER_NATIVE:-bench/dpumesh-controller:native}"
IMG_ECHO="${IMG_ECHO_NATIVE:-bench/echo-dpumesh:native}"
IMG_BENCH="${IMG_BENCH_NATIVE:-bench/bench-dpumesh:native}"
[ "$SLOTS" = 2 ] || { echo "native deployment requires DPUMESH_SLOTS=2" >&2; exit 2; }
[ "$RINGS" = 8 ] || { echo "native deployment requires DPUMESH_RINGS_PER_POD=8" >&2; exit 2; }

require_rig() {
    : "${HOST_PASS:?HOST_PASS is required}"
    : "${DPU_HOST:?DPU_HOST is required}"
    : "${DPU_PASS:?DPU_PASS is required}"
    : "${HOST_PCI:?HOST_PCI is required}"
}

sudo_auth() {
    printf '%s\n' "$HOST_PASS" | sudo -S -p '' true
}

collect_doca_libs() {
    mkdir -p "$PROJ_ROOT/doca-libs"
    local library
    for library in \
        /opt/mellanox/doca/lib/x86_64-linux-gnu/libdoca_common.so* \
        /opt/mellanox/doca/lib/x86_64-linux-gnu/libdoca_comch.so* \
        /opt/mellanox/doca/lib/x86_64-linux-gnu/libdoca_dpa.so* \
        /opt/mellanox/flexio/lib/libflexio.so* \
        /lib/x86_64-linux-gnu/libmlx5.so* \
        /lib/x86_64-linux-gnu/libibverbs.so*; do
        [ -e "$library" ] && cp -a "$library" "$PROJ_ROOT/doca-libs/"
    done
}

import_image() {
    local image=$1
    docker save "$image" | sudo ctr -n k8s.io images import - >/dev/null
    sudo ctr -n k8s.io images list -q | grep -Fxq "docker.io/$image"
}

build_images() {
    require_rig
    sudo_auth
    make -C "$PROJ_ROOT" -j"$(nproc)" bench
    collect_doca_libs
    docker build -f "$BENCH_DIR/docker/dpumesh_controller.Dockerfile" \
        -t "$IMG_CONTROLLER" "$PROJ_ROOT"
    docker build -f "$BENCH_DIR/docker/echo_dpumesh.Dockerfile" \
        -t "$IMG_ECHO" "$PROJ_ROOT"
    docker build -f "$BENCH_DIR/docker/bench_dpumesh.Dockerfile" \
        -t "$IMG_BENCH" "$PROJ_ROOT"
    import_image "$IMG_CONTROLLER"
    import_image "$IMG_ECHO"
    import_image "$IMG_BENCH"
}

stop_workloads() {
    kubectl -n "$NS" scale deployment/echo-dpumesh-native \
        deployment/bench-dpumesh-native --replicas=0 >/dev/null 2>&1 || true
    kubectl -n "$NS" wait --for=delete pod \
        -l 'app in (echo-dpumesh-native,bench-dpumesh-native)' \
        --timeout=90s >/dev/null 2>&1 || true
    # The broker has a bounded DPU-side unregistration barrier outside the Pod.
    sleep 12
}

deploy_controller() {
    kubectl get namespace "$NS" >/dev/null 2>&1 || kubectl create namespace "$NS"
    kubectl label namespace "$NS" \
        pod-security.kubernetes.io/enforce=restricted \
        pod-security.kubernetes.io/enforce-version=v1.31 \
        pod-security.kubernetes.io/audit=restricted \
        pod-security.kubernetes.io/audit-version=v1.31 \
        pod-security.kubernetes.io/warn=restricted \
        pod-security.kubernetes.io/warn-version=v1.31 --overwrite >/dev/null
    "$BENCH_DIR/dpumesh_controller.sh" prepare
    IMG_CONTROLLER="$IMG_CONTROLLER" "$BENCH_DIR/dpumesh_controller.sh" deploy
}

configure_host() {
    local controller_ip dpu_feed pci temporary
    controller_ip=$(kubectl -n "$NS" get service dpumesh-controller \
        -o jsonpath='{.spec.clusterIP}')
    dpu_feed="${DPUMESH_DPU_FEED_HOST:-${DPU_HOST##*@}}"
    pci="${HOST_PCI#0000:}"
    temporary=$(mktemp)
    trap 'rm -f "$temporary"' RETURN
    {
        printf 'DPUMESH_NODE_NAME=%s\n' "$NODE_NAME"
        printf 'DPUMESH_SLOTS=%s\n' "$SLOTS"
        printf 'DPUMESH_CONTROLLER_URL=https://%s:8443\n' "$controller_ip"
        printf 'DPUMESH_DPU_FEED_HOST=%s\n' "$dpu_feed"
        printf 'DPUMESH_DPU_FEED_PORT=4788\n'
        printf 'DPUMESH_NODE_RDMA_ADDR=%s\n' \
            "${DPUMESH_NODE_RDMA_ADDR:-$dpu_feed:47900}"
        printf 'DPUMESH_PCI_ADDR=%s\n' "$pci"
        printf 'DPUMESH_RINGS_PER_POD=%s\n' "$RINGS"
        printf 'DPUMESH_WORKER_CPU_MAX=50000\n'
        printf 'DPUMESH_WORKER_MEMORY_HIGH=805306368\n'
        printf 'DPUMESH_WORKER_MEMORY_MAX=1073741824\n'
        printf 'DPUMESH_WORKER_PIDS_MAX=64\n'
    } > "$temporary"
    sudo "$PROJ_ROOT/packaging/configure-kubelet-reserve.sh" \
        /var/lib/kubelet/config.yaml 0-2 3Gi
    sudo "$PROJ_ROOT/packaging/install-host.sh" "$PROJ_ROOT"
    sudo install -o root -g root -m 0600 "$temporary" /etc/dpumesh/dpumeshd.env
    sudo systemctl enable --now dpumeshd
    kubectl wait --for=condition=Ready "node/$NODE_NAME" --timeout=120s >/dev/null
    rm -f "$temporary"
    trap - RETURN
}

start_dpu() {
    DPUMESH_RINGS_PER_POD="$RINGS" "$BENCH_DIR/bench.sh" build
    DPUMESH_RINGS_PER_POD="$RINGS" "$BENCH_DIR/bench.sh" restart
}

deploy_workloads() {
    local manifest
    export NS RINGS IMG_ECHO IMG_BENCH
    manifest=$(envsubst < "$BENCH_DIR/k8s/native-hw.yaml")
    printf '%s\n' "$manifest" | kubectl apply --dry-run=server -f - >/dev/null
    printf '%s\n' "$manifest" | kubectl apply -f - >/dev/null
    kubectl -n "$NS" rollout status deployment/echo-dpumesh-native --timeout=240s
    kubectl -n "$NS" rollout status deployment/bench-dpumesh-native --timeout=240s
}

smoke() {
    local address='' pong='' probe='' result route_ready=''
    # A host-runtime restart intentionally crashes attached clients; kubelet's
    # container backoff plus two serial DOCA broker registrations can exceed
    # 30 seconds on the real rig.
    for _attempt in $(seq 1 90); do
        address=$(kubectl -n "$NS" get pod -l app=bench-dpumesh-native \
            --field-selector=status.phase=Running \
            -o jsonpath='{.items[0].status.podIP}')
        if [ -n "$address" ] &&
           pong=$(printf 'PING\n' | timeout 3 nc -N "$address" 9092 2>/dev/null) &&
           [ "$pong" = PONG ]; then
            break
        fi
        sleep 1
    done
    [ "$pong" = PONG ] || { echo "native bench did not become ready" >&2; exit 1; }

    # The Pod can accept TCP before the controller has published its endpoint
    # generation to the DPU. Exercise the real data path until that converges;
    # PING alone only proves that the benchmark process is listening.
    for _attempt in $(seq 1 20); do
        probe=$(printf 'RUN 64 64 1 1 20 1\n' |
            timeout 15 nc -N "$address" 9092 2>/dev/null || true)
        if printf '%s' "$probe" | rg -q '^OK ' &&
           printf '%s' "$probe" | rg -q ' fail=0 ' &&
           printf '%s' "$probe" | rg -q ' drops=0 '; then
            route_ready=yes
            break
        fi
        sleep 1
    done
    if [ "$route_ready" != yes ]; then
        printf '%s\n' "$probe" >&2
        echo "native DPU data path did not become ready" >&2
        exit 1
    fi

    result=$(printf 'RUN 64 64 1 3 100 1\n' | timeout 30 nc -N "$address" 9092)
    printf '%s\n' "$result"
    printf '%s' "$result" | rg -q '^OK '
    printf '%s' "$result" | rg -q ' fail=0 '
    printf '%s' "$result" | rg -q ' drops=0 '
}

deploy() {
    require_rig
    sudo_auth
    stop_workloads
    deploy_controller
    configure_host
    start_dpu
    deploy_workloads
}

status() {
    kubectl -n "$NS" get deployment/dpumesh-controller \
        deployment/echo-dpumesh-native deployment/bench-dpumesh-native
    sudo systemctl --no-pager --full status dpumeshd
}

case "${1:-all}" in
    all) build_images; deploy; smoke ;;
    build) build_images; DPUMESH_RINGS_PER_POD="$RINGS" "$BENCH_DIR/bench.sh" build ;;
    deploy) deploy ;;
    smoke) smoke ;;
    status) require_rig; sudo_auth; status ;;
    *) echo "usage: $0 all|build|deploy|smoke|status" >&2; exit 2 ;;
esac
