#!/bin/bash
# Benchmark build, deployment, measurement, validation, pinning, and diagnostics.
# Use `deploy` to start the DPU and pods as one registration lifecycle. Deployment
# and pinning read configuration from the environment and, when present, the
# repository-root `.env`; live runs require kubectl and nc.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
err()   { echo -e "${RED}[ERR]${NC} $*" >&2; }
step()  { echo -e "${BLUE}[STEP]${NC} $*" >&2; }

### ------------------------------------------------------------ config
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"     # .../bench
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"                      # repo root
if [[ -v DPUMESH_ENV_FILE ]]; then
    [ -n "$DPUMESH_ENV_FILE" ] || {
        err "DPUMESH_ENV_FILE must not be empty"
        exit 1
    }
    BENCH_ENV_FILE="$DPUMESH_ENV_FILE"
    BENCH_ENV_EXPLICIT=1
else
    BENCH_ENV_FILE="$PROJ_ROOT/.env"
    BENCH_ENV_EXPLICIT=0
fi
if [ -e "$BENCH_ENV_FILE" ] || [ -L "$BENCH_ENV_FILE" ]; then
    [ -f "$BENCH_ENV_FILE" ] && [ -r "$BENCH_ENV_FILE" ] || {
        err "configuration file is not a readable regular file: $BENCH_ENV_FILE"
        exit 1
    }
    set -a; source "$BENCH_ENV_FILE"; set +a
elif [ "$BENCH_ENV_EXPLICIT" -eq 1 ]; then
    # The default file is optional because pure commands and workload drivers
    # need no rig credentials. An explicit path is an operator assertion and a
    # typo in it must not silently fall back to an unconfigured environment.
    err "DPUMESH_ENV_FILE does not exist: $BENCH_ENV_FILE"
    exit 1
fi

NS="${NS:-test-bench}"                 # k8s namespace
CTRL_PORT="${CTRL_PORT:-9092}"
MANIFEST="$BENCH_DIR/k8s/pods.yaml"
GRPC_MANIFEST="$BENCH_DIR/k8s/grpc-pods.yaml"
GRPC_LINKERD_MANIFEST="$BENCH_DIR/k8s/grpc-linkerd-pods.yaml"

# Every registration is attested. The deploy provisions a Host-only keyring and
# a namespace-scoped node agent; no key or authoritative claims are mounted into
# application Pods.
DPUMESH_REGISTRATION_KEY_DIR_DPU="${DPUMESH_REGISTRATION_KEY_DIR_DPU:-/etc/dpumesh/registration.keys}"
DPUMESH_FEED_KEY_DIR_DPU="${DPUMESH_FEED_KEY_DIR_DPU:-/etc/dpumesh/feed.keys}"
DPUMESH_ATTEST_SOCKET="${DPUMESH_ATTEST_SOCKET:-/run/dpumesh/attest.sock}"

INCLUDE_SRC="$PROJ_ROOT/include"
DOCA_SRC="$PROJ_ROOT/doca"
# The DPU binary also compiles the L7 adapter contract and its consumer.
LINKERD_INCLUDE_SRC="$PROJ_ROOT/linkerd/include"
# The embedded Linkerd sources. `rust` and `port` are siblings on the DPU
# because linkerd/rust/Cargo.toml resolves the port by relative path.
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
IMG_WORKLOAD_AGENT="bench/dpumesh-workload-agent:latest"
IMG_CONTROLLER="bench/dpumesh-controller:latest"

# benchmark sweep knobs
OUT="${OUT:-/tmp/dpumesh-bench}"
LAT_DUR="${LAT_DUR:-10}"; BW_DUR="${BW_DUR:-10}"; RATE_DUR="${RATE_DUR:-10}"
WARMUP="${WARMUP:-1000}"; BW_CONC="${BW_CONC:-32}"; RATE_CONC="${RATE_CONC:-32}"
RATE_THREADS="${RATE_THREADS:-1 2 4 8}"
LAT_SIZES="${LAT_SIZES:-64 128 256 512 1024}"
BW_SIZES="${BW_SIZES:-32 128 512 2048 8192 32768 131072 524288 1000000 2097152 8000000}"
BENCH_NUMA_NODE="${BENCH_NUMA_NODE:-}"
# Induced backend failure for the retry and failure-accrual arms; 0 = never.
BENCH_FAIL_EVERY="${BENCH_FAIL_EVERY:-0}"
export BENCH_FAIL_EVERY
BENCH_CORE_BASE="${BENCH_CORE_BASE:-}"
BENCH_NUMA_POLICY="${BENCH_NUMA_POLICY:-local}"
BENCH_DEPLOY_SCOPE="${BENCH_DEPLOY_SCOPE:-all}"
BENCH_DST_SERVICES="${BENCH_DST_SERVICES:-}"
ECHO_13_SERVICE="${ECHO_13_SERVICE:-echo-dpumesh}"
ECHO_14_SERVICE="${ECHO_14_SERVICE:-echo-dpumesh}"
# Hot-service deployments should not independently spell N/K/A and the Linkerd
# worker selection. DPUMESH_THROUGHPUT_WORKERS is the canonical one-knob
# geometry: A=K=W, every worker owns one landing stripe/runtime, and N is the
# largest multiple of W no greater than the 32-EU device cap. The independent
# variables remain available when node density (K>A) is deliberately studied;
# they are constraints, not aliases, in that deployment class.
DPUMESH_THROUGHPUT_WORKERS="${DPUMESH_THROUGHPUT_WORKERS:-}"
if [ -n "$DPUMESH_THROUGHPUT_WORKERS" ]; then
    case "$DPUMESH_THROUGHPUT_WORKERS" in
        4|6|8|12) ;;
        *) err "DPUMESH_THROUGHPUT_WORKERS must be a measured preset: 4, 6, 8 or 12"
           exit 2 ;;
    esac
    DPUMESH_ARM_WORKERS="$DPUMESH_THROUGHPUT_WORKERS"
    DPUMESH_RINGS_PER_POD="$DPUMESH_THROUGHPUT_WORKERS"
    DPUMESH_DPA_THREADS=$((32 / DPUMESH_THROUGHPUT_WORKERS * DPUMESH_THROUGHPUT_WORKERS))
    DPUMESH_L7_LINKERD_WORKER=all
else
    DPUMESH_DPA_THREADS="${DPUMESH_DPA_THREADS:-32}"
    DPUMESH_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-8}"
    DPUMESH_ARM_WORKERS="${DPUMESH_ARM_WORKERS:-8}"
    DPUMESH_L7_LINKERD_WORKER="${DPUMESH_L7_LINKERD_WORKER:-all}"
fi
export DPUMESH_DPA_THREADS DPUMESH_RINGS_PER_POD DPUMESH_ARM_WORKERS
export DPUMESH_L7_LINKERD_WORKER DPUMESH_THROUGHPUT_WORKERS
# Every deployed data Service is assigned to the one supported embedded-Linkerd
# topology. Native/preload protocols are opaque streams; gRPC is HTTP/2. These
# are architecture, not benchmark knobs: an environment override must not
# silently turn one API into an unmeshed L4 deployment.
DPUMESH_L7_OPAQUE_SVC="$NS/echo-dpumesh,$NS/loopback-dpumesh,$NS/verbs-dpumesh,$NS/preload-dpumesh,$NS/preload-sock"
DPUMESH_L7_SVC="$NS/echo-grpc-dpumesh,$NS/echo-grpc-alt,$NS/http1-dpumesh"
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
    ssh_dpu "mkdir -p ~/$DPU_DOCA ~/$DPU_INCLUDE ~/$DPU_LINKERD_INCLUDE"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        --exclude='build/' --exclude='builddir/' --exclude='*.o' --exclude='*.a' \
        "$DOCA_SRC/" "$DPU_HOST:~/$DPU_DOCA/"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$INCLUDE_SRC/" "$DPU_HOST:~/$DPU_INCLUDE/"
    rsync -avz --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$LINKERD_INCLUDE_SRC/" "$DPU_HOST:~/$DPU_LINKERD_INCLUDE/"
    ssh_dpu "find ~/$DPU_DOCA ~/$DPU_INCLUDE ~/$DPU_LINKERD_INCLUDE -type f -exec touch {} +" 2>/dev/null || true
    info "Source sync complete"
}

build_dpu() {
    step "=== Building on DPU (ninja) ==="
    # Before meson: linking against an artifact that is not there fails deep in
    # the link, where the message names a symbol rather than a missing build.
    preflight_linkerd
    local bt="${DPU_BUILDTYPE:-debugoptimized}"
    local out
    if ! out=$(ssh_dpu "[ -d ~/$DPU_BUILD ] || (cd ~/$DPU_DOCA && meson setup build --buildtype=$bt)" 2>&1); then
        err "DPU build setup failed:"
        printf '%s\n' "$out"
        exit 1
    fi
    if [ -n "$out" ]; then printf '%s\n' "$out" | grep -vE '^\s*$' || true; fi
    ssh_dpu "rm -f ~/$DPU_BUILD/dpa_kernel.a" 2>/dev/null || true
    local l7_opts="-Dl7_lib_path=$(linkerd_staticlib)"
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
    # The old image is removed so the import replaces it, which leaves the tag
    # absent until the import lands. Treat a failed import as fatal: the pods
    # run with imagePullPolicy=Never, so continuing would deploy a scope whose
    # images no longer exist and report success while every pod fails to start.
    echo "$HOST_PASS" | sudo -S true 2>/dev/null
    sudo ctr -n k8s.io images rm "docker.io/$tag" 2>/dev/null || true
    if ! docker save "$tag" | sudo ctr -n k8s.io images import -; then
        err "containerd import failed for $tag — the tag is now absent"
        exit 1
    fi
    docker image prune -f >/dev/null 2>&1 || true
}

