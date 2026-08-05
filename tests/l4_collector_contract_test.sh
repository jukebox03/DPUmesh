#!/bin/sh
set -eu

collector=${1:-bench/suite/l4_proxy_data.sh}
underfilled=$(mktemp)
trap 'rm -f "$underfilled"' EXIT

output=$(SCOUT_MAX_RPS=0 GENERATOR_SELFTEST_MAX_RPS=0 \
    "$collector" --dry-run --no-perf)
printf '%s\n' "$output" | grep -q 'scout RPS cap:    no fixed cap'
printf '%s\n' "$output" | grep -q 'frame=64B cap=195312500 rps'
printf '%s\n' "$output" | grep -q 'frame=1024B cap=12207031 rps'
printf '%s\n' "$output" | grep -q 'frame=8192B cap=1525879 rps'
printf '%s\n' "$output" | grep -q 'generator cap:    none'
printf '%s\n' "$output" |
    grep -q 'connections:       8 persistent (1 per shard across 8 DPU shards)'
printf '%s\n' "$output" |
    grep -q 'generator clean:  schedule ratio 0.98..1.02'
printf '%s\n' "$output" | grep -q 'scheduler drop ratio <= 0.001'
printf '%s\n' "$output" | grep -q 'latency is reported, not gated'
printf '%s\n' "$output" |
    grep -q 'run retries:      at most 3 retries after validated runtime failure'

limited=$(SCOUT_MAX_RPS=3000000 GENERATOR_SELFTEST_MAX_RPS=4000000 \
    "$collector" --dry-run --no-perf)
printf '%s\n' "$limited" |
    grep -q 'scout RPS cap:    min(3000000 rps, 100Gb/s per-direction frame bound)'
printf '%s\n' "$limited" | grep -q 'frame=64B cap=3000000 rps'
printf '%s\n' "$limited" | grep -q 'generator cap:    4000000 rps'

if THREADS=7 "$collector" --dry-run --no-perf >"$underfilled" 2>&1; then
    echo "l4_collector_contract_test: underfilled DPU shards were accepted" >&2
    exit 1
fi
grep -q 'underfills the 8 connection-affine DPU shards' \
    "$underfilled"

if THREADS=10 "$collector" --dry-run --no-perf >"$underfilled" 2>&1; then
    echo "l4_collector_contract_test: uneven DPU shard distribution was accepted" >&2
    exit 1
fi
grep -q 'does not distribute evenly across 8 connection-affine DPU shards' \
    "$underfilled"

if CORE_SATURATION_THRESHOLD=1.1 \
    "$collector" --dry-run --no-perf >"$underfilled" 2>&1; then
    echo "l4_collector_contract_test: invalid saturation threshold was accepted" >&2
    exit 1
fi
grep -q 'CORE_SATURATION_THRESHOLD must be in (0, 1]' "$underfilled"

if MAX_RECOVERY_REDEPLOYS=0 \
    "$collector" --dry-run --no-perf >"$underfilled" 2>&1; then
    echo "l4_collector_contract_test: zero recovery redeploy limit was accepted" >&2
    exit 1
fi
grep -q 'positive integer required, got: 0' "$underfilled"

if MAX_RUN_RETRIES=0 \
    "$collector" --dry-run --no-perf >"$underfilled" 2>&1; then
    echo "l4_collector_contract_test: zero run retry limit was accepted" >&2
    exit 1
fi
grep -q 'positive integer required, got: 0' "$underfilled"

echo "l4_collector_contract_test: PASS"
