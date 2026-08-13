#!/bin/bash
# Collect the L7-layer dataset: what each operating mode costs against the
# transport carrying the same workload on its own.
#
# A mode is a DPU-side property, so every mode is its own deployment of the same
# pod topology. Within a deployment the workload, the frame sizes and the
# open-loop method are the ones bench/suite/l4_proxy_data.sh uses, so a row here
# and a row there mean the same thing.
#
# Usage:
#   ./bench/suite/l7_modes.sh [--out DIR] [--quick] [--modes "a b c"] [--no-deploy]
# Reusing --out resumes by run_id without repeating completed rows.
set -euo pipefail

COLLECTOR_VERSION=2
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
BENCH_HEADER_BYTES=16

# The canonical stack. Every mode is measured on it.
DPU_DPA_THREADS=32
DPU_RINGS_PER_POD=8
DPU_ARM_WORKERS=8
THREADS="${THREADS:-$DPU_ARM_WORKERS}"

# Frame sizes include the benchmark header, as they do in the L4 dataset.
FRAME_SIZES="${FRAME_SIZES:-64 1024 8192}"
HEADLINE_FRAME="${HEADLINE_FRAME:-1024}"   # the frame the ARM-core row uses
ARRIVAL="${ARRIVAL:-const}"
REPS="${REPS:-2}"
DUR="${DUR:-5}"
WARMUP="${WARMUP:-200}"
CLOSED_CONC="${CLOSED_CONC:-32}"

# Capacity scout: ramp until the achieved rate falls behind what was offered.
SCOUT_START_RPS="${SCOUT_START_RPS:-50000}"
SCOUT_GROWTH="${SCOUT_GROWTH:-1.5}"
SCOUT_MAX_STEPS="${SCOUT_MAX_STEPS:-14}"
SCOUT_DUR="${SCOUT_DUR:-4}"
MIN_ACHIEVED_RATIO="${MIN_ACHIEVED_RATIO:-0.98}"
MAX_DROP_RATIO="${MAX_DROP_RATIO:-0.001}"
# Offered rates as fractions of the common anchor, so every mode is compared at
# the same load rather than at its own knee. The anchor is the slowest mode's
# knee, which is the only grid every mode can serve — the rule the L4 collector
# applies across configurations.
COMMON_FACTORS="${COMMON_FACTORS:-0.25 0.50 0.75 0.90}"
# Plus one rate at each mode's own knee, so a mode is also seen at the operating
# point it would really run at.
OWN_FACTORS="${OWN_FACTORS:-0.90}"
# Cores per request is a property of a path *at a load*, so the modes are also
# compared at rates every one of them serves. Without this the slowest mode is
# credited with a cost that is partly just its lower operating point.
MATCHED_RATES="${MATCHED_RATES:-50000 100000 150000}"

# The service the client addresses. Each mode assigns it to a different gate.
TARGET_SVC=11

# The matched TCP path. Measured as a mode of its own, it records what the load
# generator can sustain without any data path under it: a mode whose knee equals
# this one is bounded by the instrument, and at that frame the modes cannot be
# told apart no matter how finely the knee is resolved.
INSTRUMENT_APP="${INSTRUMENT_APP:-bench-tcp}"
TARGET_APP=bench-dpumesh
is_instrument() { [ "$TARGET_APP" = "$INSTRUMENT_APP" ]; }

# Slowest first: the first mode measured sets the common grid.
MODES_DEFAULT="instrument l7-message l7-conn opaque decision dataplane"
MODES="${MODES:-$MODES_DEFAULT}"
OUT=""
DO_DEPLOY=1
CORES_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out)    OUT="$2"; shift 2 ;;
    --modes)  MODES="$2"; shift 2 ;;
    --quick)  REPS=1; DUR=3; SCOUT_MAX_STEPS=8; FRAME_SIZES="1024"; shift ;;
    --no-deploy) DO_DEPLOY=0; shift ;;
    --cores-only) CORES_ONLY=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

