#!/bin/bash
# Host and DPU CPU cost per request for the native, preload, and gRPC APIs over
# the same embedded-Linkerd path.
#
# The accounting window opens after the load is already in steady state and
# closes before it ends, so neither connection setup nor drain is charged to it.
# Host CPU comes from each Pod's cgroup `usage_usec`, which is scheduler
# accounting rather than the 100 Hz tick: a core running in short bursts is
# undercounted by tick sampling. DPU CPU is the summed tick delta of the data
# path's own threads over the same window.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$SUITE_DIR/../bench.sh"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"

OUT_DIR="${OUT_DIR:-$SUITE_DIR/../report/data/api-l7-cost-$(date +%Y%m%d-%H%M%S)}"
REPS="${REPS:-3}"
DUR="${DUR:-20}"          # total load duration
LEAD="${LEAD:-5}"         # settle before the window opens
WINDOW="${WINDOW:-10}"    # accounting window
WARM="${WARM:-1000}"
REQ="${REQ:-1024}"
CONC="${CONC:-32}"
THREADS="${THREADS:-1}"
ARMS="${ARMS:-dpumesh preload grpc-dpumesh}"
# closed: each arm runs at its own saturation point. open: every arm is offered
# the same constant rate, which is the only way a per-request cost comparison
# is not really a comparison of how far each arm sits from its own knee.
MODE="${MODE:-closed}"
RATE="${RATE:-8000}"
PIN="${PIN:-}"

mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/cost.csv"
LOG="$OUT_DIR/raw.log"
[ -f "$CSV" ] || echo "arm,rep,req,conc,threads,mrps,p50,p99,fail,client_core,server_core,host_core,arm_core,host_us_per_req,arm_us_per_req" >"$CSV"

client_app() { case "$1" in
        dpumesh) echo bench-dpumesh ;; preload) echo preload-bench ;;
        grpc-dpumesh) echo bench-grpc-dpumesh ;; esac; }
# Every backend of the Service, because the endpoint Linkerd picks for a
# session is not known in advance: charging one Pod would read as zero whenever
# the balancer chose another.
server_apps() { case "$1" in
        dpumesh) echo echo-dpumesh echo-dpumesh-13 echo-dpumesh-14 ;;
        preload) echo preload-echo ;;
        grpc-dpumesh) echo echo-grpc-dpumesh ;; esac; }

field() { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }

container_pid() {
    local app="$1" pod cid
    pod=$(kubectl get pod -n "$NS" -l "app=$app" \
        -o jsonpath='{.items[0].metadata.name}' 2>/dev/null) || return
    [ -n "$pod" ] || return
    cid=$(kubectl get pod -n "$NS" "$pod" -o json 2>/dev/null |
        jq -r --arg c "$app" '.status.containerStatuses[] | select(.name==$c) | .containerID' |
        sed -n '1p')
    cid="${cid#*://}"
    [ -n "$cid" ] && [ "$cid" != null ] || return
    printf '%s\n' "$HOST_PASS" | sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid'
}

cgroup_usage_usec() {
    local pid="$1" rel
    [ "${pid:-0}" -gt 0 ] 2>/dev/null || { echo 0; return; }
    rel=$(awk -F: '$1=="0"{print $3; exit}' "/proc/$pid/cgroup" 2>/dev/null) || true
    [ -n "$rel" ] || { echo 0; return; }
    awk '$1=="usage_usec"{print $2; f=1} END{if(!f) print 0}' \
        "/sys/fs/cgroup$rel/cpu.stat" 2>/dev/null || echo 0
}

sum_usage() {
    local total=0 pid
    for pid in $1; do total=$((total + $(cgroup_usage_usec "$pid"))); done
    echo "$total"
}

# Cumulative user+system ticks of the DPU data path's own threads, plus CLK_TCK.
dpu_ticks() {
    ssh -n -o ConnectTimeout=8 -o StrictHostKeyChecking=no "$DPU_HOST" \
        "echo '$DPU_PASS' | sudo -S sh -c 'p=\$(pgrep -x dpumesh_dpu | head -1); \
          [ -n \"\$p\" ] || exit 1; total=0; \
          for t in /proc/\$p/task/*; do \
            stat=\$(cat \$t/stat 2>/dev/null) || continue; \
            rest=\${stat#*) }; set -- \$rest; \
            total=\$((total + \${12} + \${13})); \
          done; echo \"\$(getconf CLK_TCK) \$total\"' 2>/dev/null" 2>/dev/null |
        tr -d '\r' | tail -1
}

