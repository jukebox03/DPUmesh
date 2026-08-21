#!/usr/bin/env bash
# Channel-count sweep for the gRPC DPUmesh path.
#
# A gRPC channel maps to one native QP, and a QP is pinned to one forward ring,
# one DPA EU and one ARM worker. This sweep holds the client's offered load
# fixed (same worker-thread count, same rate grid) and varies only the number of
# channels, so the transport connection count is the single moving variable.
#
# Each point records what the client achieved, what the two endpoint cores cost,
# and how the DPU's ARM workers shared the work, the last one read per worker
# thread over a window that excludes connection setup and teardown.
#
# A full sweep outlives an interactive login, and a dropped terminal takes a
# foreground run's remaining points with it. Detach it from the session:
#
#   setsid nohup ./bench/suite/grpc_conns_sweep.sh --out /tmp/conns \
#       >/tmp/conns.log 2>&1 </dev/null &
#   tail -f /tmp/conns.log
#
# points.csv is appended per point, so a run that dies partway keeps what it
# measured. There is no resume: a rerun repeats the whole grid and appends to
# whatever is already there, so give it a fresh --out.
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
: "${HOST_PASS:?.env missing HOST_PASS}" "${DPU_HOST:?.env missing DPU_HOST}" \
  "${DPU_PASS:?.env missing DPU_PASS}"

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
CLIENT_APP="${CLIENT_APP:-bench-grpc-dpumesh}"
SERVER_APP="${SERVER_APP:-echo-grpc-dpumesh}"

CHANNELS="${CHANNELS:-1 2 4 8}"
RATES="${RATES:-2000 4000 8000 12000 16000 24000 32000}"
REPS="${REPS:-3}"
DUR="${DUR:-10}"
WARMUP="${WARMUP:-200}"
THREADS="${THREADS:-8}"
FRAME="${FRAME:-64}"
HEADER_BYTES=16
# The CPU window opens after the channels are up and closes before the run ends,
# so setup and teardown land outside it.
SETTLE="${SETTLE:-2.5}"
SAMPLE="${SAMPLE:-6}"
# Lets the previous run's connections drain before the next one dials.
GAP="${GAP:-3}"
OUT="${OUT:-/tmp/grpc-conns-sweep}"
REACTORS_TAG="${REACTORS_TAG:-8}"
PIN_PROFILE="${PIN_PROFILE:-grpc}"
# Overload can kill an endpoint. Redeploying is the only way to clear the DPU
# afterwards, so the sweep does it itself rather than stopping at the first one.
MAX_RECOVERIES="${MAX_RECOVERIES:-6}"

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --channels) CHANNELS="$2"; shift 2 ;;
    --rates) RATES="$2"; shift 2 ;;
    --reps) REPS="$2"; shift 2 ;;
    --frame) FRAME="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --tag) REACTORS_TAG="$2"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--out DIR] [--channels \"1 2 4 8\"] [--rates \"...\"]"
      echo "          [--reps N] [--frame BYTES] [--threads N] [--tag REACTORS]"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

BODY=$((FRAME - HEADER_BYTES))
[ "$BODY" -gt 0 ] || { echo "frame must exceed ${HEADER_BYTES}B" >&2; exit 2; }

# Two campaigns under load contend for the DPU and the memory system even on
# disjoint cores, and their results interleave into one another's output.
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
    jq -r --arg c "$app" \
      '.status.containerStatuses[] | select(.name==$c) | .containerID' | sed -n '1p')
  cid="${cid#*://}"
  [ -n "$cid" ] && [ "$cid" != null ] || return
  printf '%s\n' "$HOST_PASS" |
    sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid'
}
cgroup_usage_usec() {
  local pid="$1" rel
  [ "${pid:-0}" -gt 0 ] || { echo 0; return; }
  rel=$(awk -F: '$1=="0"{print $3; exit}' "/proc/$pid/cgroup" 2>/dev/null) || true
  [ -n "$rel" ] || { echo 0; return; }
  awk '$1=="usage_usec"{print $2; found=1} END{if(!found) print 0}' \
    "/sys/fs/cgroup$rel/cpu.stat" 2>/dev/null || echo 0
}

# One ssh round trip returns the DPU clock, the tick rate, the data-path pid and
# every thread's cumulative ticks. A thread is keyed by tid: only the workers are
# renamed, so the remaining threads share one comm and keying by name would
# collapse them onto a single baseline.
dpu_worker_snapshot() {
  ssh -n -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 \
    "$DPU_HOST" \
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

# A pod that dies mid-campaign leaves the DPU unable to reclaim its RX mmap,
# which corrupts every later run. Stop at the first restart rather than collect
# through it.
restart_count() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{range .status.containerStatuses}}{{.restartCount}}{{end}}{{end}}' \
    2>/dev/null || echo ""
}