log()  { printf '\033[0;34m[l7]\033[0m %s\n' "$*" >&2; }
# `set -e` aborts silently, and a silent abort in a 40-minute campaign reads as
# a hang. Name the line before leaving.
trap 'rc=$?; printf "\033[0;31m[l7]\033[0m aborted: line $LINENO, exit $rc\n" >&2; exit $rc' ERR
warn() { printf '\033[1;33m[l7]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[0;31m[l7]\033[0m %s\n' "$*" >&2; exit 1; }

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${OUT:-$PROJ_ROOT/bench/report/data/l7-$STAMP}"
mkdir -p "$OUT/raw"
POINTS="$OUT/points.csv"
CAPACITY="$OUT/capacity.csv"
CORES="$OUT/cores.csv"
PROVENANCE="$OUT/provenance.txt"

POINTS_HEADER='run_id,mode,kind,frame,threads,conc,offered_rps,achieved_rps,gbps,p50_us,p95_us,p99_us,p999_us,fail,drops,reorder,overflow,rcnt,dist'
if [ ! -s "$POINTS" ]; then
  printf '%s\n' "$POINTS_HEADER" >"$POINTS"
elif [ "$(head -1 "$POINTS")" != "$POINTS_HEADER" ] && [ "$CORES_ONLY" != 1 ]; then
  # Appending a row with a different field count silently shifts every column
  # after it. A dataset from another collector version is refused, not corrupted.
  # A cores-only pass writes no point rows, so it may still extend the dataset.
  die "$POINTS came from a different collector version; use a new --out"
fi
[ -s "$CAPACITY" ] || printf 'mode,frame,knee_rps,limit_reason\n' >"$CAPACITY"
MATCHED="$OUT/cores_matched.csv"
[ -s "$MATCHED" ] || printf 'mode,frame,offered_rps,rep,achieved_rps,arm_cores_pct,cores_per_mrps,p50_us,p99_us,drops,reorder\n' >"$MATCHED"
CORES_HEADER='mode,frame,conc,threads,achieved_rps,arm_cores_pct,reorder'
if [ ! -s "$CORES" ]; then
  printf '%s\n' "$CORES_HEADER" >"$CORES"
elif [ "$(head -1 "$CORES")" != "$CORES_HEADER" ]; then
  sed -i "1s/.*/$CORES_HEADER/" "$CORES"
fi

completed() { grep -q "^$2," "$1" 2>/dev/null || return 1; }
frame_body() { echo $(( $1 - BENCH_HEADER_BYTES )); }
field() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1"; }
# dist is "pod:count,pod:count"; its commas would split the CSV row.
dist_field() { field "$1" dist | tr ',' ';'; }

control_ip() {
  kubectl get pods -n "$NS" -l "app=$1" \
    -o jsonpath='{.items[?(@.status.phase=="Running")].status.podIP}' 2>/dev/null |
    awk '{print $1}'
}

run_control() {
  local line="$1" duration="$2" ip
  ip=$(control_ip "$TARGET_APP")
  [ -n "$ip" ] || { echo "ERR no-running-pod"; return; }
  printf '%s\n' "$line" |
    timeout "$((duration + 60))s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null || echo "ERR control"
}

