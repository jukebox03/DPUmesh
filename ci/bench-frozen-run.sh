#!/bin/bash
# Measure every operating point in ci/bench-frozen.txt and append one JSON
# object per point to the file named by $1 (default: stdout).
#
#   ci/bench-frozen-run.sh out.jsonl
#
# A point that does not come back OK is skipped, not recorded — a series with a
# failed run silently folded into it is worse than a gap. The script still exits
# non-zero so the gap is visible in CI instead of only in the data.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-/dev/stdout}"
# Overridable only so the script itself can be exercised on a short set; the
# published series always reads the committed file.
FROZEN="${FROZEN:-$ROOT/ci/bench-frozen.txt}"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD)"
STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
HOST="$(hostname)"

# What the DPU is, read once before the first point. A point that cannot say
# which machine produced it is not comparable to any other point, so failing
# here is better than recording one.
mapfile -t CFG < <("$ROOT/ci/bench-config.sh") || exit 1
[ "${#CFG[@]}" -gt 0 ] || { echo "[fail]  no configuration fingerprint; refusing to measure" >&2; exit 1; }
printf '[config] %s\n' "${CFG[@]}" >&2

[ "$OUT" = /dev/stdout ] || : >>"$OUT"

failed=0 recorded=0
while read -r label sol req reply conc dur warmup threads; do
    case "$label" in ''|\#*) continue;; esac
    echo "[point] $label: $sol req=$req reply=$reply conc=$conc dur=${dur}s threads=$threads" >&2
    raw="$("$ROOT/bench/bench.sh" point "$sol" "$req" "$reply" "$conc" "$dur" "$warmup" "$threads" 2>&1 </dev/null)"
    if json="$(printf '%s\n' "$raw" | "$ROOT/ci/bench-to-json.py" \
            label="$label" solution="$sol" commit="$COMMIT" ts="$STAMP" host="$HOST" \
            "${CFG[@]}")"; then
        printf '%s\n' "$json" >>"$OUT"
        recorded=$((recorded + 1))
    else
        echo "[skip]  $label did not produce a point" >&2
        failed=$((failed + 1))
    fi
done < "$FROZEN"

echo "[done]  recorded=$recorded skipped=$failed -> $OUT" >&2
[ "$failed" -eq 0 ]
