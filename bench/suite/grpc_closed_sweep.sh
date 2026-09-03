#!/usr/bin/env bash
# Closed-loop concurrency sweep for the DPUmesh gRPC path.
#
# An open-loop rate sweep lets the queue grow without bound, so a path that
# trades latency for throughput scores higher: deeper batching amortises the
# per-wake cost and the offered rate is still met, just later. Holding the
# in-flight window fixed removes that degree of freedom — a completion is what
# frees a slot, so latency and throughput are tied together and neither can be
# bought with the other. This describes the closed-loop latency/throughput
# shape. Published capacity still comes from the repeated open-loop grid's
# `highest_clean_rps`; the two instruments are not interchangeable.
#
# Each point drives one concurrency window through the client's control port and
# brackets a CPU window inside the run, on both endpoints and on the DPU.
#
#   setsid nohup ./bench/suite/grpc_closed_sweep.sh --out /tmp/closed \
#       >/tmp/closed.log 2>&1 </dev/null &
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
: "${HOST_PASS:?.env missing HOST_PASS}" "${DPU_HOST:?.env missing DPU_HOST}" \
  "${DPU_PASS:?.env missing DPU_PASS}"

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
CONFIGS="${CONFIGS:-grpc-dpumesh}"
FRAMES="${FRAMES:-64 1024 8192}"
CONCS="${CONCS:-1 2 4 8 16 32}"
REPS="${REPS:-1}"
DUR="${DUR:-10}"
WARMUP="${WARMUP:-200}"
THREADS="${THREADS:-8}"
HEADER_BYTES=16
SETTLE="${SETTLE:-2.5}"
SAMPLE="${SAMPLE:-6}"
GAP="${GAP:-3}"
OUT="${OUT:-/tmp/grpc-closed-sweep}"

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --configs) CONFIGS="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --concs) CONCS="$2"; shift 2 ;;
    --reps) REPS="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--out DIR] [--configs \"...\"] [--frames \"64 1024 8192\"]"
      echo "          [--concs \"1 2 4 8 16 32\"] [--reps N] [--threads N]"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# Shared with the other campaigns: two of them under load contend for the DPU
# and each one's traffic lands in the other's CPU window.
LOCK="${BENCH_LOCK:-/tmp/dpumesh-bench.lock}"
exec 9>"$LOCK"
flock -n 9 || {
  echo "another bench campaign holds $LOCK; run them one at a time" >&2
  exit 1
}

mkdir -p "$OUT/raw"
CSV="$OUT/points.csv"
LOG="$OUT/sweep.log"
CRASHES="$OUT/crashes.csv"
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG" >&2; }
field() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1"; }

client_app() {
    case "$1" in
    grpc-dpumesh)          echo bench-grpc-dpumesh ;;
    grpc-linkerd)          echo bench-grpc-linkerd ;;
    *) return 1 ;;
  esac
}
server_app() {
  case "$1" in
    grpc-dpumesh)          echo echo-grpc-dpumesh ;;
    grpc-linkerd)          echo echo-grpc-linkerd ;;
    *) return 1 ;;
  esac
}

