#!/bin/bash
# run_suite.sh — the STAGED evaluation run against a live `bench.sh deploy` (DPU host).
#
# Repetitions + CI, ABBA ordering, output pipeline tidy CSV -> analyze.py -> plot.py.
# It drives the deployed pods by resolving each transport's control endpoint the way
# bench.sh does (kubectl pod IP + nc), so the DPU columns fill in from real hardware.
#
# It runs whichever transports are actually deployed; a missing pod is warned and
# skipped, so a partial deploy still yields partial figures. See STAGES.md for the
# full transport matrix and which pods need to be added to k8s/pods.yaml.
#
#   bench/suite/run_suite.sh [stage ...]        # default: rtt conc curve bw
#   REPS=5 DUR=30 WARMUP=1000 bench/suite/run_suite.sh rtt curve
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd "$SUITE_DIR/.." && pwd)"
PROJ_ROOT="$(cd "$BENCH_DIR/.." && pwd)"
# .env gives DPU_HOST / passwords so the metadata freeze can read the LIVE DPU config
# (authoritative) instead of this shell's env. Runs themselves need only kubectl + nc.
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
NS="${NS:-test-bench}"; CTRL_PORT="${CTRL_PORT:-9092}"
OUT="${OUT:-/tmp/dpumesh-bench/suite}"; mkdir -p "$OUT/figs"
TIDY="$OUT/tidy.csv"; SUMMARY="$OUT/summary.csv"; META="$OUT/meta.txt"

DUR="${DUR:-20}"; WARMUP="${WARMUP:-1000}"; REPS="${REPS:-5}"
RTT_SIZES="${RTT_SIZES:-64 256 1024 4096 16384 65536}"
CONC_STEPS="${CONC_STEPS:-1 2 4 8 16 32 64}"
BW_SIZES="${BW_SIZES:-1024 8192 65536 524288 2097152 8000000}"
BW_CONC="${BW_CONC:-32}"
CURVE_FRACS="${CURVE_FRACS:-0.10 0.25 0.50 0.70 0.85 0.90 0.95 1.00 1.10}"

# transport id | app label | control port | kind (native|tcp|sock) | open-capable
# Uncomment the last two once k8s/pods.yaml exposes them (see STAGES.md "Deploy TODO").
TRANSPORTS=(
  "dpumesh-native|bench-dpumesh|$CTRL_PORT|native|no"
  "tcp-envoy|bench-tcp|$CTRL_PORT|sock|yes"
  "tcp-direct|bench-direct|$CTRL_PORT|sock|yes"        # bench_sock → echo_sock, NO sidecar (isolates the Envoy tax)
  # "dpumesh-preload|...|sock|yes"   # needs shim epoll support for bench_sock (see STAGES.md)
)

log()  { echo -e "\033[0;34m[suite]\033[0m $*"; }
warn() { echo -e "\033[1;33m[suite]\033[0m $*" >&2; }

pod_ip() { kubectl get pod -n "$NS" -l "app=$1" --field-selector=status.phase=Running \
             -o jsonpath='{.items[0].status.podIP}' 2>/dev/null || true; }
field()  { awk -v k="$2" '{for(i=1;i<=NF;i++){p=k"=";if(index($i,p)==1){print substr($i,length(p)+1);exit}}}' <<<"$1"; }
nc_to()  { awk -v d="$DUR" 'BEGIN{printf "%d", d+90}'; }
run_line() { printf '%s\n' "$1" | timeout "$(nc_to)s" nc -N "$2" "$CTRL_PORT" 2>/dev/null || echo "ERR nc"; }

