#!/bin/bash
set -euo pipefail
c=bench/report/data/arena-tx-batching-perf-20260906/campaign.py
for arm in direct batch; do
    python3 "$c" deploy --arm "$arm" --protocol grpc
    python3 "$c" sample --arm "$arm" --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --mode matched --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --kind stat --sizes 65536 --duration 12 --reps 3
    python3 "$c" sample --arm "$arm" --kind trace --duration 12 --reps 1
done
# Reverse arm order for the opaque matrix.
for arm in batch direct; do
    python3 "$c" deploy --arm "$arm" --protocol opaque
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --mode matched --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind trace --duration 12 --reps 1
done
# Revisit the most important large-message baseline after the main A/B.
python3 "$c" deploy --arm direct --protocol grpc
python3 "$c" sample --arm direct --kind clean --sizes 65536 --start-rep 4 --reps 3
python3 "$c" restore
python3 bench/report/data/arena-tx-batching-perf-20260906/summarize.py
