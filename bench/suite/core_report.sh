#!/bin/bash
# Turn raw core_profile.sh runs into the published attribution artefacts.
#
#   data/     per-layer, per-site, owner x site, per-point and top-symbol CSVs
#   flame/    one flame graph per configuration, frame size, rate and endpoint,
#             coloured by the layer each frame belongs to
#   figures/  the stacked-core, per-request and load-curve figures
#
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
info() { echo -e "${GREEN}[core]${NC} $*"; }
die()  { echo -e "${RED}[core]${NC} $*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FLAMEGRAPH="${FLAMEGRAPH:-$HOME/FlameGraph/flamegraph.pl}"
OUT=""
STEM="core"
RUNS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)  OUT="$2"; shift 2 ;;
    --stem) STEM="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: $0 RUNDIR... --out REPORTDIR [--stem NAME]"; exit 0 ;;
    *) RUNS+=("$1"); shift ;;
  esac
done
[ -n "$OUT" ] || die "--out is required"
[ "${#RUNS[@]}" -gt 0 ] || die "at least one run directory is required"
[ -x "$FLAMEGRAPH" ] || die "flamegraph.pl not found at $FLAMEGRAPH (set FLAMEGRAPH)"

DATA="$OUT/data/$STEM"
FLAME="$OUT/flame"
mkdir -p "$DATA" "$FLAME" "$OUT/figures"

info "classifying ${#RUNS[@]} runs"
python3 "$ROOT/bench/suite/core_layers.py" "${RUNS[@]}" --out "$DATA" --fold

info "rendering flame graphs"
FLAME="$(cd "$FLAME" && pwd)"
( cd "$DATA"
  for folded in *.folded; do
    [ -e "$folded" ] || continue
    stem="${folded%.folded}"
    role="${stem##*_}"; point="${stem%_*}"
    "$FLAMEGRAPH" --cp --width 1500 --minwidth 0.4 --fontsize 11 \
      --countname samples --title "$point — $role core" \
      --subtitle "blue application · aqua transport library · yellow gRPC runtime · pink Envoy · green verbs/DOCA driver · violet libc + vDSO · grey kernel" \
      "$folded" >"$FLAME/$stem.svg"
  done )

info "rendering figures"
python3 "$ROOT/bench/suite/plot_core.py" "$DATA" --out "$OUT/figures" \
  --stem "$STEM"

rm -f "$DATA"/*.folded
info "-> $OUT"
