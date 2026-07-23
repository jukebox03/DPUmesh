#!/bin/bash
# Measure host-container and DPU ARM CPU across one fixed load window using process
# tick deltas. TCP accounting includes Envoy. crictl maps pods to container PIDs.
# Usage: cpu_probe.sh <dpumesh|tcp|direct> <req> <reply> <conc> <dur> [threads] [tidy.csv]
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
NS="${NS:-test-bench}"; CTRL_PORT="${CTRL_PORT:-9092}"
CLK="$(getconf CLK_TCK)"

TR="${1:?transport dpumesh|tcp|direct}"; REQ="${2:-1024}"; REPLY="${3:-8}"; CONC="${4:-32}"
DUR="${5:-10}"; THREADS="${6:-1}"; TIDY="${7:-}"
CPU_REP="${CPU_REP:-1}"
ALLOW_REORDER="${ALLOW_REORDER:-0}"   # L7 per-message LB may complete correlated RPCs out of order
CPU_IDLE="${CPU_IDLE:-0}"             # 1 = sample the same processes without issuing RUN

case "$TR" in
  # dpumesh service has multiple backend pods; passthrough pins the conn to ONE of
  # them, so sample ALL and sum — the inactive backends read ~0, the active one shows
  # the real server-side host cost.
  dpumesh) CLIENT_APP=bench-dpumesh; SERVER_APPS="echo-dpumesh echo-dpumesh-13 echo-dpumesh-14"; USE_DPU=1;;
  tcp)     CLIENT_APP=bench-tcp;     SERVER_APPS="echo-tcp";                                     USE_DPU=0;;
  direct)  CLIENT_APP=bench-direct;  SERVER_APPS="echo-tcp";                                     USE_DPU=0;;
  *) echo "transport must be dpumesh|tcp|direct"; exit 2;;
esac

pod_ip()  { kubectl get pod -n "$NS" -l "app=$1" --field-selector=status.phase=Running \
              -o jsonpath='{.items[0].status.podIP}' 2>/dev/null; }
# All container main-PIDs of a pod (app + any sidecar), via crictl.
pod_pids() {
  local app="$1" pod cid
  pod=$(echo "$HOST_PASS" | sudo -S crictl pods --label "app=$app" -q 2>/dev/null | head -1)
  [ -z "$pod" ] && return
  for cid in $(echo "$HOST_PASS" | sudo -S crictl ps --pod "$pod" -q 2>/dev/null); do
    echo "$HOST_PASS" | sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid'
  done
}
comm_of() { cat "/proc/$1/comm" 2>/dev/null || echo "?"; }
ticks()   { awk '{n=split($0,a,") ");split(a[2],b," ");print b[12]+b[13]}' "/proc/$1/stat" 2>/dev/null || echo 0; }
dpu_pid() { echo "$DPU_PASS" | ssh -o ConnectTimeout=8 "$DPU_HOST" "echo '$DPU_PASS' | sudo -S pgrep -x dpumesh_dpu 2>/dev/null | head -1" 2>/dev/null | tr -dc '0-9'; }
dpu_ticks() { local p="$1"; echo "$DPU_PASS" | ssh -o ConnectTimeout=8 "$DPU_HOST" "echo '$DPU_PASS' | sudo -S awk '{n=split(\$0,a,\") \");split(a[2],b,\" \");print b[12]+b[13]}' /proc/$p/stat 2>/dev/null" 2>/dev/null | tr -dc '0-9'; }
field()   { awk -v k="$2" '{for(i=1;i<=NF;i++){p=k"=";if(index($i,p)==1){print substr($i,length(p)+1);exit}}}' <<<"$1"; }

CIP="$(pod_ip "$CLIENT_APP")"; [ -z "$CIP" ] && { echo "no $CLIENT_APP pod"; exit 1; }
mapfile -t CPIDS < <(pod_pids "$CLIENT_APP")
SPIDS=(); for sapp in $SERVER_APPS; do while read -r p; do [ -n "$p" ] && SPIDS+=("$p"); done < <(pod_pids "$sapp"); done
# The direct path shares echo-tcp's pod with an idle Envoy container but bypasses
# it on port 9092. Exclude that unrelated process from direct-path accounting.
if [ "$TR" = direct ]; then
  FILTERED=()
  for p in "${SPIDS[@]}"; do [ "$(comm_of "$p")" = envoy ] || FILTERED+=("$p"); done
  SPIDS=("${FILTERED[@]}")
fi
DPID=""; DT0=0; [ "$USE_DPU" = 1 ] && DPID="$(dpu_pid)"