# One deployment per mode: the gate is read when the DPU process starts.
deploy_mode() {
  local mode="$1"
  local decision="" opaque="" full="" rr=""
  TARGET_APP=bench-dpumesh
  case "$mode" in
    instrument) TARGET_APP="$INSTRUMENT_APP"; return 0 ;;
    dataplane)  ;;
    decision)   decision="$TARGET_SVC" ;;
    opaque)     opaque="$TARGET_SVC" ;;
    l7-conn)    full="$TARGET_SVC"; rr=conn ;;
    l7-message) full="$TARGET_SVC"; rr=message ;;
    *) die "unknown mode: $mode" ;;
  esac
  [ "$DO_DEPLOY" = 1 ] || { warn "--no-deploy: assuming $mode is already up"; return 0; }
  log "deploying mode=$mode (N/K/A=$DPU_DPA_THREADS/$DPU_RINGS_PER_POD/$DPU_ARM_WORKERS)"
  env DPUMESH_DPA_THREADS="$DPU_DPA_THREADS" \
      DPUMESH_RINGS_PER_POD="$DPU_RINGS_PER_POD" \
      DPUMESH_ARM_WORKERS="$DPU_ARM_WORKERS" \
      DPUMESH_L7_DECISION_SVC="$decision" \
      DPUMESH_L7_OPAQUE_SVC="$opaque" \
      DPUMESH_L7_SVC="$full" \
      DPUMESH_L7_FRAMED_RR="$rr" \
      DPUMESH_LOG_LEVEL=40 BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=all \
      "$BENCH" deploy >"$OUT/raw/deploy-$mode.log" 2>&1 ||
    die "deploy failed for $mode; see $OUT/raw/deploy-$mode.log"
  grep -a "PROXY MODE ON" "$OUT/raw/deploy-$mode.log" >>"$PROVENANCE" 2>/dev/null || true
}

# An open-loop point. Records what was offered and what arrived.
open_point() {
  local mode="$1" frame="$2" offered="$3" rep="$4" kind="$5"
  local run_id="$kind-$mode-$frame-$offered-r$rep"
  if completed "$POINTS" "$run_id"; then return 0; fi
  local body result
  body=$(frame_body "$frame")
  result=$(run_control "OPEN $body $body $THREADS $DUR $WARMUP $offered $ARRIVAL" "$DUR")
  if [ -z "$result" ] || [ "$(field "$result" fail)" != 0 ] ||
     [ "$(field "$result" rcnt)" = 0 ]; then
    warn "$run_id: retrying after a failed attempt"
    settle
    result=$(run_control "OPEN $body $body $THREADS $DUR $WARMUP $offered $ARRIVAL" "$DUR")
  fi
  printf '%s\n' "$result" >"$OUT/raw/$run_id.txt"
  case "$result" in ERR*|"") warn "$run_id: $result"; return 1 ;; esac
  printf '%s,%s,%s,%s,%s,,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$run_id" "$mode" "$kind" "$frame" "$THREADS" "$offered" \
    "$(field "$result" mrps)" "$(field "$result" gbps)" \
    "$(field "$result" p50)" "$(field "$result" p95)" "$(field "$result" p99)" \
    "$(field "$result" p999)" "$(field "$result" fail)" "$(field "$result" drops)" \
    "$(field "$result" reorder)" "$(field "$result" overflow)" \
    "$(field "$result" rcnt)" \
    "$(dist_field "$result")" >>"$POINTS"
  return 0
}

# A closed-loop point at a fixed concurrency window.
closed_point() {
  local mode="$1" frame="$2" conc="$3" rep="$4"
  local run_id="closed-$mode-$frame-c$conc-r$rep"
  if completed "$POINTS" "$run_id"; then return 0; fi
  local body result threads="$THREADS"
  [ "$conc" = 1 ] && threads=1
  body=$(frame_body "$frame")
  result=$(run_control "RUN $body $body $conc $DUR $WARMUP $threads" "$DUR")
  if [ -z "$result" ] || [ "$(field "$result" fail)" != 0 ] ||
     [ "$(field "$result" rcnt)" = 0 ]; then
    warn "$run_id: retrying after a failed attempt"
    settle
    result=$(run_control "RUN $body $body $conc $DUR $WARMUP $threads" "$DUR")
  fi
  printf '%s\n' "$result" >"$OUT/raw/$run_id.txt"
  case "$result" in ERR*|"") warn "$run_id: $result"; return 1 ;; esac
  printf '%s,%s,closed,%s,%s,%s,,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$run_id" "$mode" "$frame" "$threads" "$conc" \
    "$(field "$result" mrps)" "$(field "$result" gbps)" \
    "$(field "$result" p50)" "$(field "$result" p95)" "$(field "$result" p99)" \
    "$(field "$result" p999)" "$(field "$result" fail)" "$(field "$result" drops)" \
    "$(field "$result" reorder)" "$(field "$result" overflow)" \
    "$(field "$result" rcnt)" \
    "$(dist_field "$result")" >>"$POINTS"
  return 0
}

