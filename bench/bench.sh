#!/bin/bash
# bench.sh — the single entry point for the DPUmesh benchmark.
#
# One script for everything bench: build + deploy the environment (DPU transport,
# container images, k8s pods, CPU pinning), then drive the benchmark and the
# feature validators. The heavy k8s manifest lives declaratively in
# bench/k8s/pods.yaml (applied via envsubst); the host build lives in the Makefile
# (this script calls `make`).
#
#   bench.sh deploy                                     # build + DPU + images + pods + pin
#   bench.sh latency   [dpumesh|tcp|both]               # concurrency=1 latency vs req_size
#   bench.sh bandwidth [dpumesh|tcp|both]               # Gb/s goodput vs req_size (to 8 MB)
#   bench.sh rate      [dpumesh|tcp|both]               # Mrps vs client threads {1,2,4,8}
#   bench.sh all       [dpumesh|tcp|both]               # all three (default both)
#   bench.sh point <dpumesh|tcp> <req> <reply> <conc> <dur> <warmup> <threads>
#   bench.sh loopback  [<N> <size> <zc>]                # validator: self-routing
#   bench.sh stream    [<N> <size> <svcs> <fpw>]        # validator: L7 frame proxy (DPUMESH_PROXY=frame)
#   bench.sh preload   [<N> <size> <conns>]             # validator: LD_PRELOAD shim
#   bench.sh pin       [fair|hw|hw3|hw6]                # (re)pin pods to cores
#   bench.sh status | logs | cleanup | dpulog [n] | dpucpu
#
# Requires .env at the repo root (DPU_HOST, HOST_PASS, DPU_PASS, HOST_PCI, DPU_PCI,
# …) for deploy/pin. Benchmark/validator runs need only kubectl + nc.
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

INCLUDE_SRC="$PROJ_ROOT/include"
DOCA_SRC="$PROJ_ROOT/doca"
LIB_OUT="$PROJ_ROOT/build/lib"
BIN_OUT="$PROJ_ROOT/build/bin"
DPU_PROJ="${DPU_PROJ:-DPUmesh}"        # repo checkout name on the DPU
DPU_DOCA="$DPU_PROJ/doca"
DPU_INCLUDE="$DPU_PROJ/include"
DPU_BUILD="$DPU_DOCA/build"
DPU_LOG="/tmp/dpumesh_dpu_bench.log"
DOCA_LIB_DIR="/opt/mellanox/doca/lib/x86_64-linux-gnu"
FLEXIO_LIB_DIR="/opt/mellanox/flexio/lib"

IMG_BENCH_DPU="bench/bench-dpumesh:latest"
IMG_ECHO_DPU="bench/echo-dpumesh:latest"
IMG_LOOPBACK_DPU="bench/loopback-dpumesh:latest"
IMG_PRELOAD_DPU="bench/preload-dpumesh:latest"
IMG_STREAM_DPU="bench/stream-dpumesh:latest"
IMG_BENCH_TCP="bench/bench-tcp:latest"
IMG_ECHO_TCP="bench/echo-tcp:latest"
IMG_ENVOY="envoyproxy/envoy:v1.30-latest"

# benchmark sweep knobs
OUT="${OUT:-/tmp/dpumesh-bench}"
LAT_DUR="${LAT_DUR:-10}"; BW_DUR="${BW_DUR:-10}"; RATE_DUR="${RATE_DUR:-10}"
WARMUP="${WARMUP:-1000}"; BW_CONC="${BW_CONC:-32}"; RATE_CONC="${RATE_CONC:-32}"
RATE_THREADS="${RATE_THREADS:-1 2 4 8}"
LAT_SIZES="${LAT_SIZES:-64 128 256 512 1024}"
BW_SIZES="${BW_SIZES:-32 128 512 2048 8192 32768 131072 524288 1000000 2097152 8000000}"

need_env() { : "${DPU_HOST:?.env missing DPU_HOST}" "${HOST_PASS:?.env missing HOST_PASS}" \
                "${DPU_PASS:?.env missing DPU_PASS}" "${HOST_PCI:?.env missing HOST_PCI}" \
                "${DPU_PCI:?.env missing DPU_PCI}"; }