pod_name() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.metadata.name}}{{"\n"}}{{end}}{{end}}{{end}}' \
    2>/dev/null | sed -n '1p'
}
control_ip() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' \
    2>/dev/null | sed -n '1p'
}
container_pid() {
  local app="$1" pod cid
  pod=$(pod_name "$app"); [ -n "$pod" ] || return
  cid=$(kubectl get pod -n "$NS" "$pod" -o json |
    jq -r --arg c "$app" '.status.containerStatuses[]|select(.name==$c)|.containerID' | sed -n '1p')
  cid="${cid#*://}"
  [ -n "$cid" ] && [ "$cid" != null ] || return
  printf '%s\n' "$HOST_PASS" | sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid'
}
sidecar_pid() {
  local app="$1" pod cid
  pod=$(pod_name "$app"); [ -n "$pod" ] || return
  cid=$(kubectl get pod -n "$NS" "$pod" -o json |
    jq -r '((.status.containerStatuses // []) +
            (.status.initContainerStatuses // []))[] |
           select(.name == "linkerd-proxy") | .containerID' | sed -n '1p')
  cid="${cid#*://}"
  [ -n "$cid" ] && [ "$cid" != null ] || return
  printf '%s\n' "$HOST_PASS" | sudo -S crictl inspect "$cid" 2>/dev/null |
    jq -r '.info.pid'
}
container_cgroup_usec() {
  local pid="$1" rel
  [ "${pid:-0}" -gt 0 ] || { echo 0; return; }
  rel=$(awk -F: '$1=="0"{print $3; exit}' "/proc/$pid/cgroup" 2>/dev/null) || true
  [ -n "$rel" ] || { echo 0; return; }
  awk '$1=="usage_usec"{print $2; f=1} END{if(!f) print 0}' \
    "/sys/fs/cgroup$rel/cpu.stat" 2>/dev/null || echo 0
}
# Charge the complete workload Pod cgroup.
pod_cgroup_usec() {
  local pid="$1" rel podrel
  [ "${pid:-0}" -gt 0 ] || { echo 0; return; }
  rel=$(awk -F: '$1=="0"{print $3; exit}' "/proc/$pid/cgroup" 2>/dev/null) || true
  [ -n "$rel" ] || { echo 0; return; }
  podrel="$rel"
  # Charge the recursive Pod parent. The broker is in its own sibling child
  # cgroup, so client_core/server_core include application + broker once.
  case "${podrel##*/}" in
    cri-containerd-*.scope|crio-*.scope|docker-*.scope) podrel=${podrel%/*} ;;
  esac
  awk '$1=="usage_usec"{print $2; f=1} END{if(!f) print 0}' \
    "/sys/fs/cgroup$podrel/cpu.stat" 2>/dev/null || echo 0
}
dpu_snapshot() {
  ssh -n -o ConnectTimeout=8 -o ServerAliveInterval=15 -o BatchMode=yes "$DPU_HOST" \
    "echo '$DPU_PASS' | sudo -S sh -c 'p=\$(pgrep -x dpumesh_dpu | head -1); \
      [ -n \"\$p\" ] || exit 1; \
      echo \"CLOCK \$(date +%s.%N) \$(getconf CLK_TCK) \$p\"; \
      for t in /proc/\$p/task/*; do \
        comm=\$(cat \$t/comm 2>/dev/null) || continue; \
        stat=\$(cat \$t/stat 2>/dev/null) || continue; \
        rest=\${stat#*) }; set -- \$rest; \
        echo \"T \${t##*/} \$comm \$((\${12}+\${13}))\"; \
      done'" 2>/dev/null
}

if [ ! -s "$CSV" ]; then
  printf 'config,frame,threads,channels,window_per_worker,total_concurrency,rep,achieved,p50_us,p99_us,p999_us,avg_us,fail,drops,pending,worker_fail,credit_hold_dropped,eq_budget_exhausted,client_core,server_core,client_app_core,client_sidecar_core,server_app_core,server_sidecar_core,dpu_arm_cores,dpu_worker_cores,dpu_nonworker_cores,window_s\n' >"$CSV"
fi
[ -s "$CRASHES" ] || echo "config,frame,window_per_worker,rep,restarts_before,restarts_after" >"$CRASHES"

total=0
for _c in $CONFIGS; do for _f in $FRAMES; do for _k in $CONCS; do total=$((total + REPS)); done; done; done
done_n=0
log "closed-loop sweep: configs=[$CONFIGS] frames=[$FRAMES] concs=[$CONCS] reps=$REPS threads=$THREADS"

restart_count() {
  # A rolling update briefly leaves the terminating and replacement Pods in
  # the same label selection. Concatenating their counts (for example `00`)
  # makes the later single Pod's `0` look like a restart. Ignore Pods already
  # selected for deletion and reduce the remaining container counts as a
  # number.
  kubectl get pods -n "$NS" -l "app=$1" -o json 2>/dev/null |
    jq -r '[.items[] | select(.metadata.deletionTimestamp == null) |
            ((.status.containerStatuses // []) +
             (.status.initContainerStatuses // []))[]?.restartCount] |
           add // 0' || echo ""
}

for config in $CONFIGS; do
  capp=$(client_app "$config"); sapp=$(server_app "$config")
  cip=$(control_ip "$capp")
  cpid=$(container_pid "$capp" || true); spid=$(container_pid "$sapp" || true)
  csidepid=$(sidecar_pid "$capp" || true); ssidepid=$(sidecar_pid "$sapp" || true)
  [ -n "$cip" ] && [[ "${cpid:-}" =~ ^[0-9]+$ ]] && [[ "${spid:-}" =~ ^[0-9]+$ ]] || {
    log "$config: endpoints unresolvable, skipping"; continue; }
  base_rs="$(restart_count "$capp")/$(restart_count "$sapp")"
  log "$config: client=$capp($cpid) server=$sapp($spid) restarts=$base_rs"

  for frame in $FRAMES; do
    body=$((frame - HEADER_BYTES))
    for conc in $CONCS; do
      for rep in $(seq 1 "$REPS"); do
        # A restarted Pod changes the PIDs the CPU window is charged to. Stop instead of
        # continuing against a data path in an unknown state.
        now_rs="$(restart_count "$capp")/$(restart_count "$sapp")"
        if [ "$now_rs" != "$base_rs" ]; then
          log "CRASH before ${config}/${frame}B/conc=${conc} (restarts $base_rs -> $now_rs); stopping"
          echo "$config,$frame,$conc,$rep,$base_rs,$now_rs" >>"$CRASHES"
          exit 3
        fi
        run_id="${config}_f${frame}_c${conc}_${rep}"
        raw="$OUT/raw/$run_id.txt"

        ( printf 'RUN %s %s %s %s %s %s\n' "$body" "$body" "$conc" "$DUR" "$WARMUP" "$THREADS" |
          timeout "$((DUR + 90))s" nc -N "$cip" "$CTRL_PORT" >"$raw" 2>/dev/null ||
          echo "ERR control" >"$raw" ) &
        ncpid=$!

        sleep "$SETTLE"
        d0=$(dpu_snapshot || true)
        cg_c0=$(pod_cgroup_usec "$cpid"); cg_s0=$(pod_cgroup_usec "$spid")
        cg_ca0=$(container_cgroup_usec "$cpid"); cg_sa0=$(container_cgroup_usec "$spid")
        cg_cs0=$(container_cgroup_usec "${csidepid:-0}")
        cg_ss0=$(container_cgroup_usec "${ssidepid:-0}")
        t0=$(date +%s.%N)
        sleep "$SAMPLE"
        cg_c1=$(pod_cgroup_usec "$cpid"); cg_s1=$(pod_cgroup_usec "$spid")
        cg_ca1=$(container_cgroup_usec "$cpid"); cg_sa1=$(container_cgroup_usec "$spid")
        cg_cs1=$(container_cgroup_usec "${csidepid:-0}")
        cg_ss1=$(container_cgroup_usec "${ssidepid:-0}")
        t1=$(date +%s.%N)
        d1=$(dpu_snapshot || true)
        wait "$ncpid" || true
        result=$(cat "$raw")

        win=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.4f", b-a}')
        ccore=$(awk -v a="$cg_c0" -v b="$cg_c1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')
        score=$(awk -v a="$cg_s0" -v b="$cg_s1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')
        cacore=$(awk -v a="$cg_ca0" -v b="$cg_ca1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')
        sacore=$(awk -v a="$cg_sa0" -v b="$cg_sa1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')
        cscore=$(awk -v a="$cg_cs0" -v b="$cg_cs1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')
        sscore=$(awk -v a="$cg_ss0" -v b="$cg_ss1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')

        arm=NA; arm_workers=NA; arm_nonworkers=NA
        if [ -n "$d0" ] && [ -n "$d1" ]; then
          p0=$(awk '/^CLOCK /{print $4}' <<<"$d0" | head -1)
          p1=$(awk '/^CLOCK /{print $4}' <<<"$d1" | head -1)
          hz=$(awk '/^CLOCK /{print $3}' <<<"$d1" | head -1); hz="${hz:-100}"
          dw=$(awk -v a="$(awk '/^CLOCK /{print $2}' <<<"$d0"|head -1)" \
                   -v b="$(awk '/^CLOCK /{print $2}' <<<"$d1"|head -1)" 'BEGIN{printf "%.4f",b-a}')
          if [ -n "${p0:-}" ] && [ "$p0" = "${p1:-}" ]; then
            arm=$(awk -v hz="$hz" -v d="$dw" '
              NR==FNR { if ($1=="T") a[$2]=$4; next }
              $1=="T" && ($2 in a) && ($4-a[$2])>=0 { t+=($4-a[$2])/hz/d }
              END { printf "%.4f", t }' <(printf '%s\n' "$d0") <(printf '%s\n' "$d1"))
            arm_workers=$(awk -v hz="$hz" -v d="$dw" '
              NR==FNR { if ($1=="T") a[$2]=$4; next }
              $1=="T" && $3 ~ /^dmesh-w([0-9]|1[0-5])$/ && ($2 in a) && ($4-a[$2])>=0 {
                t+=($4-a[$2])/hz/d
              }
              END { printf "%.4f", t }' <(printf '%s\n' "$d0") <(printf '%s\n' "$d1"))
            arm_nonworkers=$(awk -v a="$arm" -v w="$arm_workers" \
              'BEGIN{printf "%.4f", a-w}')
          fi
        fi

        mrps=$(field "$result" mrps)
        ach=$(awk -v m="${mrps:-0}" 'BEGIN{printf "%.1f", m*1e6}')
        channels=$(field "$result" channels)
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
          "$config" "$frame" "$THREADS" "${channels:-NA}" "$conc" \
          "$((conc * THREADS))" "$rep" "$ach" \
          "$(field "$result" p50)" "$(field "$result" p99)" "$(field "$result" p999)" \
          "$(field "$result" avg)" "$(field "$result" fail)" "$(field "$result" drops)" \
          "$(field "$result" pending)" "$(field "$result" worker_fail)" \
          "$(field "$result" credit_hold_dropped)" "$(field "$result" eq_budget_exhausted)" \
          "$ccore" "$score" "$cacore" "$cscore" "$sacore" "$sscore" \
          "$arm" "$arm_workers" "$arm_nonworkers" "$win" >>"$CSV"

        done_n=$((done_n + 1))
        log "[$done_n/$total] $config ${frame}B conc=$conc -> ach=$ach p50=$(field "$result" p50) p99=$(field "$result" p99) cli=$ccore(app=$cacore sidecar=$cscore) srv=$score(app=$sacore sidecar=$sscore) arm=$arm workers=$arm_workers nonworkers=$arm_nonworkers"

        now_rs="$(restart_count "$capp")/$(restart_count "$sapp")"
        if [ "$now_rs" != "$base_rs" ]; then
          log "CRASH during ${config}/${frame}B/conc=${conc} (restarts $base_rs -> $now_rs); dropping point and stopping"
          sed -i '$d' "$CSV"
          echo "$config,$frame,$conc,$rep,$base_rs,$now_rs" >>"$CRASHES"
          exit 3
        fi
        sleep "$GAP"
      done
    done
  done
done
log "closed-loop sweep complete: $CSV"
