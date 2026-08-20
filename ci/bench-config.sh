#!/bin/bash
# Print the configuration a measurement was taken under, as KEY=VALUE lines for
# ci/bench-to-json.py to merge into the point.
#
# ci/bench-frozen.txt fixes what the client asks for: sizes, concurrency,
# threads. It cannot fix what the DPU is. How many ARM workers are running,
# how many rings a pod holds, which core each process sits on, what else shares
# the machine — all of that moves with a deploy, and all of it moves the number.
# Two points carrying the same label and a different config_id are measurements
# of two different machines, so the published page breaks its line between them
# instead of drawing a slope that means nothing.
#
# config_id hashes only the fields that a deploy sets and that stay put while it
# runs. cfg_l7_active is reported but deliberately left out of the hash: it is
# read from a log window, so a quiet proxy would otherwise split the series for
# no reason.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS="${NS:-test-bench}"
LOG_LINES="${DPU_LOG_LINES:-4000}"

fail() { echo "bench-config: $*" >&2; exit 1; }

# --- what the DPU is running ------------------------------------------------
# Every ARM worker reports itself periodically, so the highest index seen in the
# window is the worker count minus one. Without this number a throughput point
# cannot be read at all: the same build serves a very different rate with one
# worker than with four.
log="$(cd "$ROOT" && timeout 120 bench/bench.sh dpulog "$LOG_LINES" 2>/dev/null)" \
    || fail "cannot read the DPU log (is .env present, and is the DPU up?)"
top_worker="$(printf '%s\n' "$log" | grep -o 'proxy: worker [0-9]\+' | awk '{print $3}' | sort -n | tail -1)"
[ -n "$top_worker" ] || fail "no worker line in the last $LOG_LINES log lines; the DPU may be idle or wedged"
workers=$((top_worker + 1))

# Matched without a pipeline on purpose: `grep -q` closes the pipe on its first
# hit, the writer dies of SIGPIPE, and under `pipefail` the whole pipeline then
# reports failure even though the match succeeded.
l7=none
case "$log" in *linkerd_app*) l7=linkerd;; esac

# --- what the pods are ------------------------------------------------------
rings="$(kubectl get pod -n "$NS" -l app=echo-dpumesh \
    -o jsonpath='{.items[0].spec.containers[0].env[?(@.name=="DPUMESH_RINGS_PER_POD")].value}' 2>/dev/null)"
[ -n "$rings" ] || fail "no Running echo-dpumesh pod in namespace $NS"

# The set of Running pods, not just the ones under test: a campaign left over in
# the same namespace is a load the measurement cannot see but does feel.
pods="$(kubectl get pods -n "$NS" --no-headers 2>/dev/null \
    | awk '$3=="Running"{print $1}' | sed 's/-[a-z0-9]\{6,10\}-[a-z0-9]\{5\}$//' | sort -u)"
pod_count="$(printf '%s\n' "$pods" | grep -c .)"
pods_id="$(printf '%s\n' "$pods" | sha1sum | cut -c1-8)"

# --- where the processes sit ------------------------------------------------
# The pods run on this node, so their affinity is readable here. Pinning is the
# knob that moved these numbers the most in past campaigns.
pin_of() {
    local pid; pid="$(pgrep -x "$1" | head -1)"
    [ -n "$pid" ] && taskset -pc "$pid" 2>/dev/null | sed 's/.*: //' || echo unknown
}
pin_bench="$(pin_of bench_dpumesh)"
pin_echo="$(pin_of echo_dpumesh)"

hashed="workers=$workers rings=$rings pin_bench=$pin_bench pin_echo=$pin_echo pods=$pods_id cpus=$(nproc)"
config_id="$(printf '%s' "$hashed" | sha1sum | cut -c1-12)"

cat <<OUT
config_id=$config_id
cfg_workers=$workers
cfg_rings_per_pod=$rings
cfg_pin_bench=$pin_bench
cfg_pin_echo=$pin_echo
cfg_pods=$pod_count
cfg_pods_id=$pods_id
cfg_cpus=$(nproc)
cfg_l7_active=$l7
OUT
