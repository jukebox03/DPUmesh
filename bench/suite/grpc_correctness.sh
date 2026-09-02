#!/usr/bin/env bash
# Maintained correctness gate for the DPUmesh gRPC adapter.
#
# Local gates are non-hardware tests.  The hardware gate expects the gRPC
# benchmark scope to be deployed already; it deliberately churns the client Pod
# and temporarily applies the policy/routing fixtures.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"
RELEASE_BUILD="${GRPC_RELEASE_BUILD:-$PROJ_ROOT/build/grpc-release}"
SANITIZER_BUILD="${GRPC_SANITIZER_BUILD:-$PROJ_ROOT/build/grpc-asan-clang}"
MODE="${1:-all}"

say() { printf '\n=== %s ===\n' "$*"; }
die() { printf 'grpc_correctness: %s\n' "$*" >&2; exit 1; }

require_ctest_tree() {
    [ -f "$1/CTestTestfile.cmake" ] ||
        die "missing CTest tree $1 (configure integrations/grpc first)"
}

run_local() {
    say "host transport contracts"
    make -C "$PROJ_ROOT" test-hostfree

    # This unit links the real DPU proxy queue/SG-DMA implementation and thus
    # needs the DOCA SDK. Keep the host-only gate usable on developer machines,
    # but exercise the queue contract whenever the SDK is present.
    if pkg-config --exists doca-common doca-comch doca-dpa 2>/dev/null; then
        say "DPU proxy lane and SG-DMA queue contracts"
        make -C "$PROJ_ROOT" build/test/proxy_lane_queue_test
        "$PROJ_ROOT/build/test/proxy_lane_queue_test"
    else
        printf 'SKIP: DOCA SDK unavailable; proxy_lane_queue_test requires hardware SDK headers\n'
    fi

    require_ctest_tree "$RELEASE_BUILD"
    say "release adapter and real-cHTTP2 contracts"
    cmake --build "$RELEASE_BUILD" -j"${BUILD_JOBS:-2}"
    ctest --test-dir "$RELEASE_BUILD" --output-on-failure
}

run_sanitizer() {
    require_ctest_tree "$SANITIZER_BUILD"
    say "Clang ASAN+UBSAN adapter contracts"
    cmake --build "$SANITIZER_BUILD" -j"${BUILD_JOBS:-2}"
    # LeakSanitizer cannot attach under the benchmark host's ptrace policy.
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
        ctest --test-dir "$SANITIZER_BUILD" --output-on-failure
}

grpc_restart_total() {
    kubectl get pods -n "${NS:-test-bench}" \
        -l 'app in (bench-grpc-dpumesh,echo-grpc-dpumesh)' -o json |
        jq '[.items[].status.containerStatuses[]?.restartCount] | add // 0'
}

run_hardware() {
    [ -f "$PROJ_ROOT/.env" ] || die ".env is required for the hardware gate"
    command -v kubectl >/dev/null || die "kubectl is required for the hardware gate"
    command -v jq >/dev/null || die "jq is required for the hardware gate"
    set -a
    # shellcheck disable=SC1091
    source "$PROJ_ROOT/.env"
    set +a

    local before after receipt
    before=$(grpc_restart_total)
    [ "$before" = 0 ] || die "gRPC Pods already have $before container restart(s)"

    say "real-DPU shutdown, quiescence, slot reuse and four-channel exchange"
    "$BENCH" grpcshutdown

    receipt="${POLICY_RECEIPT:-$PROJ_ROOT/bench/report/data/policy-route-$(date +%Y%m%d-%H%M%S)}"
    say "gRPC policy and routing surfaces"
    OUT_DIR="$receipt" "$SUITE_DIR/policy_route.sh" grpc-surfaces

    say "final DPU session/task accounting"
    "$BENCH" l7metrics
    after=$(grpc_restart_total)
    [ "$after" = 0 ] || die "gRPC Pods accumulated $after container restart(s)"
    printf 'hardware receipt: %s\n' "$receipt"
}

case "$MODE" in
    local)     run_local ;;
    sanitizer) run_sanitizer ;;
    hardware)  run_hardware ;;
    all)       run_local; run_sanitizer; run_hardware ;;
    -h|--help)
        echo "usage: $0 [local|sanitizer|hardware|all]"
        echo "  GRPC_RELEASE_BUILD and GRPC_SANITIZER_BUILD select CTest trees."
        echo "  hardware requires an already-deployed gRPC benchmark scope."
        ;;
    *) die "unknown mode '$MODE' (expected local, sanitizer, hardware or all)" ;;
esac
