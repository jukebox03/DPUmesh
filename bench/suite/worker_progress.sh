#!/usr/bin/env bash
# Capture and gate the state needed to diagnose one ARM worker losing progress.
#
# This script is deliberately read-only except for its receipt directory and an
# optional benchmark point. It never restarts the DPU or a Pod. The new
# dmesh_worker_* metrics come from worker-local counters copied to Prometheus on
# the existing maintenance pass. A missing metric is an incomplete deployment
# and cannot pass the strict quiescence gate.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"

if [ -f "$PROJ_ROOT/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    source "$PROJ_ROOT/.env"
    set +a
fi

NS="${NS:-test-bench}"
LINKERD_ADMIN_ADDR="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}"
WORKERS="${DPUMESH_ARM_WORKERS:-8}"
QUIESCENCE_TIMEOUT="${QUIESCENCE_TIMEOUT:-60}"
WATCH_INTERVAL="${WATCH_INTERVAL:-5}"
WATCH_PROGRESS_AGE_MS="${WATCH_PROGRESS_AGE_MS:-5000}"
RECEIPT_ROOT="${WORKER_RECEIPT_ROOT:-$PROJ_ROOT/bench/report/data}"

log() { printf '[%s] %s\n' "$(date -Is)" "$*" >&2; }
die() { log "FAIL: $*"; exit 1; }

need_remote() {
    : "${DPU_HOST:?.env missing DPU_HOST}" "${DPU_PASS:?.env missing DPU_PASS}"
}

ssh_dpu() {
    ssh -o ConnectTimeout=8 -o ServerAliveInterval=15 "$DPU_HOST" "$@"
}

dpu_sudo() {
    local command="$1"
    ssh_dpu "echo '$DPU_PASS' | sudo -S -p '' sh -c '$command'"
}

worker_count() {
    local value="$WORKERS"
    [[ "$value" =~ ^[0-9]+$ ]] && [ "$value" -ge 1 ] && [ "$value" -le 16 ] ||
        die "DPUMESH_ARM_WORKERS must be an integer in [1,16], got '$value'"
    printf '%s\n' "$value"
}

metrics_snapshot() {
    need_remote
    local addr="$LINKERD_ADMIN_ADDR"
    local host="${addr%:*}" base="${addr##*:}" count worker port metrics
    [ "$host" != "$addr" ] && [[ "$base" =~ ^[0-9]+$ ]] ||
        die "LINKERD_ADMIN_ADDR must be host:port, got '$addr'"
    count=$(worker_count)
    for ((worker = 0; worker < count; worker++)); do
        port=$((base + worker))
        metrics=$(ssh_dpu "curl -g -sf --max-time 3 'http://${host}:${port}/metrics'") ||
            return 1
        printf '# dmesh_worker %d admin_port %d\n%s\n' "$worker" "$port" "$metrics"
    done
}

# One CSV row per worker. Missing metrics are printed as NA in diagnostic
# snapshots, while the gate below rejects that incomplete state.
metrics_csv() {
    awk '
        BEGIN {
            OFS=",";
            print "worker,opened,closed,active,pending,tasks,progress_age_ms,drains,progressed,completion_q,cross_q,deferred_recv,dma_inflight,dma_retries,dma_stalled,stalled_conns,emit_pending,ack_release_q,ack_retry,remote_fin,parked,wake_posted"
        }
        function emit(   i,n,key) {
            if (worker == "") return;
            printf "%s", worker;
            n=split("dmesh_sessions_opened_total dmesh_sessions_closed_total dmesh_sessions_active dmesh_registrations_pending dmesh_tasks_live dmesh_worker_last_progress_age_milliseconds dmesh_worker_drain_calls_total dmesh_worker_drain_progressed_total dmesh_worker_completion_queue_depth dmesh_worker_cross_queue_depth dmesh_worker_deferred_receives dmesh_worker_dma_tasks_inflight dmesh_worker_dma_retry_batches dmesh_worker_dma_stalled dmesh_worker_stalled_connections dmesh_worker_emit_pending dmesh_worker_ack_release_depth dmesh_worker_ack_retry_pending dmesh_worker_remote_fin_pending dmesh_worker_parked dmesh_worker_wake_posted", keys, " ");
            for (i=1; i<=n; i++) {
                key=keys[i];
                printf ",%s", (key in value) ? value[key] : "NA";
            }
            printf "\n";
            delete value;
        }
        $1 == "#" && $2 == "dmesh_worker" { emit(); worker=$3; next }
        $1 ~ /^dmesh_/ { value[$1]=$2 }
        END { emit() }
    '
}

strict_quiescent() {
    local metrics="$1"
    awk '
        function finish(   i,key) {
            if (worker == "") return;
            workers++;
            split("dmesh_sessions_opened_total dmesh_sessions_closed_total dmesh_sessions_active dmesh_registrations_pending dmesh_tasks_live dmesh_worker_completion_queue_depth dmesh_worker_cross_queue_depth dmesh_worker_deferred_receives dmesh_worker_dma_tasks_inflight dmesh_worker_dma_retry_batches dmesh_worker_dma_stalled dmesh_worker_stalled_connections dmesh_worker_emit_pending dmesh_worker_ack_release_depth dmesh_worker_ack_retry_pending dmesh_worker_remote_fin_pending", required, " ");
            for (i in required) {
                key=required[i];
                if (!(key in value)) {
                    printf "worker %s missing %s\n", worker, key > "/dev/stderr";
                    bad=1;
                }
            }
            if (("dmesh_sessions_opened_total" in value) &&
                ("dmesh_sessions_closed_total" in value) &&
                value["dmesh_sessions_opened_total"] != value["dmesh_sessions_closed_total"]) {
                printf "worker %s opened=%s closed=%s\n", worker,
                       value["dmesh_sessions_opened_total"],
                       value["dmesh_sessions_closed_total"] > "/dev/stderr";
                bad=1;
            }
            split("dmesh_sessions_active dmesh_registrations_pending dmesh_tasks_live dmesh_worker_completion_queue_depth dmesh_worker_cross_queue_depth dmesh_worker_deferred_receives dmesh_worker_dma_tasks_inflight dmesh_worker_dma_retry_batches dmesh_worker_dma_stalled dmesh_worker_stalled_connections dmesh_worker_emit_pending dmesh_worker_ack_release_depth dmesh_worker_ack_retry_pending dmesh_worker_remote_fin_pending", zero, " ");
            for (i in zero) {
                key=zero[i];
                if ((key in value) && value[key] != 0) {
                    printf "worker %s %s=%s\n", worker, key, value[key] > "/dev/stderr";
                    bad=1;
                }
            }
            delete value;
        }
        $1 == "#" && $2 == "dmesh_worker" { finish(); worker=$3; next }
        $1 ~ /^dmesh_/ { value[$1]=$2 }
        END { finish(); if (workers == 0 || bad) exit 1 }
    ' <<<"$metrics"
}

wait_quiescent() {
    local timeout="${1:-$QUIESCENCE_TIMEOUT}" start now metrics
    [[ "$timeout" =~ ^[0-9]+$ ]] || die "quiescence timeout must be integer seconds"
    start=$(date +%s)
    while true; do
        if metrics=$(metrics_snapshot 2>/dev/null) && strict_quiescent "$metrics"; then
            printf '%s\n' "$metrics" | metrics_csv
            log "strict worker/session quiescence reached"
            return 0
        fi
        now=$(date +%s)
        [ $((now - start)) -lt "$timeout" ] || return 1
        sleep 1
    done
}

new_receipt_dir() {
    local label="$1" stamp dir
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    dir="$RECEIPT_ROOT/worker-progress-${stamp}-${label}"
    mkdir -p "$RECEIPT_ROOT"
    [ ! -e "$dir" ] || die "receipt already exists: $dir"
    mkdir "$dir"
    printf '%s\n' "$dir"
}

capture() {
    local file="$1"
    shift
    "$@" >"$file" 2>&1 || true
}

snapshot_into() {
    local dir="$1" reason="${2:-manual}" metrics=""
    mkdir -p "$dir"
    {
        printf 'captured_at=%s\n' "$(date -Is)"
        printf 'reason=%s\n' "$reason"
        printf 'host=%s\n' "$(hostname)"
        printf 'namespace=%s\n' "$NS"
        printf 'git_commit=%s\n' "$(git -C "$PROJ_ROOT" rev-parse HEAD 2>/dev/null || true)"
        printf 'git_branch=%s\n' "$(git -C "$PROJ_ROOT" branch --show-current 2>/dev/null || true)"
        printf 'workers=%s\n' "$(worker_count)"
        printf 'admin_addr=%s\n' "$LINKERD_ADMIN_ADDR"
    } >"$dir/metadata.env"

    capture "$dir/git-status.txt" git -C "$PROJ_ROOT" status --short
    capture "$dir/k8s-pods.json" kubectl get pods -n "$NS" -o json
    capture "$dir/k8s-workloads.txt" kubectl get deployment,daemonset -n "$NS" -o wide
    capture "$dir/k8s-events.txt" kubectl get events -n "$NS" --sort-by=.lastTimestamp
    capture "$dir/node-agent.log" kubectl logs -n "$NS" -l app=dpumesh-node-agent --tail=500 --prefix=true

    if metrics=$(metrics_snapshot 2>"$dir/metrics-error.txt"); then
        printf '%s\n' "$metrics" >"$dir/l7-metrics.prom"
        printf '%s\n' "$metrics" | metrics_csv >"$dir/worker-summary.csv"
        if strict_quiescent "$metrics" 2>"$dir/quiescence.txt"; then
            printf 'PASS strict-quiescent\n' >>"$dir/quiescence.txt"
        else
            printf 'NOT-QUIESCENT or metrics unavailable\n' >>"$dir/quiescence.txt"
        fi
    fi

    if [[ -v HOST_PASS ]]; then
        capture "$dir/broker-state.txt" bash -c \
            'printf "%s\n" "$1" | sudo -S -p "" find /run/dpumesh/brokers -maxdepth 1 -type f -name "*.state" -exec sh -c '\''for f do echo "=== $f"; cat "$f"; done'\'' sh {} +' \
            _ "$HOST_PASS"
    fi

    if [[ -v DPU_HOST && -v DPU_PASS ]]; then
        capture "$dir/dpu-threads.txt" dpu_sudo \
            'p=$(pgrep -x dpumesh_dpu | head -1); [ -n "$p" ] || exit 1; echo "pid=$p"; for t in /proc/$p/task/*; do printf "%s " "${t##*/}"; cat "$t/comm" 2>/dev/null | tr "\n" " "; awk "{print \$14+\$15,\$39}" "$t/stat" 2>/dev/null; done'
        capture "$dir/dpu-thread-status.txt" dpu_sudo \
            'p=$(pgrep -x dpumesh_dpu | head -1); [ -n "$p" ] || exit 1; for t in /proc/$p/task/*; do echo "=== ${t##*/}"; grep -E "^(Name|State|voluntary_ctxt_switches|nonvoluntary_ctxt_switches):" "$t/status"; printf "wchan="; cat "$t/wchan"; done'
        capture "$dir/dpu-process-status.txt" dpu_sudo \
            'p=$(pgrep -x dpumesh_dpu | head -1); [ -n "$p" ] || exit 1; cat /proc/$p/status; cat /proc/$p/schedstat'
        capture "$dir/dpu-log-tail.txt" ssh_dpu "tail -n 1200 /tmp/dpumesh_dpu_bench.log"
        capture "$dir/dpu-kernel-tail.txt" dpu_sudo 'dmesg | tail -n 200'
    fi
    log "snapshot: $dir"
}

snapshot() {
    local label="${1:-manual}" dir
    dir=$(new_receipt_dir "$label")
    snapshot_into "$dir" "$label"
    printf '%s\n' "$dir"
}

point_probe() {
    local req="${POINT_REQ:-48}" reply="${POINT_REPLY:-48}"
    local conc="${POINT_CONC:-1024}" dur="${POINT_DUR:-10}"
    local warmup="${POINT_WARMUP:-1000}" threads="${POINT_THREADS:-8}"
    local dir result
    wait_quiescent "$QUIESCENCE_TIMEOUT" || {
        dir=$(snapshot "prepoint-dirty")
        die "pre-point quiescence failed; snapshot=$dir"
    }
    dir=$(new_receipt_dir point)
    snapshot_into "$dir/before" pre-point
    set +e
    result=$("$BENCH" point grpc-dpumesh "$req" "$reply" "$conc" "$dur" "$warmup" "$threads" 2>&1)
    local rc=$?
    set -e
    printf '%s\n' "$result" >"$dir/point.txt"
    snapshot_into "$dir/after" post-point
    [ "$rc" -eq 0 ] && [[ "$result" == OK* ]] ||
        die "point failed; receipt=$dir result=$result"
    wait_quiescent "$QUIESCENCE_TIMEOUT" >"$dir/quiescent.csv" ||
        die "post-point quiescence failed; receipt=$dir"
    log "point and quiescence passed: $dir"
    printf '%s\n' "$dir"
}

watch_progress() {
    local interval="${1:-$WATCH_INTERVAL}" threshold="${2:-$WATCH_PROGRESS_AGE_MS}"
    local metrics bad worker age backlog dir
    [[ "$interval" =~ ^[0-9]+$ ]] && [ "$interval" -ge 1 ] ||
        die "watch interval must be a positive integer"
    [[ "$threshold" =~ ^[0-9]+$ ]] || die "watch threshold must be integer ms"
    log "watching worker progress every ${interval}s (threshold=${threshold}ms with backlog)"
    while true; do
        if ! metrics=$(metrics_snapshot 2>/dev/null); then
            dir=$(snapshot admin-unreachable)
            die "worker admin endpoint became unreachable; snapshot=$dir"
        fi
        bad=$(printf '%s\n' "$metrics" | metrics_csv | awk -F, -v limit="$threshold" '
            NR == 1 { next }
            {
                age=$7;
                backlog=0;
                for (i=10; i<=20; i++) if ($i != "NA") backlog += $i;
                if (age != "NA" && age > limit && backlog > 0) {
                    print $1 ":" age ":" backlog;
                    exit;
                }
            }')
        if [ -n "$bad" ]; then
            IFS=: read -r worker age backlog <<<"$bad"
            dir=$(snapshot "worker${worker}-stalled")
            die "worker=$worker progress_age_ms=$age backlog=$backlog; snapshot=$dir"
        fi
        sleep "$interval"
    done
}

usage() {
    cat <<'EOF'
usage: worker_progress.sh <command> [args]

  snapshot [label]                capture Kubernetes, broker, DPU thread/log and per-worker metrics
  wait-quiescent [timeout_s]      require sessions, queues, DMA and retries to reach zero
  point                           bracket one configured gRPC point with snapshots and quiescence
  watch [interval_s] [age_ms]     snapshot and fail when backlog exists without worker progress

Point variables: POINT_REQ POINT_REPLY POINT_CONC POINT_DUR POINT_WARMUP POINT_THREADS
Other variables: NS DPUMESH_ARM_WORKERS LINKERD_ADMIN_ADDR QUIESCENCE_TIMEOUT
                 WORKER_RECEIPT_ROOT WATCH_INTERVAL WATCH_PROGRESS_AGE_MS
EOF
}

case "${1:-}" in
    snapshot) shift; snapshot "${1:-manual}" ;;
    wait-quiescent) shift; wait_quiescent "${1:-$QUIESCENCE_TIMEOUT}" ;;
    point) shift; [ "$#" -eq 0 ] || die "point takes no positional arguments"; point_probe ;;
    watch) shift; watch_progress "${1:-$WATCH_INTERVAL}" "${2:-$WATCH_PROGRESS_AGE_MS}" ;;
    -h|--help|help|'') usage ;;
    *) usage >&2; die "unknown command '$1'" ;;
esac
