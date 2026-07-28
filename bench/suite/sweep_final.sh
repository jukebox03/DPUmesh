#!/bin/bash
# Reproduce the final topology/load campaign with repetitions and resumable CSVs.
# Usage:
#   ./bench/suite/sweep_final.sh [--dry-run] [--out DIR]
# Re-run with the same OUT/--out directory to skip completed run_ids.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"
CPU_PROBE="$SUITE_DIR/cpu_probe.sh"
ANALYZE="$SUITE_DIR/analyze_sweep.py"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

REPS="${REPS:-3}"
DUR="${DUR:-20}"
BACKEND_REPS="${BACKEND_REPS:-3}"  # three complete backend triplets
BACKEND_DUR="${BACKEND_DUR:-5}"
NUMA_REPS="${NUMA_REPS:-5}"
NUMA_DUR="${NUMA_DUR:-10}"
REQ="${REQ:-8192}"
REPLY="${REPLY:-8}"
CONC="${CONC:-32}"
WARMUP="${WARMUP:-200}"
DRY_RUN=0
OUT="${OUT:-}"

usage() {
  cat <<EOF
Usage: $0 [--dry-run] [--out DIR]

Environment:
  REPS=$REPS DUR=$DUR BACKEND_REPS=$BACKEND_REPS BACKEND_DUR=$BACKEND_DUR
  NUMA_REPS=$NUMA_REPS NUMA_DUR=$NUMA_DUR REQ=$REQ REPLY=$REPLY CONC=$CONC

The default campaign measures N=32 topology, concurrency, TCP/Envoy, fair/hw
pinning, N=8/16/32 backend controls, local/automatic NUMA, and validators.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --out) [ "$#" -ge 2 ] || { echo "--out needs a directory" >&2; exit 2; }
           OUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

is_uint() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }
for value in "$REPS" "$DUR" "$BACKEND_REPS" "$BACKEND_DUR" "$NUMA_REPS" "$NUMA_DUR"; do
  is_uint "$value" || { echo "repeat counts and durations must be positive integers" >&2; exit 2; }
done

OUT="${OUT:-$PROJ_ROOT/bench/report/data/sweep-final-$(date +%Y%m%d-%H%M%S)}"
RESULTS="$OUT/results.csv"
BACKENDS="$OUT/backends.csv"
SUMMARY="$OUT/summary.csv"
BACKEND_SUMMARY="$OUT/backend_summary.csv"
META="$OUT/meta.txt"
LOG="$OUT/run.log"

mkdir -p "$OUT"
exec > >(tee -a "$LOG") 2>&1

log() { printf '[sweep] %s\n' "$*"; }
field() {
  awk -v k="$2" '{for(i=1;i<=NF;i++){p=k"=";if(index($i,p)==1){print substr($i,length(p)+1);exit}}}' <<<"$1"
}
completed() {
  local csv="$1" run_id="$2"
  [ -s "$csv" ] && awk -F, -v id="$run_id" 'NR > 1 && $1 == id { found=1 } END { exit !found }' "$csv"
}
forward_order() {
  local rep="$1" values="$2"
  if (( rep % 2 == 1 )); then
    printf '%s\n' "$values"
  else
    awk '{for(i=NF;i>=1;i--) printf "%s%s", $i, (i==1 ? ORS : OFS)}' <<<"$values"
  fi
}

if [ "$DRY_RUN" = 0 ]; then
  for cmd in kubectl nc ssh awk; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "missing command: $cmd" >&2; exit 1; }
  done
  [ -f "$PROJ_ROOT/.env" ] || { echo "missing $PROJ_ROOT/.env" >&2; exit 1; }
  make -C "$PROJ_ROOT" -j4 test
fi

if [ ! -s "$META" ]; then
  cpu_model="$(awk -F: '/model name/{sub(/^ /,"",$2); print $2; exit}' /proc/cpuinfo)"
  {
    echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit=$(git -C "$PROJ_ROOT" rev-parse HEAD 2>/dev/null || echo NA)"
    echo "dirty_paths=$(git -C "$PROJ_ROOT" status --porcelain 2>/dev/null | wc -l)"
    echo "host=$(uname -n)"
    echo "kernel=$(uname -r)"
    echo "cpu=$cpu_model"
    echo "params=REPS=$REPS DUR=$DUR BACKEND_REPS=$BACKEND_REPS BACKEND_DUR=$BACKEND_DUR NUMA_REPS=$NUMA_REPS NUMA_DUR=$NUMA_DUR REQ=$REQ REPLY=$REPLY CONC=$CONC WARMUP=$WARMUP"
  } > "$META"
