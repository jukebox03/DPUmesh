#!/bin/bash
# Compare the two Linkerd output paths on the target hardware.
#
# The adapter publishes a session's output either by reserving a chunk of the
# DPUmesh egress arena and copying into it once (`DMESH_L7_TX_RESERVE=1`), or by
# draining the endpoint into a temporary buffer and handing that to
# `dmesh_l7_send` (`DMESH_L7_TX_RESERVE=0`). The paths differ in one copy per
# published byte, so what separates them is ARM CPU per request rather than
# correctness.
#
# The setting is read when the DPU process starts, so each arm is its own
# deployment of the same tree. Arms run one after another: two DPU processes
# cannot share the hardware, and a run that overlapped another would attribute
# its cores to the wrong arm.
#
# Each point is an ARM-balance run, so a row carries the load result, the DPU's
# own per-thread CPU over the same window and the per-worker session deltas.
# `bench.sh armbalance` already refuses a point with a nonzero fail, drop or
# reorder count, and refuses a worker that received no session under `all`.
#
# Usage:
#   ./bench/suite/l7_tx_ab.sh --out DIR [options]
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${GREEN}[ab]${NC} $*"; }
warn() { echo -e "${YELLOW}[ab]${NC} $*" >&2; }
err()  { echo -e "${RED}[ab]${NC} $*" >&2; }
step() { echo -e "${BLUE}[ab]${NC} $*"; }
die()  { err "$*"; exit 1; }

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"

OUT=""
ARMS="1 0"
CONCS="1 32 128"
REPS=3
REQ=1024
REPLY=8
DUR=10
THREADS=1
DEPLOY=1

# The stack every arm is measured on. It is the one the ARM profile note used,
# so a row here can be read against that note.
DPA_THREADS="${DPUMESH_DPA_THREADS:-32}"
RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-8}"
ARM_WORKERS="${DPUMESH_ARM_WORKERS:-4}"
LINKERD_WORKER="${DPUMESH_L7_LINKERD_WORKER:-all}"
OPAQUE_SVC="${DPUMESH_L7_OPAQUE_SVC:-11,13,14}"

usage() {
  cat <<EOF
Usage: $0 --out DIR [options]

  --out DIR       write points.csv, summary.txt, provenance and per-point logs
  --arms "1 0"    DMESH_L7_TX_RESERVE values to compare (default "$ARMS")
  --concs "..."   closed-loop concurrencies (default "$CONCS")
  --reps N        repetitions per point (default $REPS)
  --req B         request bytes (default $REQ)
  --reply B       reply bytes (default $REPLY)
  --dur S         measured seconds per repetition (default $DUR)
  --threads N     client threads (default $THREADS)
  --no-deploy     measure the running deployment; only valid for a single arm
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)       OUT="$2"; shift 2 ;;
    --arms)      ARMS="$2"; shift 2 ;;
    --concs)     CONCS="$2"; shift 2 ;;
    --reps)      REPS="$2"; shift 2 ;;
    --req)       REQ="$2"; shift 2 ;;
    --reply)     REPLY="$2"; shift 2 ;;
    --dur)       DUR="$2"; shift 2 ;;
    --threads)   THREADS="$2"; shift 2 ;;
    --no-deploy) DEPLOY=0; shift ;;
    -h|--help)   usage; exit 0 ;;
    *)           usage; die "unknown argument: $1" ;;
  esac
done

[ -n "$OUT" ] || { usage; die "--out is required"; }
if [ "$DEPLOY" = 0 ] && [ "$(wc -w <<<"$ARMS")" -gt 1 ]; then
  die "--no-deploy measures one deployment, so it admits a single arm"
fi

mkdir -p "$OUT"
POINTS="$OUT/points.csv"
echo "arm,conc,rep,mrps,p50_us,p99_us,requests,dpu_total_pct,dpu_workers_pct,worker_cv_pct,arm_core_us_per_req" >"$POINTS"

# Read one `key=value` field out of a benchmark result line.
field() {
  local line="$1" key="$2"
  sed -n "s/.*[[:space:]]${key}=\([^[:space:]]*\).*/\1/p" <<<"$line" | head -1
}

