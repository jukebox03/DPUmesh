#!/bin/bash
# Measure the current L4 data plane against direct TCP and a two-sided Envoy mesh.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
REPORT_DIR="${REPORT_DIR:-$PROJ_ROOT/bench/report}"
DATA_DIR="$REPORT_DIR/data"
FIG_DIR="$REPORT_DIR/figures"
mkdir -p "$DATA_DIR" "$FIG_DIR"

DUR="${DUR:-10}"
CPU_DUR="${CPU_DUR:-10}"
REPS="${REPS:-5}"
RTT_SIZES="${RTT_SIZES:-64 256 1024 8192 65536 1048576}"
CONC_STEPS="${CONC_STEPS:-1 2 4 8 16 32 64}"
BW_SIZES="${BW_SIZES:-64 1024 8192 65536 524288 1048576}"
CPU_SIZES="${CPU_SIZES:-64 1024 8192 65536 1048576}"
CPU_CONC="${CPU_CONC:-32}"

OUT="$DATA_DIR" FIG_DIR="$FIG_DIR" \
TRANSPORT_FILTER="dpumesh-native tcp-envoy tcp-direct" \
DUR="$DUR" REPS="$REPS" WARMUP=1000 \
RTT_SIZES="$RTT_SIZES" RTT_REPLY=8 \
CONC_REQ=1024 CONC_REPLY=1024 CONC_STEPS="$CONC_STEPS" \
BW_SIZES="$BW_SIZES" BW_REPLY=8 BW_CONC=32 \
"$SUITE_DIR/run_suite.sh" rtt conc bw

CPU_RAW="$DATA_DIR/cpu.csv"
CPU_SUMMARY="$DATA_DIR/cpu_summary.csv"
rm -f "$CPU_RAW" "$CPU_SUMMARY"
CPU_TRANSPORTS=(dpumesh tcp direct)
for rep in $(seq 1 "$REPS"); do
  shift_by=$((rep % ${#CPU_TRANSPORTS[@]}))
  for size in $CPU_SIZES; do
    for ((i=0; i<${#CPU_TRANSPORTS[@]}; i++)); do
      transport="${CPU_TRANSPORTS[$(( (shift_by+i) % ${#CPU_TRANSPORTS[@]} ))]}"
      CPU_REP="$rep" "$SUITE_DIR/cpu_probe.sh" \
        "$transport" "$size" 8 "$CPU_CONC" "$CPU_DUR" 1 "$CPU_RAW"
    done
  done
done

python3 "$SUITE_DIR/analyze_cpu.py" "$CPU_RAW" "$CPU_SUMMARY"
python3 "$SUITE_DIR/validate_l4.py" "$DATA_DIR/tidy.csv" "$CPU_RAW" "$REPS"
python3 "$SUITE_DIR/plot.py" "$DATA_DIR/summary.csv" "$FIG_DIR" "$CPU_SUMMARY"
echo "current_l4: PASS report=$REPORT_DIR"
