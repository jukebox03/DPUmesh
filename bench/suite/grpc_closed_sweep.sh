#!/usr/bin/env bash
# Closed-loop concurrency sweep for the four gRPC paths.
#
# An open-loop rate sweep lets the queue grow without bound, so a path that
# trades latency for throughput scores higher: deeper batching amortises the
# per-wake cost and the offered rate is still met, just later. Holding the
# in-flight window fixed removes that degree of freedom — a completion is what
# frees a slot, so latency and throughput are tied together and neither can be
# bought with the other. This is the measurement that answers "what is the
# maximum this path sustains", while the open-loop collector answers "what does
# a given load cost".
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
CONFIGS="${CONFIGS:-grpc-envoy-permissive grpc-envoy-strict grpc-tcp grpc-dpumesh}"
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
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG" >&2; }
field() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1"; }

client_app() {
  case "$1" in
    grpc-envoy-permissive) echo bench-grpc-envoy ;;
    grpc-envoy-strict)     echo bench-grpc-envoy-strict ;;
    grpc-tcp)              echo bench-grpc-tcp ;;
    grpc-dpumesh)          echo bench-grpc-dpumesh ;;
    grpc-linkerd)          echo bench-grpc-linkerd ;;
    grpc-linkerd-opaque)   echo bench-grpc-linkerd-opaque ;;
  esac
}
server_app() {
  case "$1" in
    grpc-envoy-permissive) echo echo-grpc-envoy ;;
    grpc-envoy-strict)     echo echo-grpc-envoy-strict ;;
    grpc-tcp)              echo echo-grpc-tcp ;;
    grpc-dpumesh)          echo echo-grpc-dpumesh ;;
    grpc-linkerd)          echo echo-grpc-linkerd ;;
    grpc-linkerd-opaque)   echo echo-grpc-linkerd-opaque ;;
  esac
}
is_dpu() { [ "$1" = grpc-dpumesh ]; }

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
# An Envoy path runs its sidecar in the same pod and on the same core, so the
# pod cgroup is what the configuration costs.
pod_cgroup_usec() {
  local pid="$1" rel podrel
  [ "${pid:-0}" -gt 0 ] || { echo 0; return; }
  rel=$(awk -F: '$1=="0"{print $3; exit}' "/proc/$pid/cgroup" 2>/dev/null) || true
  [ -n "$rel" ] || { echo 0; return; }
  podrel="${rel%/*}"
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
  printf 'config,frame,threads,conc,rep,achieved,p50_us,p99_us,p999_us,avg_us,fail,client_core,server_core,dpu_arm_cores,window_s\n' >"$CSV"
fi

total=0
for _c in $CONFIGS; do for _f in $FRAMES; do for _k in $CONCS; do total=$((total + REPS)); done; done; done
done_n=0
log "closed-loop sweep: configs=[$CONFIGS] frames=[$FRAMES] concs=[$CONCS] reps=$REPS threads=$THREADS"

restart_count() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{range .status.containerStatuses}}{{.restartCount}}{{end}}{{end}}' \
    2>/dev/null || echo ""
}

for config in $CONFIGS; do
  capp=$(client_app "$config"); sapp=$(server_app "$config")
  cip=$(control_ip "$capp")
  cpid=$(container_pid "$capp" || true); spid=$(container_pid "$sapp" || true)
  [ -n "$cip" ] && [[ "${cpid:-}" =~ ^[0-9]+$ ]] && [[ "${spid:-}" =~ ^[0-9]+$ ]] || {
    log "$config: endpoints unresolvable, skipping"; continue; }
  base_rs="$(restart_count "$capp")/$(restart_count "$sapp")"
  log "$config: client=$capp($cpid) server=$sapp($spid) restarts=$base_rs"

  for frame in $FRAMES; do
    body=$((frame - HEADER_BYTES))
    for conc in $CONCS; do
      for rep in $(seq 1 "$REPS"); do
        # An endpoint that died leaves this loop reading a dead pid, which
        # reports zero CPU for every later point. Re-resolve before each run and
        # record the restart rather than collecting through it.
        now_rs="$(restart_count "$capp")/$(restart_count "$sapp")"
        if [ "$now_rs" != "$base_rs" ]; then
          log "CRASH before ${config}/${frame}B/conc=${conc} (restarts $base_rs -> $now_rs); re-resolving endpoints"
          echo "$config,$frame,$conc,$rep,$base_rs,$now_rs" >>"$OUT/crashes.csv"
          base_rs="$now_rs"
          sleep 5
          cip=$(control_ip "$capp")
          cpid=$(container_pid "$capp" || true); spid=$(container_pid "$sapp" || true)
          [ -n "$cip" ] && [[ "${cpid:-}" =~ ^[0-9]+$ ]] && [[ "${spid:-}" =~ ^[0-9]+$ ]] || {
            log "$config: endpoints unresolvable after restart, skipping rest of config"; break 3; }
        fi
        run_id="${config}_f${frame}_c${conc}_${rep}"
        raw="$OUT/raw/$run_id.txt"

        ( printf 'RUN %s %s %s %s %s %s\n' "$body" "$body" "$conc" "$DUR" "$WARMUP" "$THREADS" |
          timeout "$((DUR + 90))s" nc -N "$cip" "$CTRL_PORT" >"$raw" 2>/dev/null ||
          echo "ERR control" >"$raw" ) &
        ncpid=$!

        sleep "$SETTLE"
        d0=""; is_dpu "$config" && d0=$(dpu_snapshot || true)
        cg_c0=$(pod_cgroup_usec "$cpid"); cg_s0=$(pod_cgroup_usec "$spid")
        t0=$(date +%s.%N)
        sleep "$SAMPLE"
        cg_c1=$(pod_cgroup_usec "$cpid"); cg_s1=$(pod_cgroup_usec "$spid")
        t1=$(date +%s.%N)
        d1=""; is_dpu "$config" && d1=$(dpu_snapshot || true)
        wait "$ncpid" || true
        result=$(cat "$raw")

        win=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.4f", b-a}')
        ccore=$(awk -v a="$cg_c0" -v b="$cg_c1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')
        score=$(awk -v a="$cg_s0" -v b="$cg_s1" -v d="$win" 'BEGIN{if(d>0)printf "%.4f",(b-a)/1e6/d; else print "NA"}')

        arm=NA
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
          fi
        fi

        mrps=$(field "$result" mrps)
        ach=$(awk -v m="${mrps:-0}" 'BEGIN{printf "%.1f", m*1e6}')
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
          "$config" "$frame" "$THREADS" "$conc" "$rep" "$ach" \
          "$(field "$result" p50)" "$(field "$result" p99)" "$(field "$result" p999)" \
          "$(field "$result" avg)" "$(field "$result" fail)" \
          "$ccore" "$score" "$arm" "$win" >>"$CSV"

        done_n=$((done_n + 1))
        log "[$done_n/$total] $config ${frame}B conc=$conc -> ach=$ach p50=$(field "$result" p50) p99=$(field "$result" p99) cli=$ccore srv=$score arm=$arm"
        sleep "$GAP"
      done
    done
  done
done
log "closed-loop sweep complete: $CSV"
