#!/bin/bash
# batch_cpu.sh — DPU-ARM occupancy for unbatched vs matched-batched (report §4).
#
# For each (conc, mode) it runs a sustained DPUmesh RUN and samples the per-thread
# %CPU of the dpumesh_dpu process (top -bH, summed) twice mid-run, then derives
# ARM µs/RPC = (arm%/100) / mrps. Isolates whether the ARM cost is per-RPC overhead
# (halves under batching) or a fixed core-speed ceiling (would not move).
#
# Needs .env (DPU_HOST/DPU_PASS) for the ssh sample. Run with nothing else driving
# the pods. Host-side CPU is in data/cpu.csv (cpu_probe.sh); this driver is ARM-only.
#
#   bench/suite/batch_cpu.sh [out.csv]
set -u
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
OUT="${1:-$SUITE_DIR/../report/data/batch_cpu.csv}"
NS="${NS:-test-bench}"; PORT="${CTRL_PORT:-9092}"; DUR="${DUR:-26}"
BD=$(kubectl get pod -n $NS -l app=bench-dpumesh --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}')

set_rb() { local a pod; for a in echo-dpumesh echo-dpumesh-13 echo-dpumesh-14; do
    pod=$(kubectl get pod -n $NS -l app=$a --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}')
    kubectl exec -n $NS "$pod" -- sh -c "echo $1 > /tmp/reply_batch" 2>/dev/null; done; sleep 0.8; }
arm_pct() {  # sum %CPU across dpumesh_dpu threads, 2nd top sample
  ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S bash -c 'pid=\$(pgrep -x dpumesh_dpu|head -1); top -bH -d 1 -n 2 -p \$pid'" 2>/dev/null \
    | sed 's/^\[sudo\][^:]*: *//' | awk 'BEGIN{s=0}/%CPU/{b++} b==2 && $9 ~ /^[0-9.]+$/{s+=$9} END{printf "%.1f",s}'; }

cell() { # label conc batch rb
  local lab="$1" conc="$2" batch="$3" rb="$4" bpid a1 a2 mrps p50 fail arm
  set_rb "$rb"
  ( printf 'RUN %s 8 %s %s 2000 1 0 %s\n' 1024 "$conc" "$DUR" "$batch" | timeout 120 nc -N "$BD" $PORT > "/tmp/bcpu_$lab.ok" 2>/dev/null ) &
  bpid=$!; sleep 9; a1=$(arm_pct); a2=$(arm_pct); wait $bpid
  mrps=$(sed -n 's/.*[[:space:]]mrps=\([^ ]*\).*/\1/p' "/tmp/bcpu_$lab.ok")
  p50=$(sed -n 's/.*[[:space:]]p50=\([^ ]*\).*/\1/p' "/tmp/bcpu_$lab.ok")
  fail=$(sed -n 's/.*[[:space:]]fail=\([^ ]*\).*/\1/p' "/tmp/bcpu_$lab.ok")
  arm=$(awk -v a="$a1" -v b="$a2" 'BEGIN{printf "%.2f",(a+b)/2}')
  awk -v cell="$1" -v c="$conc" -v ba="$batch" -v rb="$rb" -v m="$mrps" -v p="$p50" -v ar="$arm" -v f="$fail" \
    'BEGIN{u=(ar/100)/m; pk=ar/m; printf "%s,%s,%s,%s,%s,%s,%.2f,%.2f,%.0f,%s\n",cell,c,ba,rb,m,p,ar,u,pk,f}'
}

echo "cell,conc,batch,reply_batch,mrps,p50_us,dpu_arm_pct,arm_us_per_rpc,arm_pct_per_mrps,fail" | tee "$OUT"
for c in 32 64; do cell "unbatched" "$c" 0 0 | tee -a "$OUT"; cell "both" "$c" 1 1 | tee -a "$OUT"; done
set_rb 0