dpu_sudo() {
    ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S bash -c '$1'" 2>&1 | sed 's/^\[sudo\][^:]*: *//'
}

### ------------------------------------------------------------ build
sync_sources() {
    step "=== Syncing sources to DPU ($DPU_HOST:~/$DPU_PROJ) ==="
    ssh "$DPU_HOST" "mkdir -p ~/$DPU_DOCA ~/$DPU_INCLUDE"
    rsync -avz --delete --exclude='build/' --exclude='builddir/' --exclude='*.o' --exclude='*.a' \
        "$DOCA_SRC/" "$DPU_HOST:~/$DPU_DOCA/"
    rsync -avz --delete "$INCLUDE_SRC/" "$DPU_HOST:~/$DPU_INCLUDE/"
    ssh "$DPU_HOST" "find ~/$DPU_DOCA ~/$DPU_INCLUDE -type f -exec touch {} +" 2>/dev/null || true
    info "Source sync complete"
}

build_dpu() {
    step "=== Building on DPU (ninja) ==="
    local bt="${DPU_BUILDTYPE:-debugoptimized}"
    ssh "$DPU_HOST" "[ -d ~/$DPU_BUILD ] || (cd ~/$DPU_DOCA && meson setup build --buildtype=$bt)" 2>&1 | grep -vE '^\s*$' || true
    ssh "$DPU_HOST" "rm -f ~/$DPU_BUILD/dpa_kernel.a" 2>/dev/null || true
    local out
    out=$(ssh "$DPU_HOST" "cd ~/$DPU_BUILD && meson configure -Dbuildtype=$bt 2>&1; ninja 2>&1" 2>&1)
    if echo "$out" | grep -q "error:"; then err "DPU build failed:"; echo "$out"; exit 1; fi
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
    step "=== Building bench/validator binaries (make bench go) ==="
    if ! make -C "$PROJ_ROOT" bench go 2>&1 | tail -30; then err "Bench build failed"; exit 1; fi
    command -v go >/dev/null 2>&1 || { err "go not found (needed for bench_tcp/echo_tcp)"; exit 1; }
    info "Bench binaries built ($BIN_OUT + $LIB_OUT/libdmesh_preload.so)"
}

### ------------------------------------------------------------ container images
build_image() {  # $1 = Dockerfile, $2 = tag, $3 = build context
    docker build -f "$1" -t "$2" "$3"
    sudo ctr -n k8s.io images rm "docker.io/$2" 2>/dev/null || true
    docker save "$2" | sudo ctr -n k8s.io images import -
    docker image prune -f >/dev/null 2>&1 || true
}

build_images() {
    step "=== Building Docker images ==="
    collect_doca_libs
    echo "$HOST_PASS" | sudo -S true 2>/dev/null
    build_image "$BENCH_DIR/docker/bench_dpumesh.Dockerfile"        "$IMG_BENCH_DPU"    "$PROJ_ROOT"
    build_image "$BENCH_DIR/docker/echo_dpumesh.Dockerfile"         "$IMG_ECHO_DPU"     "$PROJ_ROOT"
    build_image "$BENCH_DIR/validators/loopback_dpumesh.Dockerfile" "$IMG_LOOPBACK_DPU" "$PROJ_ROOT"
    build_image "$BENCH_DIR/validators/stream_dpumesh.Dockerfile"   "$IMG_STREAM_DPU"   "$PROJ_ROOT"
    build_image "$BENCH_DIR/validators/preload_dpumesh.Dockerfile"  "$IMG_PRELOAD_DPU"  "$PROJ_ROOT"
    build_image "$BENCH_DIR/docker/bench_tcp.Dockerfile"            "$IMG_BENCH_TCP"    "$PROJ_ROOT"
    build_image "$BENCH_DIR/docker/echo_tcp.Dockerfile"             "$IMG_ECHO_TCP"     "$PROJ_ROOT"
    docker builder prune -f >/dev/null 2>&1 || true
    info "All images built and imported to containerd"
}

