#!/usr/bin/env bash
# Reproducible ARM-worker scaling arm.
#
# WORKERS is deliberately the only parallelism/geometry knob. The deployment
# derives N/K/A and all-worker L7 from it, and the gRPC generator owns one
# thread and one persistent channel per DPU data worker. Every worker therefore
# receives the same one-session topology at every scale point.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKERS="${WORKERS:-8}"
REPS="${REPS:-3}"
DUR="${DUR:-10}"
WARMUP="${WARMUP:-1000}"
FRAME="${FRAME:-64}"
OUT="${OUT:-/tmp/grpc-worker-scale-a${WORKERS}}"

case "$WORKERS" in
  4)
    RATES="${RATES:-20000 30000 40000 50000 60000}"
    ;;
  6)
    RATES="${RATES:-40000 50000 60000 70000 80000}"
    ;;
  8)
    RATES="${RATES:-60000 80000 90000 100000}"
    ;;
  12)
    RATES="${RATES:-80000 100000 110000 120000 130000 140000}"
    ;;
  *)
    echo "WORKERS must be 4, 6, 8 or 12 (16 would share worker 0 with main/control)" >&2
    exit 2
    ;;
esac

COMMON=(
  "DPUMESH_THROUGHPUT_WORKERS=$WORKERS"
  "BENCH_REACTORS=8"
  "BENCH_NUMA_POLICY=local"
)

cd "$ROOT"
mkdir -p "$OUT"
# N/K/A come from the deployment's own resolution of WORKERS, not a second
# copy of its formula.
{
  printf 'workers=%s\nthreads=%s\nchannels=%s\nframe=%s\n' \
    "$WORKERS" "$WORKERS" "$WORKERS" "$FRAME"
  env "${COMMON[@]}" bash bench/bench.sh geometry | tr ' ' '\n'
} >"$OUT/geometry.txt"

env "${COMMON[@]}" BENCH_DEPLOY_SCOPE=grpc \
  bash bench/bench.sh deploy >"$OUT/deploy.log" 2>&1
env "${COMMON[@]}" bash bench/bench.sh pin grpcmax >"$OUT/pin.log" 2>&1

env "${COMMON[@]}" CHANNELS="$WORKERS" THREADS="$WORKERS" RATES="$RATES" \
  REPS="$REPS" DUR="$DUR" WARMUP="$WARMUP" FRAME="$FRAME" OUT="$OUT" \
  PIN_PROFILE=grpcmax STOP_ON_OVERLOAD=1 \
  bash bench/suite/grpc_conns_sweep.sh