deploy_arm() {
  local reserve="$1"
  step "=== deploying arm DMESH_L7_TX_RESERVE=$reserve ==="
  L7_BACKEND=linkerd \
  DPUMESH_L7_OPAQUE_SVC="$OPAQUE_SVC" \
  DPUMESH_L7_LINKERD_WORKER="$LINKERD_WORKER" \
  DPUMESH_ARM_WORKERS="$ARM_WORKERS" \
  DPUMESH_DPA_THREADS="$DPA_THREADS" \
  DPUMESH_RINGS_PER_POD="$RINGS_PER_POD" \
  DPUMESH_L7_FAIL_CLOSED=1 \
  BENCH_DEPLOY_SCOPE=core \
  DMESH_L7_TX_RESERVE="$reserve" \
    "$BENCH" deploy >"$OUT/deploy-reserve$reserve.log" 2>&1 ||
      die "deploy failed for arm $reserve; see $OUT/deploy-reserve$reserve.log"
}

# What the arm actually ran on. A comparison without this is not reproducible.
record_provenance() {
  # One assignment per statement: bash expands a `local` list before it assigns,
  # so a second word may not read the first.
  local reserve="$1"
  local out="$OUT/provenance-reserve$reserve.txt"
  {
    echo "arm DMESH_L7_TX_RESERVE=$reserve"
    echo "tree $(git -C "$PROJ_ROOT" rev-parse HEAD) $(git -C "$PROJ_ROOT" status --short | tr '\n' ' ')"
    echo "request ${REQ}B reply ${REPLY}B threads $THREADS dur ${DUR}s reps $REPS"
    echo "stack dpa=$DPA_THREADS rings=$RINGS_PER_POD workers=$ARM_WORKERS l7_worker=$LINKERD_WORKER opaque=$OPAQUE_SVC"
    echo "--- DPU ---"
    "$BENCH" dpulog 400 2>/dev/null | grep -iE "N=|workers|l7 |linkerd" | tail -20 || true
  } >"$out" 2>&1
  # Clock and affinity belong to the DPU, so they are read there.
  # shellcheck disable=SC1091
  { set -a; . "$PROJ_ROOT/.env"; set +a; }
  sshpass -p "$DPU_PASS" ssh -o StrictHostKeyChecking=no "$DPU_HOST" '
    echo "--- ARM clocks ---"
    for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
      [ -r "$c" ] || continue
      echo "$c $(cat "$c")"
    done
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "cores: $(nproc)"
    pid=$(pgrep -x dpumesh_dpu | head -1)
    [ -n "$pid" ] && echo "affinity: $(taskset -pc "$pid" 2>/dev/null || echo n/a)"
  ' >>"$out" 2>&1 || warn "provenance: DPU clock read failed"
  info "-> $out"
}

# Everything a quiesced arm must report, and everything it must not have logged.
# `bench.sh` reads the worker selection from its caller's environment, so the
# poll must name it to reach every worker's admin port. The ring count travels
# with it: the worker count is clamped to divide the rings, so naming the
# workers alone silently polls fewer of them. It is named here and not on the
# point runs: `armbalance` under `all` also requires every worker to have
# received a session, which one client thread cannot satisfy and which is not
# what this comparison measures.
check_arm_quiescence() {
  local reserve="$1"
  local out="$OUT/quiescence-reserve$reserve.txt"
  {
    echo "--- l7metrics ---"
    DPUMESH_L7_LINKERD_WORKER="$LINKERD_WORKER" DPUMESH_ARM_WORKERS="$ARM_WORKERS" \
    DPUMESH_RINGS_PER_POD="$RINGS_PER_POD" \
      "$BENCH" l7metrics 2>&1 || true
    echo "--- DPU log audit ---"
    "$BENCH" dpulog 600 2>/dev/null |
      grep -iE "fallback|over_release|stray_release|single-session-limit|unknown-reply|poison|orphan" |
      tail -40 || echo "(no audit line)"
  } >"$out" 2>&1
  local bad=0
  while read -r worker opened closed active pending tasks orphaned; do
    case "$worker" in WORKER|"") continue ;; esac
    [ "${active:-0}" = 0 ] && [ "${pending:-0}" = 0 ] && [ "${tasks:-0}" = 0 ] || {
      err "worker $worker did not quiesce: active=$active pending=$pending tasks=$tasks"; bad=1; }
    [ "${opened:-0}" = "${closed:-0}" ] || {
      err "worker $worker opened=$opened closed=$closed"; bad=1; }
    [ "${orphaned:-0}" = 0 ] || { err "worker $worker orphaned=$orphaned"; bad=1; }
  done < <(sed -n '/^WORKER/,/^---/p' "$out" | grep -vE '^---')
  info "-> $out"
  return "$bad"
}

