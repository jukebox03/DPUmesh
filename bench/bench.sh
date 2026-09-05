#!/bin/bash
# Build, restart, inspect, and measure the current DPU runtime.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${GREEN}[INFO]${NC} $*" >&2; }
err() { echo -e "${RED}[ERR]${NC} $*" >&2; }
step() { echo -e "${BLUE}[STEP]${NC} $*" >&2; }

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
if [[ -v DPUMESH_ENV_FILE ]]; then
    [ -n "$DPUMESH_ENV_FILE" ] || { err "DPUMESH_ENV_FILE must not be empty"; exit 1; }
    ENV_FILE="$DPUMESH_ENV_FILE"
    ENV_REQUIRED=1
else
    ENV_FILE="$PROJ_ROOT/.env"
    ENV_REQUIRED=0
fi
if [ -e "$ENV_FILE" ] || [ -L "$ENV_FILE" ]; then
    [ -f "$ENV_FILE" ] && [ -r "$ENV_FILE" ] || {
        err "configuration file is not a readable regular file: $ENV_FILE"
        exit 1
    }
    set -a
    source "$ENV_FILE"
    set +a
elif [ "$ENV_REQUIRED" -eq 1 ]; then
    err "DPUMESH_ENV_FILE does not exist: $ENV_FILE"
    exit 1
fi

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
OUT="${OUT:-/tmp/dpumesh-bench}"
DPU_PROJ="${DPU_PROJ:-DPUmesh}"
DPU_BUILD="$DPU_PROJ/doca/build"
DPU_LOG="/tmp/dpumesh_dpu_bench.log"
LINKERD_BUILD="${DPU_L7_BUILD:-l7build}"
LINKERD_TOOLCHAIN="${LINKERD_TOOLCHAIN:-1.90.0}"
LINKERD_CARGO="${LINKERD_CARGO:-\$HOME/.cargo/bin/cargo}"

DPUMESH_THROUGHPUT_WORKERS="${DPUMESH_THROUGHPUT_WORKERS:-}"
if [ -n "$DPUMESH_THROUGHPUT_WORKERS" ]; then
    case "$DPUMESH_THROUGHPUT_WORKERS" in
        4|6|8|12) ;;
        *) err "DPUMESH_THROUGHPUT_WORKERS must be 4, 6, 8, or 12"; exit 2 ;;
    esac
    DPUMESH_DPA_THREADS=$((32 / DPUMESH_THROUGHPUT_WORKERS * DPUMESH_THROUGHPUT_WORKERS))
    DPUMESH_RINGS_PER_POD="$DPUMESH_THROUGHPUT_WORKERS"
    DPUMESH_ARM_WORKERS="$DPUMESH_THROUGHPUT_WORKERS"
    DPUMESH_L7_LINKERD_WORKER=all
else
    DPUMESH_DPA_THREADS="${DPUMESH_DPA_THREADS:-32}"
    DPUMESH_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-8}"
    DPUMESH_ARM_WORKERS="${DPUMESH_ARM_WORKERS:-8}"
    DPUMESH_L7_LINKERD_WORKER="${DPUMESH_L7_LINKERD_WORKER:-all}"
fi
export DPUMESH_DPA_THREADS DPUMESH_RINGS_PER_POD DPUMESH_ARM_WORKERS
export DPUMESH_L7_LINKERD_WORKER DPUMESH_THROUGHPUT_WORKERS

LAT_DUR="${LAT_DUR:-10}"; BW_DUR="${BW_DUR:-10}"; RATE_DUR="${RATE_DUR:-10}"
WARMUP="${WARMUP:-1000}"; BW_CONC="${BW_CONC:-32}"; RATE_CONC="${RATE_CONC:-32}"
RATE_THREADS="${RATE_THREADS:-1 2 4 8}"
LAT_SIZES="${LAT_SIZES:-64 128 256 512 1024}"
BW_SIZES="${BW_SIZES:-32 128 512 2048 8192 32768 131072 524288 1000000 2097152 8000000}"

need_rig() {
    : "${DPU_HOST:?.env missing DPU_HOST}" "${DPU_PASS:?.env missing DPU_PASS}" \
      "${DPU_PCI:?.env missing DPU_PCI}"
}

