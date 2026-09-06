#!/bin/bash
set -euo pipefail
set -a
source .env
set +a
receipt=bench/report/data/direct-tx-rdma-20260905
scp -q "$receipt/arena_quiescence.gdb" "$DPU_HOST:/tmp/arena_quiescence.gdb"
ssh "$DPU_HOST" "pid=\$(pgrep -f '^(/tmp/dpumesh-before-direct-tx|./dpumesh_dpu)( |$)'); test -n \"\$pid\"; echo '$DPU_PASS' | sudo -S -p '' gdb -q -batch -p \"\$pid\" -x /tmp/arena_quiescence.gdb" > "$receipt/raw/${1:?stage}-arena.txt" 2>&1
rg 'ARENA_QUIESCENCE.*"live_chunks": 0' "$receipt/raw/$1-arena.txt"
