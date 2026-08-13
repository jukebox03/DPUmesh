#!/bin/bash
# Benchmark build, deployment, measurement, validation, pinning, and diagnostics.
# Use `deploy` to start the DPU and pods as one registration lifecycle. Deployment
# and pinning read repository-root `.env`; live runs require kubectl and nc.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERR]${NC} $*"; }
step()  { echo -e "${BLUE}[STEP]${NC} $*"; }

### ------------------------------------------------------------ config
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"     # .../bench
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"                      # repo root
if [ -f "$PROJ_ROOT/.env" ]; then
    set -a; source "$PROJ_ROOT/.env"; set +a
else
    warn ".env not found at $PROJ_ROOT/.env (deploy/pin need it; runs work without)"
fi

NS="${NS:-test-bench}"                 # k8s namespace
CTRL_PORT="${CTRL_PORT:-9092}"
TCP_PORT="${TCP_PORT:-9091}"
MANIFEST="$BENCH_DIR/k8s/pods.yaml"
# gRPC pods live with the integration that builds them.
GRPC_BENCH_DIR="$PROJ_ROOT/integrations/grpc/bench"
GRPC_MANIFEST="$GRPC_BENCH_DIR/k8s/pods.yaml"
# linkerd reuses the gRPC images and adds injected sidecars, so its pods live
# with the linkerd integration. BENCH_LINKERD=1 admits them; without it the
# manifest is not applied and no linkerd pod reaches the cluster.
LINKERD_BENCH_DIR="$PROJ_ROOT/linkerd/bench"
LINKERD_MANIFEST="$LINKERD_BENCH_DIR/k8s/pods.yaml"
BENCH_LINKERD="${BENCH_LINKERD:-0}"

INCLUDE_SRC="$PROJ_ROOT/include"
DOCA_SRC="$PROJ_ROOT/doca"
# The DPU binary also compiles the L7 adapter contract and its consumer.
LINKERD_INCLUDE_SRC="$PROJ_ROOT/linkerd/include"
LINKERD_SHIM_SRC="$PROJ_ROOT/linkerd/shim"
# What L7_BACKEND=linkerd needs built on the DPU: the staticlib and the mock
# control plane. `rust` and `port` are siblings there because
# linkerd/rust/Cargo.toml resolves the port by relative path.
LINKERD_RUST_SRC="$PROJ_ROOT/linkerd/rust"
LINKERD_PORT_SRC="$PROJ_ROOT/linkerd/port/linkerd2-proxy"
DPU_L7_BUILD="l7build"
LINKERD_TOOLCHAIN="${LINKERD_TOOLCHAIN:-1.90.0}"
# rustup's shims are not on a non-interactive ssh PATH, and the distribution
# cargo that is there is older than the toolchain the port pins.
LINKERD_CARGO="${LINKERD_CARGO:-\$HOME/.cargo/bin/cargo}"
LIB_OUT="$PROJ_ROOT/build/lib"
BIN_OUT="$PROJ_ROOT/build/bin"
DPU_PROJ="${DPU_PROJ:-DPUmesh}"        # project directory name on the DPU
DPU_DOCA="$DPU_PROJ/doca"
DPU_INCLUDE="$DPU_PROJ/include"
DPU_LINKERD_INCLUDE="$DPU_PROJ/linkerd/include"
DPU_LINKERD_SHIM="$DPU_PROJ/linkerd/shim"
DPU_BUILD="$DPU_DOCA/build"
DPU_LOG="/tmp/dpumesh_dpu_bench.log"
DOCA_LIB_DIR="/opt/mellanox/doca/lib/x86_64-linux-gnu"
FLEXIO_LIB_DIR="/opt/mellanox/flexio/lib"

IMG_BENCH_DPU="bench/bench-dpumesh:latest"
IMG_ECHO_DPU="bench/echo-dpumesh:latest"
IMG_LOOPBACK_DPU="bench/loopback-dpumesh:latest"
IMG_PRELOAD_DPU="bench/preload-dpumesh:latest"
IMG_PRELOAD_SOCK="bench/preload-sock:latest"
IMG_VERBS_DPU="bench/verbs-dpumesh:latest"
IMG_BENCH_GRPC="bench/bench-grpc:latest"
IMG_ECHO_GRPC="bench/echo-grpc:latest"
IMG_BENCH_TCP="bench/bench-tcp:latest"
IMG_ECHO_TCP="bench/echo-tcp:latest"
# BENCH_ENVOY_DEBUG=1 selects the unstripped build of the same release, which
# resolves sidecar symbols in a profile.
if [ "${BENCH_ENVOY_DEBUG:-0}" = 1 ]; then
    IMG_ENVOY_BASE="envoyproxy/envoy:debug-v1.30-latest"
    IMG_ENVOY="bench/envoy-numa:debug-v1.30-latest"
else
    IMG_ENVOY_BASE="envoyproxy/envoy:v1.30-latest"
    IMG_ENVOY="bench/envoy-numa:v1.30-latest"
fi

# benchmark sweep knobs
OUT="${OUT:-/tmp/dpumesh-bench}"
LAT_DUR="${LAT_DUR:-10}"; BW_DUR="${BW_DUR:-10}"; RATE_DUR="${RATE_DUR:-10}"
WARMUP="${WARMUP:-1000}"; BW_CONC="${BW_CONC:-32}"; RATE_CONC="${RATE_CONC:-32}"
RATE_THREADS="${RATE_THREADS:-1 2 4 8}"
LAT_SIZES="${LAT_SIZES:-64 128 256 512 1024}"
BW_SIZES="${BW_SIZES:-32 128 512 2048 8192 32768 131072 524288 1000000 2097152 8000000}"
BENCH_NUMA_NODE="${BENCH_NUMA_NODE:-}"
BENCH_CORE_BASE="${BENCH_CORE_BASE:-}"
BENCH_NUMA_POLICY="${BENCH_NUMA_POLICY:-local}"
BENCH_DEPLOY_SCOPE="${BENCH_DEPLOY_SCOPE:-all}"
BENCH_NUMA_CONFIGURED=0

# Which build the gRPC server image carries. `asan` instruments echo_grpc and
# keeps debug info, turning a fault the release binary reports only as a signal
# into a symbolized report. The client stays on the release build either way: an
# instrumented generator cannot offer the load that provokes the fault.
BENCH_GRPC_BUILD="${BENCH_GRPC_BUILD:-release}"
GRPC_BENCH_BUILD_DIR="build/grpc-release"
case "$BENCH_GRPC_BUILD" in
    release) GRPC_ECHO_BUILD_DIR="build/grpc-release" ;;
    asan)    GRPC_ECHO_BUILD_DIR="build/grpc-asan" ;;
    *) echo "BENCH_GRPC_BUILD must be release or asan (got $BENCH_GRPC_BUILD)" >&2; exit 1 ;;
esac

need_env() { : "${DPU_HOST:?.env missing DPU_HOST}" "${HOST_PASS:?.env missing HOST_PASS}" \
                "${DPU_PASS:?.env missing DPU_PASS}" "${HOST_PCI:?.env missing HOST_PCI}" \
                "${DPU_PCI:?.env missing DPU_PCI}"; }

cpu_in_list() {
    local cpu="$1" list="$2" part lo hi
    local parts=()
    IFS=',' read -r -a parts <<<"$list"
    for part in "${parts[@]}"; do
        if [[ "$part" == *-* ]]; then
            lo="${part%-*}"; hi="${part#*-}"
        else
            lo="$part"; hi="$part"
        fi
        [ "$cpu" -ge "$lo" ] && [ "$cpu" -le "$hi" ] && return 0
    done
    return 1
}

# Bind benchmark processes and their first-touch allocations to the NUMA node
# local to the host-side BlueField PCI function. Exact per-pod taskset pinning
# happens after startup, but the image entrypoint applies BENCH_NUMA_NODE before
# dpumesh_init() allocates and registers DMA memory.
configure_host_numa() {
    [ "$BENCH_NUMA_CONFIGURED" = 0 ] || return 0
    : "${HOST_PCI:?HOST_PCI is required for NUMA placement}"

    case "$BENCH_NUMA_POLICY" in
        auto)
            BENCH_NUMA_NODE=""
            BENCH_CORE_BASE="${BENCH_CORE_BASE:-0}"
            [[ "$BENCH_CORE_BASE" =~ ^[0-9]+$ ]] || {
                err "invalid BENCH_CORE_BASE=$BENCH_CORE_BASE"
                return 1
            }
            local online_cpus; online_cpus=$(</sys/devices/system/cpu/online)
            cpu_in_list "$BENCH_CORE_BASE" "$online_cpus" &&
                cpu_in_list "$((BENCH_CORE_BASE + 17))" "$online_cpus" || {
                err "benchmark core range ${BENCH_CORE_BASE}-$((BENCH_CORE_BASE + 17)) is not online {$online_cpus}"
                return 1
            }
            BENCH_NUMA_CONFIGURED=1
            info "Host NUMA: automatic policy, benchmark cores ${BENCH_CORE_BASE}-$((BENCH_CORE_BASE + 17))"
            return 0
            ;;
        local) ;;
        *)
            err "BENCH_NUMA_POLICY must be local|auto (got $BENCH_NUMA_POLICY)"
            return 1
            ;;
    esac

    local bdf="${HOST_PCI#0000:}"
    local pci_path="/sys/bus/pci/devices/0000:$bdf"
    [ -r "$pci_path/numa_node" ] || {
        err "PCI NUMA node unavailable: $pci_path/numa_node"
        return 1
    }
    local detected_node; detected_node=$(<"$pci_path/numa_node")
    [[ "$detected_node" =~ ^[0-9]+$ ]] || {
        err "PCI $HOST_PCI has no usable NUMA node (reported $detected_node)"
        return 1
    }
    if [ -n "$BENCH_NUMA_NODE" ] && [ "$BENCH_NUMA_NODE" != "$detected_node" ]; then
        err "BENCH_NUMA_NODE=$BENCH_NUMA_NODE conflicts with PCI $HOST_PCI node $detected_node"
        return 1
    fi
    BENCH_NUMA_NODE="$detected_node"

    local cpulist_path="/sys/devices/system/node/node${BENCH_NUMA_NODE}/cpulist"
    [ -r "$cpulist_path" ] || {
        err "NUMA CPU list unavailable: $cpulist_path"
        return 1
    }
    local node_cpus; node_cpus=$(<"$cpulist_path")
    local first_cpu="${node_cpus%%[-,]*}"
    BENCH_CORE_BASE="${BENCH_CORE_BASE:-$first_cpu}"
    [[ "$BENCH_CORE_BASE" =~ ^[0-9]+$ ]] || {
        err "invalid BENCH_CORE_BASE=$BENCH_CORE_BASE"
        return 1
    }
    cpu_in_list "$BENCH_CORE_BASE" "$node_cpus" &&
        cpu_in_list "$((BENCH_CORE_BASE + 17))" "$node_cpus" || {
        err "benchmark core range ${BENCH_CORE_BASE}-$((BENCH_CORE_BASE + 17)) is not inside NUMA node $BENCH_NUMA_NODE CPUs {$node_cpus}"
        return 1
    }
    BENCH_NUMA_CONFIGURED=1
    info "Host NUMA: PCI $HOST_PCI -> node $BENCH_NUMA_NODE, benchmark cores ${BENCH_CORE_BASE}-$((BENCH_CORE_BASE + 17))"
}