# The embedded Linkerd build on the DPU can take longer than kubelet's image-GC
# interval. Workload images are deliberately imagePullPolicy=Never, so confirm
# the exact locally built tag immediately before its first Pod is created and
# restore it from Docker's content store if GC removed it in the meantime.
ensure_image_imported() { # $1 = Docker tag without docker.io/ prefix
    local tag="$1" full="docker.io/$1"
    echo "$HOST_PASS" | sudo -S true 2>/dev/null
    if sudo ctr -n k8s.io images list -q 2>/dev/null | rg -Fxq "$full"; then
        return 0
    fi
    warn "$full was reclaimed before Pod startup; restoring the built image"
    docker image inspect "$tag" >/dev/null 2>&1 || {
        err "locally built image is unavailable: $tag"
        exit 1
    }
    if ! docker save "$tag" | sudo ctr -n k8s.io images import -; then
        err "containerd restore failed for $tag"
        exit 1
    fi
    sudo ctr -n k8s.io images list -q 2>/dev/null | rg -Fxq "$full" || {
        err "containerd did not retain restored image $full"
        exit 1
    }
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
    grpc_cmake_build "$GRPC_BENCH_BUILD_DIR" OFF bench_grpc echo_grpc \
        hello_grpc_client hello_grpc_server
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
    build_image "$BENCH_DIR/docker/workload_attest.Dockerfile" \
        "$IMG_WORKLOAD_AGENT" "$PROJ_ROOT"
    build_image "$BENCH_DIR/docker/dpumesh_controller.Dockerfile" \
        "$IMG_CONTROLLER" "$PROJ_ROOT"
    if [ "$scope" = all ] || [ "$scope" = native ]; then
        build_image "$BENCH_DIR/docker/bench_dpumesh.Dockerfile" "$IMG_BENCH_DPU" "$PROJ_ROOT"
        build_image "$BENCH_DIR/docker/echo_dpumesh.Dockerfile"  "$IMG_ECHO_DPU"  "$PROJ_ROOT"
        build_image "$BENCH_DIR/validators/loopback_dpumesh.Dockerfile" "$IMG_LOOPBACK_DPU" "$PROJ_ROOT"
        build_image "$BENCH_DIR/validators/verbs_dpumesh.Dockerfile"    "$IMG_VERBS_DPU"    "$PROJ_ROOT"
    fi
    if [ "$scope" = all ] || [ "$scope" = preload ]; then
        build_image "$BENCH_DIR/validators/preload_dpumesh.Dockerfile"  "$IMG_PRELOAD_DPU"  "$PROJ_ROOT"
        build_image "$BENCH_DIR/validators/preload_sock.Dockerfile" "$IMG_PRELOAD_SOCK" "$PROJ_ROOT"
    fi
    if { [ "$scope" = all ] || [ "$scope" = grpc ]; } &&
       [ -x "$PROJ_ROOT/$GRPC_BENCH_BUILD_DIR/bench_grpc" ]; then
        build_image "$BENCH_DIR/docker/bench_grpc.Dockerfile" "$IMG_BENCH_GRPC"   "$PROJ_ROOT" \
            --build-arg "GRPC_BUILD_DIR=$GRPC_BENCH_BUILD_DIR"
        build_image "$BENCH_DIR/docker/echo_grpc.Dockerfile"  "$IMG_ECHO_GRPC"    "$PROJ_ROOT" \
            --build-arg "GRPC_BUILD_DIR=$GRPC_ECHO_BUILD_DIR"
    elif [ "$scope" = all ] || [ "$scope" = grpc ]; then
        warn "gRPC bench binaries missing; skipping grpc images (run: $0 grpcbuild)"
    fi
    info "Scope images built and imported to containerd"
}

### ------------------------------------------------------------ DPU process
# When dpumesh_dpu is already gone, snapshot what a redeploy would destroy (its `screen`
# session, the kernel ring buffer, the core pattern, the log tail) before stop_dpu kills
# anything. Never fails its caller: `screen -ls` alone exits non-zero whenever no session
# exists, and the presence probe runs with the DPU's own privileges like every other probe here.
capture_absent_dpu() {
    local snapshot="$OUT/dpu-absent-$(date +%Y%m%d-%H%M%S).log"
    dpu_sudo 'pgrep -x dpumesh_dpu >/dev/null' >/dev/null 2>&1 && return 0
    ssh_dpu "test -s $DPU_LOG" >/dev/null 2>&1 || return 0
    mkdir -p "$OUT" 2>/dev/null || return 0
    {
        # The session belongs to root, so the login user's socket directory is
        # empty whatever the process is doing. Dead sessions accumulate ahead of
        # the live listing, so only the newest are worth carrying.
        echo "=== screen -ls"; dpu_sudo 'screen -ls | head -20' 2>&1 || true
        echo "=== dmesg (tail)"; dpu_sudo 'dmesg | tail -80' 2>&1 || true
        echo "=== core pattern"
        ssh_dpu 'cat /proc/sys/kernel/core_pattern; ulimit -c' 2>&1 || true
        echo "=== $DPU_LOG (tail)"; ssh_dpu "tail -80 $DPU_LOG" 2>&1 || true
    } >"$snapshot" 2>&1 || true
    warn "dpumesh_dpu was already absent; evidence captured in $snapshot"
    return 0
}

stop_dpu() {
    capture_absent_dpu
    info "Stopping dpumesh_dpu..."
    # Match process command lines even when the main thread has been renamed.
    ssh_dpu "echo '$DPU_PASS' | sudo -S bash -c \"pids=\\\$(pgrep -f '[d]pumesh_dpu'); [ -z \\\"\\\$pids\\\" ] || kill -9 \\\$pids\" 2>/dev/null; true" 2>&1 | sed 's/^\[sudo\][^:]*: *//' || true
    sleep 5
}

# DPU paths for the static library and provisioned identity.
dpu_home_cached=""
dpu_home() {
    [ -n "$dpu_home_cached" ] || dpu_home_cached=$(ssh_dpu 'echo $HOME')
    printf '%s' "$dpu_home_cached"
}
linkerd_build_dir() { printf '%s' "$(dpu_home)/$DPU_L7_BUILD"; }
linkerd_staticlib() {
    printf '%s' "${L7_LIB_PATH:-$(linkerd_build_dir)/rust/target/release/libdmesh_l7.a}"
}
# The DPU's workload identity material: the directory holding its key, its
# service-account token and the trust anchors it validates the control plane
# with. These defaults are provisioned by linkerd_identity.sh.
linkerd_identity_dir() {
    printf '%s' "${LINKERD_IDENTITY_DIR:-/etc/dpumesh/linkerd-identity}"
}
linkerd_trust_anchors() {
    printf '%s' "${LINKERD_TRUST_ANCHORS:-/etc/dpumesh/linkerd-identity/trust-anchors.pem}"
}
# Where the node agent's delivery hop installs the derived Service-target feed.
linkerd_service_target_file() {
    printf '%s' "${DPUMESH_L7_SERVICE_TARGETS_FILE:-${DPUMESH_FEED_ROOT_DPU:-/etc/dpumesh}/service-targets.v1}"
}
# Identity names are connection-specific. The local name is the identity
# certified for this DPU proxy; the other three names authenticate the Linkerd
# services it dials.
LINKERD_LOCAL_NAME="${LINKERD_LOCAL_NAME:-dpumesh-dpu.test-bench.serviceaccount.identity.linkerd.cluster.local}"
LINKERD_IDENTITY_NAME="${LINKERD_IDENTITY_NAME:-linkerd-identity.linkerd.serviceaccount.identity.linkerd.cluster.local}"
LINKERD_DST_NAME="${LINKERD_DST_NAME:-linkerd-destination.linkerd.serviceaccount.identity.linkerd.cluster.local}"
LINKERD_POLICY_NAME="${LINKERD_POLICY_NAME:-$LINKERD_DST_NAME}"

# Management-link gateway addresses for the deployed Linkerd control plane.
# TLS remains end-to-end; these listeners only relay TCP.
LINKERD_DST_ADDR="${LINKERD_DST_ADDR:-192.168.100.1:28086}"
LINKERD_POLICY_ADDR="${LINKERD_POLICY_ADDR:-192.168.100.1:28087}"
LINKERD_IDENTITY_ADDR="${LINKERD_IDENTITY_ADDR:-192.168.100.1:28088}"

# Synchronize the embedded adapter and port sources.
sync_linkerd_sources() {
    local dest; dest=$(linkerd_build_dir)
    step "=== Syncing linkerd sources to DPU ($DPU_HOST:$dest) ==="
    ssh_dpu "mkdir -p '$dest/rust' '$dest/port/linkerd2-proxy'"
    # Preserve remote build outputs. Both trees mirror the repository: rsync
    # protects an --exclude'd path from --delete, so target/ survives while a
    # source file removed here is removed there. Without that a deleted file
    # keeps being compiled on the DPU and the build stops matching the tree.
    local ex=(--exclude='.git' --exclude='.git/' --exclude='target/'
              --exclude='build/' --exclude='*.o' --exclude='*.a')
    rsync -az --delete --timeout=120 -e "ssh ${SSH_OPTS[*]}" "${ex[@]}" \
        "$LINKERD_RUST_SRC/" "$DPU_HOST:$dest/rust/" ||
        { err "linkerd/rust sync failed"; exit 1; }
    rsync -az --delete --timeout=300 -e "ssh ${SSH_OPTS[*]}" "${ex[@]}" \
        "$LINKERD_PORT_SRC/" "$DPU_HOST:$dest/port/linkerd2-proxy/" ||
        { err "linkerd port sync failed"; exit 1; }
    info "linkerd source sync complete"
}

build_linkerd_artifacts() {
    local dest; dest=$(linkerd_build_dir)
    local cargo="$LINKERD_CARGO +$LINKERD_TOOLCHAIN"
    step "=== Building Linkerd adapter on DPU (libdmesh_l7.a) ==="
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
}