SSH_OPTS=(-o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o ConnectTimeout=10 -o BatchMode=yes)
ssh_dpu() {
    local attempt status
    for attempt in 1 2 3; do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" "$@" && return 0
        status=$?
        [ "$status" -eq 255 ] && [ "$attempt" -lt 3 ] || return "$status"
        sleep $((attempt * 2))
    done
}
dpu_sudo() {
    ssh_dpu "echo '$DPU_PASS' | sudo -S -p '' bash -c '$1'" 2>&1
}

dpu_home() { ssh_dpu 'printf %s "$HOME"'; }
linkerd_dir() { printf '%s/%s' "$(dpu_home)" "$LINKERD_BUILD"; }
linkerd_lib() { printf '%s/rust/target/release/libdmesh_l7.a' "$(linkerd_dir)"; }

sync_sources() {
    local destination
    destination=$(linkerd_dir)
    step "Syncing DPU sources"
    ssh_dpu "mkdir -p ~/$DPU_PROJ/doca ~/$DPU_PROJ/include ~/$DPU_PROJ/linkerd/include '$destination/rust' '$destination/port/linkerd2-proxy'"
    local excludes=(--exclude='.git' --exclude='target/' --exclude='build/' --exclude='builddir/' --exclude='*.o' --exclude='*.a')
    rsync -az --delete --timeout=120 -e "ssh ${SSH_OPTS[*]}" "${excludes[@]}" \
        "$PROJ_ROOT/doca/" "$DPU_HOST:~/$DPU_PROJ/doca/"
    rsync -az --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$PROJ_ROOT/include/" "$DPU_HOST:~/$DPU_PROJ/include/"
    rsync -az --delete --timeout=60 -e "ssh ${SSH_OPTS[*]}" \
        "$PROJ_ROOT/linkerd/include/" "$DPU_HOST:~/$DPU_PROJ/linkerd/include/"
    rsync -az --delete --timeout=120 -e "ssh ${SSH_OPTS[*]}" "${excludes[@]}" \
        "$PROJ_ROOT/linkerd/rust/" "$DPU_HOST:$destination/rust/"
    rsync -az --delete --timeout=300 -e "ssh ${SSH_OPTS[*]}" "${excludes[@]}" \
        "$PROJ_ROOT/linkerd/port/linkerd2-proxy/" "$DPU_HOST:$destination/port/linkerd2-proxy/"
}

build_dpu() {
    local destination cargo library buildtype output
    destination=$(linkerd_dir)
    cargo="$LINKERD_CARGO +$LINKERD_TOOLCHAIN"
    step "Building DPU runtime"
    ssh_dpu "test -f '$destination/rust/Cargo.lock'" || {
        err "missing linkerd/rust/Cargo.lock"
        exit 1
    }
    ssh_dpu "cd '$destination/rust' && $cargo build --release --locked"
    library=$(linkerd_lib)
    for symbol in l7_worker_run l7_conn_open l7_conn_segment l7_conn_eof l7_conn_close; do
        ssh_dpu "nm -g --defined-only '$library' | awk '{print \$3}' | grep -Fx '$symbol' >/dev/null" || {
            err "$library does not export $symbol"
            exit 1
        }
    done
    buildtype="${DPU_BUILDTYPE:-debugoptimized}"
    ssh_dpu "[ -d ~/$DPU_BUILD ] || (cd ~/$DPU_PROJ/doca && meson setup build --buildtype='$buildtype')"
    ssh_dpu "rm -f ~/$DPU_BUILD/dpa_kernel.a"
    output=$(ssh_dpu "cd ~/$DPU_BUILD && meson configure -Dbuildtype='$buildtype' -Dwarning_level=2 -Dl7_lib_path='$library' && ninja") || {
        printf '%s\n' "$output" >&2
        exit 1
    }
    info "DPU build complete"
}

stop_dpu() {
    ssh_dpu "echo '$DPU_PASS' | sudo -S -p '' bash -c \"pids=\\\$(pgrep -f '[d]pumesh_dpu'); [ -z \\\"\\\$pids\\\" ] || kill -9 \\\$pids\"" >/dev/null 2>&1 || true
    sleep 5
}

