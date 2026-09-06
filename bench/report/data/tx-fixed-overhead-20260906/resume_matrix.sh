#!/bin/bash
# Resume after the batch/opaque deploy warmup raced the controller feed and the
# batch runtime exited (raw/batch-opaque/dpu-log-3447022-died.txt). gRPC both
# arms and lean/opaque are complete; this finishes batch/opaque, the batch
# 64 KiB re-baseline and the restoration.
set -euo pipefail
c=bench/report/data/tx-fixed-overhead-20260906/campaign.py
python3 "$c" deploy --arm batch --protocol opaque
python3 "$c" sample --arm batch --protocol opaque --sizes 64 1024 --kind clean --reps 3
python3 "$c" sample --arm batch --protocol opaque --sizes 64 1024 --mode matched --kind clean --reps 3
python3 "$c" sample --arm batch --protocol opaque --sizes 64 1024 --kind trace --duration 12 --reps 1
python3 "$c" deploy --arm batch --protocol grpc
python3 "$c" sample --arm batch --kind clean --sizes 65536 --start-rep 4 --reps 3
python3 "$c" restore
python3 bench/report/data/tx-fixed-overhead-20260906/summarize.py
python3 bench/report/data/tx-fixed-overhead-20260906/compare.py