# ---- metadata freeze (provenance) -----------------------------------------
# Record the ACTUAL running config, not this shell's env: the DPU operating point
# is read live from /proc/<dpumesh_dpu>/environ, and the host binaries are pinned
# by container-image digest. This is what makes "(4,4)" a receipt, not a claim.
{
  echo "date_utc   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit     $(git -C "$PROJ_ROOT" rev-parse HEAD 2>/dev/null || echo NA)"
  echo "dirty      $(git -C "$PROJ_ROOT" status --porcelain 2>/dev/null | wc -l) files (uncommitted eval-harness edits)"
  echo "node       $(uname -n)  kernel $(uname -r)"
  echo "cpu        $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
  echo "doca       $(dpkg -l 2>/dev/null | awk '/doca-runtime/{print $3; exit}' || echo NA)"
  echo "governor   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo NA) @ $(awk '{printf "%.2fGHz",$1/1e6}' /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo NA)"
  echo "kubectl    $(kubectl version --client -o json 2>/dev/null | tr -d '\n' | sed 's/.*gitVersion":"\([^"]*\).*/\1/' || echo NA)"
  echo "namespace  $NS"
  echo "params     DUR=$DUR WARMUP=$WARMUP REPS=$REPS"
  # LIVE DPU operating point + binary hash, straight from the running process.
  if [ -n "${DPU_HOST:-}" ]; then
    echo "dpu        live /proc/<dpumesh_dpu>/environ + binary sha256:"
    ssh -o ConnectTimeout=8 "$DPU_HOST" "echo '${DPU_PASS:-}' | sudo -S sh -c 'p=\$(pgrep -x dpumesh_dpu | head -1); echo pid=\$p etimes=\$(ps -o etimes= -p \$p 2>/dev/null | tr -d \" \")s; sha256sum ~/${DPU_PROJ:-DPUmesh}/doca/build/dpumesh_dpu 2>/dev/null; cat /proc/\$p/environ'" 2>/dev/null \
      | tr '\0' '\n' | grep -E 'pid=|dpumesh_dpu$|^DPUMESH_' | sed 's/^/    /' || echo "    (DPU query failed)"
  else
    echo "dpu        (no .env — DPU config NOT captured)"
  fi
  # Container-image digests actually loaded in containerd = the host binaries running.
  if [ -n "${HOST_PASS:-}" ]; then
    echo "images     containerd k8s.io digests:"
    echo "$HOST_PASS" | sudo -S ctr -n k8s.io images ls 2>/dev/null \
      | awk 'NR>1 && ($1 ~ /bench\// || $1 ~ /envoy/){printf "    %s  %s\n",$1,$3}' || true
  fi
} > "$META"
log "metadata -> $META"

# ---- resolve live transports ----------------------------------------------
declare -A IP KIND OPEN
LIVE=()
for row in "${TRANSPORTS[@]}"; do
  IFS='|' read -r id app port kind open <<<"$row"
  ip="$(pod_ip "$app")"
  if [ -z "$ip" ]; then warn "transport $id: pod app=$app not Running — skipping"; continue; fi
  IP[$id]="$ip"; KIND[$id]="$kind"; OPEN[$id]="$open"; LIVE+=("$id")
  log "transport $id -> $app @ $ip (kind=$kind open=$open)"
done
[ "${#LIVE[@]}" -eq 0 ] && { warn "no transports deployed — run 'bench.sh deploy' first"; exit 1; }