CLIENT_IP=$(control_ip "$CLIENT_APP")
[ -n "$CLIENT_IP" ] || { echo "no running pod for $CLIENT_APP" >&2; exit 1; }
CLIENT_PID=$(container_pid "$CLIENT_APP" || true)
SERVER_PID=$(container_pid "$SERVER_APP" || true)
[[ "${CLIENT_PID:-}" =~ ^[0-9]+$ ]] || { echo "cannot resolve $CLIENT_APP PID" >&2; exit 1; }
[[ "${SERVER_PID:-}" =~ ^[0-9]+$ ]] || { echo "cannot resolve $SERVER_APP PID" >&2; exit 1; }

reply=$(printf 'PING\n' | timeout 10s nc -N "$CLIENT_IP" "$CTRL_PORT" 2>/dev/null || true)
[ "${reply%%$'\n'*}" = PONG ] || { echo "client control port not answering: $reply" >&2; exit 1; }

if [ ! -s "$CSV" ]; then
  {
    printf 'channels,reactors,frame,rep,offered,achieved,ratio,p50_us,p99_us,p999_us,'
    printf 'fail,drops,client_core,server_core,dpu_core_total,dpu_core_workers,'
    printf 'w0,w1,w2,w3,w4,w5,w6,w7,window_s\n'
  } >"$CSV"
fi

resolve_endpoints() {
  CLIENT_IP=$(control_ip "$CLIENT_APP")
  CLIENT_PID=$(container_pid "$CLIENT_APP" || true)
  SERVER_PID=$(container_pid "$SERVER_APP" || true)
  CLIENT_RESTARTS=$(restart_count "$CLIENT_APP")
  SERVER_RESTARTS=$(restart_count "$SERVER_APP")
  [ -n "$CLIENT_IP" ] && [[ "${CLIENT_PID:-}" =~ ^[0-9]+$ ]] &&
    [[ "${SERVER_PID:-}" =~ ^[0-9]+$ ]]
}

# A dead pod leaves the DPU holding its RX mmap, and only a full deploy releases
# it. A pod-only restart would leave the DPU blocked on its first connection.
recover_deploy() {
  RECOVERIES=$((RECOVERIES + 1))
  [ "$RECOVERIES" -le "$MAX_RECOVERIES" ] || {
    log "giving up after $MAX_RECOVERIES recoveries"
    return 1
  }
  log "recovery $RECOVERIES/$MAX_RECOVERIES: full redeploy"
  (
    cd "$PROJ_ROOT" &&
    DPUMESH_DPA_THREADS=32 DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=8 \
    BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=grpc \
      bash bench/bench.sh deploy
  ) >>"$OUT/recover.log" 2>&1 || { log "redeploy failed"; return 1; }
  ( cd "$PROJ_ROOT" && bash bench/bench.sh pin "$PIN_PROFILE" ) \
    >>"$OUT/recover.log" 2>&1 || { log "repin failed"; return 1; }
  resolve_endpoints || { log "endpoints unresolvable after redeploy"; return 1; }
  log "recovered: client=$CLIENT_IP($CLIENT_PID) server=$SERVER_PID"
}

RECOVERIES=0
CLIENT_RESTARTS=$(restart_count "$CLIENT_APP")
SERVER_RESTARTS=$(restart_count "$SERVER_APP")
CRASHES="$OUT/crashes.csv"
[ -s "$CRASHES" ] || echo "channels,reactors,frame,offered,rep" >"$CRASHES"

log "sweep: channels=[$CHANNELS] rates=[$RATES] reps=$REPS frame=${FRAME}B threads=$THREADS reactors=$REACTORS_TAG"
log "client=$CLIENT_APP($CLIENT_PID) server=$SERVER_APP($SERVER_PID) out=$OUT"
log "restart baseline: client=$CLIENT_RESTARTS server=$SERVER_RESTARTS"

total=0; done_n=0
for _c in $CHANNELS; do for _r in $RATES; do total=$((total + REPS)); done; done