pod_ip() { kubectl get pod -n "$NS" -l "app=$1" \
    -o jsonpath='{.items[0].status.podIP}' 2>/dev/null; }

echo "output -> $OUT_DIR"
for rep in $(seq 1 "$REPS"); do
    for arm in $ARMS; do
        capp=$(client_app "$arm")
        if [ -n "$PIN" ]; then
            case "$arm" in
                dpumesh) prof=native ;; preload) prof=preload ;; grpc-dpumesh) prof=grpc ;;
            esac
            bash "$BENCH" pin "$prof" >>"$LOG" 2>&1; sleep 3
        fi
        cpid=$(container_pid "$capp" || true)
        spids=""
        for sapp in $(server_apps "$arm"); do
            sp=$(container_pid "$sapp" || true)
            [[ "${sp:-}" =~ ^[0-9]+$ ]] && spids="${spids:+$spids }$sp"
        done
        ip=$(pod_ip "$capp")
        [ -n "$ip" ] || { echo "  $arm: no client pod"; continue; }
        out=$(mktemp "$OUT_DIR/run.XXXXXX")
        if [ "$MODE" = open ]; then
            cmd=$(printf 'OPEN %s 8 %s %s %s %s const\n' "$REQ" "$THREADS" "$DUR" "$WARM" "$RATE")
        else
            cmd=$(printf 'RUN %s 8 %s %s %s %s\n' "$REQ" "$CONC" "$DUR" "$WARM" "$THREADS")
        fi
        ( printf '%s\n' "$cmd" |
              timeout "$((DUR + 90))s" nc -N "$ip" "$CTRL_PORT" >"$out" 2>/dev/null ) &
        load=$!
        sleep "$LEAD"
        c0=$(cgroup_usage_usec "$cpid"); s0=$(sum_usage "$spids")
        read -r hz d0 <<<"$(dpu_ticks)"
        t0=$(date +%s.%N)
        sleep "$WINDOW"
        c1=$(cgroup_usage_usec "$cpid"); s1=$(sum_usage "$spids")
        read -r _ d1 <<<"$(dpu_ticks)"
        t1=$(date +%s.%N)
        wait "$load" 2>/dev/null || true
        r=$(sed 's/\x1b\[[0-9;]*m//g' "$out" | grep -E '^(OK|ERR)' | tail -1)
        rm -f "$out"
        echo "$arm rep$rep :: $r" >>"$LOG"
        [[ "$r" == OK* ]] || { echo "  $arm rep$rep FAILED: $r"; continue; }
        read -r ccore score hcore acore hus aus <<<"$(awk -v c0="$c0" -v c1="$c1" \
            -v s0="$s0" -v s1="$s1" -v d0="${d0:-0}" -v d1="${d1:-0}" -v hz="${hz:-100}" \
            -v t0="$t0" -v t1="$t1" -v mrps="$(field "$r" mrps)" 'BEGIN{
                w = t1 - t0; if (w <= 0) w = 1;
                cc = (c1-c0)/1e6/w; sc = (s1-s0)/1e6/w;
                ac = (d1-d0)/hz/w; rps = mrps*1e6;
                hus = (rps > 0) ? (cc+sc)*1e6/rps : 0;
                aus = (rps > 0) ? ac*1e6/rps : 0;
                printf "%.4f %.4f %.4f %.4f %.3f %.3f", cc, sc, cc+sc, ac, hus, aus;
            }')"
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$arm" "$rep" "$REQ" "$CONC" "$THREADS" "$(field "$r" mrps)" \
            "$(field "$r" p50)" "$(field "$r" p99)" "$(field "$r" fail)" \
            "$ccore" "$score" "$hcore" "$acore" "$hus" "$aus" >>"$CSV"
        printf '  %-13s rep%s Mrps=%-9s host=%-7s (c %s + s %s) arm=%-7s host_us/req=%-7s arm_us/req=%s\n' \
            "$arm" "$rep" "$(field "$r" mrps)" "$hcore" "$ccore" "$score" "$acore" "$hus" "$aus"
        sleep 3
    done
done
echo "done -> $CSV"