# Validate Linkerd artifacts and adapter symbols.
preflight_linkerd() {
    local lib; lib=$(linkerd_staticlib)
    local data; data=$(linkerd_identity_dir)
    local anchors; anchors=$(linkerd_trust_anchors)
    local unset_vars=""
    for v in LINKERD_DST_ADDR LINKERD_POLICY_ADDR LINKERD_IDENTITY_ADDR \
             LINKERD_DST_NAME LINKERD_POLICY_NAME LINKERD_IDENTITY_NAME \
             LINKERD_LOCAL_NAME; do
        [ -n "${!v:-}" ] || unset_vars="$unset_vars $v"
    done
    if [ -n "$unset_vars" ]; then
        err "deployed Linkerd control plane configuration is missing:$unset_vars"
        exit 1
    fi
    info "control plane: deployed (dst=$LINKERD_DST_ADDR policy=$LINKERD_POLICY_ADDR" \
         "identity=$LINKERD_IDENTITY_ADDR)"
    ssh_dpu "test -r '$(linkerd_service_target_file)'" || {
        err "versioned Service target feed is missing: $(linkerd_service_target_file)"
        exit 1
    }
    local missing
    missing=$(ssh_dpu "[ -r '$lib' ] || echo 'unreadable: $lib'
                       for f in '$data/token.txt' '$data/csr.der' '$data/key.p8' '$anchors'; do
                           echo '$DPU_PASS' | sudo -S test -r \"\$f\" 2>/dev/null ||
                               echo \"unreadable: \$f\"
                       done
                       echo '$DPU_PASS' | sudo -S test -s '$data/token.txt' 2>/dev/null ||
                           echo 'empty: $data/token.txt'")
    if [ -n "$missing" ]; then
        err "linkerd preflight failed:"
        printf '%s\n' "$missing" | sed 's/^/  /'
        err "build them with: $0 linkerdbuild"
        exit 1
    fi

    local csr_san
    csr_san=$(ssh_dpu "echo '$DPU_PASS' | sudo -S openssl req -inform DER \
                           -in '$data/csr.der' -noout -text 2>/dev/null" || true)
    case "$csr_san" in
        *"DNS:$LINKERD_LOCAL_NAME"*) ;;
        *) err "identity CSR does not contain DNS:$LINKERD_LOCAL_NAME"
           err "  CSR: $data/csr.der"
           exit 1 ;;
    esac

    # Check the exported adapter and external runtime backend boundary.
    local defined undefined absent="" s
    defined=$(ssh_dpu "nm -g --defined-only '$lib' 2>/dev/null | awk '\$3 ~ /^l7_/ {print \$3}' | sort -u")
    for s in l7_worker_run l7_conn_open l7_conn_segment l7_conn_eof \
             l7_conn_close l7_inbound_verdict l7_inbound_forget \
             l7_control_event; do
        case $'\n'"$defined"$'\n' in
            *$'\n'"$s"$'\n'*) ;;
            *) absent="$absent $s" ;;
        esac
    done
    if [ -n "$absent" ]; then
        err "libdmesh_l7.a does not export:$absent"
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
    info "linkerd preflight OK (staticlib exports the contract, identity material" \
         "readable, deployed control plane required)"
}

