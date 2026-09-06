#!/bin/bash
set -euo pipefail
receipt=bench/report/data/direct-tx-rdma-20260905
kubectl apply -f "$receipt/deny-server.yaml"
trap 'kubectl delete -f "$receipt/deny-server.yaml" --ignore-not-found >/dev/null' EXIT
sleep 5
timeout 25 ./bench/bench.sh point 64 64 1 1 0 1 > "$receipt/raw/policy-denied.txt" 2>&1 || true
set -a
source .env
set +a
ssh "$DPU_HOST" 'for port in 4191 4192 4193 4194 4195 4196 4197 4198; do curl -fsS --max-time 2 http://127.0.0.1:$port/metrics; done' > "$receipt/raw/policy-denied-metrics.txt"
ssh "$DPU_HOST" 'tail -n 35 /tmp/dpumesh_dpu_bench.log' > "$receipt/raw/policy-denied-dpu.log"
kubectl delete -f "$receipt/deny-server.yaml"
trap - EXIT
sleep 5
timeout 25 ./bench/bench.sh point 64 64 1 2 20 1 > "$receipt/raw/policy-restored.txt" 2>&1