### ------------------------------------------------------------ remote shell
# A campaign spans tens of minutes and opens a new connection for every DPU round
# trip, so a link that blips takes the run with it. Keepalives turn a silent drop
# into a prompt failure and BatchMode refuses to wait at a prompt.
SSH_OPTS=(-o ServerAliveInterval=15 -o ServerAliveCountMax=4
          -o ConnectTimeout=10 -o BatchMode=yes)

# Retries connection-level failures only (255). A command that reached the DPU
# and failed there is reported as-is, because rerunning it is not always safe.
ssh_dpu() {
    local attempt status
    for attempt in 1 2 3; do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" "$@" && return 0
        status=$?
        [ "$status" -eq 255 ] || return "$status"
        [ "$attempt" -lt 3 ] || return "$status"
        info "ssh $DPU_HOST unreachable (attempt $attempt/3), retrying"
        sleep $((attempt * 2))
    done
}

dpu_sudo() {
    ssh_dpu "echo '$DPU_PASS' | sudo -S bash -c '$1'" 2>&1 | sed 's/^\[sudo\][^:]*: *//'
}

### ------------------------------------------------------------ build
sync_sources() {
    step "=== Syncing sources to DPU ($DPU_HOST:~/$DPU_PROJ) ==="
    ssh_dpu "mkdir -p ~/$DPU_DOCA ~/$DPU_INCLUDE ~/$DPU_LINKERD_INCLUDE ~/$DPU_LINKERD_SHIM"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        --exclude='build/' --exclude='builddir/' --exclude='*.o' --exclude='*.a' \
        "$DOCA_SRC/" "$DPU_HOST:~/$DPU_DOCA/"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$INCLUDE_SRC/" "$DPU_HOST:~/$DPU_INCLUDE/"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$LINKERD_INCLUDE_SRC/" "$DPU_HOST:~/$DPU_LINKERD_INCLUDE/"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$LINKERD_SHIM_SRC/" "$DPU_HOST:~/$DPU_LINKERD_SHIM/"
    ssh_dpu "find ~/$DPU_DOCA ~/$DPU_INCLUDE ~/$DPU_LINKERD_INCLUDE ~/$DPU_LINKERD_SHIM -type f -exec touch {} +" 2>/dev/null || true
    info "Source sync complete"
}

build_dpu() {
    step "=== Building on DPU (ninja) ==="
    # Before meson: linking against an artifact that is not there fails deep in
    # the link, where the message names a symbol rather than a missing build.
    [ "${L7_BACKEND:-null}" = linkerd ] && preflight_linkerd
    local bt="${DPU_BUILDTYPE:-debugoptimized}"
    local out
    if ! out=$(ssh_dpu "[ -d ~/$DPU_BUILD ] || (cd ~/$DPU_DOCA && meson setup build --buildtype=$bt)" 2>&1); then
        err "DPU build setup failed:"
        printf '%s\n' "$out"
        exit 1
    fi
    if [ -n "$out" ]; then printf '%s\n' "$out" | grep -vE '^\s*$' || true; fi
    ssh_dpu "rm -f ~/$DPU_BUILD/dpa_kernel.a" 2>/dev/null || true
    # L7_BACKEND=linkerd links the Rust staticlib carrying linkerd2-proxy in
    # place of the reference consumer. L7_LIB_PATH names it on the DPU.
    local l7_opts="-Dl7_backend=${L7_BACKEND:-null}"
    if [ "${L7_BACKEND:-null}" = linkerd ]; then
        l7_opts="$l7_opts -Dl7_lib_path=$(linkerd_staticlib)"
    fi
    if ! out=$(ssh_dpu "cd ~/$DPU_BUILD && meson configure -Dbuildtype=$bt $l7_opts && ninja" 2>&1); then
        err "DPU build failed:"
        printf '%s\n' "$out"
        exit 1
    fi
    local nobj; nobj=$(echo "$out" | grep -cE "Compiling C object" || true)
    info "DPU build OK (buildtype=$bt, recompiled $nobj C objects)"
}

build_host() {
    step "=== Building host libdpumesh.so (make lib) ==="
    if ! make -C "$PROJ_ROOT" lib 2>&1 | tail -20; then err "Host lib build failed"; exit 1; fi
    [ -e "$LIB_OUT/libdpumesh.so" ] || { err "libdpumesh.so was not produced"; exit 1; }
    info "Host build OK ($LIB_OUT/libdpumesh.so)"
}

collect_doca_libs() {
    rm -rf "$PROJ_ROOT/doca-libs"; mkdir -p "$PROJ_ROOT/doca-libs"
    for lib in \
        "$DOCA_LIB_DIR"/libdoca_common.so* "$DOCA_LIB_DIR"/libdoca_comch.so* \
        "$DOCA_LIB_DIR"/libdoca_dpa.so* "$FLEXIO_LIB_DIR"/libflexio.so* \
        /lib/x86_64-linux-gnu/libmlx5.so* /lib/x86_64-linux-gnu/libibverbs.so*; do
        [ -e "$lib" ] && cp -a "$lib" "$PROJ_ROOT/doca-libs/"
    done
}

build_bench_binaries() {
    step "=== Building bench/validator binaries (make bench) ==="
    if ! make -C "$PROJ_ROOT" bench 2>&1 | tail -30; then err "Bench build failed"; exit 1; fi
    info "Bench binaries built ($BIN_OUT + $LIB_OUT/libdmesh_preload.so)"
}

### ------------------------------------------------------------ container images
build_image() {  # $1 = Dockerfile, $2 = tag, $3 = build context, $4.. = --build-arg
    local df="$1" tag="$2" ctx="$3"; shift 3
    docker build -f "$df" -t "$tag" "$@" "$ctx"
    sudo ctr -n k8s.io images rm "docker.io/$tag" 2>/dev/null || true
    docker save "$tag" | sudo ctr -n k8s.io images import -
    docker image prune -f >/dev/null 2>&1 || true
}

# The gRPC apps live in their own CMake tree and link gRPC statically.
grpc_cmake_build() {  # $1 = build dir, $2 = sanitizers ON|OFF, $3 = targets...
    local out="$PROJ_ROOT/$1" sanitize="$2"; shift 2
    local grpc_src="${DPUMESH_GRPC_SOURCE_DIR:-/home/jukebox/deps/grpc-v1.80.0}"
    local buildtype=Release
    [ "$sanitize" = ON ] && buildtype=RelWithDebInfo
    cmake -S "$PROJ_ROOT/integrations/grpc" -B "$out" -DCMAKE_BUILD_TYPE="$buildtype" \
        -DDPUMESH_GRPC_SOURCE_DIR="$grpc_src" \
        -DDPUMESH_GRPC_ENABLE_SANITIZERS="$sanitize" \
        -DDPUMESH_GRPC_BUILD_QPS_BENCHMARK=ON -DBUILD_TESTING=OFF >/dev/null ||
        { err "gRPC cmake configure failed ($out)"; exit 1; }
    cmake --build "$out" --target "$@" -j"$(nproc)" >/dev/null ||
        { err "gRPC app build failed ($out)"; exit 1; }
}

build_grpc_apps() {
    local grpc_src="${DPUMESH_GRPC_SOURCE_DIR:-/home/jukebox/deps/grpc-v1.80.0}"
    [ -d "$grpc_src" ] || { warn "gRPC source $grpc_src not found; skipping gRPC apps"; return 0; }
    step "=== Building gRPC bench apps (bench_grpc, echo_grpc; server=$BENCH_GRPC_BUILD) ==="
    grpc_cmake_build "$GRPC_BENCH_BUILD_DIR" OFF bench_grpc echo_grpc
    if [ "$BENCH_GRPC_BUILD" = asan ]; then
        grpc_cmake_build "$GRPC_ECHO_BUILD_DIR" ON echo_grpc
    fi
    info "gRPC apps built ($GRPC_BENCH_BUILD_DIR/bench_grpc, $GRPC_ECHO_BUILD_DIR/echo_grpc)"
}

build_images() {
    local scope="$BENCH_DEPLOY_SCOPE"
    step "=== Building Docker images (scope=$scope) ==="
    collect_doca_libs
    echo "$HOST_PASS" | sudo -S true 2>/dev/null
    if [ "$scope" != grpc ]; then
        build_image "$BENCH_DIR/docker/bench_dpumesh.Dockerfile" "$IMG_BENCH_DPU" "$PROJ_ROOT"
        build_image "$BENCH_DIR/docker/echo_dpumesh.Dockerfile"  "$IMG_ECHO_DPU"  "$PROJ_ROOT"
    fi
    if [ "$scope" = all ]; then
        build_image "$BENCH_DIR/validators/loopback_dpumesh.Dockerfile" "$IMG_LOOPBACK_DPU" "$PROJ_ROOT"
        build_image "$BENCH_DIR/validators/verbs_dpumesh.Dockerfile"    "$IMG_VERBS_DPU"    "$PROJ_ROOT"
        build_image "$BENCH_DIR/validators/preload_dpumesh.Dockerfile"  "$IMG_PRELOAD_DPU"  "$PROJ_ROOT"
    fi
    if [ "$scope" = all ] || [ "$scope" = l4 ]; then
        build_image "$BENCH_DIR/validators/preload_sock.Dockerfile" "$IMG_PRELOAD_SOCK" "$PROJ_ROOT"
        build_image "$BENCH_DIR/docker/bench_sock.Dockerfile"       "$IMG_BENCH_TCP"    "$PROJ_ROOT"
        build_image "$BENCH_DIR/docker/echo_sock.Dockerfile"        "$IMG_ECHO_TCP"     "$PROJ_ROOT"
    fi
    if { [ "$scope" = all ] || [ "$scope" = grpc ]; } &&
       [ -x "$PROJ_ROOT/$GRPC_BENCH_BUILD_DIR/bench_grpc" ]; then
        build_image "$GRPC_BENCH_DIR/docker/bench_grpc.Dockerfile" "$IMG_BENCH_GRPC"   "$PROJ_ROOT" \
            --build-arg "GRPC_BUILD_DIR=$GRPC_BENCH_BUILD_DIR"
        build_image "$GRPC_BENCH_DIR/docker/echo_grpc.Dockerfile"  "$IMG_ECHO_GRPC"    "$PROJ_ROOT" \
            --build-arg "GRPC_BUILD_DIR=$GRPC_ECHO_BUILD_DIR"
    elif [ "$scope" = all ] || [ "$scope" = grpc ]; then
        warn "gRPC bench binaries missing; skipping grpc images (run: $0 grpcbuild)"
    fi
    info "Scope images built and imported to containerd"
}

ensure_envoy_image() {
    if ! docker image inspect "$IMG_ENVOY_BASE" >/dev/null 2>&1; then
        info "Pulling Envoy base image: $IMG_ENVOY_BASE"
        docker pull "$IMG_ENVOY_BASE" || {
            err "docker pull $IMG_ENVOY_BASE failed"
            exit 1
        }
    fi
    info "Building NUMA-aware Envoy image"
    echo "$HOST_PASS" | sudo -S true 2>/dev/null
    build_image "$BENCH_DIR/docker/envoy_numa.Dockerfile" "$IMG_ENVOY" "$PROJ_ROOT" \
        --build-arg "ENVOY_BASE=$IMG_ENVOY_BASE"
}

