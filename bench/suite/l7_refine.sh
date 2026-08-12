#!/bin/bash
# Push each mode past its knee and record where the achieved rate stops rising.
#
# l7_modes.sh stops the ramp at the first offered rate a mode fails to deliver,
# so it reports the last rung that passed. That number is a property of the
# ladder as much as of the path: two modes land on the same rung while their
# real limits differ, and a mode whose curve is still climbing is credited with
# the rung below. What distinguishes the modes is the plateau -- the highest
# rate the path actually carries once the offered rate no longer matters.
#
# This continues the ramp from the highest rate already on record and records
# the achieved rate at each further step. The plateau is the maximum achieved.
#
# Usage: ./bench/suite/l7_refine.sh <dataset-dir> [--modes "..."] [--frames "..."]
#                                   [--extra N] [--no-deploy]
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
BENCH_HEADER_BYTES=16

DPU_DPA_THREADS=32
DPU_RINGS_PER_POD=8
DPU_ARM_WORKERS=8
THREADS="${THREADS:-$DPU_ARM_WORKERS}"
DUR="${DUR:-4}"
WARMUP="${WARMUP:-200}"
ARRIVAL="${ARRIVAL:-const}"
GROWTH="${GROWTH:-1.5}"
EXTRA="${EXTRA:-2}"
TARGET_SVC=11
INSTRUMENT_APP="${INSTRUMENT_APP:-bench-tcp}"
TARGET_APP=bench-dpumesh
is_instrument() { [ "$TARGET_APP" = "$INSTRUMENT_APP" ]; }

MODES="${MODES:-instrument l7-message l7-conn opaque decision dataplane}"
FRAMES="${FRAMES:-64 1024 8192}"
DO_DEPLOY=1

DATASET="${1:-}"
[ -n "$DATASET" ] || { echo "usage: $0 <dataset-dir> [--modes ...] [--frames ...]" >&2; exit 2; }
shift || true
while [ $# -gt 0 ]; do
  case "$1" in
    --modes)  MODES="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --extra)  EXTRA="$2"; shift 2 ;;
    --no-deploy) DO_DEPLOY=0; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

log()  { printf '\033[0;34m[sat]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[sat]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[0;31m[sat]\033[0m %s\n' "$*" >&2; exit 1; }
trap 'rc=$?; printf "\033[0;31m[sat]\033[0m aborted: line $LINENO, exit $rc\n" >&2; exit $rc' ERR

[ -d "$DATASET" ] || die "no dataset at $DATASET"
RAW="$DATASET/raw"; mkdir -p "$RAW"
SAT="$DATASET/saturation.csv"
[ -s "$SAT" ] || printf 'mode,frame,offered_rps,achieved_rps,drops,rcnt,p99_us\n' >"$SAT"

frame_body() { echo $(( $1 - BENCH_HEADER_BYTES )); }
field() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1"; }

control_ip() {
  kubectl get pods -n "$NS" -l "app=$1" \
    -o jsonpath='{.items[?(@.status.phase=="Running")].status.podIP}' 2>/dev/null | awk '{print $1}'
}

run_control() {
  local line="$1" duration="$2" ip
  ip=$(control_ip "$TARGET_APP")
  [ -n "$ip" ] || { echo "ERR no-running-pod"; return; }
  printf '%s\n' "$line" |
    timeout "$((duration + 60))s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null || echo "ERR control"
}

dpu_alive() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
    "$DPU_HOST" 'pgrep -x dpumesh_dpu >/dev/null' 2>/dev/null
}
require_dpu() { is_instrument && return 0; dpu_alive && return 0
  die "the DPU process is gone after $1 — every later row would be a zero"; }

deploy_mode() {
  local mode="$1" decision="" opaque="" full="" rr=""
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
  log "deploying mode=$mode"
  env DPUMESH_DPA_THREADS="$DPU_DPA_THREADS" \
      DPUMESH_RINGS_PER_POD="$DPU_RINGS_PER_POD" \
      DPUMESH_ARM_WORKERS="$DPU_ARM_WORKERS" \
      DPUMESH_L7_DECISION_SVC="$decision" \
      DPUMESH_L7_OPAQUE_SVC="$opaque" \
      DPUMESH_L7_SVC="$full" \
      DPUMESH_L7_FRAMED_RR="$rr" \
      DPUMESH_LOG_LEVEL=40 BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=all \
      "$BENCH" deploy >"$RAW/deploy-sat-$mode.log" 2>&1 ||
    die "deploy failed for $mode; see $RAW/deploy-sat-$mode.log"
}