fi

deploy_config() {
  local label="$1" n="$2" k="$3" a="$4" policy="$5" scope="$6"
  log "deploy $label: N/K/A=$n/$k/$a numa=$policy scope=$scope"
  if [ "$DRY_RUN" = 1 ]; then return 0; fi
  env \
    DPUMESH_DPA_THREADS="$n" \
    DPUMESH_RINGS_PER_POD="$k" \
    DPUMESH_ARM_WORKERS="$a" \
    DPUMESH_PROXY_L7_SVC= \
    DPUMESH_LOG_LEVEL=40 \
    BENCH_NUMA_POLICY="$policy" \
    BENCH_DEPLOY_SCOPE="$scope" \
    "$BENCH" deploy

  local dpu_log
  dpu_log="$("$BENCH" dpulog 200)"
  grep -q "Connection-affine topology active: N/K/A=$n/$k/$a" <<<"$dpu_log" || {
    echo "live topology mismatch after $label" >&2
    grep -E "DPA auto-detect|Connection-affine topology|Invalid" <<<"$dpu_log" >&2 || true
    exit 1
  }
  grep "Connection-affine topology active: N/K/A=$n/$k/$a" <<<"$dpu_log" | tail -n 1
}

pin_profile() {
  local profile="$1" policy="$2"
  log "pin profile=$profile numa=$policy"
  [ "$DRY_RUN" = 1 ] || BENCH_NUMA_POLICY="$policy" "$BENCH" pin "$profile"
}

run_cpu() {
  local stage="$1" n="$2" k="$3" a="$4" pin="$5" policy="$6"
  local rep="$7" transport="$8" conc="$9" threads="${10}"
  local run_id="${stage}-N${n}K${k}A${a}-${pin}-${policy}-${transport}-c${conc}-t${threads}-r${rep}"
  if completed "$RESULTS" "$run_id"; then
    log "skip completed $run_id"
    return 0
  fi
  log "measure $run_id"
  [ "$DRY_RUN" = 1 ] && return 0
  CPU_RUN_ID="$run_id" CPU_STAGE="$stage" CPU_N="$n" CPU_K="$k" CPU_A="$a" \
    CPU_PIN="$pin" CPU_NUMA_POLICY="$policy" CPU_REP="$rep" ALLOW_REORDER=1 \
    "$CPU_PROBE" "$transport" "$REQ" "$REPLY" "$conc" \
    "$([ "$stage" = numa ] && echo "$NUMA_DUR" || echo "$DUR")" \
    "$threads" "$RESULTS"
}

run_transport_pair() {
  local stage="$1" n="$2" k="$3" a="$4" pin="$5" policy="$6"
  local rep="$7" conc="$8" threads="$9"
  local order="dpumesh tcp"
  (( (rep + conc + threads) % 2 == 0 )) || order="tcp dpumesh"
  local transport
  for transport in $order; do
    run_cpu "$stage" "$n" "$k" "$a" "$pin" "$policy" "$rep" "$transport" "$conc" "$threads"
  done
}

run_backend_point() {
  local n="$1" rep="$2" sequence="$3"
  local run_id="backend-N${n}K8A8-r${rep}-s${sequence}"
  if completed "$BACKENDS" "$run_id"; then
    log "skip completed $run_id"
    return 0
  fi
  log "measure $run_id"
  [ "$DRY_RUN" = 1 ] && return 0

  local result fail reorder waits dist backend requests
  result="$("$BENCH" point dpumesh "$REQ" "$REPLY" "$CONC" "$BACKEND_DUR" "$WARMUP" 1)"
  log "$result"
  [[ "$result" == OK* ]] || { echo "$run_id failed: $result" >&2; exit 1; }
  fail="$(field "$result" fail)"; reorder="$(field "$result" reorder)"
  [ "${fail:-0}" = 0 ] && [ "${reorder:-0}" = 0 ] || {
    echo "$run_id invalid: fail=${fail:-NA} reorder=${reorder:-NA}" >&2
    exit 1
  }
  waits="$(field "$result" waits)"; waits="${waits:-0}"
  dist="$(field "$result" dist)"
  [[ "$dist" =~ ^[0-2]:[0-9]+$ ]] || {
    echo "$run_id expected exactly one backend, got dist=${dist:-empty}" >&2
    exit 1
  }
  backend="${dist%%:*}"; requests="${dist#*:}"
  [ -s "$BACKENDS" ] || \
    echo "run_id,n,k,a,rep,sequence,backend,requests,mrps,gbps,p50,p99,grow_waits,fail,reorder" > "$BACKENDS"
  printf '%s\n' \
    "$run_id,$n,8,8,$rep,$sequence,$backend,$requests,$(field "$result" mrps),$(field "$result" gbps),$(field "$result" p50),$(field "$result" p99),$waits,${fail:-0},${reorder:-0}" \
    >> "$BACKENDS"
}