### ------------------------------------------------------------ DPU process
stop_dpu() {
    info "Stopping dpumesh_dpu..."
    # Match process command lines even when the main thread has been renamed.
    ssh_dpu "echo '$DPU_PASS' | sudo -S bash -c \"pids=\\\$(pgrep -f '[d]pumesh_dpu'); [ -z \\\"\\\$pids\\\" ] || kill -9 \\\$pids\" 2>/dev/null; true" 2>&1 | sed 's/^\[sudo\][^:]*: *//' || true
    sleep 5
}

# DPU paths for the static library, mock control plane and fixtures.
dpu_home_cached=""
dpu_home() {
    [ -n "$dpu_home_cached" ] || dpu_home_cached=$(ssh_dpu 'echo $HOME')
    printf '%s' "$dpu_home_cached"
}
linkerd_build_dir() { printf '%s' "$(dpu_home)/$DPU_L7_BUILD"; }
linkerd_mock_dir()  { printf '%s' "${LINKERD_MOCK_DIR:-$(linkerd_build_dir)/mock}"; }
linkerd_staticlib() {
    printf '%s' "${L7_LIB_PATH:-$(linkerd_build_dir)/rust/target/release/libdmesh_l7.a}"
}
linkerd_data_dir() {
    printf '%s' "${LINKERD_DATA_DIR:-$(linkerd_build_dir)/port/linkerd2-proxy/linkerd/app/integration/src/data}"
}
# The DPU's workload identity material: the directory holding its key, its
# service-account token and the trust anchors it validates the control plane
# with. The defaults are the port's test fixture; a deployment overrides them
# with the material provisioned for the DPU.
linkerd_identity_dir() {
    printf '%s' "${LINKERD_IDENTITY_DIR:-$(linkerd_data_dir)/$LINKERD_FIXTURE}"
}
linkerd_trust_anchors() {
    printf '%s' "${LINKERD_TRUST_ANCHORS:-$(linkerd_data_dir)/ca1.pem}"
}
LINKERD_FIXTURE="${LINKERD_FIXTURE:-default-default}"
# Linkerd backend address for the selected DPUmesh service.
LINKERD_BACKEND_ADDR="${LINKERD_BACKEND_ADDR:-10.96.0.11:9092}"

# Which control plane the DPU proxy talks to. The mock identity, destination
# and policy servers are test fixtures: they run only for a benchmark, and
# LINKERD_MOCK_CONTROL_PLANE=0 points the proxy at a deployed control plane
# instead. Everything the proxy needs is then deploy-time configuration, and a
# missing piece is a startup failure rather than a silent fixture.
LINKERD_MOCK_CONTROL_PLANE="${LINKERD_MOCK_CONTROL_PLANE:-1}"
if [ "$LINKERD_MOCK_CONTROL_PLANE" = 1 ]; then
    LINKERD_DST_ADDR="${LINKERD_DST_ADDR:-127.0.0.1:8089}"
    LINKERD_POLICY_ADDR="${LINKERD_POLICY_ADDR:-127.0.0.1:8087}"
    LINKERD_IDENTITY_ADDR="${LINKERD_IDENTITY_ADDR:-127.0.0.1:8088}"
fi

# Synchronize the embedded adapter and port sources.
sync_linkerd_sources() {
    local dest; dest=$(linkerd_build_dir)
    step "=== Syncing linkerd sources to DPU ($DPU_HOST:$dest) ==="
    ssh_dpu "mkdir -p '$dest/rust' '$dest/port/linkerd2-proxy' '$dest/mock'"
    # Preserve remote build outputs.
    local ex=(--exclude='.git' --exclude='.git/' --exclude='target/'
              --exclude='build/' --exclude='*.o' --exclude='*.a')
    rsync -az --delete --timeout=120 -e "ssh ${SSH_OPTS[*]}" "${ex[@]}" \
        "$LINKERD_RUST_SRC/" "$DPU_HOST:$dest/rust/" ||
        { err "linkerd/rust sync failed"; exit 1; }
    rsync -az --timeout=300 -e "ssh ${SSH_OPTS[*]}" "${ex[@]}" \
        "$LINKERD_PORT_SRC/" "$DPU_HOST:$dest/port/linkerd2-proxy/" ||
        { err "linkerd port sync failed"; exit 1; }
    info "linkerd source sync complete"
}

build_linkerd_artifacts() {
    local dest; dest=$(linkerd_build_dir)
    local cargo="$LINKERD_CARGO +$LINKERD_TOOLCHAIN"
    step "=== Building linkerd artifacts on DPU (libdmesh_l7.a + mock control plane) ==="
    # Use pinned dependency graphs.
    if ! ssh_dpu "test -f '$dest/rust/Cargo.lock'"; then
        err "missing $dest/rust/Cargo.lock — a reproducible build needs it"
        err "  generate:  ssh $DPU_HOST \"cd $dest/rust && $cargo generate-lockfile\""
        err "  then copy it to $PROJ_ROOT/linkerd/rust/Cargo.lock and redeploy"
        exit 1
    fi
    local out
    if ! out=$(ssh_dpu "cd '$dest/rust' && $cargo build --release --locked" 2>&1); then
        err "libdmesh_l7.a build failed:"; printf '%s\n' "$out" | tail -40; exit 1
    fi
    info "libdmesh_l7.a built ($(linkerd_staticlib))"
    if ! out=$(ssh_dpu "cd '$dest/port/linkerd2-proxy' && $cargo build --release --locked \
                        -p linkerd-app-integration \
                        --bin mock-identity --bin mock-destination --bin mock-policy" 2>&1); then
        err "mock control plane build failed:"; printf '%s\n' "$out" | tail -40; exit 1
    fi
    if ! out=$(ssh_dpu "install -D -m 0755 -t '$(linkerd_mock_dir)' \
                        '$dest/port/linkerd2-proxy/target/release/mock-identity' \
                        '$dest/port/linkerd2-proxy/target/release/mock-destination' \
                        '$dest/port/linkerd2-proxy/target/release/mock-policy'" 2>&1); then
        err "staging the mock binaries failed:"; printf '%s\n' "$out"; exit 1
    fi
    info "mock control plane staged ($(linkerd_mock_dir))"
}

# Validate Linkerd artifacts and adapter symbols.
preflight_linkerd() {
    local lib; lib=$(linkerd_staticlib)
    local mocks; mocks=$(linkerd_mock_dir)
    local data; data=$(linkerd_identity_dir)
    local anchors; anchors=$(linkerd_trust_anchors)
    if [ "$LINKERD_MOCK_CONTROL_PLANE" != 1 ]; then
        local unset_vars=""
        for v in LINKERD_DST_ADDR LINKERD_POLICY_ADDR LINKERD_IDENTITY_ADDR; do
            [ -n "${!v:-}" ] || unset_vars="$unset_vars $v"
        done
        if [ -n "$unset_vars" ]; then
            err "LINKERD_MOCK_CONTROL_PLANE=0 needs the deployed control plane's"
            err "addresses:$unset_vars"
            exit 1
        fi
        info "control plane: deployed (dst=$LINKERD_DST_ADDR policy=$LINKERD_POLICY_ADDR" \
             "identity=$LINKERD_IDENTITY_ADDR)"
    fi
    local missing
    missing=$(ssh_dpu "for f in '$lib' '$data/token.txt' '$anchors'; do
                           [ -r \"\$f\" ] || echo \"unreadable: \$f\"
                       done
                       if [ '$LINKERD_MOCK_CONTROL_PLANE' = 1 ]; then
                           for m in mock-identity mock-destination mock-policy; do
                               [ -x '$mocks'/\$m ] || echo \"not executable: $mocks/\$m\"
                           done
                       fi")
    if [ -n "$missing" ]; then
        err "linkerd preflight failed:"
        printf '  %s\n' $missing
        err "build them with: L7_BACKEND=linkerd $0 linkerdbuild"
        exit 1
    fi

    # Check the exported adapter and external runtime backend boundary.
    local defined undefined absent="" s
    defined=$(ssh_dpu "nm -g --defined-only '$lib' 2>/dev/null | awk '\$3 ~ /^l7_/ {print \$3}' | sort -u")
    for s in l7_worker_run l7_conn_open l7_conn_segment l7_conn_eof \
             l7_conn_close l7_resolve l7_report; do
        case $'\n'"$defined"$'\n' in
            *$'\n'"$s"$'\n'*) ;;
            *) absent="$absent $s" ;;
        esac
    done
    if [ -n "$absent" ]; then
        err "libdmesh_l7.a does not export:$absent"
        exit 1
    fi
    undefined=$(ssh_dpu "nm -u '$lib' 2>/dev/null | awk '\$NF ~ /^dmesh_doca_/ {print \$NF}' | sort -u")
    if [ -n "$undefined" ]; then
        err "libdmesh_l7.a requires the port's own datapath — own-datapath leaked in:"
        printf '  %s\n' $undefined
        exit 1
    fi
    undefined=$(ssh_dpu "nm -u '$lib' 2>/dev/null | awk '\$NF ~ /^dmesh_l7_driver_/ {print \$NF}' | sort -u")
    for s in dmesh_l7_driver_notification_fds dmesh_l7_driver_arm \
             dmesh_l7_driver_drain dmesh_l7_driver_clear_notifications \
             dmesh_l7_driver_maintenance dmesh_l7_driver_stopped \
             dmesh_l7_driver_ready dmesh_l7_driver_failed; do
        case $'\n'"$undefined"$'\n' in
            *$'\n'"$s"$'\n'*) ;;
            *) err "libdmesh_l7.a does not require runtime backend symbol: $s"; exit 1 ;;
        esac
    done
    info "linkerd preflight OK (staticlib exports the contract and needs no port datapath," \
         "identity material readable, control plane=$([ "$LINKERD_MOCK_CONTROL_PLANE" = 1 ] &&
         echo mock || echo deployed))"
}

start_mocks() {
    step "=== Starting mock control plane (identity/destination/policy) ==="
    ssh_dpu "cat > /tmp/start_mocks.sh << 'MOCKS'
#!/bin/bash
sleep 1
cd /tmp
MOCK_IDENTITY_ADDR=127.0.0.1:8088 MOCK_IDENTITY_DATA_DIR=$(linkerd_data_dir) \
  MOCK_IDENTITY_FIXTURE=$LINKERD_FIXTURE \
  setsid nohup $(linkerd_mock_dir)/mock-identity > /tmp/mock-identity.log 2>&1 < /dev/null &
MOCK_DESTINATION_ADDR=127.0.0.1:8089 MOCK_DESTINATION_BACKEND=$LINKERD_BACKEND_ADDR \
  setsid nohup $(linkerd_mock_dir)/mock-destination > /tmp/mock-destination.log 2>&1 < /dev/null &
MOCK_POLICY_ADDR=127.0.0.1:8087 MOCK_POLICY_BACKEND=$LINKERD_BACKEND_ADDR \
  setsid nohup $(linkerd_mock_dir)/mock-policy > /tmp/mock-policy.log 2>&1 < /dev/null &
sleep 2
pgrep -c -f 'l7build/mock/mock-' || true
MOCKS
chmod +x /tmp/start_mocks.sh"
    # Restart the three mock processes.
    local n; n=$(ssh_dpu "echo '$DPU_PASS' | sudo -S pkill -f 'mock-(identity|destination|policy)\$' 2>/dev/null; \
                          bash /tmp/start_mocks.sh" 2>&1 | tail -1)
    if [ "$n" != 3 ]; then
        err "mock control plane did not start (got '$n' of 3); see /tmp/mock-*.log on the DPU"
        ssh_dpu "tail -3 /tmp/mock-identity.log /tmp/mock-destination.log /tmp/mock-policy.log" || true
        exit 1
    fi
    info "mock control plane up (identity :8088, destination :8089, policy :8087)"
}