# A dead datapath reports as a failing client on every run that follows, so
# every measurement is checked against the process still being there. Without
# this the collector fills a dataset with zeroes and says nothing is wrong.
dpu_alive() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
    "$DPU_HOST" 'pgrep -x dpumesh_dpu >/dev/null' 2>/dev/null
}

require_dpu() {
  is_instrument && return 0
  dpu_alive && return 0
  die "the DPU process is gone after $1 — the datapath died, and every later row would be a zero"
}

# A point that reports failures is a broken client, not a slow one. Give it a
# pause and one more attempt before believing it.
settle() {
  sleep "${SETTLE_S:-3}"
  run_control "RUN 1008 1008 1 2 50 1" 2 >/dev/null 2>&1 || true
}

# Ramp the offered rate until the achieved rate falls behind or drops appear.
scout_capacity() {
  local mode="$1" frame="$2"
  if grep -q "^$mode,$frame," "$CAPACITY" 2>/dev/null; then
    awk -F, -v m="$mode" -v f="$frame" '$1==m && $2==f {print $3}' "$CAPACITY"
    return 0
  fi
  local offered="$SCOUT_START_RPS" step=0 knee=0 reason=exhausted
  local body result achieved drops rcnt ratio
  body=$(frame_body "$frame")
  while [ "$step" -lt "$SCOUT_MAX_STEPS" ]; do
    result=$(run_control "OPEN $body $body $THREADS $SCOUT_DUR $WARMUP $offered $ARRIVAL" "$SCOUT_DUR")
    printf '%s\n' "$result" >"$OUT/raw/scout-$mode-$frame-$offered.txt"
    case "$result" in ERR*|"") warn "scout $mode/$frame at $offered: $result"; break ;; esac
    achieved=$(awk -v v="$(field "$result" mrps)" 'BEGIN{printf "%d", v*1000000}')
    drops=$(field "$result" drops); rcnt=$(field "$result" rcnt)
    [ -n "$drops" ] || drops=0; [ -n "$rcnt" ] || rcnt=1
    ratio=$(awk -v a="$achieved" -v o="$offered" 'BEGIN{printf "%.4f", (o>0)?a/o:0}')
    log "  scout $mode f=$frame offered=$offered achieved=$achieved ratio=$ratio drops=$drops"
    if awk -v r="$ratio" -v m="$MIN_ACHIEVED_RATIO" 'BEGIN{exit !(r < m)}'; then
      reason=rate_shortfall; break
    fi
    if awk -v d="$drops" -v n="$rcnt" -v m="$MAX_DROP_RATIO" 'BEGIN{exit !(n>0 && d/n > m)}'; then
      reason=scheduler_drops; break
    fi
    # An overflowed latency histogram means the run held samples the histogram
    # cannot represent. The rate may look served; the run is not a measurement.
    if [ "$(field "$result" overflow)" != 0 ] 2>/dev/null; then
      reason=histogram_overflow; break
    fi
    knee="$achieved"
    offered=$(awk -v o="$offered" -v g="$SCOUT_GROWTH" 'BEGIN{printf "%d", o*g}')
    step=$((step + 1))
  done
  [ -n "$knee" ] && [ "$knee" -gt 0 ] 2>/dev/null || knee="${achieved:-0}"
  if [ "$knee" -le 0 ] 2>/dev/null; then
    warn "no capacity established for $mode/$frame — the client did not serve"
    reason=no_service
  fi
  printf '%s,%s,%s,%s\n' "$mode" "$frame" "$knee" "$reason" >>"$CAPACITY"
  echo "$knee"
}

