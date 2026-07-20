#!/bin/bash
# Sweep concurrent client-pod counts through one DPU and report aggregate
# throughput, host cores, and shared DPU ARM cores. TCP scaling uses a labelled
# single-pair extrapolation. Usage: npod.sh <label> <conc> <dur> <Nmax> [out.csv].
set -u
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; PROJ="$(cd "$SUITE_DIR/../.." && pwd)"
[ -f "$PROJ/.env" ] && { set -a; source "$PROJ/.env"; set +a; }
NS="${NS:-test-bench}"; CTRL_PORT="${CTRL_PORT:-9092}"; CLK=$(getconf CLK_TCK)
LABEL="${1:-cfg}"; CONC="${2:-32}"; DUR="${3:-20}"; NMAX="${4:-3}"; OUT="${5:-/tmp/npod.csv}"
CLIENTS=(bench-dpumesh bench-dpumesh-2 bench-dpumesh-3)
BACKENDS=(echo-dpumesh echo-dpumesh-13 echo-dpumesh-14)

pod_ip(){ kubectl get pod -n "$NS" -l "app=$1" --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}' 2>/dev/null; }
pod_pids(){ local app=$1 pod cid; pod=$(echo "$HOST_PASS"|sudo -S crictl pods --label "app=$app" -q 2>/dev/null|head -1); [ -z "$pod" ]&&return; for cid in $(echo "$HOST_PASS"|sudo -S crictl ps --pod "$pod" -q 2>/dev/null); do echo "$HOST_PASS"|sudo -S crictl inspect "$cid" 2>/dev/null|jq -r '.info.pid'; done; }
ticks(){ awk '{n=split($0,a,") ");split(a[2],b," ");print b[12]+b[13]}' "/proc/$1/stat" 2>/dev/null||echo 0; }
dpu_ticks(){ ssh -o ConnectTimeout=8 "$DPU_HOST" "echo '$DPU_PASS'|sudo -S awk '{n=split(\$0,a,\") \");split(a[2],b,\" \");print b[12]+b[13]}' /proc/$1/stat 2>/dev/null" 2>/dev/null|tr -dc '0-9'; }
field(){ awk -v k="$2" '{for(i=1;i<=NF;i++){p=k"=";if(index($i,p)==1){print substr($i,length(p)+1);exit}}}' <<<"$1"; }

[ -s "$OUT" ] || echo "config,transport,N,conc,agg_mrps,host_client_pct,host_server_pct,host_total_pct,dpu_arm_pct,host_per_krps" > "$OUT"
DPID=$(ssh -o ConnectTimeout=8 "$DPU_HOST" "echo '$DPU_PASS'|sudo -S pgrep -x dpumesh_dpu 2>/dev/null|head -1" 2>/dev/null|tr -dc '0-9')
echo "== N-pod amortization ($LABEL): conc=$CONC dur=${DUR}s Nmax=$NMAX, DPU pid=$DPID =="

for ((N=1; N<=NMAX; N++)); do
  ips=(); cp=(); sp=()
  for ((i=0;i<N;i++)); do ips+=("$(pod_ip "${CLIENTS[$i]}")"); done
  for ((i=0;i<N;i++)); do while read -r p;do [ -n "$p" ]&&cp+=("$p");done < <(pod_pids "${CLIENTS[$i]}"); done
  for b in "${BACKENDS[@]}"; do while read -r p;do [ -n "$p" ]&&sp+=("$p");done < <(pod_pids "$b"); done
  declare -A T0; for p in "${cp[@]}" "${sp[@]}"; do T0[$p]=$(ticks "$p"); done
  DT0=$(dpu_ticks "$DPID")
  w0=$(date +%s.%N); tmp=$(mktemp -d)
  for ((i=0;i<N;i++)); do ( printf 'RUN 1024 8 %s %s 200 1\n' "$CONC" "$DUR" | timeout $((DUR+40))s nc -N "${ips[$i]}" "$CTRL_PORT" 2>/dev/null > "$tmp/$i" ) & done
  wait
  w1=$(date +%s.%N); dt=$(awk -v a="$w0" -v b="$w1" 'BEGIN{print b-a}')
  agg=0; for ((i=0;i<N;i++)); do m=$(field "$(cat "$tmp/$i" 2>/dev/null)" mrps); agg=$(awk -v a="$agg" -v b="${m:-0}" 'BEGIN{print a+b}'); done
  rm -rf "$tmp"
  csum=0; for p in "${cp[@]}"; do d=$(( $(ticks "$p") - ${T0[$p]} )); csum=$(awk -v a="$csum" -v d="$d" -v c="$CLK" -v t="$dt" 'BEGIN{print a+100*d/c/t}'); done
  ssum=0; for p in "${sp[@]}"; do d=$(( $(ticks "$p") - ${T0[$p]} )); ssum=$(awk -v a="$ssum" -v d="$d" -v c="$CLK" -v t="$dt" 'BEGIN{print a+100*d/c/t}'); done
  dd=$(( $(dpu_ticks "$DPID") - DT0 )); dpu=$(awk -v d="$dd" -v c="$CLK" -v t="$dt" 'BEGIN{printf "%.1f",100*d/c/t}')
  tot=$(awk -v a="$csum" -v b="$ssum" 'BEGIN{print a+b}')
  eff=$(awk -v h="$tot" -v m="$agg" 'BEGIN{printf (m>0)?"%.3f":"n/a",h/m/1000}')
  unset T0
  printf "%s,dpumesh,%d,%s,%.4f,%.1f,%.1f,%.1f,%s,%s\n" "$LABEL" "$N" "$CONC" "$agg" "$csum" "$ssum" "$tot" "$dpu" "$eff" | tee -a "$OUT"
done

# TCP reference: measure ONE app+sidecar pair, extrapolate linearly (independent pairs).
TCPCSV=$(mktemp)
bash "$SUITE_DIR/cpu_probe.sh" tcp 1024 8 "$CONC" "$DUR" 1 "$TCPCSV" >/dev/null 2>&1
row=$(tail -1 "$TCPCSV"); rm -f "$TCPCSV"
tm=$(cut -d, -f6 <<<"$row"); tcl=$(cut -d, -f10 <<<"$row"); tsr=$(cut -d, -f11 <<<"$row"); tto=$(cut -d, -f12 <<<"$row")
teff=$(awk -v h="${tto:-0}" -v m="${tm:-1}" 'BEGIN{printf (m>0)?"%.3f":"n/a",h/m/1000}')
for ((N=1; N<=NMAX; N++)); do
  printf "%s,tcp-envoy,%d,%s,%.4f,%.1f,%.1f,%.1f,0.0,%s\n" "$LABEL" "$N" "$CONC" \
    "$(awk -v x="${tm:-0}" -v n="$N" 'BEGIN{print x*n}')" \
    "$(awk -v x="${tcl:-0}" -v n="$N" 'BEGIN{print x*n}')" \
    "$(awk -v x="${tsr:-0}" -v n="$N" 'BEGIN{print x*n}')" \
    "$(awk -v x="${tto:-0}" -v n="$N" 'BEGIN{print x*n}')" "$teff" | tee -a "$OUT"
done
echo "DONE -> $OUT  (tcp N>1 = linear extrapolation of the single measured pair)"