ensure_envoy_image() {
    local img="$IMG_ENVOY"
    if echo "$HOST_PASS" | sudo -S ctr -n k8s.io images list -q 2>/dev/null | grep -v '^\[sudo\]' | grep -q "docker.io/$img"; then
        info "Envoy image already in containerd"; return 0
    fi
    if ! docker image inspect "$img" >/dev/null 2>&1; then
        info "Pulling Envoy image: $img"
        docker pull "$img" || { err "docker pull $img failed"; exit 1; }
    fi
    info "Importing Envoy image to containerd k8s.io ns..."
    echo "$HOST_PASS" | sudo -S true 2>/dev/null
    docker save "$img" | sudo ctr -n k8s.io images import -
}

### ------------------------------------------------------------ DPU process
stop_dpu() {
    info "Stopping dpumesh_dpu..."
    ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S killall -9 dpumesh_dpu 2>/dev/null; true" 2>&1 | sed 's/^\[sudo\][^:]*: *//' || true
    sleep 5
}

start_dpu() {
    local log_level="${DPUMESH_LOG_LEVEL:-40}"
    local proxy="${DPUMESH_PROXY:-}" frame_svc="${DPUMESH_PROXY_FRAME_SVC:-}" l7_svc="${DPUMESH_PROXY_L7_SVC:-}"
    local arm_threads="${DPUMESH_ARM_EGRESS_THREADS:-}" rings="${DPUMESH_RINGS_PER_POD:-}"
    step "=== Starting dpumesh_dpu (proxy='$proxy' frame_svc='$frame_svc' l7_svc='$l7_svc' arm_egress_threads='$arm_threads' rings_per_pod='$rings') ==="
    stop_dpu
    local dpu_home; dpu_home=$(ssh "$DPU_HOST" 'echo $HOME')
    ssh "$DPU_HOST" "cat > /tmp/start_dpu_bench.sh << 'LAUNCHER'
#!/bin/bash
screen -dmS dpumesh-bench bash -c \"cd $dpu_home/$DPU_BUILD && DPUMESH_PROXY=$proxy DPUMESH_PROXY_FRAME_SVC=$frame_svc DPUMESH_PROXY_L7_SVC=$l7_svc DPUMESH_ARM_EGRESS_THREADS=$arm_threads DPUMESH_RINGS_PER_POD=$rings ./dpumesh_dpu $DPU_PCI -l $log_level > $DPU_LOG 2>&1\"
sleep 2
pgrep -f 'dpumesh_dpu.*03:00' || echo NO_PID
LAUNCHER
chmod +x /tmp/start_dpu_bench.sh"
    local pid; pid=$(ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S bash /tmp/start_dpu_bench.sh" 2>&1 | sed 's/^\[sudo\][^:]*: *//')
    if [ "$pid" = "NO_PID" ] || [ -z "$pid" ]; then err "dpumesh_dpu failed to start"; exit 1; fi
    info "dpumesh_dpu running (PID: $pid)"
}

### ------------------------------------------------------------ CPU pinning
# fair (default): 1 host core per pod — the apples-to-apples 1-core comparison
# (dpumesh app gets a full core since transport is on the DPU; tcp app shares its
# core with its sidecar). hw/hw3/hw6: multi-core for the dpumesh side only, to
# chase the transport ceiling (not comparable to TCP).
get_pod_cores() {
    local app="$1" profile="${2:-fair}"
    case "$profile" in
        hw)  case "$app" in bench-dpumesh) echo "0,4";; echo-dpumesh) echo "1,5";; bench-tcp) echo "2";; echo-tcp) echo "3";; *) echo "";; esac ;;
        hw3) case "$app" in bench-dpumesh) echo "0,4,6";; echo-dpumesh) echo "1,5,7";; bench-tcp) echo "2";; echo-tcp) echo "3";; *) echo "";; esac ;;
        hw6) case "$app" in bench-dpumesh) echo "0,4,6,8,10,12";; echo-dpumesh) echo "1,5,7,9,11,13";; bench-tcp) echo "2";; echo-tcp) echo "3";; *) echo "";; esac ;;
        fair|*)
            case "$app" in
                bench-dpumesh) echo "0";; echo-dpumesh) echo "1";;
                bench-tcp) echo "2";; echo-tcp) echo "3";;
                loopback-dpumesh) echo "4,5";; preload-dpumesh) echo "4,5";; stream-dpumesh) echo "4,5";;
                echo-dpumesh-13) echo "6";; echo-dpumesh-14) echo "7";; *) echo "";;
            esac ;;
    esac
}

