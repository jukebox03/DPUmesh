#!/bin/bash
# Resume after the lean2 runtime exited silently before its matched-rate runs
# (raw/lean2-grpc/dpu-log-died.txt) and after `bench/bench.sh build` produced
# lean3 (pinned maintenance timer, selective notification clears, exit traces).
# lean/gRPC is complete; lean2's 64 KiB capacity samples were contaminated by
# host-side builds and are re-taken as reps 4-6. Run from the repository root.
set -euo pipefail
c=bench/report/data/park-arm-20260906/campaign.py
python3 "$c" setup3
python3 "$c" deploy --arm lean2 --protocol grpc
python3 "$c" sample --arm lean2 --kind clean --sizes 65536 --start-rep 4 --reps 3
python3 "$c" sample --arm lean2 --mode matched --kind clean --reps 3
python3 "$c" sample --arm lean2 --kind stat --sizes 65536 --duration 12 --reps 3
python3 "$c" syscalls --arm lean2 --sizes 64 65536
python3 "$c" deploy --arm lean3 --protocol grpc
python3 "$c" sample --arm lean3 --kind clean --reps 3
python3 "$c" sample --arm lean3 --mode matched --kind clean --reps 3
python3 "$c" sample --arm lean3 --kind stat --sizes 65536 --duration 12 --reps 3
python3 "$c" syscalls --arm lean3 --sizes 64 65536
python3 "$c" sample --arm lean3 --kind profile --sizes 64 65536 --duration 16 --reps 1
for arm in lean3 lean2; do
    python3 "$c" deploy --arm "$arm" --protocol opaque
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind clean --reps 3
    python3 "$c" sample --arm "$arm" --protocol opaque --sizes 64 1024 --mode matched --kind clean --reps 3
done
# Revisit the lean large-message baseline after the A/B to bound rig drift.
python3 "$c" deploy --arm lean --protocol grpc
python3 "$c" sample --arm lean --kind clean --sizes 65536 --start-rep 4 --reps 3
python3 "$c" restore