start_dpu() {
    local log_level="${DPUMESH_LOG_LEVEL:-40}"
    # Services routed through each L7 mode.
    local l7_decision="${DPUMESH_L7_DECISION_SVC:-}"
    local l7_opaque="${DPUMESH_L7_OPAQUE_SVC:-}"
    local l7_full="${DPUMESH_L7_SVC:-}"
    local l7_trace="${DPUMESH_L7_NULL_TRACE:-}"
    local l7_rr="${DPUMESH_L7_FRAMED_RR:-}"
    # Linkerd environment and identity material.
    local l7_env=""
    if [ "${L7_BACKEND:-null}" = linkerd ]; then
        local id_name="${LINKERD_LOCAL_NAME:-default.default.serviceaccount.identity.linkerd.cluster.local}"
        ssh_dpu "cat > /tmp/dpumesh-l7.env << 'L7ENV'
LINKERD2_PROXY_DESTINATION_SVC_ADDR=${LINKERD_DST_ADDR:-127.0.0.1:8089}
LINKERD2_PROXY_DESTINATION_SVC_NAME=$id_name
LINKERD2_PROXY_POLICY_SVC_ADDR=${LINKERD_POLICY_ADDR:-127.0.0.1:8087}
LINKERD2_PROXY_POLICY_SVC_NAME=$id_name
LINKERD2_PROXY_POLICY_WORKLOAD=${LINKERD_WORKLOAD:-default:dpumesh}
LINKERD2_PROXY_IDENTITY_SVC_ADDR=${LINKERD_IDENTITY_ADDR:-127.0.0.1:8088}
LINKERD2_PROXY_IDENTITY_SVC_NAME=$id_name
LINKERD2_PROXY_IDENTITY_LOCAL_NAME=$id_name
LINKERD2_PROXY_IDENTITY_DIR=$(linkerd_identity_dir)
LINKERD2_PROXY_IDENTITY_TOKEN_FILE=$(linkerd_identity_dir)/token.txt
LINKERD2_PROXY_DESTINATION_PROFILE_NETWORKS=0.0.0.0/0
LINKERD2_PROXY_CLUSTER_NETWORKS=0.0.0.0/0
LINKERD2_PROXY_INBOUND_LISTEN_ADDR=127.0.0.1:0
LINKERD2_PROXY_OUTBOUND_LISTEN_ADDR=127.0.0.1:0
LINKERD2_PROXY_CONTROL_LISTEN_ADDR=127.0.0.1:0
LINKERD2_PROXY_ADMIN_LISTEN_ADDR=${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}
LINKERD2_PROXY_LOG=${LINKERD_LOG:-warn,linkerd=info,dmesh_l7=info}
DPUMESH_L7_LINKERD_WORKER=${DPUMESH_L7_LINKERD_WORKER:-0}
DPUMESH_L7_FAIL_CLOSED=${DPUMESH_L7_FAIL_CLOSED:-0}
DMESH_L7_TX_RESERVE=${DMESH_L7_TX_RESERVE:-1}
L7ENV
{ printf 'LINKERD2_PROXY_IDENTITY_TRUST_ANCHORS=\"'; cat $(linkerd_trust_anchors); printf '\"\n'; } >> /tmp/dpumesh-l7.env"
        l7_env='set -a; . /tmp/dpumesh-l7.env; set +a;'
    fi
    local dpa_threads="${DPUMESH_DPA_THREADS:-}"
    local rings="${DPUMESH_RINGS_PER_POD:-}"
    local workers="${DPUMESH_ARM_WORKERS:-}"
    step "=== Starting dpumesh_dpu (l7_decision='$l7_decision' l7_opaque='$l7_opaque' l7_full='$l7_full' dpa_threads='$dpa_threads' rings_per_pod='$rings' arm_workers='$workers') ==="
    stop_dpu
    if [ "${L7_BACKEND:-null}" = linkerd ] && [ "$LINKERD_MOCK_CONTROL_PLANE" = 1 ]; then
        start_mocks
    fi
    local dpu_home; dpu_home=$(ssh_dpu 'echo $HOME')
    # Start one DPU process.
    ssh_dpu "cat > /tmp/start_dpu_bench.sh << 'LAUNCHER'
#!/bin/bash
running=\$(pgrep -x dpumesh_dpu | head -1)
if [ -n \"\$running\" ]; then echo \"\$running\"; exit 0; fi
ulimit -c unlimited
screen -dmS dpumesh-bench bash -c \"ulimit -c unlimited; cd $dpu_home/$DPU_BUILD && $l7_env DPUMESH_L7_DECISION_SVC=$l7_decision DPUMESH_L7_OPAQUE_SVC=$l7_opaque DPUMESH_L7_SVC=$l7_full DPUMESH_L7_NULL_TRACE=$l7_trace DPUMESH_L7_FRAMED_RR=$l7_rr DPUMESH_DPA_THREADS=$dpa_threads DPUMESH_RINGS_PER_POD=$rings DPUMESH_ARM_WORKERS=$workers ./dpumesh_dpu $DPU_PCI -l $log_level > $DPU_LOG 2>&1\"
sleep 2
pgrep -x dpumesh_dpu | head -1 || echo NO_PID
LAUNCHER
chmod +x /tmp/start_dpu_bench.sh"
    local pid; pid=$(ssh_dpu "echo '$DPU_PASS' | sudo -S bash /tmp/start_dpu_bench.sh" 2>&1 | sed 's/^\[sudo\][^:]*: *//')
    if [ "$pid" = "NO_PID" ] || [ -z "$pid" ]; then err "dpumesh_dpu failed to start"; exit 1; fi
    info "dpumesh_dpu running (PID: $pid)"
}

### ------------------------------------------------------------ CPU pinning
# Pods run on one NUMA node, selected by BENCH_CORE_BASE. fair (default): 1
# host core per pod — the apples-to-apples 1-core comparison (dpumesh app gets a
# full core since transport is on the DPU; tcp app shares its core with its
# sidecar). hw/hw3/hw6: multi-core for the dpumesh side only, to chase the
# transport ceiling (not comparable to TCP).
# Shift a comma-separated core list onto the benchmark NUMA node.
core_list() {
    local out="" c
    for c in ${1//,/ }; do out="${out:+$out,}$((BENCH_CORE_BASE + c))"; done
    echo "$out"
}
# The gRPC/L7 configuration names the collectors use, mapped to their pods.
grpc_client_app() {
    case "$1" in
        grpc-envoy-permissive)  echo bench-grpc-envoy ;;
        grpc-envoy-strict)      echo bench-grpc-envoy-strict ;;
        grpc-tcp)               echo bench-grpc-tcp ;;
        grpc-dpumesh)           echo bench-grpc-dpumesh ;;
        grpc-linkerd)           echo bench-grpc-linkerd ;;
        grpc-linkerd-opaque)    echo bench-grpc-linkerd-opaque ;;
    esac
}
grpc_server_app() {
    case "$1" in
        grpc-envoy-permissive)  echo echo-grpc-envoy ;;
        grpc-envoy-strict)      echo echo-grpc-envoy-strict ;;
        grpc-tcp)               echo echo-grpc-tcp ;;
        grpc-dpumesh)           echo echo-grpc-dpumesh ;;
        grpc-linkerd)           echo echo-grpc-linkerd ;;
        grpc-linkerd-opaque)    echo echo-grpc-linkerd-opaque ;;
    esac
}
get_pod_cores() {
    local app="$1" profile="${2:-fair}" rel=""
    case "$profile" in
        hw)  case "$app" in bench-dpumesh) rel="0,4";; echo-dpumesh) rel="1,5";; bench-tcp) rel="2";; echo-tcp) rel="3";; esac ;;
        hw3) case "$app" in bench-dpumesh) rel="0,4,6";; echo-dpumesh) rel="1,5,7";; bench-tcp) rel="2";; echo-tcp) rel="3";; esac ;;
        hw6) case "$app" in bench-dpumesh) rel="0,4,6,8,10,12";; echo-dpumesh) rel="1,5,7,9,11,13";; bench-tcp) rel="2";; echo-tcp) rel="3";; esac ;;
        l4)
            # Four measured paths get two exclusive cores each. Every other
            # running benchmark pod is kept off those cores and off each other.
            case "$app" in
                preload-bench) rel="0";; preload-echo) rel="1";;
                bench-tcp) rel="2";; echo-tcp) rel="3";;
                bench-tcp-strict) rel="4";; echo-tcp-strict) rel="5";;
                bench-dpumesh) rel="6";; echo-dpumesh) rel="7";;
                echo-dpumesh-13) rel="10";; echo-dpumesh-14) rel="11";;
                loopback-dpumesh) rel="12";;
                verbs-dpumesh) rel="14";; preload-dpumesh) rel="15";;
            esac ;;
        l4cap)
            # Capacity profile: the server endpoint keeps one exclusive core and
            # the client gets three, so the load generator cannot be the limit
            # and the ceiling is the server core's.
            case "$app" in
                preload-bench) rel="0,1,2";;   preload-echo) rel="3";;
                bench-tcp) rel="4,5,6";;       echo-tcp) rel="7";;
                bench-tcp-strict) rel="8,9,10";; echo-tcp-strict) rel="11";;
                bench-dpumesh) rel="12,13,14";;  echo-dpumesh) rel="15";;
            esac ;;
        grpc)
            # Measured L7 paths, two exclusive cores each. A sidecar — Envoy or
            # linkerd-proxy — shares its application's core: the budget is per
            # pod, so a meshed path pays for its proxy out of the same core.
            case "$app" in
                bench-grpc-dpumesh) rel="0";; echo-grpc-dpumesh) rel="1";;
                bench-grpc-envoy)   rel="2";; echo-grpc-envoy)   rel="3";;
                bench-grpc-tcp)     rel="4";; echo-grpc-tcp)     rel="5";;
                bench-grpc-envoy-strict) rel="6";; echo-grpc-envoy-strict) rel="7";;
                bench-grpc-linkerd) rel="8";; echo-grpc-linkerd) rel="9";;
                bench-grpc-linkerd-opaque) rel="10";; echo-grpc-linkerd-opaque) rel="11";;
            esac ;;
        grpcl7cap)
            # Capacity profile for one L7 path at a time: the path named in
            # BENCH_CAP_CONFIG gets six client and six server cores, and every
            # other gRPC pod is confined to the six cores outside that budget,
            # so each path is measured against the same allocation.
            local cap_client cap_server
            cap_client=$(grpc_client_app "${BENCH_CAP_CONFIG:-}")
            cap_server=$(grpc_server_app "${BENCH_CAP_CONFIG:-}")
            if [ -n "$cap_client" ] && [ "$app" = "$cap_client" ]; then
                rel="0,1,2,3,4,5"
            elif [ -n "$cap_server" ] && [ "$app" = "$cap_server" ]; then
                rel="6,7,8,9,10,11"
            else
                case "$app" in
                    bench-grpc-*|echo-grpc-*) rel="12,13,14,15,16,17";;
                esac
            fi ;;
        grpccap)
            # Capacity profile: both DPUmesh endpoints get six cores, so neither
            # host side is the limit and what remains is the transport's. The
            # other L7 paths keep one core each, only to confine them.
            case "$app" in
                bench-grpc-dpumesh) rel="0,1,2,3,4,5";;
                echo-grpc-dpumesh)  rel="6,7,8,9,10,11";;
                bench-grpc-tcp)     rel="12";; echo-grpc-tcp)  rel="13";;
                bench-grpc-envoy)   rel="14";; echo-grpc-envoy) rel="15";;
                bench-grpc-envoy-strict) rel="16";; echo-grpc-envoy-strict) rel="17";;
            esac ;;
        grpcmax)
            # Channel-scaling profile: the two DPUmesh endpoints split node 1
            # between them, so the host stops bounding the sweep before the
            # transport does. The other L7 paths are not pinned here and must be
            # confined separately.
            case "$app" in
                bench-grpc-dpumesh) rel="0,1,2,3,4,5,6,7,8";;
                echo-grpc-dpumesh)  rel="9,10,11,12,13,14,15,16,17";;
            esac ;;
        fair|*)
            case "$app" in
                bench-dpumesh) rel="0";; echo-dpumesh) rel="1";;
                bench-tcp) rel="2";; echo-tcp) rel="3";;
                loopback-dpumesh) rel="4,5";; preload-dpumesh|preload-echo|preload-bench) rel="4,5";; verbs-dpumesh) rel="4,5";;
                echo-dpumesh-13) rel="6";; echo-dpumesh-14) rel="7";;
            esac ;;
    esac
    [ -z "$rel" ] && { echo ""; return; }
    core_list "$rel"
}