wait_linkerd_ready() {
    local base_addr="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}"
    local base_port="${base_addr##*:}"
    local selection="${DPUMESH_L7_LINKERD_WORKER:-0}"
    # The DPU clamps A to K (A <= K, K % A == 0), so the Linkerd workers that
    # will actually exist are the clamped count, not the requested env value.
    local workers
    workers=$(linkerd_worker_count)
    local timeout_s="${LINKERD_READY_TIMEOUT:-30}"
    local ports="$base_port"
    if [ "$selection" = all ]; then
        ports=""
        local worker
        for ((worker = 0; worker < workers; worker++)); do
            ports="${ports:+$ports }$((base_port + worker))"
        done
    fi

    step "=== Waiting for Linkerd identity and control-plane readiness ==="
    if ! ssh_dpu "deadline=\$((SECONDS + $timeout_s))
ports='$ports'
while [ \$SECONDS -lt \$deadline ]; do
    pending=''
    for port in \$ports; do
        curl -fsS --max-time 1 http://127.0.0.1:\$port/ready >/dev/null 2>&1 || pending=\"\$pending \$port\"
    done
    [ -z \"\$pending\" ] && exit 0
    sleep 1
done
echo \"not ready on admin port(s):\$pending\" >&2
exit 1"; then
        err "Linkerd did not become ready within ${timeout_s}s"
        ssh_dpu "grep -E 'identity|control|certif|WARN|ERROR' '$DPU_LOG' | tail -40" || true
        return 1
    fi
    info "Linkerd ready on admin port(s): $ports"
}

# The target feed carries every Service assigned to the L7 layer: one the feed
# omits is a withdrawn target, which fail-closed refuses. The `namespace/name`
# list is derived from the canonical mode assignment.
resolve_l7_services() {
    local entries entry ns name kept=""
    entries=$(printf '%s,%s' "${DPUMESH_L7_OPAQUE_SVC:-}" \
                                "${DPUMESH_L7_SVC:-}" |
          tr ', ' '\n\n' | grep -E '^[a-z0-9.-]+/[a-z0-9-]+$' | sort -u || true)
    [ -n "$entries" ] || return 0
    # An assignment may name Services this scope does not deploy — the L7 mode
    # sweeps list every echo Service whether or not it exists. Only a Service
    # that exists can be a target, and a missing one is not the withdrawal
    # this feed is meant to express.
    for entry in $entries; do
        ns=${entry%%/*}
        name=${entry##*/}
        if ! kubectl get service "$name" -n "$ns" >/dev/null 2>&1; then
            warn "L7 Service $entry does not exist; not published"
        else
            kept="${kept:+$kept,}$entry"
        fi
    done
    [ -n "$kept" ] || { err "no Service assigned to the L7 layer exists"; exit 1; }
    DPUMESH_L7_SERVICES="$kept"
    export DPUMESH_L7_SERVICES
    info "L7 Service target feed carries: $DPUMESH_L7_SERVICES"
}

start_dpu() {
    local log_level="${DPUMESH_LOG_LEVEL:-40}"
    # Cluster locality is part of Linkerd's destination context. Resolve it
    # before writing the DPU environment so endpoint translation never asks
    # Kubernetes for the empty node name.
    local node_name="${DPUMESH_NODE_NAME:-$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || true)}"
    if [ -z "$node_name" ]; then
        err "DPUMESH_NODE_NAME is unset and kubectl returned no node name"
        exit 1
    fi
    # Services routed through each L7 mode.
    local l7_opaque="${DPUMESH_L7_OPAQUE_SVC:-}"
    local l7_full="${DPUMESH_L7_SVC:-}"
    # Linkerd environment and identity material.
    local l7_env
    local id_name="$LINKERD_LOCAL_NAME"
    local default_policy_workload='{"ns":"default","pod":"dpumesh-dpu"}'
    local policy_workload="${LINKERD_WORKLOAD:-$default_policy_workload}"
    local default_destination_context="{\"ns\":\"default\",\"nodeName\":\"$node_name\",\"pod\":\"dpumesh-dpu\"}"
    local destination_context="${LINKERD_DESTINATION_CONTEXT:-$default_destination_context}"
    # Benchmark-only allocator A/B hook. Keep the production/default launch
    # unchanged unless an explicit absolute DPU path is supplied.
    local dpu_ld_preload="${DPUMESH_DPU_LD_PRELOAD:-}"
    case "$dpu_ld_preload" in
        ""|/*) ;;
        *) err "DPUMESH_DPU_LD_PRELOAD must be an absolute DPU path"; exit 1 ;;
    esac
    local policy_workload_q destination_context_q dpu_ld_preload_q
    printf -v policy_workload_q '%q' "$policy_workload"
    printf -v destination_context_q '%q' "$destination_context"
    printf -v dpu_ld_preload_q '%q' "$dpu_ld_preload"
    ssh_dpu "cat > /tmp/dpumesh-l7.env << 'L7ENV'
LINKERD2_PROXY_DESTINATION_SVC_ADDR=$LINKERD_DST_ADDR
LINKERD2_PROXY_DESTINATION_SVC_NAME=$LINKERD_DST_NAME
LINKERD2_PROXY_POLICY_SVC_ADDR=$LINKERD_POLICY_ADDR
LINKERD2_PROXY_POLICY_SVC_NAME=$LINKERD_POLICY_NAME
LINKERD2_PROXY_POLICY_WORKLOAD=$policy_workload_q
LINKERD2_PROXY_IDENTITY_SVC_ADDR=$LINKERD_IDENTITY_ADDR
LINKERD2_PROXY_IDENTITY_SVC_NAME=$LINKERD_IDENTITY_NAME
LINKERD2_PROXY_IDENTITY_LOCAL_NAME=$id_name
LINKERD2_PROXY_IDENTITY_DIR=$(linkerd_identity_dir)
LINKERD2_PROXY_IDENTITY_TOKEN_FILE=$(linkerd_identity_dir)/token.txt
LINKERD2_PROXY_DESTINATION_CONTEXT=$destination_context_q
LINKERD2_PROXY_DESTINATION_PROFILE_NETWORKS=0.0.0.0/0
LINKERD2_PROXY_POLICY_CLUSTER_NETWORKS=0.0.0.0/0
LINKERD2_PROXY_INBOUND_LISTEN_ADDR=127.0.0.1:0
LINKERD2_PROXY_INBOUND_DEFAULT_POLICY=all-unauthenticated
LINKERD2_PROXY_OUTBOUND_LISTEN_ADDR=127.0.0.1:0
LINKERD2_PROXY_CONTROL_LISTEN_ADDR=127.0.0.1:0
LINKERD2_PROXY_ADMIN_LISTEN_ADDR=${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}
LINKERD2_PROXY_LOG=${LINKERD_LOG:-warn}
DPUMESH_L7_LINKERD_WORKER=${DPUMESH_L7_LINKERD_WORKER:-0}
DPUMESH_L7_SERVICE_TARGETS_FILE=$(linkerd_service_target_file)
DPUMESH_PERF_STATS=${DPUMESH_PERF_STATS:-0}
LD_PRELOAD=$dpu_ld_preload_q
L7ENV
{ printf 'LINKERD2_PROXY_IDENTITY_TRUST_ANCHORS=\"'; \
  echo '$DPU_PASS' | sudo -S cat $(linkerd_trust_anchors) 2>/dev/null; \
  printf '\"\n'; } >> /tmp/dpumesh-l7.env"
    l7_env='set -a; . /tmp/dpumesh-l7.env; set +a;'
    # The environment assembled below is the DPU process's configuration
    # surface (design/CONTROL.md §5.5.1), with this rig's defaults.
    local dpa_threads="${DPUMESH_DPA_THREADS:-}"
    local rings="${DPUMESH_RINGS_PER_POD:-}"
    local workers="${DPUMESH_ARM_WORKERS:-}"
    local registration_key_dir="$DPUMESH_REGISTRATION_KEY_DIR_DPU"
    local feed_key_dir="$DPUMESH_FEED_KEY_DIR_DPU"
    # The DPU refuses grants minted for another node; the same node name was
    # placed in Linkerd's destination context above.
    # Revocation consumes the node membership generation.
    local membership_file="${DPUMESH_MEMBERSHIP_FILE:-/etc/dpumesh/membership.v1}"
    local admission_file="${DPUMESH_ADMISSION_FILE:-/etc/dpumesh/admission}"
    # Cluster facts arrive as the controller's signed topology generation.
    local topology_file="${DPUMESH_TOPOLOGY_FILE:-/etc/dpumesh/topology.v1}"
    local controller_key_dir="${DPUMESH_CONTROLLER_KEY_DIR_DPU:-/etc/dpumesh/controller.pub.keys}"
    # The node credential: generated on the DPU at first boot into a 0400 file
    # that never leaves it. Only the public half is published, and the node
    # agent is what reports it to the controller.
    local node_key_file="${DPUMESH_NODE_KEY_FILE:-/etc/dpumesh/node-static.key}"
    local node_key_public="${DPUMESH_NODE_KEY_PUBLIC_FILE:-/etc/dpumesh/node-static.pub}"
    # The inter-node carrier. Unset leaves the node without one, which is what
    # a single-node rig runs: remote destinations are refused.
    local peer_transport="${DPUMESH_PEER_TRANSPORT:-}"
    local peer_bind="${DPUMESH_PEER_BIND:-}"
    local peer_port="${DPUMESH_PEER_PORT:-47900}"
    # The mediated control-plane lookup, reached through this node's agent.
    local relay_bind="${LINKERD_GATEWAY_BIND:-192.168.100.1}"
    local relay_port="${DPUMESH_CONTROLLER_RELAY_PORT:-28089}"
    local scope_url="${DPUMESH_CONTROLLER_SCOPE_URL:-http://$relay_bind:$relay_port}"
    local trust_domain="${DPUMESH_IDENTITY_TRUST_DOMAIN:-linkerd.${LINKERD_TRUST_DOMAIN:-cluster.local}}"
    step "=== Starting dpumesh_dpu (l7_opaque='$l7_opaque' l7_full='$l7_full' dpa_threads='$dpa_threads' rings_per_pod='$rings' arm_workers='$workers' peer='$peer_transport') ==="
    stop_dpu
    local dpu_home; dpu_home=$(dpu_home)
    # Start one DPU process.
    ssh_dpu "cat > /tmp/start_dpu_bench.sh << 'LAUNCHER'
#!/bin/bash
running=\$(pgrep -x dpumesh_dpu | head -1)
if [ -n \"\$running\" ]; then echo \"\$running\"; exit 0; fi
ulimit -c unlimited
screen -dmS dpumesh-bench bash -c \"ulimit -c unlimited; cd $dpu_home/$DPU_BUILD && $l7_env DPUMESH_NODE_NAME=$node_name DPUMESH_REGISTRATION_KEY_DIR=$registration_key_dir DPUMESH_FEED_KEY_DIR=$feed_key_dir DPUMESH_MEMBERSHIP_FILE=$membership_file DPUMESH_ADMISSION_FILE=$admission_file DPUMESH_TOPOLOGY_FILE=$topology_file DPUMESH_CONTROLLER_KEY_DIR=$controller_key_dir DPUMESH_NODE_KEY_FILE=$node_key_file DPUMESH_NODE_KEY_PUBLIC_FILE=$node_key_public DPUMESH_CONTROLLER_SCOPE_URL=$scope_url DPUMESH_IDENTITY_TRUST_DOMAIN=$trust_domain DPUMESH_L7_OPAQUE_SVC=$l7_opaque DPUMESH_L7_SVC=$l7_full DPUMESH_DPA_THREADS=$dpa_threads DPUMESH_RINGS_PER_POD=$rings DPUMESH_ARM_WORKERS=$workers DPUMESH_PEER_TRANSPORT=$peer_transport DPUMESH_PEER_BIND=$peer_bind DPUMESH_PEER_PORT=$peer_port ./dpumesh_dpu $DPU_PCI -l $log_level > $DPU_LOG 2>&1\"
sleep 2
pgrep -x dpumesh_dpu | head -1 || echo NO_PID
LAUNCHER
chmod +x /tmp/start_dpu_bench.sh"
    local pid; pid=$(ssh_dpu "echo '$DPU_PASS' | sudo -S bash /tmp/start_dpu_bench.sh" 2>&1 | sed 's/^\[sudo\][^:]*: *//')
    if [ "$pid" = "NO_PID" ] || [ -z "$pid" ]; then err "dpumesh_dpu failed to start"; exit 1; fi
    info "dpumesh_dpu running (PID: $pid)"
}

### ------------------------------------------------------------ CPU pinning
# Pods run on the NUMA node local to the host-side BlueField function. Profiles
# allocate host cores only among the three supported DPUmesh API surfaces.
# Shift a comma-separated core list onto the benchmark NUMA node.
core_list() {
    local out="" c
    for c in ${1//,/ }; do out="${out:+$out,}$((BENCH_CORE_BASE + c))"; done
    echo "$out"
}
get_pod_cores() {
    local app="$1" profile="${2:-fair}" rel=""
    case "$profile" in
        native)
            case "$app" in
                bench-dpumesh) rel="0,1,2,3,4,5";;
                echo-dpumesh) rel="6,7,8,9,10,11";;
                echo-dpumesh-13) rel="12";; echo-dpumesh-14) rel="13";;
                loopback-dpumesh) rel="14";; verbs-dpumesh) rel="15";;
            esac ;;
        preload)
            case "$app" in
                preload-bench) rel="0,1,2,3,4,5";;
                preload-echo) rel="6,7,8,9,10,11";;
                preload-dpumesh) rel="12,13,14,15,16,17";;
            esac ;;
        grpc)
            case "$app" in
                bench-grpc-dpumesh) rel="0,1,2,3,4,5";;
                echo-grpc-dpumesh)  rel="6,7,8,9,10,11";;
                echo-grpc-alt)      rel="12,13,14,15,16,17";;
            esac ;;
        grpcmax)
            case "$app" in
                bench-grpc-dpumesh|bench-grpc-linkerd) rel="0,1,2,3,4,5,6,7,8";;
                echo-grpc-dpumesh|echo-grpc-linkerd)  rel="9,10,11,12,13,14,15,16,17";;
            esac ;;
        grpclimit1)
            # Host-bottleneck comparison: one physical Host CPU for each
            # application+transport Pod. Keep the two transport arms on
            # separate CPUs so their idle control processes cannot contend
            # with the arm under measurement. The per-Pod broker and Linkerd
            # sidecar are pinned with their application below.
            case "$app" in
                bench-grpc-dpumesh) rel="0";;
                echo-grpc-dpumesh)  rel="1";;
                bench-grpc-linkerd) rel="2";;
                echo-grpc-linkerd)  rel="3";;
            esac ;;
        fair|*)
            case "$app" in
                bench-dpumesh) rel="0";; echo-dpumesh) rel="1";;
                echo-dpumesh-13) rel="2";; echo-dpumesh-14) rel="3";;
                loopback-dpumesh) rel="4";; verbs-dpumesh) rel="5";;
                preload-dpumesh) rel="6";; preload-echo) rel="7";;
                preload-bench) rel="8";; bench-grpc-dpumesh) rel="9";;
                echo-grpc-dpumesh) rel="10";;
                http1-echo) rel="11";; http1-bench) rel="12";;
                echo-grpc-alt) rel="13";;
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
    for app in bench-dpumesh echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 \
               loopback-dpumesh verbs-dpumesh preload-dpumesh preload-echo \
               preload-bench bench-grpc-dpumesh echo-grpc-dpumesh echo-grpc-alt \
               bench-grpc-linkerd echo-grpc-linkerd; do
        local cores pod_id desired pod_uid uid_token broker_pid pod_json
        cores=$(get_pod_cores "$app" "$profile"); [ -z "$cores" ] && continue
        # A rollout can leave a stale CRI sandbox with the same app label. Pin
        # the sandbox whose UID is the current Ready Kubernetes Pod, not the
        # first label match returned by containerd.
        pod_json=$(kubectl get pods -n "$NS" -l "app=$app" -o json 2>/dev/null || true)
        pod_uid=$(printf '%s' "$pod_json" | jq -r '
            [.items[] | select(
                .metadata.deletionTimestamp == null and
                .status.phase == "Running" and
                any(.status.conditions[]?; .type == "Ready" and .status == "True"))]
            | .[0].metadata.uid // empty' 2>/dev/null)
        pod_id=$(echo "$HOST_PASS" | sudo -S crictl pods -o json 2>/dev/null |
            jq -r --arg uid "$pod_uid" '
                [.items[] | select(.labels["io.kubernetes.pod.uid"] == $uid)]
                | .[0].id // empty' 2>/dev/null)
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
        done
        # The per-Pod broker is intentionally outside every container scope. Match it by
        # the authoritative Pod UID embedded in its cgroup and pin it with its Pod's allocation.
        uid_token=${pod_uid//-/_}
        if [ -n "$uid_token" ]; then
            for broker_pid in $(pgrep -x dmesh_broker 2>/dev/null || true); do
                if rg -q "pod${uid_token}.*dpumesh-broker" "/proc/$broker_pid/cgroup" 2>/dev/null; then
                    info "  dmesh_broker (PID $broker_pid) → $cores"
                    echo "$HOST_PASS" | sudo -S taskset -apc "$cores" "$broker_pid" >/dev/null
                    for child in $(pgrep -P "$broker_pid" 2>/dev/null); do
                        echo "$HOST_PASS" | sudo -S taskset -apc "$cores" "$child" >/dev/null 2>&1 || true
                    done
                fi
            done
        fi
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
    local node="${DPUMESH_NODE_NAME:-$(hostname -s)}"
    kubectl label node "$node" dpumesh.io/dpu=true --overwrite >/dev/null
}

clean_failed_pods() {
    local n; n=$(kubectl get pods -n "$NS" --field-selector=status.phase=Failed --no-headers 2>/dev/null | wc -l)
    if [ "$n" -gt 0 ]; then
        info "Removing $n stale Failed/Evicted pod(s) in $NS"
        kubectl delete pod -n "$NS" --field-selector=status.phase=Failed --ignore-not-found=true >/dev/null 2>&1 || true
    fi
}

# A sidecarless workload still has to be visible to Linkerd's policy index,
# while its DMA port must not make destination discovery expect an injected
# proxy container. Keep the paired metadata an enforced deployment contract.
validate_mesh_metadata() {
    local entries=(
        bench-dpumesh:"$CTRL_PORT" echo-dpumesh:"$CTRL_PORT"
        echo-dpumesh-13:"$CTRL_PORT" echo-dpumesh-14:"$CTRL_PORT"
        loopback-dpumesh:"$CTRL_PORT" verbs-dpumesh:"$CTRL_PORT"
        preload-dpumesh:9095 preload-echo:9100 preload-bench:"$CTRL_PORT"
        bench-grpc-dpumesh:"$CTRL_PORT" echo-grpc-dpumesh:"$CTRL_PORT"
        echo-grpc-alt:"$CTRL_PORT"
        http1-echo:9103 http1-bench:"$CTRL_PORT"
    )
    local entry deployment port control_plane skipped
    for entry in "${entries[@]}"; do
        deployment=${entry%%:*}
        port=${entry##*:}
        control_plane=$(kubectl get deployment "$deployment" -n "$NS" \
            -o go-template='{{index .spec.template.metadata.labels "linkerd.io/control-plane-ns"}}')
        skipped=$(kubectl get deployment "$deployment" -n "$NS" \
            -o go-template='{{index .spec.template.metadata.annotations "config.linkerd.io/skip-inbound-ports"}}')
        [ "$control_plane" = linkerd ] || {
            err "$deployment is absent from Linkerd's workload policy index"
            return 1
        }
        case ",$skipped," in
            *",$port,"*) ;;
            *) err "$deployment DMA port $port is not excluded from Pod-local proxy discovery"
               return 1 ;;
        esac
    done
    info "sidecarless workload policy/discovery metadata validated"
}

prepare_trusted_registration() {
    case "$DPUMESH_ATTEST_SOCKET" in
        /run/dpumesh/*) ;;
        *) err "benchmark attestation socket must be under /run/dpumesh"; exit 1 ;;
    esac
    export DPUMESH_ATTEST_SOCKET
    "$BENCH_DIR/workload_attest.sh" prepare
}

# What the node agent needs to be the DPU's only control peer: where the DPU
# receives feeds, where the controller serves them, and which Services the
# derived L7 target feed names.
export_agent_channel() {
    export LINKERD_CONTROL_NAMESPACE="${LINKERD_CONTROL_NAMESPACE:-linkerd}"
    export LINKERD_IDENTITY_SERVICE_ACCOUNT="${LINKERD_IDENTITY_SERVICE_ACCOUNT:-dpumesh-dpu}"
    # The relay listens where the DPU is configured to reach it: the three
    # control-plane addresses the DPU is given are the same three listeners.
    LINKERD_GATEWAY_BIND="${LINKERD_GATEWAY_BIND:-${LINKERD_DST_ADDR%%:*}}"
    LINKERD_GATEWAY_DST_PORT="${LINKERD_GATEWAY_DST_PORT:-${LINKERD_DST_ADDR##*:}}"
    LINKERD_GATEWAY_POLICY_PORT="${LINKERD_GATEWAY_POLICY_PORT:-${LINKERD_POLICY_ADDR##*:}}"
    LINKERD_GATEWAY_IDENTITY_PORT="${LINKERD_GATEWAY_IDENTITY_PORT:-${LINKERD_IDENTITY_ADDR##*:}}"
    for value in "$LINKERD_GATEWAY_BIND" "$LINKERD_GATEWAY_DST_PORT" \
                 "$LINKERD_GATEWAY_POLICY_PORT" "$LINKERD_GATEWAY_IDENTITY_PORT"; do
        [ -n "$value" ] || { err "the control-plane relay addresses are not configured"; exit 1; }
    done
    export LINKERD_GATEWAY_BIND LINKERD_GATEWAY_DST_PORT
    export LINKERD_GATEWAY_POLICY_PORT LINKERD_GATEWAY_IDENTITY_PORT
    export DPUMESH_CONTROLLER_RELAY_PORT="${DPUMESH_CONTROLLER_RELAY_PORT:-28089}"
    export DPUMESH_DPU_FEED_HOST="${DPUMESH_DPU_FEED_HOST:-192.168.100.2}"
    export DPUMESH_DPU_FEED_PORT="${DPUMESH_DPU_FEED_PORT:-4788}"
    export DPUMESH_IDENTITY_STAGE_DIR="${LINKERD_PROVISION_DIR:-$PROJ_ROOT/build/linkerd-identity}"
    local controller_ip
    controller_ip=$(kubectl get service dpumesh-controller -n "$NS" \
        -o jsonpath='{.spec.clusterIP}' 2>/dev/null || true)
    [ -n "$controller_ip" ] || { err "dpumesh-controller Service has no ClusterIP"; exit 1; }
    export DPUMESH_CONTROLLER_URL="${DPUMESH_CONTROLLER_URL:-http://$controller_ip:8080}"
    DPUMESH_AGENT_SERVICE_ARGS=$("$BENCH_DIR/linkerd_service_registry.sh" service-args)
    export DPUMESH_AGENT_SERVICE_ARGS
}

# The feeds the DPU consumes, as the DPU holds them. What is waited on is the
# agent's delivery landing, because that is the only way any of them arrives —
# and the DPU build's preflight reads them.
await_feeds() {
    local waited=0 deadline="${FEED_DELIVERY_DEADLINE:-90}" want
    want="${DPUMESH_MEMBERSHIP_FILE:-/etc/dpumesh/membership.v1} ${DPUMESH_TOPOLOGY_FILE:-/etc/dpumesh/topology.v1}"
    want="$want $(linkerd_service_target_file)"
    step "=== Waiting for the node agent to deliver the DPU's feeds ==="
    while [ "$waited" -lt "$deadline" ]; do
        local missing="" f
        for f in $want; do
            ssh_dpu "test -s '$f'" 2>/dev/null || missing="${missing:+$missing }$f"
        done
        if [ -z "$missing" ]; then
            info "every feed delivered after ${waited}s"
            return 0
        fi
        sleep 3
        waited=$((waited + 3))
    done
    err "the node agent did not deliver: $missing"
    err "  see: kubectl logs -n $NS -l app=dpumesh-node-agent"
    return 1
}

# One delivery interval plus slack. The bundle moves as a unit, so what is
# waited on is the token the DPU ends up holding, not a file appearing.
await_identity_delivery() {
    local waited=0 deadline="${IDENTITY_DELIVERY_DEADLINE:-60}"
    while [ "$waited" -lt "$deadline" ]; do
        if ssh_dpu "test -s '$(linkerd_identity_dir)/token.txt'" 2>/dev/null; then
            info "identity bundle delivered after ${waited}s"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done
    err "the node agent did not deliver an identity bundle in ${deadline}s"
    return 1
}

# Render pods.yaml, grpc-pods.yaml and grpc-linkerd-pods.yaml with envsubst and
# apply them (replicas: 0).
apply_manifest() {
    configure_host_numa
    step "=== Applying K8s manifest (replicas=0) ==="
    command -v envsubst >/dev/null 2>&1 || { err "envsubst not found (apt install gettext-base)"; exit 1; }
    export IMG_BENCH_DPU IMG_ECHO_DPU IMG_LOOPBACK_DPU IMG_VERBS_DPU \
           IMG_PRELOAD_DPU IMG_PRELOAD_SOCK IMG_BENCH_GRPC IMG_ECHO_GRPC
    export CTRL_PORT HOST_PCI LIB_OUT BENCH_NUMA_NODE
    export DPUMESH_ATTEST_SOCKET
    export DPUMESH_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-8}" \
           BENCH_DST_SERVICES ECHO_13_SERVICE ECHO_14_SERVICE \
           DMESH_PRELOAD_DEBUG="${DMESH_PRELOAD_DEBUG:-0}" \
           BENCH_REACTORS="${BENCH_REACTORS:-8}"
    # A killed container keeps no log, so a sanitizer report has to land on the
    # host to survive the process it describes. The mount is DirectoryOrCreate,
    # so kubelet creates the directory and the privileged container can write it.
    export ASAN_LOG_DIR="${ASAN_LOG_DIR:-/var/log/dpumesh-asan}"
    { cat "$MANIFEST"; echo ---; cat "$GRPC_MANIFEST"; echo ---; cat "$GRPC_LINKERD_MANIFEST"; } |
        envsubst | kubectl apply -n "$NS" -f -
    info "DPUmesh API resources applied"
}

broker_data_ready() { # $1 = app label
    local app="$1" pod_uid token pid uid caps nspid
    pod_uid=$(kubectl get pod -n "$NS" -l "app=$app" \
        --field-selector=status.phase=Running \
        -o jsonpath='{.items[0].metadata.uid}' 2>/dev/null || true)
    [ -n "$pod_uid" ] || return 1
    token=${pod_uid//-/_}
    for pid in $(pgrep -x dmesh_broker 2>/dev/null || true); do
        rg -q "pod${token}.*dpumesh-broker" "/proc/$pid/cgroup" 2>/dev/null || continue
        uid=$(awk '$1=="Uid:"{print $2}' "/proc/$pid/status" 2>/dev/null)
        caps=$(awk '$1=="CapEff:"{print $2}' "/proc/$pid/status" 2>/dev/null)
        nspid=$(awk '$1=="NSpid:"{print $NF}' "/proc/$pid/status" 2>/dev/null)
        [ "$uid" = 65532 ] && [ "$caps" = 0000000000000000 ] &&
            [ "$nspid" = 1 ] && return 0
    done
    return 1
}

scale_up_with_wait() {
    local app="$1" expected_log="$2" image="$3"
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
    ensure_image_imported "$image"
    kubectl scale deployment "$app" --replicas=1 -n "$NS"
    # `kubectl wait` on a label set that is still empty fails immediately rather
    # than waiting, and admission runs between the scale and the Pod object, so
    # the object has to be there before there is a condition to wait on.
    i=0
    while [ "$i" -lt 120 ] &&
          [ -z "$(kubectl get pods -n "$NS" -l "app=$app" -o name 2>/dev/null)" ]; do
        sleep 0.25
        i=$((i + 1))
    done
    if ! kubectl wait --for=condition=Ready pod -l "app=$app" -n "$NS" --timeout=120s 2>&1; then
        err "$app failed to start"; kubectl describe pod -l "app=$app" -n "$NS" | tail -15; exit 1
    fi
    info "$app pod Ready"
    if [ -n "$expected_log" ]; then
        # The 'DPUmesh DOCA initialized' line is printed once POD_INIT_READY has
        # confirmed the registration and mmap/DPA setup.
        info "Waiting for DPUmesh init: $app ($expected_log)"
        local attempts=0
        while [ $attempts -lt 35 ]; do
            local line; line=$(kubectl logs -n "$NS" -l "app=$app" --tail=80 2>/dev/null || true)
            # The init line comes from a broker-attached context; the Pod is ready only once
            # its broker also holds the reduced identity.
            if echo "$line" | grep -Eq "$expected_log" &&
               { ! echo "$line" | grep -q "broker-attached" ||
                 broker_data_ready "$app"; }; then
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

# `all` proves the native, preload, and gRPC adapters against the same embedded
# Linkerd/DMA data plane. Narrow scopes are useful while diagnosing one adapter;
# they never select another transport implementation.
start_pods() {
    local scope="$BENCH_DEPLOY_SCOPE"
    case "$scope" in all|native|preload|grpc) ;;
        *) err "BENCH_DEPLOY_SCOPE must be all|native|preload|grpc (got $scope)"; exit 1;;
    esac
    step "=== Starting pods (innermost first, scope=$scope) ==="
    local ready="DPUmesh DOCA initialized"
    if [ "$scope" = all ] || [ "$scope" = native ]; then
        scale_up_with_wait "echo-dpumesh"     "$ready" "$IMG_ECHO_DPU"
        scale_up_with_wait "echo-dpumesh-13"  "$ready" "$IMG_ECHO_DPU"
        scale_up_with_wait "echo-dpumesh-14"  "$ready" "$IMG_ECHO_DPU"
        scale_up_with_wait "bench-dpumesh"    "$ready" "$IMG_BENCH_DPU"
        scale_up_with_wait "loopback-dpumesh" "$ready" "$IMG_LOOPBACK_DPU"
        scale_up_with_wait "verbs-dpumesh"    "$ready" "$IMG_VERBS_DPU"
    fi
    if [ "$scope" = all ] || [ "$scope" = preload ]; then
        scale_up_with_wait "preload-dpumesh" "$ready" "$IMG_PRELOAD_DPU"
        scale_up_with_wait "preload-echo"    "$ready" "$IMG_PRELOAD_SOCK"
        scale_up_with_wait "preload-bench"   ""       "$IMG_PRELOAD_SOCK"
        scale_up_with_wait "http1-echo"      "$ready" "$IMG_PRELOAD_SOCK"
        scale_up_with_wait "http1-bench"     ""       "$IMG_PRELOAD_SOCK"
    fi
    if [ "$scope" = all ] || [ "$scope" = grpc ]; then
        scale_up_with_wait "echo-grpc-dpumesh"  "$ready" "$IMG_ECHO_GRPC"
        scale_up_with_wait "echo-grpc-alt"      "$ready" "$IMG_ECHO_GRPC"
        scale_up_with_wait "bench-grpc-dpumesh" "$ready" "$IMG_BENCH_GRPC"
    fi
}

deploy_webhook() {
    step "=== Deploying the workload admission webhook ==="
    export NS IMG_CONTROLLER LIB_OUT HOST_PCI DPUMESH_RINGS_PER_POD
    export DPUMESH_ATTEST_SOCKET
    envsubst < "$BENCH_DIR/k8s/webhook.yaml" | kubectl apply -f -
    kubectl rollout restart deployment/dpumesh-webhook -n "$NS"
    kubectl rollout status deployment/dpumesh-webhook -n "$NS" --timeout=120s
}

deploy() {
    need_env
    configure_host_numa
    ensure_namespace
    clean_failed_pods
    prepare_trusted_registration
    apply_manifest
    validate_mesh_metadata || exit 1
    resolve_l7_services
    "$BENCH_DIR/linkerd_service_registry.sh" validate
    sync_sources
    build_host
    build_bench_binaries
    if [ "$BENCH_DEPLOY_SCOPE" = all ] || [ "$BENCH_DEPLOY_SCOPE" = grpc ]; then
        build_grpc_apps
    fi
    build_images
    # The control plane comes up before anything that reads what it publishes.
    # The DPU build's preflight checks the feeds the DPU will consume, and every one
    # of them arrives through the agent, so the agent has to be running — with its
    # image built — before that preflight.
    "$BENCH_DIR/dpumesh_controller.sh" prepare
    IMG_CONTROLLER="$IMG_CONTROLLER" "$BENCH_DIR/dpumesh_controller.sh" deploy
    # After every root-owned keyring is provisioned: the hop's account owns the
    # feed directory, and a keyring created under it would take that back.
    "$BENCH_DIR/workload_attest.sh" install-hop
    export_agent_channel
    IMG_WORKLOAD_AGENT="$IMG_WORKLOAD_AGENT" "$BENCH_DIR/workload_attest.sh" deploy
    await_feeds || exit 1
    deploy_webhook
    # The staticlib the DPU binary links has to exist before it is linked.
    sync_linkerd_sources
    build_linkerd_artifacts
    build_dpu
    start_dpu
    start_pods
    wait_linkerd_ready
    pin_pods fair
    validate_linkerd_session
    info "=== Deploy complete ==="
    echo "  Run:  $0 latency|bandwidth|rate|all [dpumesh|preload|grpc-dpumesh]"
    echo "        $0 loopback|verbs|preload ...   (validators)"
    echo "  Re-pin:  $0 pin [fair|native|preload|grpc|grpcmax|grpclimit1]"
}

# Protected admission is a file the DPU control thread polls, so it can be set
# without restarting the proxy.
set_admission() {
    local state="$1" path="${DPUMESH_ADMISSION_FILE:-/etc/dpumesh/admission}"
    case "$state" in
        open|drain) ;;
        *) err "admission state must be open or drain"; exit 1 ;;
    esac
    ssh_dpu "printf '%s\n' '$state' > /tmp/dpumesh-admission.in && \
        echo '$DPU_PASS' | sudo -S -p '' mkdir -p '${path%/*}' && \
        echo '$DPU_PASS' | sudo -S -p '' install -o root -g root -m 0644 /tmp/dpumesh-admission.in '$path.new' && \
        echo '$DPU_PASS' | sudo -S -p '' mv '$path.new' '$path' && \
        rm -f /tmp/dpumesh-admission.in" >/dev/null
    info "protected admission: $state"
}

# The DPU polls the switch, so writing it is not the same as it taking effect.
# The state change is counted, which is what a caller can wait on.
control_events() {
    local kind="$1" reason="$2" admin="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}" seen
    seen=$(ssh_dpu "curl -s $admin/metrics | sed -n 's/^dmesh_control_events_total{kind=\"$kind\",reason=\"$reason\"} //p'" 2>/dev/null | tr -d '[:space:]')
    printf '%s\n' "${seen:-0}"
}

admission_events() {
    control_events admission "$1"
}

# The policy observation gate. Enforcement must not stand on an unobserved
# dependency, so what the policy controller actually serves to a caller
# authenticated as `dpumesh-dpu` is recorded before enforcement is trusted:
# whether it serves a policy, denies the request, or serves an empty one.
#
# The verdict counters separate the three. `admitted` and `denied` are
# decisions taken against a served policy. `no-policy` is the dependency not
# answering — a controller that denies the caller, or one that serves an empty
# policy the union rule then refuses, both land here, and the proxy's own
# inbound policy family distinguishes them.
observe_policy() {
    need_env
    local admin="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}"
    step "=== Observing what the policy controller serves to dpumesh-dpu ==="
    validate_linkerd_session || true
    echo "--- inbound verdicts ---"
    ssh_dpu "curl -s $admin/metrics | grep -E '^dmesh_control_events_total\{kind=\"inbound\"' || true"
    echo "--- inbound policy watches ---"
    ssh_dpu "curl -s $admin/metrics | grep -E '^inbound_(http_)?(policy|server)' || true"
    echo "--- control-plane policy client ---"
    ssh_dpu "curl -s $admin/metrics | grep -E '^control_policy' || true"
    info "record the three blocks above with the deploy they were taken from"
}

await_admission() {
    local state="$1" before="$2" waited=0
    while [ "$waited" -lt 20 ]; do
        [ "$(admission_events "$state")" -gt "$before" ] && { info "protected admission observed: $state"; return 0; }
        sleep 1
        waited=$((waited + 1))
    done
    err "the DPU did not observe admission=$state within ${waited}s"
    return 1
}

# Replace identity material against a quiet proxy: nothing in flight is cut, and
# nothing new is admitted until the new certificate is installed.
rotate_identity() {
    need_env
    local deadline="${LINKERD_DRAIN_TIMEOUT:-60}"
    local admin="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}"

    step "=== Draining protected admission ==="
    local drain_before; drain_before=$(admission_events drain)
    set_admission drain
    await_admission drain "$drain_before" || exit 1
    local waited=0 active=1
    while [ "$waited" -lt "$deadline" ]; do
        active=$(ssh_dpu "curl -s $admin/metrics | sed -n 's/^dmesh_sessions_active //p'" 2>/dev/null | tr -d '[:space:]')
        [ -n "$active" ] || active=0
        [ "$active" = 0 ] && break
        sleep 2
        waited=$((waited + 2))
    done
    if [ "${active:-1}" != 0 ]; then
        err "drain did not reach zero active sessions in ${deadline}s (active=$active)"
        set_admission open
        exit 1
    fi
    unset drain_before
    info "drained after ${waited}s"

    step "=== Re-provisioning identity material ==="
    # The node agent mints the token and delivers the bundle, so rotation is
    # provisioning the long-lived half and waiting one delivery interval.
    "$BENCH_DIR/linkerd_identity.sh" refresh-token
    await_identity_delivery || exit 1

    step "=== Restarting with the new material ==="
    start_dpu
    start_pods
    wait_linkerd_ready
    # A restart re-creates the Pods, so their placement has to be restored:
    # without it the rotation reads as an L7 latency regression.
    pin_pods fair

    local open_before; open_before=$(admission_events open)
    set_admission open
    await_admission open "$open_before" || exit 1
    validate_linkerd_session
    info "=== Identity material rotated ==="
}

validate_linkerd_session() {
    step "=== Validating deployed API adapters through embedded Linkerd ==="
    local targets="" target reply
    local admitted_before denied_before missing_before poison_before
    local admitted_after denied_after missing_after poison_after
    admitted_before=$(control_events inbound admitted)
    denied_before=$(control_events inbound denied)
    missing_before=$(control_events inbound no-policy)
    poison_before=$(control_events peer poison)
    case "$BENCH_DEPLOY_SCOPE" in
        all) targets="dpumesh preload grpc-dpumesh" ;;
        native) targets="dpumesh" ;;
        preload) targets="preload" ;;
        grpc) targets="grpc-dpumesh" ;;
    esac
    for target in $targets; do
        reply=$(run_point "$target" 1024 8 1 5 100 1)
        printf '  %-13s %s\n' "$target" "$reply"
        case "$reply" in
            OK*) ;;
            *)   err "$target embedded-Linkerd validation failed: $reply"
                 err "  see: $0 dpulog 200 and the gateway/control-plane Pod logs"
                 return 1 ;;
        esac
    done
    admitted_after=$(control_events inbound admitted)
    denied_after=$(control_events inbound denied)
    missing_after=$(control_events inbound no-policy)
    poison_after=$(control_events peer poison)
    [ "$admitted_after" -gt "$admitted_before" ] || {
        err "API smoke moved data without an inbound Linkerd policy verdict"
        return 1
    }
    [ "$denied_after" -eq "$denied_before" ] &&
        [ "$missing_after" -eq "$missing_before" ] &&
        [ "$poison_after" -eq "$poison_before" ] || {
        err "API smoke changed failure counters: denied=$denied_before->$denied_after "\
            "no-policy=$missing_before->$missing_after poison=$poison_before->$poison_after"
        return 1
    }
    info "all deployed API adapters passed the embedded-Linkerd smoke gate"
}

cleanup() {
    info "Deleting namespace $NS"
    kubectl delete ns "$NS" --ignore-not-found=true 2>/dev/null || true
    stop_dpu
    "$BENCH_DIR/workload_attest.sh" stop || true
    "$BENCH_DIR/dpumesh_controller.sh" stop >/dev/null 2>&1 || true
}

show_logs() {
    for app in bench-dpumesh echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 \
               loopback-dpumesh verbs-dpumesh preload-dpumesh preload-echo \
               preload-bench bench-grpc-dpumesh echo-grpc-dpumesh; do
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
    local snap0 snap1 result w0 w1 dt hz dpid l7_before="" l7_after=""
    declare -A tick0 comm0

    snap0=$(dpu_thread_snapshot) || { err "dpumesh_dpu is not running on the DPU"; return 1; }
    l7_before=$(linkerd_metrics_snapshot || true)
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
    l7_after=$(linkerd_metrics_snapshot || true)
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
    if [ -n "$l7_before" ] && [ -n "$l7_after" ]; then
        local worker opened0 opened1 closed1 active pending tasks delta balance_bad=0
        printf "   %-8s %12s %12s %8s %8s %8s\n" \
               L7_WORKER OPENED_DELTA CLOSED_TOTAL ACTIVE PENDING TASKS
        for worker in $(linkerd_worker_ids); do
            opened0=$(metric_worker_value "$l7_before" "$worker" dmesh_sessions_opened_total)
            opened1=$(metric_worker_value "$l7_after" "$worker" dmesh_sessions_opened_total)
            closed1=$(metric_worker_value "$l7_after" "$worker" dmesh_sessions_closed_total)
            active=$(metric_worker_value "$l7_after" "$worker" dmesh_sessions_active)
            pending=$(metric_worker_value "$l7_after" "$worker" dmesh_registrations_pending)
            tasks=$(metric_worker_value "$l7_after" "$worker" dmesh_tasks_live)
            delta=$(( ${opened1:-0} - ${opened0:-0} ))
            printf "   %-8s %12s %12s %8s %8s %8s\n" \
                   "$worker" "$delta" "${closed1:-NA}" "${active:-NA}" \
                   "${pending:-NA}" "${tasks:-NA}"
            if linkerd_all_workers && [ "$delta" -le 0 ]; then
                balance_bad=1
            fi
        done
        [ "$balance_bad" = 0 ] || {
            err "L7 session placement did not reach every configured worker"
            return 1
        }
    else
        warn "Linkerd metrics unavailable; session placement was not validated"
    fi
    [ -z "$csv" ] || info "-> $csv"
}

### ------------------------------------------------------------ benchmark (RUN)
# The client Pod behind a `point` target. Every client answers the same RUN line on
# the same control port; all but the native client read six fields and ignore the
# churn period, which only bench_dpumesh implements.
app_of()     { case "$1" in
                 dpumesh)      echo bench-dpumesh ;;
                 preload)      echo preload-bench ;;
                 http1)        echo http1-bench ;;
                 grpc-dpumesh) echo bench-grpc-dpumesh ;;
                 *)            echo "" ;;
               esac; }
targets_of() { echo "${1:-dpumesh}"; }
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

# run_ping <sol> -> OK pong | ERR no_pod(app) | ERR silent | ERR refused
# Liveness that costs the client nothing. The control server is a serial accept
# loop, so a client in the middle of another RUN leaves this connection sitting
# in its backlog and answers nothing. That silence is `silent`, and this probe
# alone cannot tell an occupied client from a wedged one. A port that refuses
# the connection is `refused`, and that is not ambiguous: nothing is listening.
run_ping() {
    local app ip reply rc
    app="$(app_of "$1")"; [ -z "$app" ] && { echo "ERR bad_target($1)"; return 0; }
    ip=$(running_pod_ip "$app" || true)
    [ -z "$ip" ] && { echo "ERR no_pod($app)"; return 0; }
    rc=0
    reply=$(printf 'PING\n' | timeout "${PING_TIMEOUT:-2}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null) || rc=$?
    case "$reply" in PONG*) echo "OK pong"; return 0 ;; esac
    [ "$rc" -eq 124 ] && { echo "ERR silent"; return 0; }   # timeout killed it
    echo "ERR refused"
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
    [ "$sol" = dpumesh ] && warn "server is single-consumer; only CLIENT threads scale. Pin more cores first ($0 pin native) for a real curve."
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
run_loopback() {  # self-routing: the loopback-dpumesh Pod is client + server of its own service
    local N="${1:-50000}" size="${2:-8192}" zc="${3:-0}" ip resp
    ip=$(running_pod_ip loopback-dpumesh || true)
    [ -z "$ip" ] && { err "loopback-dpumesh pod not found — run '$0 deploy'"; return 1; }
    step "=== loopback (self-service): N=$N size=${size}B zerocopy=$zc ==="
    resp=$(printf 'RUN %s %s %s\n' "$N" "$size" "$zc" | timeout 120s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "loopback replied: $resp"; return 1; }
    read -r _ ok fail served p50 <<<"$resp"
    printf "  OK/Fail: %s/%s  served: %s  p50: %s us\n" "$ok" "$fail" "$served" "$p50"
    [ "$ok" -eq "$N" ] && [ "$fail" -eq 0 ] && [ "$served" -eq "$N" ] || {
        err "loopback validation failed (expected OK/Fail/served=$N/0/$N)"
        return 1
    }
}

run_verbs() {  # verbs-façade self-routing: the verbs-dpumesh Pod is client + server of its own service
    local N="${1:-50000}" size="${2:-8192}" zc="${3:-0}" window="${4:-1}" pipe="${5:-1}" ip resp
    ip=$(running_pod_ip verbs-dpumesh || true)
    [ -z "$ip" ] && { err "verbs-dpumesh pod not running — run '$0 deploy' (the validator waits for a RUN command)"; return 1; }
    step "=== verbs (self-service): N=$N size=${size}B zc=$zc window=$window pipeline=$pipe ==="
    resp=$(printf 'RUN %s %s %s %s %s\n' "$N" "$size" "$zc" "$window" "$pipe" | timeout 180s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "verbs replied: $resp"; return 1; }
    read -r _ ok fail served p50 <<<"$resp"
    printf "  OK/Fail: %s/%s  served: %s  p50: %s us\n" "$ok" "$fail" "$served" "$p50"
    [ "$ok" -eq "$N" ] && [ "$fail" -eq 0 ] && [ "$served" -eq "$N" ] || {
        err "verbs validation failed (expected OK/Fail/served=$N/0/$N)"
        return 1
    }
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
    [ "$ok" -eq "$N" ] && [ "$fail" -eq 0 ] || {
        err "preload validation failed (expected OK/Fail=$N/0)"
        return 1
    }
}

# Sum one unlabelled Prometheus sample across worker snapshots.
metric_value() {
    awk -v name="$2" '$1 == name { value += $2; found = 1 }
        END { if (found) print value + 0 }' <<<"$1"
}

# Effective worker geometry, matching dpu_worker.c's K/A normalization.
linkerd_all_workers() {
    [[ "${DPUMESH_L7_LINKERD_WORKER:-0}" == [aA][lL][lL] ]]
}

linkerd_worker_count() {
    if ! linkerd_all_workers; then
        echo 1
        return
    fi
    local workers="${DPUMESH_ARM_WORKERS:-1}" rings="${DPUMESH_RINGS_PER_POD:-2}"
    [[ "$workers" =~ ^[0-9]+$ ]] || workers=1
    [[ "$rings" =~ ^[0-9]+$ ]] || rings=2
    [ "$workers" -ge 1 ] || workers=1
    [ "$workers" -le 16 ] || workers=16
    while [ "$workers" -gt 1 ] &&
          { [ "$workers" -gt "$rings" ] || [ $((rings % workers)) -ne 0 ]; }; do
        workers=$((workers - 1))
    done
    echo "$workers"
}

linkerd_worker_ids() {
    if linkerd_all_workers; then
        seq 0 $(( $(linkerd_worker_count) - 1 ))
    else
        echo "${DPUMESH_L7_LINKERD_WORKER:-0}"
    fi
}

# Fetch every worker's existing Linkerd metrics registry. Marker lines retain
# worker identity while metric_value can still aggregate the snapshots.
linkerd_metrics_snapshot() {
    local addr="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}"
    local host="${addr%:*}" base_port="${addr##*:}" count worker port metrics
    [ "$host" != "$addr" ] && [[ "$base_port" =~ ^[0-9]+$ ]] || return 1
    count=$(linkerd_worker_count)
    for ((worker = 0; worker < count; worker++)); do
        port="$base_port"
        if linkerd_all_workers; then
            port=$((base_port + worker))
        fi
        [ "$port" -le 65535 ] || return 1
        metrics=$(ssh_dpu "curl -g -sf --max-time 3 'http://${host}:${port}/metrics'" \
                  2>/dev/null) || return 1
        if linkerd_all_workers; then
            printf '# dmesh_worker %d admin_port %d\n%s\n' "$worker" "$port" "$metrics"
        else
            printf '# dmesh_worker %s admin_port %d\n%s\n' \
                   "${DPUMESH_L7_LINKERD_WORKER:-0}" "$port" "$metrics"
        fi
    done
}

metric_worker_value() {
    awk -v wanted_worker="$2" -v name="$3" '
        $1 == "#" && $2 == "dmesh_worker" { worker = $3; next }
        worker == wanted_worker && $1 == name { print $2; exit }
    ' <<<"$1"
}

show_linkerd_metrics() {
    need_env
    local metrics worker
    metrics=$(linkerd_metrics_snapshot) || {
        err "one or more DPU Linkerd metrics endpoints are unavailable"
        return 1
    }
    printf "%-8s %10s %10s %8s %8s %8s %10s\n" \
           WORKER OPENED CLOSED ACTIVE PENDING TASKS ORPHANED
    for worker in $(linkerd_worker_ids); do
        printf "%-8s %10s %10s %8s %8s %8s %10s\n" \
            "$worker" \
            "$(metric_worker_value "$metrics" "$worker" dmesh_sessions_opened_total)" \
            "$(metric_worker_value "$metrics" "$worker" dmesh_sessions_closed_total)" \
            "$(metric_worker_value "$metrics" "$worker" dmesh_sessions_active)" \
            "$(metric_worker_value "$metrics" "$worker" dmesh_registrations_pending)" \
            "$(metric_worker_value "$metrics" "$worker" dmesh_tasks_live)" \
            "$(metric_worker_value "$metrics" "$worker" dmesh_registrations_orphaned_total)"
    done
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
    metrics=$(linkerd_metrics_snapshot || true)
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
        metrics=$(linkerd_metrics_snapshot || true)
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
        metrics=$(linkerd_metrics_snapshot || true)
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
    kubectl rollout status deployment/"$app" -n "$NS" --timeout=120s >/dev/null
    pod=$(kubectl get pod -n "$NS" -l "app=$app" \
        --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}')
    local ready=0 logs reply
    for _ in $(seq 1 60); do
        logs=$(kubectl logs -n "$NS" "$pod" 2>&1 || true)
        if rg -q 'DPUmesh DOCA initialized' <<<"$logs" &&
           { ! rg -q 'broker-attached' <<<"$logs" || broker_data_ready "$app"; }; then
            ready=1; break
        fi
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

    metrics=$(linkerd_metrics_snapshot || true)
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
    geometry) printf 'throughput_workers=%s N=%s K=%s A=%s l7_workers=%s\n' \
                    "${DPUMESH_THROUGHPUT_WORKERS:-custom}" \
                    "$DPUMESH_DPA_THREADS" "$DPUMESH_RINGS_PER_POD" \
                    "$DPUMESH_ARM_WORKERS" "$DPUMESH_L7_LINKERD_WORKER" ;;
    deploy)    deploy ;;
    build)     need_env; sync_sources; build_dpu ;;
    restart)   need_env; start_dpu ;;
    rotate-identity) rotate_identity ;;
    admission) need_env; set_admission "${1:?usage: $0 admission open|drain}" ;;
    policy-observe) observe_policy ;;
    grpcbuild) build_grpc_apps ;;
    linkerdbuild) need_env; sync_linkerd_sources; build_linkerd_artifacts; preflight_linkerd ;;
    # NOTE: `restart` is valid only while no pod is meshed, and there is no
    # per-pod start. Restarting the DPU under live pods — or starting a pod against
    # an already-running DPU — leaves the two sides' registration state inconsistent.
    # `deploy` is the path for anything with pods: it brings up the DPU and every
    # pod together.
    latency)   for s in $(targets_of "${1:-dpumesh}"); do bench_latency   "$s"; done ;;
    bandwidth) for s in $(targets_of "${1:-dpumesh}"); do bench_bandwidth "$s"; done ;;
    rate)      for s in $(targets_of "${1:-dpumesh}"); do bench_rate      "$s"; done ;;
    all)       for s in $(targets_of "${1:-dpumesh}"); do bench_latency "$s"; bench_bandwidth "$s"; bench_rate "$s"; done; info "results under $OUT" ;;
    point)     [ $# -eq 7 ] || [ $# -eq 8 ] || { err "point <sol> <req> <reply> <conc> <dur> <warmup> <threads> [reconn]"; exit 1; }; run_point "$@" ;;
    ping)      [ $# -eq 1 ] || { err "ping <sol>"; exit 1; }; run_ping "$@" ;;
    loopback)  run_loopback "${1:-50000}" "${2:-8192}" "${3:-0}" ;;
    verbs)     run_verbs    "${1:-50000}" "${2:-8192}" "${3:-0}" "${4:-1}" "${5:-1}" ;;
    preload)   run_preload  "${1:-5000}"  "${2:-1024}" "${3:-8}" ;;
    grpcshutdown) run_grpc_shutdown ;;
    pin)       need_env; pin_pods "${1:-fair}" ;;
    status)    show_status ;;
    logs)      show_logs ;;
    cleanup)   cleanup ;;
    dpulog)    ssh_dpu "echo '$DPU_PASS' | sudo -S tail -${1:-40} $DPU_LOG" 2>&1 | sed 's/^\[sudo\][^:]*: *//' ;;
    dpubanner) ssh_dpu "echo '$DPU_PASS' | sudo -S grep -h 'DPU PROXY MODE ON' $DPU_LOG | tail -1" 2>&1 | sed 's/^\[sudo\][^:]*: *//' ;;
    dpucpu)    dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -z "$pid" ] && { echo "dpumesh_dpu not running"; exit 0; }; echo "=== dpumesh_dpu pid=$pid per-thread %CPU ==="; top -bH -d 1 -n 2 -p "$pid" | awk "/ PID +USER/{n++} n==2{print}"' ;;
    armbalance) arm_balance "$@" ;;
    l7metrics)  show_linkerd_metrics ;;
    *)
        cat <<EOF