start_dpu() {
    local home node_name log_level cluster_id relay_bind
    [[ "$DPU_PCI" =~ ^-p[[:space:]][0-9A-Fa-f:.]+[[:space:]]+-r[[:space:]][0-9A-Fa-f:.]+$ ]] || {
        err "DPU_PCI must be '-p PCI_ADDRESS -r REPRESENTOR_ADDRESS'"
        exit 2
    }
    home=$(dpu_home)
    node_name="${DPUMESH_NODE_NAME:-$(kubectl get nodes -o jsonpath='{.items[0].metadata.name}')}"
    [ -n "$node_name" ] || { err "cannot resolve Kubernetes node name"; exit 1; }
    log_level="${DPUMESH_LOG_LEVEL:-40}"
    cluster_id="${DPUMESH_CLUSTER_ID:-dpumesh-test}"
    relay_bind="${LINKERD_GATEWAY_BIND:-192.168.100.1}"
    stop_dpu
    step "Starting DPU runtime"
    ssh_dpu "cat >/tmp/start-dpumesh.sh <<'LAUNCH'
#!/bin/bash
ulimit -c unlimited
screen -dmS dpumesh bash -c \"cd '$home/$DPU_BUILD' && \
DPUMESH_CLUSTER_ID='$cluster_id' \
DPUMESH_NODE_NAME='$node_name' \
DPUMESH_REGISTRATION_KEY_DIR='${DPUMESH_REGISTRATION_KEY_DIR_DPU:-/etc/dpumesh/registration.keys}' \
DPUMESH_FEED_KEY_DIR='${DPUMESH_FEED_KEY_DIR_DPU:-/etc/dpumesh/feed.keys}' \
DPUMESH_MEMBERSHIP_FILE='${DPUMESH_MEMBERSHIP_FILE:-/etc/dpumesh/feeds/membership.v1}' \
DPUMESH_TOPOLOGY_FILE='${DPUMESH_TOPOLOGY_FILE:-/etc/dpumesh/feeds/topology.v1}' \
DPUMESH_CONTROLLER_KEY_DIR='${DPUMESH_CONTROLLER_KEY_DIR_DPU:-/etc/dpumesh/controller.pub.keys}' \
DPUMESH_NODE_KEY_FILE='${DPUMESH_NODE_KEY_FILE:-/etc/dpumesh/node-static.key}' \
DPUMESH_NODE_KEY_PUBLIC_FILE='${DPUMESH_NODE_KEY_PUBLIC_FILE:-/etc/dpumesh/node-static.pub}' \
DPUMESH_CONTROLLER_SCOPE_URL='${DPUMESH_CONTROLLER_SCOPE_URL:-http://$relay_bind:28089}' \
DPUMESH_IDENTITY_TRUST_DOMAIN='${DPUMESH_IDENTITY_TRUST_DOMAIN:-linkerd.cluster.local}' \
DPUMESH_L7_OPAQUE_SVC= DPUMESH_L7_SVC= \
DPUMESH_L7_LINKERD_WORKER='$DPUMESH_L7_LINKERD_WORKER' \
DPUMESH_DPA_THREADS='$DPUMESH_DPA_THREADS' \
DPUMESH_RINGS_PER_POD='$DPUMESH_RINGS_PER_POD' \
DPUMESH_ARM_WORKERS='$DPUMESH_ARM_WORKERS' \
DPUMESH_PEER_TRANSPORT='${DPUMESH_PEER_TRANSPORT:-}' \
DPUMESH_PEER_BIND='${DPUMESH_PEER_BIND:-}' \
DPUMESH_PEER_PORT='${DPUMESH_PEER_PORT:-47900}' \
./dpumesh_dpu $DPU_PCI -l '$log_level' >'$DPU_LOG' 2>&1\"
sleep 2
pgrep -x dpumesh_dpu | head -1
LAUNCH
chmod +x /tmp/start-dpumesh.sh
echo '$DPU_PASS' | sudo -S -p '' /tmp/start-dpumesh.sh" >/dev/null
    info "DPU runtime started"
}