for channels in $CHANNELS; do
  for rate in $RATES; do
    for rep in $(seq 1 "$REPS"); do
      run_id="c${channels}_r${rate}_${rep}"
      raw="$OUT/raw/$run_id.txt"

      # The load runs in the background so the CPU window can be placed inside it.
      ( printf 'OPEN %s %s %s %s %s %s const %s\n' \
          "$BODY" "$BODY" "$THREADS" "$DUR" "$WARMUP" "$rate" "$channels" |
        timeout "$((DUR + 90))s" nc -N "$CLIENT_IP" "$CTRL_PORT" >"$raw" 2>/dev/null ||
        echo "ERR control" >"$raw" ) &
      ncpid=$!

      sleep "$SETTLE"
      snap0=$(dpu_worker_snapshot || true)
      cg_c0=$(cgroup_usage_usec "$CLIENT_PID")
      cg_s0=$(cgroup_usage_usec "$SERVER_PID")
      host_t0=$(date +%s.%N)

      sleep "$SAMPLE"

      snap1=$(dpu_worker_snapshot || true)
      cg_c1=$(cgroup_usage_usec "$CLIENT_PID")
      cg_s1=$(cgroup_usage_usec "$SERVER_PID")
      host_t1=$(date +%s.%N)

      wait "$ncpid" || true
      result=$(cat "$raw")

      window=$(awk -v a="$host_t0" -v b="$host_t1" 'BEGIN{printf "%.4f", b-a}')
      # The DPU's own clock bounds the tick deltas; the host clock bounds the
      # cgroup deltas. Mixing them would charge one side the ssh round trip.
      dpu_window=$(awk '/^CLOCK /{print $2}' <<<"$snap0" | head -1)
      dpu_window1=$(awk '/^CLOCK /{print $2}' <<<"$snap1" | head -1)
      if [ -n "${dpu_window:-}" ] && [ -n "${dpu_window1:-}" ]; then
        dpu_window=$(awk -v a="$dpu_window" -v b="$dpu_window1" 'BEGIN{printf "%.4f", b-a}')
      else
        dpu_window="$window"
      fi
      hz=$(awk '/^CLOCK /{print $3}' <<<"$snap1" | head -1); hz="${hz:-100}"

      # A data path that restarted between the snapshots renumbers its threads
      # and resets their counters, so the bracket describes two different
      # processes and the deltas are discarded.
      dpu_pid0=$(awk '/^CLOCK /{print $4}' <<<"$snap0" | head -1)
      dpu_pid1=$(awk '/^CLOCK /{print $4}' <<<"$snap1" | head -1)

      # Per-thread tick deltas, keyed by tid and bucketed by comm.
      if [ -n "${dpu_pid0:-}" ] && [ "$dpu_pid0" = "${dpu_pid1:-}" ]; then
        deltas=$(awk -v hz="$hz" -v d="$dpu_window" '
          NR==FNR { if ($1=="T") a[$2]=$4; next }
          $1=="T" && ($2 in a) && ($4-a[$2]) >= 0 {
            printf "%s %.6f\n", $3, ($4-a[$2])/hz/d }
        ' <(printf '%s\n' "$snap0") <(printf '%s\n' "$snap1"))
      else
        log "$run_id: DPU data path changed between snapshots; ARM CPU dropped"
        deltas=""
      fi

      w=(0 0 0 0 0 0 0 0)
      dpu_workers_total=0
      dpu_total=0
      while read -r name val; do
        [ -n "$name" ] || continue
        dpu_total=$(awk -v a="$dpu_total" -v b="$val" 'BEGIN{printf "%.6f", a+b}')
        case "$name" in
          dmesh-w[0-7])
            idx="${name#dmesh-w}"
            w[$idx]="$val"
            dpu_workers_total=$(awk -v a="$dpu_workers_total" -v b="$val" 'BEGIN{printf "%.6f", a+b}')
            ;;
        esac
      done <<<"$deltas"

      client_core=$(awk -v a="$cg_c0" -v b="$cg_c1" -v d="$window" \
        'BEGIN{if(d>0) printf "%.4f", (b-a)/1e6/d; else print "NA"}')
      server_core=$(awk -v a="$cg_s0" -v b="$cg_s1" -v d="$window" \
        'BEGIN{if(d>0) printf "%.4f", (b-a)/1e6/d; else print "NA"}')

      achieved=$(field "$result" mrps)
      achieved=$(awk -v m="${achieved:-0}" 'BEGIN{printf "%.1f", m*1e6}')
      p50=$(field "$result" p50); p99=$(field "$result" p99); p999=$(field "$result" p999)
      fail=$(field "$result" fail); drops=$(field "$result" drops)
      ratio=$(awk -v a="${achieved:-0}" -v o="$rate" 'BEGIN{if(o>0) printf "%.4f", a/o; else print 0}')

      printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$channels" "$REACTORS_TAG" "$FRAME" "$rep" "$rate" "$achieved" "$ratio" \
        "${p50:-NA}" "${p99:-NA}" "${p999:-NA}" "${fail:-NA}" "${drops:-NA}" \
        "$client_core" "$server_core" "$dpu_total" "$dpu_workers_total" \
        "${w[0]}" "${w[1]}" "${w[2]}" "${w[3]}" "${w[4]}" "${w[5]}" "${w[6]}" "${w[7]}" \
        "$dpu_window" >>"$CSV"

      done_n=$((done_n + 1))
      log "[$done_n/$total] c=$channels rate=$rate rep=$rep -> achieved=$achieved ratio=$ratio p50=${p50:-NA} p99=${p99:-NA} client=$client_core server=$server_core workers=$dpu_workers_total"

      now_c=$(restart_count "$CLIENT_APP"); now_s=$(restart_count "$SERVER_APP")
      if [ "$now_c" != "$CLIENT_RESTARTS" ] || [ "$now_s" != "$SERVER_RESTARTS" ]; then
        log "CRASH during $run_id (client $CLIENT_RESTARTS->$now_c, server $SERVER_RESTARTS->$now_s)"
        # The run that killed the endpoint measured a dying process, and the
        # rates above it would only kill it again.
        sed -i '$d' "$CSV"
        echo "$channels,$REACTORS_TAG,$FRAME,$rate,$rep" >>"$CRASHES"
        recover_deploy || exit 3
        done_n=$((done_n + REPS - rep))
        break 2
      fi

      sleep "$GAP"
    done
  done
done

log "done: $CSV"