Usage: $0 <command> [args]

  deploy                                     build + DPU + images + pods + pin (the ONLY bring-up path)
  geometry                                   print the resolved DPU N/K/A/L7 geometry
  build | restart                            rebuild the DPU binary | restart the DPU alone (no pod may be meshed)
  linkerdbuild                               sync + build libdmesh_l7.a on the DPU
                                             (deploy does this itself)
  latency|bandwidth|rate|all [dpumesh|preload|grpc-dpumesh]
                                             benchmark one supported API -> CSVs under $OUT
  point <sol> <req> <reply> <conc> <dur> <warmup> <threads> [reconn]   one raw RUN (reconn = conn-churn period)
  ping <sol>                                 PING the client's control port (PING_TIMEOUT, default 2s)
  loopback|preload [args]                    feature validators
  grpcshutdown                              real-DPU HTTP/2 process-stop + slot-reuse gate
  verbs <N> <size> <zc> <window> <pipeline>  native-API loopback validator: window conns x pipeline outstanding
  pin [fair|native|preload|grpc|grpcmax|grpclimit1]
                                             (re)pin supported API pods to cores
  armbalance [req reply conc dur threads [csv]]   DPU main/worker per-core CPU during one point
  status | logs | cleanup | dpulog [n] | dpubanner | dpucpu | l7metrics

Deploy knobs (env): BENCH_NUMA_POLICY=local|auto BENCH_DEPLOY_SCOPE=all|native|preload|grpc
                    DPUMESH_THROUGHPUT_WORKERS=W derives A=K=W, valid N, L7=all
                    Every deploy brings up the root Host workload agent; grants
                    are signed and DPU admission is fail-closed
                    BENCH_DST_SERVICES=a,b,... assigns native client threads round-robin
                    ECHO_13_SERVICE/ECHO_14_SERVICE split the extra echo pods into services
                    Linkerd is always embedded; deploy provisions its control plane
                    and builds libdmesh_l7.a on the DPU
                    BENCH_GRPC_BUILD=release|asan (asan instruments echo_grpc only;
                    reports land in ASAN_LOG_DIR, default /var/log/dpumesh-asan)
Sweep knobs (env): OUT LAT_DUR BW_DUR RATE_DUR WARMUP BW_CONC RATE_CONC RATE_THREADS LAT_SIZES BW_SIZES
EOF
        ;;
esac