echo "stage,transport,workload,mode,arrival,req_size,reply_size,conc,threads,offered_mrps,rep,mrps,gbps,p50,p95,p99,p999,p9999,avg,min,max,drops,overflow,fail,reorder" > "$TIDY"
emit() { # emit <stage> <transport> <workload> <mode> <arrival> <req> <reply> <conc> <threads> <offered> <rep> <ok>
  local ok="${12}"
  if [[ "$ok" != OK* ]]; then
    warn "$2 $3 rep${11}: ${ok:-no-output}"
    # Record the failed rep explicitly (metrics NA, fail=1) so a FAILED point is
    # distinguishable from a NEVER-RUN point (a missing row) when analysing.
    echo "$1,$2,$3,$4,$5,$6,$7,$8,$9,${10},${11},NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,0,0,1,0" >> "$TIDY"
    return
  fi
  echo "$1,$2,$3,$4,$5,$6,$7,$8,$9,${10},${11},$(field "$ok" mrps),$(field "$ok" gbps),$(field "$ok" p50),$(field "$ok" p95),$(field "$ok" p99),$(field "$ok" p999),$(field "$ok" p9999),$(field "$ok" avg),$(field "$ok" min),$(field "$ok" max),$(field "$ok" drops),$(field "$ok" overflow),$(field "$ok" fail),$(field "$ok" reorder)" >> "$TIDY"
}
# ABBA: rotate transport order by rep so time-order/thermal bias is spread.
order_for_rep() { local n=${#LIVE[@]} s=$(( $1 % ${#LIVE[@]} )) i;
  for ((i=0;i<n;i++)); do echo -n "${LIVE[$(( (s+i)%n ))]} "; done; }

# ---- stages ---------------------------------------------------------------
stage_rtt() {
  log "STAGE rtt: conc=1, sizes {$RTT_SIZES}, reps=$REPS"
  for rep in $(seq 1 "$REPS"); do for sz in $RTT_SIZES; do for tr in $(order_for_rep "$rep"); do
    ok="$(run_line "RUN $sz 8 1 $DUR $WARMUP 1" "${IP[$tr]}")"
    emit rtt "$tr" "${sz}B/8B" closed const "$sz" 8 1 1 "" "$rep" "$ok"
  done; done; done
}
stage_conc() {
  log "STAGE conc: 1KB, conc {$CONC_STEPS}, reps=$REPS"
  for rep in $(seq 1 "$REPS"); do for c in $CONC_STEPS; do for tr in $(order_for_rep "$rep"); do
    ok="$(run_line "RUN 1024 8 $c $DUR $WARMUP 1" "${IP[$tr]}")"
    emit conc "$tr" 1024B/8B closed const 1024 8 "$c" 1 "" "$rep" "$ok"
  done; done; done
}
stage_bw() {
  log "STAGE bw: goodput vs size {$BW_SIZES}, conc=$BW_CONC, reps=$REPS"
  for rep in $(seq 1 "$REPS"); do for sz in $BW_SIZES; do for tr in $(order_for_rep "$rep"); do
    w=$WARMUP; [ "$sz" -ge 262144 ] && w=100
    ok="$(run_line "RUN $sz 8 $BW_CONC $DUR $w 1" "${IP[$tr]}")"
    emit bw "$tr" "${sz}B/8B" closed const "$sz" 8 "$BW_CONC" 1 "" "$rep" "$ok"
  done; done; done
}
stage_curve() {  # OPEN loop — only transports whose client supports it (kind=sock)
  local any=0
  for tr in "${LIVE[@]}"; do [ "${OPEN[$tr]}" = yes ] && any=1; done
  [ "$any" -eq 0 ] && { warn "STAGE curve: no open-capable transport deployed (need dpumesh-preload / tcp-direct with bench_sock) — skipping"; return; }
  for tr in "${LIVE[@]}"; do
    [ "${OPEN[$tr]}" = yes ] || continue
    # Open-loop over CURVE_THREADS connections so the generator is not single-conn
    # limited: the offered rate is split across the conns, each scheduling a fraction.
    local CT="${CURVE_THREADS:-4}"
    for wl in "64 8" "1024 8"; do
      set -- $wl; local req=$1 rep8=$2
      local peak_ok peak rps
      peak_ok="$(run_line "RUN $req $rep8 32 $DUR $WARMUP $CT" "${IP[$tr]}")"
      peak="$(field "$peak_ok" mrps)"; peak="${peak:-0.1}"
      rps="$(awk -v m="$peak" 'BEGIN{printf "%.0f", m*1e6}')"
      log "STAGE curve: $tr ${req}B peak~${peak} Mrps ($CT conns); fracs {$CURVE_FRACS}"
      for r in $(seq 1 "$REPS"); do for fr in $CURVE_FRACS; do
        local rate off ok
        rate="$(awk -v p="$rps" -v f="$fr" 'BEGIN{printf "%.0f", p*f}')"
        off="$(awk -v x="$rate" 'BEGIN{printf "%.6f", x/1e6}')"
        ok="$(run_line "OPEN $req $rep8 $CT $DUR $WARMUP $rate const" "${IP[$tr]}")"
        emit curve "$tr" "${req}B/${rep8}B" open const "$req" "$rep8" 0 "$CT" "$off" "$r" "$ok"
      done; done
    done
  done
}

STAGES=("${@:-rtt conc curve bw}")
for st in ${STAGES[@]}; do
  case "$st" in rtt) stage_rtt;; conc) stage_conc;; bw) stage_bw;; curve) stage_curve;;
    *) warn "unknown stage: $st (have: rtt conc bw curve)";; esac
done

log "aggregating -> $SUMMARY"; python3 "$SUITE_DIR/analyze.py" "$TIDY" "$SUMMARY"
log "plotting -> $OUT/figs"; python3 "$SUITE_DIR/plot.py" "$SUMMARY" "$OUT/figs"
log "DONE. tidy=$TIDY summary=$SUMMARY meta=$META figs=$OUT/figs"