echo "== CPU probe: $TR  ${REQ}B/${REPLY}B conc=$CONC threads=$THREADS dur=${DUR}s rep=$CPU_REP =="
echo "   client=$CLIENT_APP pids: ${CPIDS[*]:-none}  server={$SERVER_APPS} pids: ${SPIDS[*]:-none}  dpu_pid=${DPID:-n/a}"

# --- T0 snapshot ---
declare -A T0
for p in "${CPIDS[@]}" "${SPIDS[@]}"; do T0[$p]=$(ticks "$p"); done
[ -n "$DPID" ] && DT0=$(dpu_ticks "$DPID")
W0=$(date +%s.%N)

# --- load window (blocks dur seconds) ---
if [ "$CPU_IDLE" = 1 ]; then
  sleep "$DUR"
  OK="OK mrps=0 gbps=0 p50=0 p99=0 fail=0 drops=0 reorder=0"
else
  OK=$(printf 'RUN %s %s %s %s 200 %s\n' "$REQ" "$REPLY" "$CONC" "$DUR" "$THREADS" \
        | timeout "$((DUR+30))s" nc -N "$CIP" "$CTRL_PORT" 2>/dev/null || echo "ERR")
fi
W1=$(date +%s.%N)
MRPS=$(field "$OK" mrps); GBPS=$(field "$OK" gbps); P50=$(field "$OK" p50); P99=$(field "$OK" p99)
[ -z "$MRPS" ] && { echo "   run failed: $OK"; exit 1; }
FAIL=$(field "$OK" fail); DROPS=$(field "$OK" drops); REORDER=$(field "$OK" reorder)
[ "${FAIL:-0}" = 0 ] && [ "${DROPS:-0}" = 0 ] && \
  { [ "${REORDER:-0}" = 0 ] || [ "$ALLOW_REORDER" = 1 ]; } || {
  echo "   invalid run: fail=${FAIL:-NA} drops=${DROPS:-NA} reorder=${REORDER:-NA}"; exit 1;
}
[ "${REORDER:-0}" = 0 ] || echo "   note: accepting reorder=$REORDER (ALLOW_REORDER=1)"

# --- T1 snapshot + deltas ---
DT=$(awk -v a="$W0" -v b="$W1" 'BEGIN{print b-a}')
pct() { awk -v d="$1" -v clk="$CLK" -v dt="$DT" 'BEGIN{printf "%.1f", 100*d/clk/dt}'; }
csum=0; ssum=0
echo "   --- per-process %core (100% = one full core) ---"
for p in "${CPIDS[@]}"; do d=$(( $(ticks "$p") - ${T0[$p]} )); v=$(pct "$d"); csum=$(awk -v a="$csum" -v b="$v" 'BEGIN{print a+b}')
  printf "     [client] %-16s pid=%-7s %6s%%\n" "$(comm_of "$p")" "$p" "$v"; done
for p in "${SPIDS[@]}"; do d=$(( $(ticks "$p") - ${T0[$p]} )); v=$(pct "$d"); ssum=$(awk -v a="$ssum" -v b="$v" 'BEGIN{print a+b}')
  printf "     [server] %-16s pid=%-7s %6s%%\n" "$(comm_of "$p")" "$p" "$v"; done
DPU_PCT="0.0"
if [ -n "$DPID" ]; then d=$(( $(dpu_ticks "$DPID") - DT0 )); DPU_PCT=$(pct "$d"); fi

HOST=$(awk -v a="$csum" -v b="$ssum" 'BEGIN{print a+b}')
EFF=$(awk -v h="$HOST" -v m="$MRPS" 'BEGIN{if(m>0) printf "%.3f",h/m/1000; else printf "n/a"}')   # %core per Krps
echo "   --- totals ---"
printf "     achieved:   %.4f Mrps  gbps=%s  p50=%sus p99=%sus\n" "$MRPS" "$GBPS" "$P50" "$P99"
printf "     HOST CPU:   client=%.1f%%  server=%.1f%%  TOTAL=%.1f%% (of one core)\n" "$csum" "$ssum" "$HOST"
printf "     DPU ARM:    %s%%\n" "$DPU_PCT"
printf "     host-eff:   %s %%core per Krps  (LOWER = more host-efficient)\n" "$EFF"

if [ -n "$TIDY" ]; then
  [ -s "$TIDY" ] || echo "rep,transport,req,reply,conc,threads,mrps,gbps,p50,p99,host_client_pct,host_server_pct,host_total_pct,dpu_arm_pct,host_pct_per_krps" > "$TIDY"
  echo "$CPU_REP,$TR,$REQ,$REPLY,$CONC,$THREADS,$MRPS,$GBPS,$P50,$P99,$csum,$ssum,$HOST,$DPU_PCT,$EFF" >> "$TIDY"
fi
