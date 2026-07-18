#!/bin/bash
# fair_cores.sh — the logically-fair comparison: give BOTH client sides the SAME host
# core budget B (Envoy counted inside the TCP budget), drive each to saturation, and
# report the peak throughput plus host CPU (client+server) and DPU ARM.
#
# "equal host cores": both bench pods are pinned to exactly B cores; the TCP pod's
# B cores are shared by bench_sock + its Envoy sidecar (the sidecar is IN the budget).
# The DPU ARM is reported separately — it is the offload silicon, not a host core.
#
# The thread/QP count is FIXED (FAIR_THREADS, default 4) across all B, so this sweep
# varies ONLY the host-core budget and isolates core-scaling from connection-scaling.
# CAVEAT this sweep cannot remove: DPUmesh's client threads fan out across up to 3
# echo backends (cores 1/6/7) while TCP has ONE echo backend — the server side is NOT
# symmetric, and it favours DPUmesh. Read the result with that asymmetry in mind.
#
#   bench/suite/fair_cores.sh ["1 2 4"]   # B values; writes $OUT/fair.csv
set -euo pipefail
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
NS="${NS:-test-bench}"
BUDGETS="${1:-1 2 4}"
OUT="${OUT:-/tmp/dpumesh-bench/fair}"; mkdir -p "$OUT"
FAIR="$OUT/fair.csv"
S(){ echo "$HOST_PASS" | sudo -S "$@" 2>/dev/null; }

# free, performance-governed core pools (0-7 are the deploy's pinned cores)
DPU_POOL=(8 9 10 11); TCP_POOL=(12 13 14 15)
S cpupower -c 0-15 frequency-set -g performance >/dev/null 2>&1 || true
S cpupower -c 8-15 frequency-set -d 2.5GHz -u 2.5GHz >/dev/null 2>&1 || true

pin_to(){ # pin every container PID (+threads) of a pod to a core list
  local app="$1" cores="$2" pod cid pid
  pod=$(S crictl pods --label "app=$app" -q | head -1); [ -z "$pod" ] && return
  for cid in $(S crictl ps --pod "$pod" -q); do
    pid=$(S crictl inspect "$cid" | jq -r '.info.pid')
    [ -z "$pid" ] || [ "$pid" = null ] && continue
    S taskset -apc "$cores" "$pid" >/dev/null
    for ch in $(pgrep -P "$pid" 2>/dev/null); do S taskset -apc "$cores" "$ch" >/dev/null 2>&1 || true; done
  done
}

echo "== fair symmetric host-core comparison (1KB/8B, both clients driven to saturation) ==" | tee "$OUT/fair.log"
for B in $BUDGETS; do
  dc=$(IFS=,; echo "${DPU_POOL[*]:0:$B}"); tc=$(IFS=,; echo "${TCP_POOL[*]:0:$B}")
  T="${FAIR_THREADS:-4}"     # FIXED across B: vary only the core budget, not the QP count
  echo "--- B=$B host cores/side (dpumesh->$dc  tcp->$tc)  threads=$T ---" | tee -a "$OUT/fair.log"
  pin_to bench-dpumesh "$dc"; pin_to bench-tcp "$tc"
  bash "$SUITE_DIR/cpu_probe.sh" dpumesh 1024 8 32 8 "$T" "$FAIR" | tee -a "$OUT/fair.log"
  bash "$SUITE_DIR/cpu_probe.sh" tcp     1024 8 32 8 "$T" "$FAIR" | tee -a "$OUT/fair.log"
done
echo "restoring fair 1-core pinning..." | tee -a "$OUT/fair.log"
bash "$PROJ_ROOT/bench/bench.sh" pin fair >/dev/null 2>&1 || true
echo "DONE -> $FAIR" | tee -a "$OUT/fair.log"