pin_pods() {
    local profile="${1:-fair}"
    step "=== Pinning pods to dedicated cores (taskset, profile=$profile) ==="
    command -v jq >/dev/null 2>&1 || { err "jq not found (apt install jq)"; return 1; }
    if command -v cpupower >/dev/null 2>&1; then
        info "CPU governor=performance, fixed 2.5 GHz on cores 0-7"
        echo "$HOST_PASS" | sudo -S cpupower -c 0-7 frequency-set -g performance >/dev/null 2>&1 || true
        echo "$HOST_PASS" | sudo -S cpupower -c 0-7 frequency-set -d 2.5GHz -u 2.5GHz >/dev/null 2>&1 || true
    else
        warn "cpupower not found; skipping DVFS lock"
    fi
    for app in bench-dpumesh echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 loopback-dpumesh stream-dpumesh preload-dpumesh bench-tcp echo-tcp; do
        local cores pod_id; cores=$(get_pod_cores "$app" "$profile"); [ -z "$cores" ] && continue
        pod_id=$(echo "$HOST_PASS" | sudo -S crictl pods --label "app=$app" -q 2>/dev/null | head -n 1)
        if [ -z "$pod_id" ]; then warn "$app: pod not found, skipping"; continue; fi
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

clean_failed_pods() {
    local n; n=$(kubectl get pods -n "$NS" --field-selector=status.phase=Failed --no-headers 2>/dev/null | wc -l)
    if [ "$n" -gt 0 ]; then
        info "Removing $n stale Failed/Evicted pod(s) in $NS"
        kubectl delete pod -n "$NS" --field-selector=status.phase=Failed --ignore-not-found=true >/dev/null 2>&1 || true
    fi
}

# Render bench/k8s/pods.yaml with envsubst and apply it (replicas: 0).
apply_manifest() {
    step "=== Applying K8s manifest (replicas=0) ==="
    command -v envsubst >/dev/null 2>&1 || { err "envsubst not found (apt install gettext-base)"; exit 1; }
    export IMG_BENCH_DPU IMG_ECHO_DPU IMG_LOOPBACK_DPU IMG_STREAM_DPU IMG_PRELOAD_DPU IMG_BENCH_TCP IMG_ECHO_TCP IMG_ENVOY
    export CTRL_PORT TCP_PORT HOST_PCI LIB_OUT
    export DPUMESH_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-2}" ASYNC_THREADS="${ASYNC_THREADS:-4}" \
           BENCH_PIPELINE="${BENCH_PIPELINE:-8}" BENCH_COALESCE="${BENCH_COALESCE:-0}" \
           ECHO_THREADS="${ECHO_THREADS:-3}" DPUMESH_ARENA_SLOTS="${DPUMESH_ARENA_SLOTS:-512}" \
           DMESH_PRELOAD_DEBUG="${DMESH_PRELOAD_DEBUG:-0}"
    envsubst < "$MANIFEST" | kubectl apply -n "$NS" -f -
    info "K8s resources applied"
}