pin_pods() {
    local profile="${1:-fair}"
    configure_host_numa
    step "=== Pinning pods to dedicated cores (taskset, profile=$profile) ==="
    command -v jq >/dev/null 2>&1 || { err "jq not found (apt install jq)"; return 1; }
    if command -v cpupower >/dev/null 2>&1; then
        local dvfs="${BENCH_CORE_BASE}-$((BENCH_CORE_BASE + 17))"
        info "CPU governor=performance, fixed 2.5 GHz on cores $dvfs"
        echo "$HOST_PASS" | sudo -S cpupower -c "$dvfs" frequency-set -g performance >/dev/null 2>&1 || true
        echo "$HOST_PASS" | sudo -S cpupower -c "$dvfs" frequency-set -d 2.5GHz -u 2.5GHz >/dev/null 2>&1 || true
    else
        warn "cpupower not found; skipping DVFS lock"
    fi
    for app in bench-dpumesh echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 loopback-dpumesh verbs-dpumesh preload-dpumesh preload-echo preload-bench bench-tcp echo-tcp bench-tcp-strict echo-tcp-strict bench-grpc-dpumesh echo-grpc-dpumesh bench-grpc-envoy echo-grpc-envoy bench-grpc-tcp echo-grpc-tcp bench-grpc-envoy-strict echo-grpc-envoy-strict bench-grpc-linkerd echo-grpc-linkerd bench-grpc-linkerd-opaque echo-grpc-linkerd-opaque; do
        local cores pod_id desired
        cores=$(get_pod_cores "$app" "$profile"); [ -z "$cores" ] && continue
        # sed consumes the full stream. `head` can close early and make crictl
        # exit with SIGPIPE under this script's `set -o pipefail`.
        pod_id=$(echo "$HOST_PASS" | sudo -S crictl pods --label "app=$app" -q 2>/dev/null |
            sed -n '1p')
        if [ -z "$pod_id" ]; then
            desired=$(kubectl get deployment "$app" -n "$NS" \
                -o jsonpath='{.spec.replicas}' 2>/dev/null || true)
            [ "${desired:-0}" = 0 ] && continue
            warn "$app: desired=$desired but pod not found, skipping"
            continue
        fi
        info "$app → core(s) $cores (pod=$pod_id)"
        for cid in $(echo "$HOST_PASS" | sudo -S crictl ps --pod "$pod_id" -q 2>/dev/null); do
            local cname pid
            cname=$(echo "$HOST_PASS" | sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.status.metadata.name' 2>/dev/null)
            pid=$(echo "$HOST_PASS"   | sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid'              2>/dev/null)
            if [ -z "$pid" ] || [ "$pid" = "null" ]; then continue; fi
            info "  $cname (PID $pid) → $cores"
            echo "$HOST_PASS" | sudo -S taskset -apc "$cores" "$pid" >/dev/null
            for child in $(pgrep -P "$pid" 2>/dev/null); do
                echo "$HOST_PASS" | sudo -S taskset -apc "$cores" "$child" >/dev/null 2>&1 || true
            done
            # Benchmark images start under numactl, so their pages are already
            # on the benchmark node. An injected linkerd-proxy is not ours to
            # wrap, and it allocates before this pinning runs, so move what it
            # allocated onto the node it now runs on. Without this the proxy
            # reads its own memory across the interconnect and the linkerd
            # columns carry a penalty no other path pays.
            if [ "$cname" = linkerd-proxy ] && [ -n "$BENCH_NUMA_NODE" ]; then
                local other
                for other in $(seq 0 7); do
                    [ "$other" = "$BENCH_NUMA_NODE" ] && continue
                    [ -d "/sys/devices/system/node/node$other" ] || continue
                    echo "$HOST_PASS" | sudo -S migratepages "$pid" "$other" "$BENCH_NUMA_NODE" \
                        >/dev/null 2>&1 || true
                done
            fi
        done
    done
    info "Pinning done"
}

### ------------------------------------------------------------ k8s
ensure_namespace() {
    local phase; phase=$(kubectl get ns "$NS" -o jsonpath='{.status.phase}' 2>/dev/null || echo "")
    if [ "$phase" = "Terminating" ]; then
        info "Namespace $NS is Terminating — waiting up to 120s..."
        local i=0; while [ $i -lt 60 ]; do kubectl get ns "$NS" &>/dev/null || break; sleep 2; i=$((i+1)); done
        if kubectl get ns "$NS" &>/dev/null; then err "Namespace $NS still Terminating; aborting"; exit 1; fi
        phase=""
    fi
    if [ "$phase" != "Active" ]; then info "Creating namespace $NS"; kubectl create ns "$NS"; fi
}

# Issue a short-lived benchmark CA plus separate client/server certificates.
# The Secret is recreated before pods start, so STRICT always exercises mTLS
# without checking private key material into the repository.
ensure_envoy_tls_secret() {
    command -v openssl >/dev/null 2>&1 || {
        err "openssl not found; it is required for the STRICT Envoy path"
        exit 1
    }
    (
        local cert_dir
        cert_dir=$(mktemp -d /tmp/dpumesh-envoy-mtls.XXXXXX)
        trap 'rm -rf -- "$cert_dir"' EXIT

        openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
            -subj "/CN=dpumesh-bench-ca" \
            -keyout "$cert_dir/ca.key" -out "$cert_dir/ca.crt" >/dev/null 2>&1
        openssl req -newkey rsa:2048 -nodes \
            -subj "/CN=echo-tcp-strict" \
            -addext "subjectAltName=DNS:echo-tcp-strict" \
            -addext "extendedKeyUsage=serverAuth" \
            -keyout "$cert_dir/server.key" -out "$cert_dir/server.csr" >/dev/null 2>&1
        openssl x509 -req -days 2 -copy_extensions copy \
            -in "$cert_dir/server.csr" -CA "$cert_dir/ca.crt" \
            -CAkey "$cert_dir/ca.key" -CAcreateserial \
            -out "$cert_dir/server.crt" >/dev/null 2>&1
        openssl req -newkey rsa:2048 -nodes \
            -subj "/CN=bench-tcp-strict" \
            -addext "extendedKeyUsage=clientAuth" \
            -keyout "$cert_dir/client.key" -out "$cert_dir/client.csr" >/dev/null 2>&1
        openssl x509 -req -days 2 -copy_extensions copy \
            -in "$cert_dir/client.csr" -CA "$cert_dir/ca.crt" \
            -CAkey "$cert_dir/ca.key" -CAcreateserial \
            -out "$cert_dir/client.crt" >/dev/null 2>&1

        kubectl create secret generic envoy-mtls -n "$NS" \
            --from-file=ca.crt="$cert_dir/ca.crt" \
            --from-file=server.crt="$cert_dir/server.crt" \
            --from-file=server.key="$cert_dir/server.key" \
            --from-file=client.crt="$cert_dir/client.crt" \
            --from-file=client.key="$cert_dir/client.key" \
            --dry-run=client -o yaml | kubectl apply -f -
    )
    info "Envoy benchmark mTLS Secret ready"
}

clean_failed_pods() {
    local n; n=$(kubectl get pods -n "$NS" --field-selector=status.phase=Failed --no-headers 2>/dev/null | wc -l)
    if [ "$n" -gt 0 ]; then
        info "Removing $n stale Failed/Evicted pod(s) in $NS"
        kubectl delete pod -n "$NS" --field-selector=status.phase=Failed --ignore-not-found=true >/dev/null 2>&1 || true
    fi
}

# Render bench/k8s/pods.yaml with envsubst and apply it (replicas: 0).
apply_manifest() {
    configure_host_numa
    step "=== Applying K8s manifest (replicas=0) ==="
    command -v envsubst >/dev/null 2>&1 || { err "envsubst not found (apt install gettext-base)"; exit 1; }
    export IMG_BENCH_DPU IMG_ECHO_DPU IMG_LOOPBACK_DPU IMG_VERBS_DPU IMG_PRELOAD_DPU IMG_PRELOAD_SOCK IMG_BENCH_TCP IMG_ECHO_TCP IMG_ENVOY IMG_BENCH_GRPC IMG_ECHO_GRPC
    export CTRL_PORT TCP_PORT HOST_PCI LIB_OUT BENCH_NUMA_NODE
    export DPUMESH_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-2}" \
           ASYNC_THREADS="${ASYNC_THREADS:-4}" \
           BENCH_PIPELINE="${BENCH_PIPELINE:-8}" BENCH_COALESCE="${BENCH_COALESCE:-0}" \
           ECHO_THREADS="${ECHO_THREADS:-3}" \
           DMESH_PRELOAD_DEBUG="${DMESH_PRELOAD_DEBUG:-0}" \
           BENCH_REACTORS="${BENCH_REACTORS:-8}"
    # A killed container keeps no log, so a sanitizer report has to land on the
    # host to survive the process it describes. The mount is DirectoryOrCreate,
    # so kubelet creates the directory and the privileged container can write it.
    export ASAN_LOG_DIR="${ASAN_LOG_DIR:-/var/log/dpumesh-asan}"
    {
        cat "$MANIFEST"; echo ---; cat "$GRPC_MANIFEST"
        if [ "$BENCH_LINKERD" = 1 ]; then echo ---; cat "$LINKERD_MANIFEST"; fi
    } | envsubst | kubectl apply -n "$NS" -f -
    info "K8s resources applied (linkerd=$BENCH_LINKERD)"
}

scale_up_with_wait() {
    local app="$1" expected_log="$2"
    kubectl scale deployment "$app" --replicas=0 -n "$NS" 2>/dev/null || true
    # Wait for an empty label set before creating the replacement pod.
    local i=0
    while [ "$i" -lt 120 ] &&
          [ -n "$(kubectl get pods -n "$NS" -l "app=$app" -o name 2>/dev/null)" ]; do
        sleep 0.25
        i=$((i + 1))
    done
    if [ -n "$(kubectl get pods -n "$NS" -l "app=$app" -o name 2>/dev/null)" ]; then
        err "$app old pod did not terminate"; exit 1
    fi
    kubectl scale deployment "$app" --replicas=1 -n "$NS"
    if ! kubectl wait --for=condition=Ready pod -l "app=$app" -n "$NS" --timeout=120s 2>&1; then
        err "$app failed to start"; kubectl describe pod -l "app=$app" -n "$NS" | tail -15; exit 1
    fi
    info "$app pod Ready"
    if [ -n "$expected_log" ]; then
        # POD_INIT_READY confirms the DPUmesh registration and mmap/DPA setup.
        info "Waiting for DPUmesh init: $app ($expected_log)"
        local attempts=0
        while [ $attempts -lt 35 ]; do
            local line; line=$(kubectl logs -n "$NS" -l "app=$app" --tail=80 2>/dev/null || true)
            if echo "$line" | grep -q "$expected_log"; then
                info "$app DPUmesh data-ready"
                return 0
            fi
            sleep 1; attempts=$((attempts+1))
        done
        err "$app did not reach DPUmesh data-ready state"
        kubectl logs -n "$NS" -l "app=$app" --tail=80 2>/dev/null || true
        exit 1
    fi
}

# `core` starts only the three backends and one client. This is required for
# low-N controls such as N=8/K=8, whose DPA ring capacity admits only eight live
# pods. `l4` starts the five measured paths but only the base native backend, so
# dead weighted-LB backends can never enter the DPU registry. The default `all`
# scope starts the complete benchmark and validator set.
start_pods() {
    local scope="$BENCH_DEPLOY_SCOPE"
    case "$scope" in all|core|l4|grpc) ;;
        *) err "BENCH_DEPLOY_SCOPE must be all|core|l4|grpc (got $scope)"; exit 1;;
    esac
    step "=== Starting pods (innermost first, scope=$scope) ==="
    local ready="DPU pod is data-ready"
    # `grpc` starts only the four L7 paths, so no other backend can enter the
    # DPU registry while the gRPC campaign runs.
    if [ "$scope" = grpc ]; then
        scale_up_with_wait "echo-grpc-dpumesh"  "$ready"
        scale_up_with_wait "bench-grpc-dpumesh" "$ready"
        scale_up_with_wait "echo-grpc-envoy"    ""
        scale_up_with_wait "bench-grpc-envoy"   ""
        scale_up_with_wait "echo-grpc-tcp"      ""
        scale_up_with_wait "bench-grpc-tcp"     ""
        scale_up_with_wait "echo-grpc-envoy-strict"  ""
        scale_up_with_wait "bench-grpc-envoy-strict" ""
        if [ "$BENCH_LINKERD" = 1 ]; then
            scale_up_with_wait "echo-grpc-linkerd"         ""
            scale_up_with_wait "bench-grpc-linkerd"        ""
            scale_up_with_wait "echo-grpc-linkerd-opaque"  ""
            scale_up_with_wait "bench-grpc-linkerd-opaque" ""
        fi
        return 0
    fi
    scale_up_with_wait "echo-dpumesh"     "$ready"
    if [ "$scope" != l4 ]; then
        scale_up_with_wait "echo-dpumesh-13"  "$ready"
        scale_up_with_wait "echo-dpumesh-14"  "$ready"
    fi
    scale_up_with_wait "bench-dpumesh"    "$ready"
    [ "$scope" = core ] && return 0
    if [ "$scope" = all ]; then
        scale_up_with_wait "loopback-dpumesh" "$ready"
        scale_up_with_wait "preload-dpumesh"  "$ready"
    fi
    scale_up_with_wait "preload-echo"     "$ready"
    scale_up_with_wait "preload-bench"    ""
    if [ "$scope" = all ]; then
        scale_up_with_wait "verbs-dpumesh"    "$ready"
    fi
    scale_up_with_wait "echo-tcp-strict"  ""
    scale_up_with_wait "bench-tcp-strict" ""
    scale_up_with_wait "echo-tcp"  ""
    scale_up_with_wait "bench-tcp" ""
}