run_backend_campaign() {
  local n="$1"
  deploy_config "backend-N$n" "$n" 8 8 local core
  local rep sequence
  for rep in $(seq 1 "$BACKEND_REPS"); do
    for sequence in 1 2 3; do
      run_backend_point "$n" "$rep" "$sequence"
    done
  done
}

run_validators() {
  log "final correctness validators"
  if [ "$DRY_RUN" = 1 ]; then return 0; fi
  "$BENCH" point tcp "$REQ" "$REPLY" "$CONC" 5 "$WARMUP" 1
  "$BENCH" preload 5000 1024 8
  "$BENCH" loopback 10000 8192 0
  "$BENCH" verbs 10000 8192 0 1 1
}

log "output=$OUT"
log "planned CPU rows=$((48 * REPS + 2 * NUMA_REPS)); backend rows=$((9 * BACKEND_REPS))"

# N32 topology sweep. A4/K4 also owns concurrency, transport, and pinning stages.
for spec in "1 2" "2 2" "2 4" "4 4" "8 8"; do
  a="${spec% *}"; k="${spec#* }"
  deploy_config "topology-A${a}K${k}" 32 "$k" "$a" local all

  for rep in $(seq 1 "$REPS"); do
    for threads in $(forward_order "$rep" "1 2 4 8"); do
      run_cpu topology 32 "$k" "$a" fair local "$rep" dpumesh "$CONC" "$threads"
    done
  done

  if [ "$a" = 4 ] && [ "$k" = 4 ]; then
    for rep in $(seq 1 "$REPS"); do
      for conc in $(forward_order "$rep" "1 2 4 8 16 32"); do
        run_transport_pair concurrency 32 4 4 fair local "$rep" "$conc" 2
      done
    done
    for rep in $(seq 1 "$REPS"); do
      for threads in $(forward_order "$rep" "1 2 4 8"); do
        run_transport_pair transport 32 4 4 fair local "$rep" "$CONC" "$threads"
      done
    done

    pin_profile hw local
    for rep in $(seq 1 "$REPS"); do
      for threads in $(forward_order "$rep" "1 2 4 8"); do
        run_transport_pair pinning 32 4 4 hw local "$rep" "$CONC" "$threads"
      done
    done
    pin_profile fair local
  fi

  if [ "$a" = 8 ] && [ "$k" = 8 ]; then
    for rep in $(seq 1 "$NUMA_REPS"); do
      run_cpu numa 32 8 8 fair local "$rep" dpumesh "$CONC" 8
    done
  fi
done

# Low-N backend controls use only four meshed pods; N8/K8 admits eight live pods.
for n in 8 16 32; do
  run_backend_campaign "$n"
done

# Reproduce the legacy automatic NUMA placement with the same N32/A8/K8 load.
deploy_config numa-auto 32 8 8 auto all
for rep in $(seq 1 "$NUMA_REPS"); do
  run_cpu numa 32 8 8 fair auto "$rep" dpumesh "$CONC" 8
done

# Leave the machine in the canonical final configuration and run correctness checks.
deploy_config final-local 32 8 8 local all
run_validators

if [ "$DRY_RUN" = 0 ]; then
  python3 "$ANALYZE" "$RESULTS" "$BACKENDS" "$SUMMARY" "$BACKEND_SUMMARY" \
    "$REPS" "$NUMA_REPS" "$BACKEND_REPS"
  log "PASS results=$RESULTS summary=$SUMMARY backends=$BACKEND_SUMMARY"
else
  log "dry-run complete"
fi
