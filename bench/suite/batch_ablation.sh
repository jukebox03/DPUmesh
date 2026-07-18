#!/bin/bash
# batch_ablation.sh — the matched-batching fairness experiment (report §2–§4).
#
# The headline benchmark coalesces the TCP client/server (one write() per read/write
# batch) but flushes DPUmesh per RPC. This driver measures the fair variant: it gives
# DPUmesh the SAME coalescing and runs the 4-way ablation
#   none / request-only / reply-only / both
# against the two TCP baselines, over a concurrency sweep, with repetitions.
#
#   client batching : the 8th RUN arg `batch` (bench_dpumesh.c coalesces the burst it
#                     issues in one loop pass into ONE doorbell).
#   server batching : /tmp/reply_batch on the echo pods (echo_dpumesh.c coalesces a CQ
#                     batch's replies into one doorbell/conn), polled every 0.5 s.
#
# Prereq: `bench.sh deploy` is up. TCP baselines and DPUmesh share bench.h/methodology,
# so a measured gap is transport+coalescing, not the runtime. 0 fail / 0 reorder is
# required for a cell to be trusted.
#
#   [CONC="1 8 32 64" REPS_HI=6 REPS_LO=3 DUR=10] bench/suite/batch_ablation.sh [out.csv]
set -u
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$SUITE_DIR/../report/data/batch_ablation.csv}"
NS="${NS:-test-bench}"; PORT="${CTRL_PORT:-9092}"
REQ=1024; REPLY=8; TH=1; DUR="${DUR:-10}"; WARM="${WARM:-1000}"
CONC="${CONC:-1 8 32 64}"; ECHO_PODS="echo-dpumesh echo-dpumesh-13 echo-dpumesh-14"

podip()  { kubectl get pod -n $NS -l "app=$1" --field-selector=status.phase=Running -o jsonpath='{.items[0].status.podIP}' 2>/dev/null; }
fld()    { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }
reps_for(){ case "$1" in 32|64) echo "${REPS_HI:-6}";; *) echo "${REPS_LO:-3}";; esac; }

# server reply-coalesce on ALL backends (one conn pins to one, but the LB can pick any),
# then wait out the 0.5 s poll so the setting is in effect before the run.
set_reply_batch() {
  local v="$1" a pod
  for a in $ECHO_PODS; do
    pod=$(kubectl get pod -n $NS -l "app=$a" --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
    [ -n "$pod" ] && kubectl exec -n $NS "$pod" -- sh -c "echo $v > /tmp/reply_batch" 2>/dev/null
  done; sleep 0.8
}

run_one() { # transport conc batch reply_batch rep
  local tr="$1" conc="$2" batch="$3" rb="$4" rep="$5" app ip cmd r
  case "$tr" in
    dpumesh) app=bench-dpumesh; cmd="RUN $REQ $REPLY $conc $DUR $WARM $TH 0 $batch";;
    direct)  app=bench-direct;  cmd="RUN $REQ $REPLY $conc $DUR $WARM $TH";;
    envoy)   app=bench-tcp;     cmd="RUN $REQ $REPLY $conc $DUR $WARM $TH";;
  esac
  ip=$(podip "$app"); [ -z "$ip" ] && { echo "$tr,$conc,$batch,$rb,$rep,NO_POD"; return; }
  r=$(printf '%s\n' "$cmd" | timeout $((DUR+90))s nc -N "$ip" "$PORT" 2>/dev/null)
  [[ "$r" == OK* ]] || { echo "$tr,$conc,$batch,$rb,$rep,ERR"; return; }
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$tr" "$conc" "$batch" "$rb" "$rep" \
    "$(fld "$r" mrps)" "$(fld "$r" gbps)" "$(fld "$r" p50)" "$(fld "$r" p95)" "$(fld "$r" p99)" \
    "$(fld "$r" avg)" "$(fld "$r" fail)" "$(fld "$r" reorder)" "$(fld "$r" rcnt)" "$(fld "$r" dist)"
}

echo "transport,conc,batch,reply_batch,rep,mrps,gbps,p50,p95,p99,avg,fail,reorder,rcnt,dist" | tee "$OUT"
# reply_batch=0 block: dpumesh {none, req-only} interleaved with the two TCP baselines.
set_reply_batch 0
for c in $CONC; do for rep in $(seq 1 "$(reps_for "$c")"); do
  run_one dpumesh "$c" 0 0 "$rep" | tee -a "$OUT"
  run_one dpumesh "$c" 1 0 "$rep" | tee -a "$OUT"
  run_one direct  "$c" 0 0 "$rep" | tee -a "$OUT"
  run_one envoy   "$c" 0 0 "$rep" | tee -a "$OUT"
done; done
# reply_batch=1 block: dpumesh {reply-only, both}.
set_reply_batch 1
for c in $CONC; do for rep in $(seq 1 "$(reps_for "$c")"); do
  run_one dpumesh "$c" 0 1 "$rep" | tee -a "$OUT"
  run_one dpumesh "$c" 1 1 "$rep" | tee -a "$OUT"
done; done
set_reply_batch 0
echo "DONE -> $OUT"
