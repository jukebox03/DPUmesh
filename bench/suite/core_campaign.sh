#!/bin/bash
# Drive core_profile.sh over one evaluation family, serially.
#
# Each configuration is profiled at three offered rates for the 64 B frame and
# at one rate for the 1 KiB frame. Repetitions are the outer loop.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${GREEN}[camp]${NC} $*"; }
step() { echo -e "${BLUE}[camp]${NC} $*"; }
die()  { echo -e "${RED}[camp]${NC} $*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FAMILY=""
OUT=""
REPS=3
REP_FROM=1
REP_TO=0
CONFIGS=""

usage() {
  cat <<EOF
Usage: $0 --family l4|grpc --out DIR [options]

  --reps N          repetitions per point (default $REPS)
  --rep-from N      first repetition to run (default $REP_FROM)
  --rep-to N        last repetition to run (default: --reps)
  --configs "a b"   restrict to these configurations
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --family)   FAMILY="$2"; shift 2 ;;
    --out)      OUT="$2"; shift 2 ;;
    --reps)     REPS="$2"; shift 2 ;;
    --rep-from) REP_FROM="$2"; shift 2 ;;
    --rep-to)   REP_TO="$2"; shift 2 ;;
    --configs)  CONFIGS="$2"; shift 2 ;;
    -h|--help)  usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[ -n "$FAMILY" ] && [ -n "$OUT" ] || { usage >&2; exit 2; }
[ "$REP_TO" -gt 0 ] || REP_TO=$REPS

case "$FAMILY" in
  l4)
    ALL="dpumesh-native dpumesh-preload envoy-permissive envoy-strict"
    LOADS_64="120000 260000 400000"
    LOADS_1K="300000"
    ;;
  grpc)
    ALL="grpc-dpumesh grpc-tcp grpc-envoy-permissive grpc-envoy-strict"
    LOADS_64="8000 16000 24000"
    LOADS_1K="20000"
    ;;
  *) die "family must be l4 or grpc" ;;
esac
[ -n "$CONFIGS" ] || CONFIGS="$ALL"

# Cycles between samples; a point lands near ten thousand samples.
period_of() {
  case "$1" in
    dpumesh-native)        echo 1000000 ;;
    grpc-dpumesh)          echo 3000000 ;;
    grpc-tcp)              echo 3500000 ;;
    *)                     echo 4000000 ;;
  esac
}

mkdir -p "$OUT"
for rep in $(seq "$REP_FROM" "$REP_TO"); do
  for config in $CONFIGS; do
    period=$(period_of "$config")
    for spec in $(for r in $LOADS_64; do echo "48:$r"; done) \
                $(for r in $LOADS_1K; do echo "1008:$r"; done); do
      body=${spec%%:*}; rate=${spec##*:}
      dir="$OUT/${config}_${body}b_${rate}_r${rep}"
      if [ -s "$dir/meta.txt" ]; then
        info "skip $(basename "$dir") (already collected)"
        continue
      fi
      step "$config body=$body rate=$rate rep=$rep"
      bash "$ROOT/bench/suite/core_profile.sh" --config "$config" --out "$dir" \
        --body "$body" --rate "$rate" --period "$period" --rep "$rep" ||
        { rm -rf "$dir"; info "FAILED $config $body $rate rep$rep"; }
      sleep 5
    done
  done
done
info "campaign $FAMILY reps $REP_FROM-$REP_TO complete -> $OUT"
