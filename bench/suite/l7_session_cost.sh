#!/bin/bash
# Price a DMesh session on the target hardware, from two directions.
#
# A DMesh frontend connection takes its outbound stack from a per-target cache; a
# miss builds a complete one (the per-target closure in `Config::build` calls
# `outbound.mk`, constructing discovery, policy, endpoint and reconnect caches). Two other
# paths are linear in live sessions: `Worker::poll_internal` walks every session
# on each runtime poll, and `Backends::take_session` scans the published
# services. A steady one-connection point hides all three, because it builds one
# session and then reuses it.
#
#   churn  varies the reconnect period at a fixed load, so sessions per second
#          moves while everything else holds. The slope of ARM CPU against it is
#          what building and tearing down one session costs.
#   count  varies the client thread count while holding the total outstanding
#          request window fixed (`conc` is per thread, so conc x threads is the
#          load). Sessions move; offered work does not. A rise in CPU per
#          request is what carrying a session costs, separate from building it.
#
# Usage:
#   ./bench/suite/l7_session_cost.sh --out DIR [options]
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${GREEN}[sess]${NC} $*"; }
warn() { echo -e "${YELLOW}[sess]${NC} $*" >&2; }
err()  { echo -e "${RED}[sess]${NC} $*" >&2; }
step() { echo -e "${BLUE}[sess]${NC} $*"; }
die()  { err "$*"; exit 1; }

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"
# shellcheck disable=SC1091
[ -f "$PROJ_ROOT/.env" ] && { set -a; . "$PROJ_ROOT/.env"; set +a; }
: "${DPU_HOST:?.env must define DPU_HOST}"
: "${DPU_PASS:?.env must define DPU_PASS}"

OUT=""
# 0 is the steady point: no reconnect at all. The others reconnect a client
# after that many completions, so a smaller period is more sessions per second.
PERIODS="0 4000 1000 250 60"
# Thread counts for the count sweep. Each runs at conc = WINDOW / threads.
THREAD_COUNTS="1 2 4 8"
WINDOW=128
REPS=3
REQ=1024
REPLY=8
CONC=8
DUR=20
WARMUP=200
THREADS=1

usage() {
  cat <<EOF
Usage: $0 --out DIR [options]

  --out DIR         write churn.csv, count.csv, fit.txt and per-point logs
  --periods "..."   churn: reconnect periods; 0 means never (default "$PERIODS")
  --threads-list ".." count: client thread counts (default "$THREAD_COUNTS")
  --window N        count: total outstanding requests held fixed (default $WINDOW)
  --reps N          repetitions per point (default $REPS)
  --req B           request bytes (default $REQ)
  --reply B         reply bytes (default $REPLY)
  --conc N          churn: outstanding per thread (default $CONC)
  --dur S           measured seconds (default $DUR)
  --threads N       churn: client threads (default $THREADS)

The deployment is used as it stands; this measures one build, not two.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)           OUT="$2"; shift 2 ;;
    --periods)       PERIODS="$2"; shift 2 ;;
    --threads-list)  THREAD_COUNTS="$2"; shift 2 ;;
    --window)        WINDOW="$2"; shift 2 ;;
    --reps)          REPS="$2"; shift 2 ;;
    --req)           REQ="$2"; shift 2 ;;
    --reply)         REPLY="$2"; shift 2 ;;
    --conc)          CONC="$2"; shift 2 ;;
    --dur)           DUR="$2"; shift 2 ;;
    --threads)       THREADS="$2"; shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *)               usage; die "unknown argument: $1" ;;
  esac
done
[ -n "$OUT" ] || { usage; die "--out is required"; }
mkdir -p "$OUT"

# Record the deployment this run measured. bench.sh leaves its launch command in
# /tmp/start_dpu_bench.sh and the L7 environment in /tmp/dpumesh-l7.env, and the
# proxy prints its resolved topology at WARN.
record_provenance() {
  local out="$OUT/provenance.txt"
  {
    echo "tree $(git -C "$PROJ_ROOT" rev-parse HEAD) $(git -C "$PROJ_ROOT" status --short | tr '\n' ' ')"
    echo "request ${REQ}B reply ${REPLY}B window $WINDOW dur ${DUR}s warmup $WARMUP reps $REPS"
    echo "backends $(kubectl get pods -n "${NS:-test-bench}" -l app=echo-dpumesh \
                       --no-headers 2>/dev/null | wc -l) Pod(s) behind echo-dpumesh"
  } >"$out" 2>&1
  # shellcheck disable=SC1091
  { set -a; . "$PROJ_ROOT/.env"; set +a; }
  sshpass -p "$DPU_PASS" ssh -o StrictHostKeyChecking=no "$DPU_HOST" '
    echo "--- launch ---"
    tr " " "\n" < /tmp/start_dpu_bench.sh 2>/dev/null | grep -E "^(DPUMESH|DMESH|L7)_" || echo "n/a"
    echo "--- l7 env ---"
    grep -E "^(DPUMESH|DMESH)_" /tmp/dpumesh-l7.env 2>/dev/null || echo "n/a"
  ' >>"$out" 2>&1 || warn "provenance: DPU launch read failed"
  "$BENCH" dpulog 400 2>/dev/null | grep -a "PROXY MODE ON" | tail -1 >>"$out" 2>&1 || true
  info "-> $out"
}