deploy() {
    need_env
    configure_host_numa
    ensure_namespace
    ensure_envoy_tls_secret
    clean_failed_pods
    apply_manifest
    sync_sources
    # The staticlib the DPU binary links has to exist before it is linked, and
    # the mocks before the proxy inside it asks for a certificate. Both are
    # built from this tree, so the deploy carries what it was run from.
    if [ "${L7_BACKEND:-null}" = linkerd ]; then
        sync_linkerd_sources
        build_linkerd_artifacts
    fi
    build_dpu
    build_host
    build_bench_binaries
    if [ "$BENCH_DEPLOY_SCOPE" = all ] || [ "$BENCH_DEPLOY_SCOPE" = grpc ]; then
        build_grpc_apps
    fi
    build_images
    [ "$BENCH_DEPLOY_SCOPE" = core ] || ensure_envoy_image
    start_dpu
    start_pods
    pin_pods fair
    [ "${L7_BACKEND:-null}" = linkerd ] && validate_linkerd_session
    info "=== Deploy complete ==="
    echo "  Run:  $0 latency|bandwidth|rate|all [dpumesh|tcp|both]"
    echo "        $0 loopback|verbs|preload ...   (validators)"
    echo "  Re-pin:  $0 pin [fair|l4|hw|hw3|hw6]"
}

validate_linkerd_session() {
    step "=== Validating one connection through the L7 layer ==="
    local reply
    if [ "$BENCH_DEPLOY_SCOPE" = grpc ]; then
        local ip; ip=$(running_pod_ip bench-grpc-dpumesh || true)
        [ -n "$ip" ] || { err "bench-grpc-dpumesh pod not found"; return 1; }
        reply=$(printf 'RUN 1024 8 1 5 100 1\n' |
            timeout 90s nc -N "$ip" "$CTRL_PORT" 2>/dev/null || true)
    else
        reply=$(run_point dpumesh 1024 8 1 5 100 1)
    fi
    printf '  %s\n' "$reply"
    case "$reply" in
        OK*) info "single-connection L7 validation passed" ;;
        *)   err "single-connection L7 validation failed: $reply"
             err "  see: $0 dpulog 200, and /tmp/mock-*.log on the DPU"
             return 1 ;;
    esac
}

cleanup() { info "Deleting namespace $NS"; kubectl delete ns "$NS" --ignore-not-found=true 2>/dev/null || true; stop_dpu; }

show_logs() {
    for app in bench-dpumesh echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 loopback-dpumesh verbs-dpumesh preload-dpumesh preload-echo preload-bench bench-tcp echo-tcp bench-tcp-strict echo-tcp-strict; do
        echo "=== $app ==="
        kubectl logs -n "$NS" -l "app=$app" --all-containers=true --prefix=true --tail=20 2>/dev/null || true
        echo
    done
}
show_status() {
    echo "=== pods ===";    kubectl get pods   -n "$NS" -o wide
    echo "=== services ==="; kubectl get svc    -n "$NS"
    echo "=== deploys ===";  kubectl get deploy -n "$NS"
}

### ------------------------------------------------------------ DPU ARM worker balance
# Snapshot DPU process threads as:
#   T <tid> <comm> <last_cpu> <allowed_cpus> <user+system ticks>
dpu_thread_snapshot() {
    dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -z "$pid" ] && exit 1; echo "HZ $(getconf CLK_TCK) PID $pid"; for task in /proc/$pid/task/*; do tid=${task##*/}; comm=$(<"$task/comm"); stat=$(<"$task/stat"); rest=${stat#*) }; set -- $rest; ticks=$((${12}+${13})); cpu=${37}; allowed=$(while read -r key value; do [ "$key" = "Cpus_allowed_list:" ] && { echo "$value"; break; }; done < "$task/status"); echo "T $tid $comm $cpu $allowed $ticks"; done'
}