# The highest rate already offered to this mode at this frame, from the ramp's
# own raw files. The sweep continues from there rather than repeating it.
highest_offered() {
  local mode="$1" frame="$2" f n best=0
  for f in "$RAW"/scout-"$mode"-"$frame"-*.txt "$RAW"/sat-"$mode"-"$frame"-*.txt; do
    [ -e "$f" ] || continue
    n=$(basename "$f" .txt); n=${n##*-}
    [ "$n" -gt "$best" ] 2>/dev/null && best="$n"
  done
  echo "$best"
}

# The ramp's own points are part of the same curve. Importing them keeps the
# plateau over everything the mode was offered rather than over the extra steps
# alone, which otherwise credits a mode with whatever the last two rungs gave.
import_scout() {
  local mode="$1" frame="$2" f n result
  for f in "$RAW"/scout-"$mode"-"$frame"-*.txt; do
    [ -e "$f" ] || continue
    n=$(basename "$f" .txt); n=${n##*-}
    grep -q "^$mode,$frame,$n," "$SAT" 2>/dev/null && continue
    result=$(cat "$f")
    case "$result" in ERR*|"") continue ;; esac
    [ -n "$(field "$result" mrps)" ] || continue
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$mode" "$frame" "$n" \
      "$(awk -v v="$(field "$result" mrps)" 'BEGIN{printf "%d", v*1000000}')" \
      "$(field "$result" drops)" "$(field "$result" rcnt)" \
      "$(field "$result" p99)" >>"$SAT"
  done
}

for mode in $MODES; do
  log "=== $mode ==="
  deploy_mode "$mode"
  for frame in $FRAMES; do
    import_scout "$mode" "$frame"
    start=$(highest_offered "$mode" "$frame")
    if [ "$start" -le 0 ]; then
      warn "$mode/$frame: nothing on record to continue from"; continue
    fi
    body=$(frame_body "$frame")
    offered="$start" i=0
    best=$(awk -F, -v m="$mode" -v f="$frame" \
             '$1==m && $2==f && $4+0>b {b=$4} END{printf "%d", b+0}' "$SAT")
    while [ "$i" -lt "$EXTRA" ]; do
      offered=$(awk -v o="$offered" -v g="$GROWTH" 'BEGIN{printf "%d", o*g}')
      if grep -q "^$mode,$frame,$offered," "$SAT" 2>/dev/null; then i=$((i + 1)); continue; fi
      result=$(run_control "OPEN $body $body $THREADS $DUR $WARMUP $offered $ARRIVAL" "$DUR")
      printf '%s\n' "$result" >"$RAW/sat-$mode-$frame-$offered.txt"
      case "$result" in ERR*|"") warn "$mode/$frame at $offered: $result"; break ;; esac
      ach=$(awk -v v="$(field "$result" mrps)" 'BEGIN{printf "%d", v*1000000}')
      log "  $mode f=$frame offered=$offered achieved=$ach"
      # Past the plateau the path collapses rather than holding. Offering more
      # then measures the overload path, so stop at the first sign of it.
      if [ "$ach" -gt "$best" ]; then best="$ach"
      elif awk -v a="$ach" -v b="$best" 'BEGIN{exit !(a < b*0.9)}'; then
        log "  $mode f=$frame collapsed past the plateau; stopping"
        printf '%s,%s,%s,%s,%s,%s,%s\n' "$mode" "$frame" "$offered" "$ach" \
          "$(field "$result" drops)" "$(field "$result" rcnt)" \
          "$(field "$result" p99)" >>"$SAT"
        break
      fi
      printf '%s,%s,%s,%s,%s,%s,%s\n' "$mode" "$frame" "$offered" "$ach" \
        "$(field "$result" drops)" "$(field "$result" rcnt)" "$(field "$result" p99)" >>"$SAT"
      i=$((i + 1))
    done
    require_dpu "$mode/$frame"
  done
done

log "saturation sweep in $SAT"