scale_up_with_wait() {
    local app="$1" expected_log="$2"
    kubectl scale deployment "$app" --replicas=0 -n "$NS" 2>/dev/null || true
    sleep 1
    kubectl scale deployment "$app" --replicas=1 -n "$NS"
    if ! kubectl wait --for=condition=Ready pod -l "app=$app" -n "$NS" --timeout=120s 2>&1; then
        err "$app failed to start"; kubectl describe pod -l "app=$app" -n "$NS" | tail -15; exit 1
    fi
    info "$app pod Ready"
    if [ -n "$expected_log" ]; then
        info "Waiting for DPU register: $expected_log"
        local attempts=0
        while [ $attempts -lt 15 ]; do
            local line; line=$(ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S tail -3 $DPU_LOG" 2>&1 | sed 's/^\[sudo\][^:]*: *//' || true)
            if echo "$line" | grep -q "$expected_log"; then info "DPU registered ($expected_log)"; return 0; fi
            sleep 1; attempts=$((attempts+1))
        done
        warn "DPU register timeout — continuing"
    fi
}

start_pods() {
    step "=== Starting pods (innermost first) ==="
    scale_up_with_wait "echo-dpumesh"     "pods: 1"
    scale_up_with_wait "echo-dpumesh-13"  ""
    scale_up_with_wait "echo-dpumesh-14"  ""
    scale_up_with_wait "bench-dpumesh"    "pods: 2"
    scale_up_with_wait "loopback-dpumesh" "pods: 3"
    scale_up_with_wait "preload-dpumesh"  "pods: 4"
    scale_up_with_wait "echo-tcp"  ""
    scale_up_with_wait "bench-tcp" ""
}

deploy() {
    need_env
    ensure_namespace
    clean_failed_pods
    apply_manifest
    sync_sources
    build_dpu
    build_host
    build_bench_binaries
    build_images
    ensure_envoy_image
    start_dpu
    start_pods
    pin_pods fair
    info "=== Deploy complete ==="
    echo "  Run:  $0 latency|bandwidth|rate|all [dpumesh|tcp|both]"
    echo "        $0 loopback|stream|preload ...   (validators)"
    echo "  Re-pin after a pod restart:  $0 pin [fair|hw|hw3|hw6]"
}

cleanup() { info "Deleting namespace $NS"; kubectl delete ns "$NS" --ignore-not-found=true 2>/dev/null || true; stop_dpu; }

show_logs() {
    for app in bench-dpumesh echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 loopback-dpumesh stream-dpumesh preload-dpumesh bench-tcp echo-tcp; do
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

### ------------------------------------------------------------ benchmark (RUN)
app_of()     { case "$1" in dpumesh) echo bench-dpumesh;; tcp) echo bench-tcp;; *) echo "";; esac; }
targets_of() { case "${1:-both}" in both|"") echo "dpumesh tcp";; *) echo "$1";; esac; }
field()      { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }

# run_point <sol> <req> <reply> <conc> <dur> <warmup> <threads> -> echoes the OK line
run_point() {
    local app ip to reply
    app="$(app_of "$1")"; [ -z "$app" ] && { echo "ERR bad_target($1)"; return 0; }
    ip=$(kubectl get pod -n "$NS" -l "app=$app" --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}' 2>/dev/null || true)
    [ -z "$ip" ] && { echo "ERR no_pod($app)"; return 0; }
    to=$(( ${5%.*} + 90 ))
    reply=$(printf 'RUN %s %s %s %s %s %s\n' "$2" "$3" "$4" "$5" "$6" "$7" | timeout "${to}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null) || reply="ERR nc"
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
    ip=$(kubectl get pod -n "$NS" -l app=loopback-dpumesh --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}' 2>/dev/null || true)
    [ -z "$ip" ] && { err "loopback-dpumesh pod not found — run '$0 deploy'"; return 1; }
    step "=== loopback (self-service): N=$N size=${size}B zerocopy=$zc ==="
    resp=$(printf 'RUN %s %s %s\n' "$N" "$size" "$zc" | timeout 120s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "loopback replied: $resp"; return 1; }
    read -r _ ok fail served p50 <<<"$resp"
    printf "  OK/Fail: %s/%s  served: %s  p50: %s us\n" "$ok" "$fail" "$served" "$p50"
}

run_preload() {  # LD_PRELOAD shim: vanilla TCP apps over DPUmesh
    local N="${1:-5000}" size="${2:-1024}" conns="${3:-8}" ip resp
    ip=$(kubectl get pod -n "$NS" -l app=preload-dpumesh --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}' 2>/dev/null || true)
    [ -z "$ip" ] && { err "preload-dpumesh pod not found — run '$0 deploy'"; return 1; }
    step "=== preload (LD_PRELOAD shim): N=$N size=${size}B conns=$conns ==="
    resp=$(printf 'RUN %s %s %s\n' "$N" "$size" "$conns" | timeout 620s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "preload replied: $resp"; return 1; }
    read -r _ ok fail p50 p99 rps <<<"$resp"
    printf "  OK/Fail: %s/%s  p50: %s us  p99: %s us  rps: %s\n" "$ok" "$fail" "$p50" "$p99" "${rps:-n/a}"
}

run_stream() {  # L7-proxy frame validator (needs DPUMESH_PROXY=frame)
    local N="${1:-20000}" size="${2:-1024}" svcs="${3:-self}" fpw="${4:-1}" have ip resp
    have=$(kubectl get pod -n "$NS" -l app=stream-dpumesh --field-selector=status.phase=Running -o name 2>/dev/null | head -1 || true)
    [ -z "$have" ] && scale_up_with_wait "stream-dpumesh" "" || info "reusing running stream-dpumesh pod (no restart)"
    ip=$(kubectl get pod -n "$NS" -l app=stream-dpumesh --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}' 2>/dev/null || true)
    [ -z "$ip" ] && { err "stream-dpumesh pod not found"; return 1; }
    step "=== stream (L7-proxy frame): N=$N size=${size}B svcs='$svcs' frames/write=$fpw ==="
    resp=$(printf 'RUN %s %s %s %s\n' "$N" "$size" "$svcs" "$fpw" | timeout 180s nc "$ip" "$CTRL_PORT" || true)
    [ -z "$resp" ] && { err "no response (timeout or pod down)"; return 1; }
    [[ "$resp" == ERR* ]] && { err "stream replied: $resp"; return 1; }
    read -r _ ok fail served p50 <<<"$resp"
    printf "  OK/Fail: %s/%s  served_bytes: %s  p50: %s us\n" "$ok" "$fail" "$served" "$p50"
}

### ------------------------------------------------------------ dispatch
CMD="${1:-help}"; shift || true
case "$CMD" in
    deploy)    deploy ;;
    latency)   for s in $(targets_of "${1:-both}"); do bench_latency   "$s"; done ;;
    bandwidth) for s in $(targets_of "${1:-both}"); do bench_bandwidth "$s"; done ;;
    rate)      for s in $(targets_of "${1:-both}"); do bench_rate      "$s"; done ;;
    all)       for s in $(targets_of "${1:-both}"); do bench_latency "$s"; bench_bandwidth "$s"; bench_rate "$s"; done; info "results under $OUT" ;;
    point)     [ $# -eq 7 ] || { err "point <sol> <req> <reply> <conc> <dur> <warmup> <threads>"; exit 1; }; run_point "$@" ;;
    loopback)  run_loopback "${1:-50000}" "${2:-8192}" "${3:-0}" ;;
    stream)    run_stream   "${1:-20000}" "${2:-1024}" "${3:-self}" "${4:-1}" ;;
    preload)   run_preload  "${1:-5000}"  "${2:-1024}" "${3:-8}" ;;
    pin)       need_env; pin_pods "${1:-fair}" ;;
    status)    show_status ;;
    logs)      show_logs ;;
    cleanup)   cleanup ;;
    dpulog)    ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S tail -${1:-40} $DPU_LOG" 2>&1 | sed 's/^\[sudo\][^:]*: *//' ;;
    dpucpu)    dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -z "$pid" ] && { echo "dpumesh_dpu not running"; exit 0; }; echo "=== dpumesh_dpu pid=$pid per-thread %CPU ==="; top -bH -d 1 -n 2 -p "$pid" | awk "/ PID +USER/{n++} n==2{print}"' ;;
    *)
        cat <<EOF
Usage: $0 <command> [args]

  deploy                                     build + DPU + images + pods + pin
  latency|bandwidth|rate|all [dpumesh|tcp|both]   benchmark families -> CSVs under $OUT
  point <sol> <req> <reply> <conc> <dur> <warmup> <threads>   one raw RUN
  loopback|stream|preload [args]             feature validators
  pin [fair|hw|hw3|hw6]                       (re)pin pods to cores
  status | logs | cleanup | dpulog [n] | dpucpu

Sweep knobs (env): OUT LAT_DUR BW_DUR RATE_DUR WARMUP BW_CONC RATE_CONC RATE_THREADS LAT_SIZES BW_SIZES
EOF
        ;;
esac
