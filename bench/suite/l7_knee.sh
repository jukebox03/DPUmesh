#!/bin/bash
# Re-measure a mode's delivered knee with repetitions, on an explicit ladder.
#
# l7_modes.sh walks its ramp once and stops at the first rung a mode fails to
# deliver. One rejected run therefore sets the reported capacity, and a rung is
# rejected by a transient as readily as by a limit. This offers each rung
# REPS times and calls it delivered only when every repetition delivers, so a
# knee that moves between runs shows up as a knee that moves rather than as a
# number.
#
# The mode must already be deployed; this changes nothing on the DPU.
#
#   usage: ./bench/suite/l7_knee.sh <dataset-dir> <mode> <frame> "<rung> ..."
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
BENCH_HEADER_BYTES=16
THREADS="${THREADS:-8}"
DUR="${DUR:-4}"
WARMUP="${WARMUP:-200}"
ARRIVAL="${ARRIVAL:-const}"
REPS="${REPS:-2}"
MIN_ACHIEVED_RATIO="${MIN_ACHIEVED_RATIO:-0.98}"
MAX_DROP_RATIO="${MAX_DROP_RATIO:-0.001}"
TARGET_APP="${TARGET_APP:-bench-dpumesh}"

DATASET="${1:-}"; MODE="${2:-}"; FRAME="${3:-}"; RUNGS="${4:-}"
[ -n "$DATASET" ] && [ -n "$MODE" ] && [ -n "$FRAME" ] && [ -n "$RUNGS" ] ||
  { echo "usage: $0 <dataset-dir> <mode> <frame> \"<rung> ...\"" >&2; exit 2; }

log()  { printf '\033[0;34m[knee]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[knee]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[0;31m[knee]\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$DATASET" ] || die "no dataset at $DATASET"
RAW="$DATASET/raw"; mkdir -p "$RAW"
OUT="$DATASET/knee_verify.csv"
[ -s "$OUT" ] || printf 'mode,frame,offered_rps,rep,achieved_rps,ratio,drops,overflow,p99_us,delivered\n' >"$OUT"

body=$(( FRAME - BENCH_HEADER_BYTES ))
field() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1"; }

ip=$(kubectl get pods -n "$NS" -l "app=$TARGET_APP" \
       -o jsonpath='{.items[?(@.status.phase=="Running")].status.podIP}' 2>/dev/null | awk '{print $1}')
[ -n "$ip" ] || die "no running $TARGET_APP pod"

knee=0
for offered in $RUNGS; do
  all=1
  for rep in $(seq 1 "$REPS"); do
    result=$(printf 'OPEN %s %s %s %s %s %s %s\n' \
               "$body" "$body" "$THREADS" "$DUR" "$WARMUP" "$offered" "$ARRIVAL" |
             timeout "$((DUR + 60))s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null || echo "ERR control")
    printf '%s\n' "$result" >"$RAW/knee-$MODE-$FRAME-$offered-r$rep.txt"
    case "$result" in ERR*|"") warn "$MODE/$FRAME at $offered r$rep: $result"; all=0; break ;; esac
    ach=$(awk -v v="$(field "$result" mrps)" 'BEGIN{printf "%d", v*1000000}')
    drops=$(field "$result" drops); ov=$(field "$result" overflow); rcnt=$(field "$result" rcnt)
    [ -n "$drops" ] || drops=0; [ -n "$ov" ] || ov=0; [ -n "$rcnt" ] || rcnt=1
    ratio=$(awk -v a="$ach" -v o="$offered" 'BEGIN{printf "%.4f", (o>0)?a/o:0}')
    ok=1
    awk -v r="$ratio" -v m="$MIN_ACHIEVED_RATIO" 'BEGIN{exit !(r < m)}' && ok=0
    awk -v d="$drops" -v n="$rcnt" -v m="$MAX_DROP_RATIO" 'BEGIN{exit !(n>0 && d/n > m)}' && ok=0
    [ "$ov" = 0 ] || ok=0
    [ "$ok" = 1 ] || all=0
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$MODE" "$FRAME" "$offered" "$rep" \
      "$ach" "$ratio" "$drops" "$ov" "$(field "$result" p99)" "$ok" >>"$OUT"
    log "  $MODE f=$FRAME offered=$offered r$rep achieved=$ach ratio=$ratio drops=$drops ov=$ov -> $ok"
  done
  if [ "$all" = 1 ]; then knee="$offered"; else break; fi
done

log "$MODE f=$FRAME: every repetition delivered up to $knee"
echo "$knee"