CHURN="$OUT/churn.csv"
COUNT="$OUT/count.csv"
echo "period,rep,mrps,p50_us,p99_us,requests,reconns,durs,dpu_core_pct,arm_core_us_per_req" >"$CHURN"
echo "threads,conc,rep,mrps,p50_us,p99_us,requests,durs,dpu_core_pct,arm_core_us_per_req" >"$COUNT"

record_provenance

field() {
  sed -n "s/.*[[:space:]]${2}=\([^[:space:]]*\).*/\1/p" <<<"$1" | head -1
}

# Total CPU ticks the DPU process has used, across every one of its threads.
dpu_ticks() {
  sshpass -p "$DPU_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$DPU_HOST" \
    "echo '$DPU_PASS' | sudo -S bash -c '
      pid=\$(pgrep -x dpumesh_dpu | head -1); [ -z \"\$pid\" ] && exit 1
      total=0
      for task in /proc/\$pid/task/*; do
        stat=\$(<\"\$task/stat\"); rest=\${stat#*) }; set -- \$rest
        total=\$((total + \${12} + \${13}))
      done
      echo \"\$(getconf CLK_TCK) \$total\"' 2>/dev/null" 2>/dev/null | tail -1
}

# Run one load point around a CPU-tick window and answer the parsed result.
# Prints `ticks hz result` on stdout; a point that failed a gate returns 1.
measure_point() {
  local conc="$1" threads="$2" reconn="$3" tag="$4"
  local before after hz t0 t1 result
  before=$(dpu_ticks) || { err "the DPU process is not running"; return 1; }
  read -r hz t0 <<<"$before"
  if [ "$reconn" = 0 ]; then
    result=$("$BENCH" point dpumesh "$REQ" "$REPLY" "$conc" "$DUR" "$WARMUP" "$threads")
  else
    result=$("$BENCH" point dpumesh "$REQ" "$REPLY" "$conc" "$DUR" "$WARMUP" "$threads" "$reconn")
  fi
  after=$(dpu_ticks)
  read -r _ t1 <<<"$after"
  echo "$result" >"$OUT/point-$tag.log"

  [[ "$result" == OK* ]] || { err "$tag: $result"; return 1; }
  local fail drops reorder wfail
  fail=$(field "$result" fail); drops=$(field "$result" drops)
  reorder=$(field "$result" reorder); wfail=$(field "$result" worker_fail)
  [ "${fail:-1}" = 0 ] && [ "${drops:-1}" = 0 ] && [ "${reorder:-1}" = 0 ] && [ "${wfail:-1}" = 0 ] || {
    err "$tag invalid: fail=$fail drops=$drops reorder=$reorder worker_fail=$wfail"
    return 1
  }
  printf '%s %s %s\n' "$((t1 - t0))" "$hz" "$result"
}

# ARM core-microseconds per request. The ticks span the connection setup and
# warmup that precede the measured window, so the absolute value sits above one
# taken from the window alone; every point here carries the same structure.
per_request_us() {
  awk -v d="$1" -v h="$2" -v n="$3" 'BEGIN{ if (h>0 && n>0) printf "%.3f", 1e6*d/h/n; else printf "" }'
}

core_pct() {
  awk -v d="$1" -v h="$2" -v s="$3" 'BEGIN{ if (h>0 && s>0) printf "%.2f", 100*d/h/s; else printf "" }'
}

step "=== churn sweep: sessions per second at a fixed load ==="
for period in $PERIODS; do
  for rep in $(seq 1 "$REPS"); do
    line=$(measure_point "$CONC" "$THREADS" "$period" "p${period}-r${rep}") || exit 1
    ticks=${line%% *}; rest=${line#* }; hz=${rest%% *}; result=${rest#* }
    durs=$(field "$result" durs); n=$(field "$result" rcnt)
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$period" "$rep" "$(field "$result" mrps)" "$(field "$result" p50)" \
      "$(field "$result" p99)" "$n" "$(field "$result" reconns)" "$durs" \
      "$(core_pct "$ticks" "$hz" "$durs")" "$(per_request_us "$ticks" "$hz" "$n")" >>"$CHURN"
    info "  period=$period rep=$rep mrps=$(field "$result" mrps) reconns=$(field "$result" reconns) us/req=$(per_request_us "$ticks" "$hz" "$n")"
  done
done

step "=== count sweep: live sessions at a fixed outstanding window ==="
for threads in $THREAD_COUNTS; do
  conc=$((WINDOW / threads))
  [ "$conc" -ge 1 ] || { warn "window $WINDOW gives no room for $threads threads; skipped"; continue; }
  for rep in $(seq 1 "$REPS"); do
    line=$(measure_point "$conc" "$threads" 0 "t${threads}-r${rep}") || exit 1
    ticks=${line%% *}; rest=${line#* }; hz=${rest%% *}; result=${rest#* }
    durs=$(field "$result" durs); n=$(field "$result" rcnt)
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$threads" "$conc" "$rep" "$(field "$result" mrps)" "$(field "$result" p50)" \
      "$(field "$result" p99)" "$n" "$durs" \
      "$(core_pct "$ticks" "$hz" "$durs")" "$(per_request_us "$ticks" "$hz" "$n")" >>"$COUNT"
    info "  threads=$threads conc=$conc rep=$rep mrps=$(field "$result" mrps) us/req=$(per_request_us "$ticks" "$hz" "$n")"
  done
done

python3 - "$CHURN" "$COUNT" >"$OUT/fit.txt" <<'PY'
import csv, statistics, sys

def load(path):
    rows = []
    for r in csv.DictReader(open(path)):
        try:
            rows.append({k: float(v) if v not in ("", None) else None for k, v in r.items()})
        except ValueError:
            continue
    return rows

churn = load(sys.argv[1])
count = load(sys.argv[2])

def med(rows, key):
    vals = [r[key] for r in rows if r.get(key) is not None]
    return statistics.median(vals) if vals else float("nan")

print("What a DMesh session costs\n")
print("## Building one: reconnect churn at a fixed load\n")
print("| reconnect period | sessions/s | Mrps | ARM us/req | p99 us |")
print("|---|---|---|---|---|")
rows = []
for p in {r["period"] for r in churn}:
    grp = [r for r in churn if r["period"] == p]
    sps = statistics.median(r["reconns"] / r["durs"] for r in grp if r["durs"])
    rows.append((sps, p, med(grp, "arm_core_us_per_req"), med(grp, "mrps"), med(grp, "p99_us")))
# Ordered by the axis the fit uses, so a shorter period reads as more churn
# rather than as a smaller number.
rows.sort()
pts = [(sps, us, mrps) for sps, _, us, mrps, _ in rows]
for sps, p, us, mrps, p99 in rows:
    print(f"| {'never' if p == 0 else int(p)} | {sps:.1f} | {mrps:.6f} | {us:.3f} | {p99:.1f} |")
print()

# ARM cores against sessions per second: intercept is the steady work, slope is
# one session. Cores = us/req x Mrps, both medians of the same point.
xy = [(sps, us * mrps) for sps, us, mrps in pts]
n = len(xy)
if n >= 2:
    sx = sum(a for a, _ in xy); sy = sum(b for _, b in xy)
    sxx = sum(a * a for a, _ in xy); sxy = sum(a * b for a, b in xy)
    d = n * sxx - sx * sx
    if d:
        slope = (n * sxy - sx * sy) / d
        intercept = (sy - slope * sx) / n
        print(f"DPU cores = {intercept:.3f} + {slope*1e3:.4f}e-3 x sessions/s")
        print(f"building and tearing down one session costs {slope*1e6:.0f} ARM core-us")
        steady = pts[0][1] if pts else None
        if steady and slope > 0:
            print(f"which is worth about {slope*1e6/steady:,.0f} requests at {steady:.2f} us/req")
print()
print("## Carrying them: live sessions at a fixed outstanding window\n")
print("| threads | conc | live sessions | Mrps | ARM us/req | p99 us |")
print("|---|---|---|---|---|---|")
base = None
for t in sorted({r["threads"] for r in count}):
    grp = [r for r in count if r["threads"] == t]
    us = med(grp, "arm_core_us_per_req")
    if base is None:
        base = us
    delta = f"{(us - base) / base * 100:+.1f}%" if base else ""
    print(f"| {int(t)} | {int(med(grp, 'conc'))} | {int(t)} | {med(grp, 'mrps'):.6f} | "
          f"{us:.3f} ({delta}) | {med(grp, 'p99_us'):.1f} |")
print()
print("The outstanding request window is the same on every row of the second")
print("table, so a rising us/req is what more live sessions cost, not more load.")
print("Every point carried fail=0, drops=0, reorder=0 and worker_fail=0.")
PY
info "-> $OUT/fit.txt"
cat "$OUT/fit.txt"
