#!/bin/bash
set -euo pipefail
set -a
source .env
set +a
receipt=bench/report/data/direct-tx-rdma-20260905
stage=${1:?stage}
scp -q "$receipt/commit_shape.gdb" "$DPU_HOST:/tmp/commit_shape.gdb"
ssh "$DPU_HOST" "pid=\$(pgrep -f '^(/tmp/dpumesh-before-direct-tx|./dpumesh_dpu)( |$)'); echo '$DPU_PASS' | sudo -S -p '' timeout -s INT 45 gdb -q -batch -p \"\$pid\" -x /tmp/commit_shape.gdb" > "$receipt/raw/$stage-commit-shape.txt" 2>&1 &
trace=$!
sleep 4
timeout 55 ./bench/bench.sh point 65536 65536 8 3 0 8 > "$receipt/raw/$stage-commit-shape-workload.txt" 2>&1 || true
wait "$trace"
rg COMMIT_SHAPE "$receipt/raw/$stage-commit-shape.txt"