arm_balance() {
    need_env
    local req="${1:-1024}" reply="${2:-8}" conc="${3:-32}"
    local dur="${4:-10}" threads="${5:-2}" csv="${6:-}"
    local snap0 snap1 result w0 w1 dt hz dpid
    declare -A tick0 comm0

    snap0=$(dpu_thread_snapshot) || { err "dpumesh_dpu is not running on the DPU"; return 1; }
    read -r _ hz _ dpid <<<"$(head -n1 <<<"$snap0")"
    while read -r tag tid comm cpu allowed ticks; do
        [ "$tag" = T ] || continue
        tick0["$tid"]="$ticks"
        comm0["$tid"]="$comm"
    done <<<"$snap0"

    step "=== DPU ARM balance: ${req}B/${reply}B conc=$conc threads=$threads dur=${dur}s ==="
    w0=$(date +%s.%N)
    result=$(run_point dpumesh "$req" "$reply" "$conc" "$dur" 200 "$threads")
    w1=$(date +%s.%N)
    [[ "$result" == OK* ]] || { err "load failed: $result"; return 1; }
    local fail drops reorder
    fail=$(field "$result" fail); drops=$(field "$result" drops); reorder=$(field "$result" reorder)
    [ "${fail:-0}" = 0 ] && [ "${drops:-0}" = 0 ] && [ "${reorder:-0}" = 0 ] ||
        { err "invalid load: fail=${fail:-NA} drops=${drops:-NA} reorder=${reorder:-NA}"; return 1; }

    snap1=$(dpu_thread_snapshot)
    dt=$(field "$result" durs)
    [ -n "$dt" ] || dt=$(awk -v a="$w0" -v b="$w1" 'BEGIN{print b-a}')
    echo "   $result"
    echo "   DPU pid=$dpid elapsed=${dt}s (100% = one ARM core)"
    printf "   %-15s %7s %8s %10s %11s\n" THREAD TID LAST_CPU ALLOWED CORE_PCT
    [ -z "$csv" ] || {
        mkdir -p "$(dirname "$csv")"
        echo "thread,tid,last_cpu,allowed_cpus,core_pct,tick_delta" >"$csv"
    }

    local worker_rows="" worker_count=0 worker_sum=0 worker_min="" worker_max=0
    local total_ticks=0 app_ticks=0
    while read -r tag tid comm cpu allowed ticks; do
        [ "$tag" = T ] || continue
        [ -n "${tick0[$tid]+x}" ] || continue
        local delta pct
        delta=$((ticks - tick0[$tid]))
        [ "$delta" -ge 0 ] || continue
        total_ticks=$((total_ticks + delta))
        if [ "$tid" = "$dpid" ]; then
            comm=dmesh-main
        fi
        case "$comm" in
            dmesh-main|dmesh-w*)
                pct=$(awk -v d="$delta" -v h="$hz" -v s="$dt" 'BEGIN{printf "%.1f",100*d/h/s}')
                printf "   %-15s %7s %8s %10s %10s%%\n" "$comm" "$tid" "$cpu" "$allowed" "$pct"
                [ -z "$csv" ] || echo "$comm,$tid,$cpu,$allowed,$pct,$delta" >>"$csv"
                app_ticks=$((app_ticks + delta))
                case "$comm" in
                    dmesh-w*)
                        worker_count=$((worker_count + 1))
                        worker_sum=$(awk -v a="$worker_sum" -v b="$pct" 'BEGIN{print a+b}')
                        worker_rows="${worker_rows}${pct}"$'\n'
                        [ -n "$worker_min" ] || worker_min="$pct"
                        worker_min=$(awk -v a="$worker_min" -v b="$pct" \
                            'BEGIN{if (a < b) print a; else print b}')
                        worker_max=$(awk -v a="$worker_max" -v b="$pct" \
                            'BEGIN{if (a > b) print a; else print b}')
                        ;;
                esac
                ;;
        esac
    done <<<"$snap1"

    local total_pct helper_pct
    total_pct=$(awk -v d="$total_ticks" -v h="$hz" -v s="$dt" 'BEGIN{printf "%.1f",100*d/h/s}')
    helper_pct=$(awk -v d="$((total_ticks-app_ticks))" -v h="$hz" -v s="$dt" 'BEGIN{printf "%.1f",100*d/h/s}')
    printf "   total process: %s%%  named main/workers: " "$total_pct"
    awk -v d="$app_ticks" -v h="$hz" -v s="$dt" 'BEGIN{printf "%.1f%%",100*d/h/s}'
    printf "  SDK/helper threads: %s%%\n" "$helper_pct"
    if [ "$worker_count" -ge 2 ]; then
        local avg cv
        avg=$(awk -v s="$worker_sum" -v n="$worker_count" 'BEGIN{printf "%.1f",s/n}')
        cv=$(awk -v mean="$avg" '
            NF {n++; sum+=($1-mean)*($1-mean)}
            END {
                if (mean > 0 && n > 0) printf "%.1f",100*sqrt(sum/n)/mean;
                else printf "0.0"
            }' <<<"$worker_rows")
        printf "   worker balance: n=%d avg=%s%% min=%s%% max=%s%% CV=%s%%\n" \
               "$worker_count" "$avg" "$worker_min" "$worker_max" "$cv"
    fi
    [ -z "$csv" ] || info "-> $csv"
}

### ------------------------------------------------------------ benchmark (RUN)
app_of()     { case "$1" in dpumesh) echo bench-dpumesh;; tcp) echo bench-tcp;; *) echo "";; esac; }
targets_of() { case "${1:-both}" in both|"") echo "dpumesh tcp";; *) echo "$1";; esac; }
field()      { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }
running_pod_ip() {
    kubectl get pod -n "$NS" -l "app=$1" -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' 2>/dev/null | head -n 1
}