run_arm() {
  local reserve="$1" conc rep line result total workers cv requests per_req
  for conc in $CONCS; do
    for rep in $(seq 1 "$REPS"); do
      local log="$OUT/point-reserve${reserve}-c${conc}-r${rep}.log"
      step "arm=$reserve conc=$conc rep=$rep"
      L7_BACKEND=linkerd "$BENCH" armbalance "$REQ" "$REPLY" "$conc" "$DUR" "$THREADS" \
        "$OUT/threads-reserve${reserve}-c${conc}-r${rep}.csv" >"$log" 2>&1 || {
          err "point failed (arm=$reserve conc=$conc rep=$rep); see $log"
          return 1
        }
      result=$(grep -m1 '^ *OK ' "$log" || true)
      [ -n "$result" ] || { err "no load result in $log"; return 1; }
      line=$(grep -m1 'total process:' "$log" || true)
      total=$(sed -n 's/.*total process: \([0-9.]*\)%.*/\1/p' <<<"$line")
      workers=$(sed -n 's/.*named main\/workers: \([0-9.]*\)%.*/\1/p' <<<"$line")
      cv=$(sed -n 's/.*CV=\([0-9.]*\)%.*/\1/p' <<<"$(grep -m1 'worker balance:' "$log" || true)")
      requests=$(field "$result" rcnt)
      # ARM core-microseconds per request. The duration cancels, leaving the
      # process's CPU ticks over what the measured window completed. The ticks
      # include the connection setup and warmup that precede the window, so the
      # absolute value sits above a figure taken from the load window alone;
      # both arms carry the same structure, so the arm-to-arm delta does not.
      per_req=$(awk -v pct="${total:-0}" -v n="${requests:-0}" -v s="$(field "$result" durs)" \
        'BEGIN{ if (n > 0 && s > 0) printf "%.3f", (pct/100.0)*s*1e6/n; else printf "" }')
      printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$reserve" "$conc" "$rep" \
        "$(field "$result" mrps)" "$(field "$result" p50)" "$(field "$result" p99)" \
        "${requests:-}" "${total:-}" "${workers:-}" "${cv:-}" "${per_req:-}" >>"$POINTS"
      info "  mrps=$(field "$result" mrps) p99=$(field "$result" p99)us dpu=${total:-NA}% us/req=${per_req:-NA}"
    done
  done
}

summarize() {
  python3 - "$POINTS" >"$OUT/summary.txt" <<'PY'
import csv, statistics, sys

rows = list(csv.DictReader(open(sys.argv[1])))
def num(row, key):
    try:
        return float(row[key])
    except (TypeError, ValueError):
        return None

arms = sorted({r["arm"] for r in rows}, reverse=True)
concs = sorted({int(r["conc"]) for r in rows})
metrics = [("mrps", "Mrps", 6), ("p50_us", "p50 us", 1), ("p99_us", "p99 us", 1),
           ("dpu_total_pct", "DPU %", 1), ("arm_core_us_per_req", "ARM us/req", 3)]

print("Linkerd output path A/B (arm 1 = reserve into the egress arena, 0 = copy then send)\n")
for key, label, digits in metrics:
    print(f"## {label}")
    header = "| conc | " + " | ".join(f"arm {a} (median of n)" for a in arms)
    if len(arms) == 2:
        header += " | delta |"
    else:
        header += " |"
    print(header)
    print("|---" * (len(arms) + 1 + (1 if len(arms) == 2 else 0)) + "|")
    for c in concs:
        cells, medians = [], {}
        for a in arms:
            vals = [num(r, key) for r in rows if r["arm"] == a and int(r["conc"]) == c]
            vals = [v for v in vals if v is not None]
            if not vals:
                cells.append("-")
                continue
            med = statistics.median(vals)
            medians[a] = med
            spread = (max(vals) - min(vals)) / med * 100 if med else 0.0
            cells.append(f"{med:.{digits}f} (n={len(vals)}, spread {spread:.1f}%)")
        line = f"| {c} | " + " | ".join(cells)
        if len(arms) == 2 and all(a in medians for a in arms):
            hi, lo = medians[arms[0]], medians[arms[1]]
            line += f" | {(hi - lo) / lo * 100:+.1f}% |" if lo else " | - |"
        else:
            line += " |"
        print(line)
    print()
print("Delta is arm 1 relative to arm 0. A negative ARM us/req or DPU % means the")
print("reservation path costs less. Spread is the range across repetitions as a")
print("share of the median: a delta smaller than the spread is not a result.")
PY
  info "-> $OUT/summary.txt"
}

status=0
for arm in $ARMS; do
  [ "$DEPLOY" = 1 ] && deploy_arm "$arm"
  record_provenance "$arm"
  run_arm "$arm" || { status=1; break; }
  check_arm_quiescence "$arm" || status=1
done
summarize
cat "$OUT/summary.txt"
exit "$status"