running_client_ip() {
    kubectl get pod -n "$NS" -l app=bench-dpumesh-native \
        -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' 2>/dev/null | head -1
}
field() { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }
run_point() {
    local ip timeout_s reply
    ip=$(running_client_ip)
    [ -n "$ip" ] || { echo "ERR no_pod(bench-dpumesh-native)"; return; }
    timeout_s=$((${4%.*} + 90))
    reply=$(printf 'RUN %s %s %s %s %s %s %s\n' "$1" "$2" "$3" "$4" "$5" "$6" "${7:-}" |
        timeout "${timeout_s}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null) || reply="ERR nc"
    printf '%s\n' "$reply"
}
run_ping() {
    local ip reply status=0
    ip=$(running_client_ip)
    [ -n "$ip" ] || { echo "ERR no_pod(bench-dpumesh-native)"; return; }
    reply=$(printf 'PING\n' | timeout "${PING_TIMEOUT:-2}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null) || status=$?
    [ "$reply" = PONG ] && { echo "OK pong"; return; }
    [ "$status" -eq 124 ] && { echo "ERR silent"; return; }
    echo "ERR refused"
}

benchmark() {
    local kind="$1" csv size result threads warmup
    mkdir -p "$OUT"
    csv="$OUT/${kind}_native.csv"
    case "$kind" in
        latency)
            echo "solution,req_size,p50_us,p95_us,p99_us,avg_us" >"$csv"
            for size in $LAT_SIZES; do
                result=$(run_point "$size" 8 1 "$LAT_DUR" "$WARMUP" 1)
                [[ "$result" == OK* ]] && echo "native,$size,$(field "$result" p50),$(field "$result" p95),$(field "$result" p99),$(field "$result" avg)" >>"$csv"
            done ;;
        bandwidth)
            echo "solution,req_size,gbps,mrps,p50_us" >"$csv"
            for size in $BW_SIZES; do
                warmup="$WARMUP"; [ "$size" -ge 262144 ] && warmup=100
                result=$(run_point "$size" 8 "$BW_CONC" "$BW_DUR" "$warmup" 1)
                [[ "$result" == OK* ]] && echo "native,$size,$(field "$result" gbps),$(field "$result" mrps),$(field "$result" p50)" >>"$csv"
            done ;;
        rate)
            echo "solution,threads,mrps,gbps,p50_us,p99_us" >"$csv"
            for threads in $RATE_THREADS; do
                result=$(run_point 32 8 "$RATE_CONC" "$RATE_DUR" "$WARMUP" "$threads")
                [[ "$result" == OK* ]] && echo "native,$threads,$(field "$result" mrps),$(field "$result" gbps),$(field "$result" p50),$(field "$result" p99)" >>"$csv"
            done ;;
    esac
    info "$csv"
}

CMD="${1:-help}"; shift || true
case "$CMD" in
    geometry) printf 'throughput_workers=%s N=%s K=%s A=%s l7_workers=%s\n' \
        "${DPUMESH_THROUGHPUT_WORKERS:-custom}" "$DPUMESH_DPA_THREADS" \
        "$DPUMESH_RINGS_PER_POD" "$DPUMESH_ARM_WORKERS" "$DPUMESH_L7_LINKERD_WORKER" ;;
    build) need_rig; sync_sources; build_dpu ;;
    restart) need_rig; start_dpu ;;
    point) [ $# -eq 6 ] || [ $# -eq 7 ] || { err "point REQ REPLY CONC DUR WARMUP THREADS [RECONNECT]"; exit 2; }; run_point "$@" ;;
    ping) [ $# -eq 0 ] || { err "ping takes no arguments"; exit 2; }; run_ping ;;
    latency|bandwidth|rate) benchmark "$CMD" ;;
    all) benchmark latency; benchmark bandwidth; benchmark rate ;;
    dpulog) need_rig; dpu_sudo "tail -${1:-40} '$DPU_LOG'" ;;
    dpubanner) need_rig; dpu_sudo "grep -h 'DPU PROXY MODE ON' '$DPU_LOG' | tail -1" ;;
    dpucpu) need_rig; dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -n "$pid" ] || { echo "dpumesh_dpu not running"; exit; }; top -bH -d 1 -n 2 -p "$pid" | awk "/ PID +USER/{n++} n==2{print}"' ;;
    *)
        echo "usage: $0 geometry|build|restart|point|ping|latency|bandwidth|rate|all|dpulog|dpubanner|dpucpu" >&2
        exit 2 ;;
esac
