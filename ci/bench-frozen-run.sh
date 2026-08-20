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

# One ARM data worker is what the DPU runs when the deploy passed no topology at
# all, so it is far more often an unset variable than a decision. The run still
# proceeds — the fingerprint keeps the series honest either way — but it says so.
case " ${CFG[*]} " in
    *" cfg_workers=1 "*)
        echo "[warn]  the DPU is running ONE ARM data worker." >&2
        echo "[warn]  A campaign is normally deployed with:" >&2
        echo "[warn]    DPUMESH_DPA_THREADS=32 DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=8 \\" >&2
        echo "[warn]    BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=grpc bash bench/bench.sh deploy" >&2
        ;;
esac

[ "$OUT" = /dev/stdout ] || : >>"$OUT"

# A gRPC campaign and an L4 campaign cannot be deployed at once: the gRPC scope
# starts only the L7 paths so no other backend enters the DPU registry while it
# runs. So this file holds the rows for both, and a row whose client Pod is not
# deployed is skipped rather than failed. The run fails only if nothing at all
# was measured — and the two campaigns never mix in one series anyway, because
# their pod sets hash to different config_ids.
failed=0 recorded=0 absent=0
while read -r label sol req reply conc dur warmup threads; do
    case "$label" in ''|\#*) continue;; esac
    echo "[point] $label: $sol req=$req reply=$reply conc=$conc dur=${dur}s threads=$threads" >&2
    raw="$("$ROOT/bench/bench.sh" point "$sol" "$req" "$reply" "$conc" "$dur" "$warmup" "$threads" 2>&1 </dev/null)"
    if json="$(printf '%s\n' "$raw" | "$ROOT/ci/bench-to-json.py" \
            label="$label" solution="$sol" commit="$COMMIT" ts="$STAMP" host="$HOST" \
            "${CFG[@]}")"; then
        printf '%s\n' "$json" >>"$OUT"
        recorded=$((recorded + 1))
    elif case "$raw" in *"ERR no_pod("*) true;; *) false;; esac; then
        echo "[absent] $label: $sol is not deployed in this campaign" >&2
        absent=$((absent + 1))
    else
        echo "[skip]  $label did not produce a point" >&2
        failed=$((failed + 1))
    fi
done < "$FROZEN"

echo "[done]  recorded=$recorded absent=$absent failed=$failed -> $OUT" >&2
[ "$recorded" -gt 0 ] || { echo "[fail]  no operating point produced a measurement" >&2; exit 1; }
[ "$failed" -eq 0 ]
