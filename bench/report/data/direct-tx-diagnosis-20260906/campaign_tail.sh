#!/bin/bash
set -euo pipefail
diag=bench/report/data/direct-tx-diagnosis-20260906/diagnose.py
# Run after the gRPC clean/profile/trace matrix, while after/grpc is live.
python3 "$diag" sample --arm after --kind stat --sizes 65536 --duration 12 --reps 3
python3 "$diag" deploy --arm before --protocol grpc
python3 "$diag" sample --arm before --kind stat --sizes 65536 --duration 12 --reps 3
for arm in before after; do
    python3 "$diag" deploy --arm "$arm" --protocol opaque
    python3 "$diag" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind clean --reps 3
    python3 "$diag" sample --arm "$arm" --protocol opaque --sizes 64 1024 --mode matched --kind clean --reps 3
    python3 "$diag" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind profile --duration 16
    python3 "$diag" sample --arm "$arm" --protocol opaque --sizes 64 1024 --kind trace --duration 12
    python3 "$diag" sample --arm "$arm" --protocol opaque --sizes 64 1024 --mode matched --kind trace --duration 12
done
