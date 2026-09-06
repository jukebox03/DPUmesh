#!/bin/bash
# Run from the repository root after `bench/bench.sh build` produced the lean binary.
set -euo pipefail
c=bench/report/data/tx-fixed-overhead-20260906/campaign.py
python3 "$c" setup
for arm in batch lean; do
    python3 "$c" deploy --arm "$arm" --protocol grpc
    python3 "$c" sample --arm "$arm" --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --mode matched --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --kind stat --sizes 65536 --duration 12 --reps 3
    python3 "$c" sample --arm "$arm" --kind trace --duration 12 --reps 1
    python3 "$c" sample --arm "$arm" --kind profile --sizes 64 1024 65536 --duration 16 --reps 1
done
# Reverse arm order for the opaque matrix.
for arm in lean batch; do
    python3 "$c" deploy --arm "$arm" --protocol opaque
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --mode matched --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind trace --duration 12 --reps 1
done
# Revisit the large-message baseline after the main A/B to bound rig drift.
python3 "$c" deploy --arm batch --protocol grpc
python3 "$c" sample --arm batch --kind clean --sizes 65536 --start-rep 4 --reps 3
python3 "$c" restore
python3 bench/report/data/tx-fixed-overhead-20260906/summarize.py
python3 bench/report/data/tx-fixed-overhead-20260906/compare.py