# ARM cores the DPU spends carrying the mode, at the headline size.
#
# Measured from the data-path process's own thread ticks rather than through
# bench.sh's balance command: that command rejects a run that reorders, and
# reordering is the defined behaviour of the modes that route per message. The
# load is the same closed-loop window either way.
dpu_sudo() {
  ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
    "$DPU_HOST" "echo '$DPU_PASS' | sudo -S bash -c '$1'" 2>/dev/null |
    sed 's/^\[sudo\][^:]*: *//'
}

# "HZ total_ticks" for the data-path process, summed over its threads.
dpu_ticks() {
  dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -z "$pid" ] && exit 1;
            hz=$(getconf CLK_TCK); t=0;
            for task in /proc/$pid/task/*; do
              stat=$(<"$task/stat"); rest=${stat#*) }; set -- $rest;
              t=$((t + ${12} + ${13}));
            done; echo "$hz $t"'
}

measure_cores() {
  local mode="$1" frame="$2"
  is_instrument && return 0
  if grep -q "^$mode,$frame," "$CORES" 2>/dev/null; then return 0; fi
  local body result mrps hz0 t0 hz1 t1 wall0 wall1 pct
  body=$(frame_body "$frame")

  read -r hz0 t0 <<<"$(dpu_ticks || true)"
  if [ -z "${t0:-}" ]; then warn "cores $mode: no tick snapshot"; return 0; fi
  wall0=$(date +%s.%N)
  result=$(run_control "RUN $body $body $CLOSED_CONC 10 $WARMUP $THREADS" 10)
  wall1=$(date +%s.%N)
  read -r hz1 t1 <<<"$(dpu_ticks || true)"
  printf '%s\n' "$result" >"$OUT/raw/cores-$mode-$frame.txt"
  if [ -z "${t1:-}" ]; then warn "cores $mode: the data path went away"; return 0; fi

  mrps=$(field "$result" mrps)
  [ -n "$mrps" ] || { warn "cores $mode: no result"; return 0; }
  pct=$(awk -v a="$t0" -v b="$t1" -v hz="$hz0" -v w0="$wall0" -v w1="$wall1" \
        'BEGIN{d=w1-w0; if(d<=0||hz<=0){print ""; exit} printf "%.1f", (b-a)/hz/d*100}')
  printf '%s,%s,%s,%s,%s,%s,%s\n' "$mode" "$frame" "$CLOSED_CONC" "$THREADS" \
    "$mrps" "$pct" "$(field "$result" reorder)" >>"$CORES"
  log "  cores(f=$frame) = ${pct}% at $mrps Mrps"
}

measure_cores_at_rate() {
  local mode="$1" frame="$2" offered="$3" rep="$4"
  if grep -q "^$mode,$frame,$offered,$rep," "$MATCHED" 2>/dev/null; then return 0; fi
  local body result mrps hz0 t0 hz1 t1 wall0 wall1 pct per
  body=$(frame_body "$frame")
  read -r hz0 t0 <<<"$(dpu_ticks || true)"
  [ -n "${t0:-}" ] || { warn "matched $mode@$offered: no tick snapshot"; return 0; }
  wall0=$(date +%s.%N)
  result=$(run_control "OPEN $body $body $THREADS 10 $WARMUP $offered $ARRIVAL" 10)
  wall1=$(date +%s.%N)
  read -r hz1 t1 <<<"$(dpu_ticks || true)"
  printf '%s\n' "$result" >"$OUT/raw/matched-$mode-$frame-$offered.txt"
  case "$result" in ERR*|"") warn "matched $mode@$offered: $result"; return 0 ;; esac
  [ -n "${t1:-}" ] || { warn "matched $mode@$offered: the data path went away"; return 0; }
  mrps=$(field "$result" mrps)
  pct=$(awk -v a="$t0" -v b="$t1" -v hz="$hz0" -v w0="$wall0" -v w1="$wall1" \
        'BEGIN{d=w1-w0; if(d<=0||hz<=0){print ""; exit} printf "%.1f", (b-a)/hz/d*100}')
  per=$(awk -v p="$pct" -v m="$mrps" 'BEGIN{ if(m+0>0 && p!="") printf "%.2f", (p/100)/m }')
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$mode" "$frame" "$offered" "$rep" \
    "$mrps" "$pct" "$per" "$(field "$result" p50)" "$(field "$result" p99)" \
    "$(field "$result" drops)" "$(field "$result" reorder)" >>"$MATCHED"
  log "  matched $mode f=$frame @$offered r$rep: ${pct}% for $mrps Mrps -> $per cores/Mrps"
}

{
  echo "collector_version=$COLLECTOR_VERSION"
  echo "stamp=$STAMP"
  echo "host=$(hostname)  dpu=${DPU_HOST:-unset}"
  echo "stack=N/K/A=$DPU_DPA_THREADS/$DPU_RINGS_PER_POD/$DPU_ARM_WORKERS threads=$THREADS"
  echo "frames=$FRAME_SIZES (including ${BENCH_HEADER_BYTES}B header)"
  echo "reps=$REPS dur=${DUR}s warmup=$WARMUP arrival=$ARRIVAL conc=$CLOSED_CONC"
  echo "modes=$MODES"
  echo "git=$(git -C "$PROJ_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "dirty=$(git -C "$PROJ_ROOT" status --porcelain 2>/dev/null | wc -l) files"
} >>"$PROVENANCE"

declare -A ANCHOR=()
for mode in $MODES; do
  log "=== mode $mode ==="
  deploy_mode "$mode"

  if [ "$CORES_ONLY" = 1 ]; then
    if ! is_instrument; then
      # One measurement moves by about a tenth between runs, which is the size
      # of the differences being looked for, so every rate is repeated.
      for rep in $(seq 1 "$REPS"); do
        for rate in $MATCHED_RATES; do
          measure_cores_at_rate "$mode" "$HEADLINE_FRAME" "$rate" "$rep"
        done
      done
      require_dpu "mode $mode"
    fi
    continue
  fi

  for frame in $FRAME_SIZES; do
    knee=$(scout_capacity "$mode" "$frame")
    log "  capacity(f=$frame) = $knee rps"
    require_dpu "the capacity search for $mode/$frame"
    settle
    # The first mode measured sets the grid for that frame, so every later mode
    # is loaded at the same absolute rates rather than at its own knee.
    if is_instrument; then continue; fi
    [ -n "${ANCHOR[$frame]:-}" ] || ANCHOR[$frame]="$knee"

    for factor in $COMMON_FACTORS; do
      offered=$(awk -v a="${ANCHOR[$frame]}" -v f="$factor" 'BEGIN{printf "%d", a*f}')
      [ "$offered" -gt 0 ] || continue
      # A rate far above this mode's own capacity measures the overload path,
      # not the mode. The common grid is kept where both can serve it.
      if awk -v o="$offered" -v k="$knee" 'BEGIN{exit !(k>0 && o > k*1.2)}'; then
        log "  skip f=$frame offered=$offered (above ${knee} knee)"
        continue
      fi
      for rep in $(seq 1 "$REPS"); do
        open_point "$mode" "$frame" "$offered" "$rep" rate || true
      done
    done
    for factor in $OWN_FACTORS; do
      offered=$(awk -v k="$knee" -v f="$factor" 'BEGIN{printf "%d", k*f}')
      [ "$offered" -gt 0 ] || continue
      for rep in $(seq 1 "$REPS"); do
        open_point "$mode" "$frame" "$offered" "$rep" knee || true
      done
    done
    for rep in $(seq 1 "$REPS"); do
      closed_point "$mode" "$frame" "$CLOSED_CONC" "$rep" || true
      closed_point "$mode" "$frame" 1 "$rep" || true
    done
  done

  measure_cores "$mode" "$HEADLINE_FRAME"
  require_dpu "mode $mode"
done

log "dataset in $OUT"
python3 "$SUITE_DIR/summarize_l7.py" "$OUT" | tee "$OUT/summary.txt"