# run_point <sol> <req> <reply> <conc> <dur> <warmup> <threads> [reconn] -> echoes the OK line
# reconn (dpumesh only): close+reconnect each conn every `reconn` completions (churn mode).
run_point() {
    local app ip to reply
    app="$(app_of "$1")"; [ -z "$app" ] && { echo "ERR bad_target($1)"; return 0; }
    ip=$(running_pod_ip "$app" || true)
    [ -z "$ip" ] && { echo "ERR no_pod($app)"; return 0; }
    to=$(( ${5%.*} + 90 ))
    reply=$(printf 'RUN %s %s %s %s %s %s %s\n' "$2" "$3" "$4" "$5" "$6" "$7" "${8:-}" | timeout "${to}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null) || reply="ERR nc"
    echo "$reply"
}

bench_latency() {
    local sol="$1"; mkdir -p "$OUT"; local csv="$OUT/latency_${sol}.csv"
    step "LATENCY ($sol): concurrency=1, dur=${LAT_DUR}s"
    echo "solution,req_size,p50_us,p95_us,p99_us,avg_us" >"$csv"
    printf "  %-9s %8s %8s %8s %8s\n" req_size p50us p95us p99us avgus
    local sz r
    for sz in $LAT_SIZES; do
        r=$(run_point "$sol" "$sz" 8 1 "$LAT_DUR" "$WARMUP" 1)
        [[ "$r" == OK* ]] || { warn "size $sz -> $r"; continue; }
        printf "  %-9s %8s %8s %8s %8s\n" "$sz" "$(field "$r" p50)" "$(field "$r" p95)" "$(field "$r" p99)" "$(field "$r" avg)"
        echo "$sol,$sz,$(field "$r" p50),$(field "$r" p95),$(field "$r" p99),$(field "$r" avg)" >>"$csv"
    done
    info "-> $csv"
}

bench_bandwidth() {
    local sol="$1"; mkdir -p "$OUT"; local csv="$OUT/bandwidth_${sol}.csv"
    step "BANDWIDTH ($sol): concurrency=$BW_CONC, dur=${BW_DUR}s"
    echo "solution,req_size,gbps,mrps,p50_us" >"$csv"
    printf "  %-10s %10s %10s %10s\n" req_size Gb/s Mrps p50us
    local sz r warm
    for sz in $BW_SIZES; do
        warm=$WARMUP; [ "$sz" -ge 262144 ] && warm=100
        r=$(run_point "$sol" "$sz" 8 "$BW_CONC" "$BW_DUR" "$warm" 1)
        [[ "$r" == OK* ]] || { warn "size $sz -> $r"; continue; }
        printf "  %-10s %10s %10s %10s\n" "$sz" "$(field "$r" gbps)" "$(field "$r" mrps)" "$(field "$r" p50)"
        echo "$sol,$sz,$(field "$r" gbps),$(field "$r" mrps),$(field "$r" p50)" >>"$csv"
    done
    info "-> $csv"
}

bench_rate() {
    local sol="$1"; mkdir -p "$OUT"; local csv="$OUT/rate_${sol}.csv"
    step "RATE ($sol): req=32, concurrency=$RATE_CONC, threads={$RATE_THREADS}, dur=${RATE_DUR}s"
    [ "$sol" = dpumesh ] && warn "server is single-consumer; only CLIENT threads scale. Pin more cores first ($0 pin hw6) for a real curve."
    echo "solution,threads,mrps,gbps,p50_us,p99_us" >"$csv"
    printf "  %-8s %12s %10s %10s %10s\n" threads Mrps Gb/s p50us p99us
    local t r
    for t in $RATE_THREADS; do
        r=$(run_point "$sol" 32 8 "$RATE_CONC" "$RATE_DUR" "$WARMUP" "$t")
        [[ "$r" == OK* ]] || { warn "threads $t -> $r"; continue; }
        printf "  %-8s %12s %10s %10s %10s\n" "$t" "$(field "$r" mrps)" "$(field "$r" gbps)" "$(field "$r" p50)" "$(field "$r" p99)"
        echo "$sol,$t,$(field "$r" mrps),$(field "$r" gbps),$(field "$r" p50),$(field "$r" p99)" >>"$csv"
    done
    info "-> $csv"
}

### ------------------------------------------------------------ validators
run_loopback() {  # self-routing: pod 12 is client + server of its own service
    local N="${1:-50000}" size="${2:-8192}" zc="${3:-0}" ip resp
    ip=$(running_pod_ip loopback-dpumesh || true)
    [ -z "$ip" ] && { err "loopback-dpumesh pod not found — run '$0 deploy'"; return 1; }
    step "=== loopback (self-service): N=$N size=${size}B zerocopy=$zc ==="
    resp=$(printf 'RUN %s %s %s\n' "$N" "$size" "$zc" | timeout 120s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "loopback replied: $resp"; return 1; }
    read -r _ ok fail served p50 <<<"$resp"
    printf "  OK/Fail: %s/%s  served: %s  p50: %s us\n" "$ok" "$fail" "$served" "$p50"
}

run_verbs() {  # verbs-façade self-routing: pod 17 is client + server of its own service
    local N="${1:-50000}" size="${2:-8192}" zc="${3:-0}" window="${4:-1}" pipe="${5:-1}" ip resp
    ip=$(running_pod_ip verbs-dpumesh || true)
    [ -z "$ip" ] && { err "verbs-dpumesh pod not running — run '$0 deploy' (validators no longer self-start)"; return 1; }
    step "=== verbs (self-service): N=$N size=${size}B zc=$zc window=$window pipeline=$pipe ==="
    resp=$(printf 'RUN %s %s %s %s %s\n' "$N" "$size" "$zc" "$window" "$pipe" | timeout 180s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "verbs replied: $resp"; return 1; }
    read -r _ ok fail served p50 <<<"$resp"
    printf "  OK/Fail: %s/%s  served: %s  p50: %s us\n" "$ok" "$fail" "$served" "$p50"
}

run_preload() {  # LD_PRELOAD shim: vanilla TCP apps over DPUmesh
    local N="${1:-5000}" size="${2:-1024}" conns="${3:-8}" ip resp
    ip=$(running_pod_ip preload-dpumesh || true)
    [ -z "$ip" ] && { err "preload-dpumesh pod not found — run '$0 deploy'"; return 1; }
    step "=== preload (LD_PRELOAD shim): N=$N size=${size}B conns=$conns ==="
    resp=$(printf 'RUN %s %s %s\n' "$N" "$size" "$conns" | timeout 620s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "preload replied: $resp"; return 1; }
    read -r _ ok fail p50 p99 rps <<<"$resp"
    printf "  OK/Fail: %s/%s  p50: %s us  p99: %s us  rps: %s\n" "$ok" "$fail" "$p50" "$p99" "${rps:-n/a}"
}

# Value of one unlabelled Prometheus sample in a metrics snapshot.
metric_value() {
    awk -v name="$2" '$1 == name { print $2; exit }' <<<"$1"
}

# Real-DPU lifecycle gate: kill a process while one HTTP/2 channel is active,
# require the DPU-owned Linkerd task and imported mappings to quiesce, then
# re-register the recycled pod slot and pass a four-channel smoke point.
run_grpc_shutdown() {
    need_env
    local app=bench-grpc-dpumesh ip pod load_pid="" load_out
    local metrics active opened closed pending tasks mmap_before mmap_after
    load_out=$(mktemp /tmp/dpumesh-grpc-shutdown.XXXXXX)

    restore_grpc_client() {
        [ -z "${load_pid:-}" ] || kill "$load_pid" 2>/dev/null || true
        kubectl scale deployment/"${app:-bench-grpc-dpumesh}" -n "$NS" --replicas=1 >/dev/null 2>&1 || true
        [ -z "${load_out:-}" ] || rm -f "$load_out"
    }
    trap restore_grpc_client EXIT

    ip=$(running_pod_ip "$app" || true)
    [ -n "$ip" ] || { err "$app pod not running — deploy the grpc scope first"; return 1; }
    metrics=$(ssh_dpu "curl -sf http://127.0.0.1:4191/metrics" || true)
    [ -n "$metrics" ] || { err "DPU Linkerd metrics endpoint is unavailable"; return 1; }
    active=$(metric_value "$metrics" dmesh_sessions_active)
    pending=$(metric_value "$metrics" dmesh_registrations_pending)
    tasks=$(metric_value "$metrics" dmesh_tasks_live)
    [ "${active:-x}" = 0 ] && [ "${pending:-x}" = 0 ] && [ "${tasks:-x}" = 0 ] || {
        err "precondition is not quiescent: active=${active:-NA} pending=${pending:-NA} tasks=${tasks:-NA}"
        return 1
    }
    mmap_before=$(ssh_dpu "grep -c 'RX mmap reclaim failed' '$DPU_LOG' || true")

    step "=== gRPC process shutdown with one live HTTP/2 channel ==="
    (printf 'RUN 1024 8 4 60 100 1\n' |
        timeout 90s nc -N "$ip" "$CTRL_PORT" >"$load_out" 2>&1) &
    load_pid=$!
    for _ in $(seq 1 30); do
        metrics=$(ssh_dpu "curl -sf http://127.0.0.1:4191/metrics" || true)
        active=$(metric_value "$metrics" dmesh_sessions_active)
        [ "${active:-0}" -gt 0 ] 2>/dev/null && break
        sleep 1
    done
    [ "${active:-0}" -gt 0 ] 2>/dev/null || {
        err "the long-lived channel did not become active"
        return 1
    }

    kubectl scale deployment/"$app" -n "$NS" --replicas=0 >/dev/null
    kubectl wait --for=delete pod -n "$NS" -l "app=$app" --timeout=90s >/dev/null
    wait "$load_pid" 2>/dev/null || true
    load_pid=""

    for _ in $(seq 1 30); do
        metrics=$(ssh_dpu "curl -sf http://127.0.0.1:4191/metrics" || true)
        active=$(metric_value "$metrics" dmesh_sessions_active)
        pending=$(metric_value "$metrics" dmesh_registrations_pending)
        tasks=$(metric_value "$metrics" dmesh_tasks_live)
        opened=$(metric_value "$metrics" dmesh_sessions_opened_total)
        closed=$(metric_value "$metrics" dmesh_sessions_closed_total)
        [ "${active:-x}" = 0 ] && [ "${pending:-x}" = 0 ] &&
            [ "${tasks:-x}" = 0 ] && [ -n "$opened" ] &&
            [ "$opened" = "$closed" ] && break
        sleep 1
    done
    [ "${active:-x}" = 0 ] && [ "${pending:-x}" = 0 ] &&
        [ "${tasks:-x}" = 0 ] && [ -n "$opened" ] && [ "$opened" = "$closed" ] || {
        err "shutdown leaked state: opened=${opened:-NA} closed=${closed:-NA} active=${active:-NA} pending=${pending:-NA} tasks=${tasks:-NA}"
        return 1
    }
    mmap_after=$(ssh_dpu "grep -c 'RX mmap reclaim failed' '$DPU_LOG' || true")
    [ "$mmap_after" = "$mmap_before" ] || {
        err "shutdown added RX mmap reclaim errors: $mmap_before -> $mmap_after"
        return 1
    }

    step "=== Re-registering the recycled pod slot ==="
    kubectl scale deployment/"$app" -n "$NS" --replicas=1 >/dev/null
    kubectl wait --for=condition=Ready pod -n "$NS" -l "app=$app" --timeout=120s >/dev/null
    pod=$(kubectl get pod -n "$NS" -l "app=$app" -o jsonpath='{.items[0].metadata.name}')
    local ready=0 logs reply
    for _ in $(seq 1 60); do
        logs=$(kubectl logs -n "$NS" "$pod" 2>&1 || true)
        if rg -q 'DPUmesh DOCA initialized' <<<"$logs"; then ready=1; break; fi
        sleep 1
    done
    [ "$ready" = 1 ] || { err "$app did not become DPUmesh data-ready after reuse"; return 1; }
    ip=$(running_pod_ip "$app")
    reply=$(printf 'RUN 1024 8 4 10 1000 4\n' |
        timeout 120s nc -N "$ip" "$CTRL_PORT" 2>/dev/null || true)
    # The first point after process creation also warms Linkerd's outbound
    # stack. If that cold start produces >1.05 s latency samples (the fixed
    # histogram's overflow bucket), require a clean repeat before accepting
    # the lifecycle gate.
    if [[ "$reply" == OK* ]] && [ "$(field "$reply" overflow)" != 0 ]; then
        reply=$(printf 'RUN 1024 8 4 10 1000 4\n' |
            timeout 120s nc -N "$ip" "$CTRL_PORT" 2>/dev/null || true)
    fi
    [[ "$reply" == OK* ]] &&
        [ "$(field "$reply" fail)" = 0 ] &&
        [ "$(field "$reply" pending)" = 0 ] &&
        [ "$(field "$reply" drops)" = 0 ] &&
        [ "$(field "$reply" overflow)" = 0 ] &&
        [ "$(field "$reply" reorder)" = 0 ] &&
        [ "$(field "$reply" worker_fail)" = 0 ] &&
        [ "$(field "$reply" credit_hold_dropped)" = 0 ] &&
        [ "$(field "$reply" eq_budget_exhausted)" = 0 ] || {
        err "post-reuse gRPC smoke failed: $reply"
        return 1
    }

    metrics=$(ssh_dpu "curl -sf http://127.0.0.1:4191/metrics" || true)
    active=$(metric_value "$metrics" dmesh_sessions_active)
    pending=$(metric_value "$metrics" dmesh_registrations_pending)
    tasks=$(metric_value "$metrics" dmesh_tasks_live)
    opened=$(metric_value "$metrics" dmesh_sessions_opened_total)
    closed=$(metric_value "$metrics" dmesh_sessions_closed_total)
    [ "$active" = 0 ] && [ "$pending" = 0 ] && [ "$tasks" = 0 ] &&
        [ "$opened" = "$closed" ] || {
        err "post-reuse sessions did not quiesce: opened=$opened closed=$closed active=$active pending=$pending tasks=$tasks"
        return 1
    }

    trap - EXIT
    rm -f "$load_out"
    info "gRPC shutdown/re-register gate passed: opened=$opened closed=$closed; $reply"
}

### ------------------------------------------------------------ dispatch
CMD="${1:-help}"; shift || true
case "$CMD" in
    deploy)    deploy ;;
    build)     need_env; sync_sources; build_dpu ;;
    restart)   need_env; start_dpu ;;
    grpcbuild) build_grpc_apps ;;
    linkerdbuild) need_env; sync_linkerd_sources; build_linkerd_artifacts; preflight_linkerd ;;
    # NOTE: `restart` is valid only while no pod is meshed, and there is no
    # per-pod start. Restarting the DPU under live pods — or starting a pod against
    # an already-running DPU — leaves the two sides' registration state inconsistent.
    # `deploy` is the path for anything with pods: it brings up the DPU and every
    # pod together.
    latency)   for s in $(targets_of "${1:-both}"); do bench_latency   "$s"; done ;;
    bandwidth) for s in $(targets_of "${1:-both}"); do bench_bandwidth "$s"; done ;;
    rate)      for s in $(targets_of "${1:-both}"); do bench_rate      "$s"; done ;;
    all)       for s in $(targets_of "${1:-both}"); do bench_latency "$s"; bench_bandwidth "$s"; bench_rate "$s"; done; info "results under $OUT" ;;
    point)     [ $# -eq 7 ] || [ $# -eq 8 ] || { err "point <sol> <req> <reply> <conc> <dur> <warmup> <threads> [reconn]"; exit 1; }; run_point "$@" ;;
    loopback)  run_loopback "${1:-50000}" "${2:-8192}" "${3:-0}" ;;
    verbs)     run_verbs    "${1:-50000}" "${2:-8192}" "${3:-0}" "${4:-1}" "${5:-1}" ;;
    preload)   run_preload  "${1:-5000}"  "${2:-1024}" "${3:-8}" ;;
    grpcshutdown) run_grpc_shutdown ;;
    pin)       need_env; pin_pods "${1:-fair}" ;;
    status)    show_status ;;
    logs)      show_logs ;;
    cleanup)   cleanup ;;
    dpulog)    ssh_dpu "echo '$DPU_PASS' | sudo -S tail -${1:-40} $DPU_LOG" 2>&1 | sed 's/^\[sudo\][^:]*: *//' ;;
    dpucpu)    dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -z "$pid" ] && { echo "dpumesh_dpu not running"; exit 0; }; echo "=== dpumesh_dpu pid=$pid per-thread %CPU ==="; top -bH -d 1 -n 2 -p "$pid" | awk "/ PID +USER/{n++} n==2{print}"' ;;
    armbalance) arm_balance "$@" ;;
    *)
        cat <<EOF
Usage: $0 <command> [args]

  deploy                                     build + DPU + images + pods + pin (the ONLY bring-up path)
  build | restart                            rebuild the DPU binary | restart the DPU alone (no pod may be meshed)
  linkerdbuild                               sync + build libdmesh_l7.a and the mock control plane on the DPU
                                             (deploy does this itself when L7_BACKEND=linkerd)
  latency|bandwidth|rate|all [dpumesh|tcp|both]   benchmark families -> CSVs under $OUT
  point <sol> <req> <reply> <conc> <dur> <warmup> <threads> [reconn]   one raw RUN (reconn = conn-churn period)
  loopback|preload [args]                    feature validators
  grpcshutdown                              real-DPU HTTP/2 process-stop + slot-reuse gate
  verbs <N> <size> <zc> <window> <pipeline>  native-API loopback validator: window conns x pipeline outstanding
  pin [fair|l4|grpc|grpccap|grpcl7cap|grpcmax|hw|hw3|hw6]  (re)pin pods to cores
                                             grpcl7cap reads BENCH_CAP_CONFIG for the 6+6 path
  armbalance [req reply conc dur threads [csv]]   DPU main/worker per-core CPU during one point
  status | logs | cleanup | dpulog [n] | dpucpu

Deploy knobs (env): BENCH_NUMA_POLICY=local|auto BENCH_DEPLOY_SCOPE=all|core|l4|grpc
                    BENCH_LINKERD=1 adds the injected linkerd L7 pods (grpc scope)
                    L7_BACKEND=null|linkerd selects the L7 consumer; linkerd builds
                    libdmesh_l7.a and the mocks on the DPU and starts the mock CP
                    BENCH_GRPC_BUILD=release|asan (asan instruments echo_grpc only;
                    reports land in ASAN_LOG_DIR, default /var/log/dpumesh-asan)
Sweep knobs (env): OUT LAT_DUR BW_DUR RATE_DUR WARMUP BW_CONC RATE_CONC RATE_THREADS LAT_SIZES BW_SIZES
EOF
        ;;
esac
