#!/bin/bash
# Report what the DPU and the deployed campaign currently are, as KEY=VALUE
# lines. Nothing here measures or records; it answers "which machine am I
# looking at", which is the question that has to be settled before any number
# from this node means anything.
#
# The DPU states its own topology in one line at startup, so that line is the
# source rather than anything inferred from traffic:
#
#   DPU PROXY MODE ON (... N/K/A=32/8/8; ... l7-layer=off, ...)
#
# N is DPA execution units, K rings per pod, A ARM data workers. The line is
# written once per DPU start, which is once per deploy, so the search widens
# until it finds the most recent one. Failing to find it is a finding: the DPU
# is not up, or its log is gone.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS="${NS:-test-bench}"

fail() { echo "bench-config: $*" >&2; exit 1; }

banner=""
for lines in 4000 40000 200000; do
    log="$(cd "$ROOT" && timeout 180 bench/bench.sh dpulog "$lines" 2>/dev/null)" \
        || fail "cannot read the DPU log (is .env present, and is the DPU up?)"
    banner="$(printf '%s\n' "$log" | grep 'DPU PROXY MODE ON' | tail -1)"
    [ -n "$banner" ] && break
done
[ -n "$banner" ] || fail "no startup banner in the last 200000 log lines; restart the DPU or widen the search"

nka="$(printf '%s' "$banner" | sed -n 's/.*N\/K\/A=\([0-9]*\)\/\([0-9]*\)\/\([0-9]*\).*/\1 \2 \3/p')"
[ -n "$nka" ] || fail "the startup banner carries no N/K/A: $banner"
read -r dpa_threads rings workers <<<"$nka"
l7="$(printf '%s' "$banner" | sed -n 's/.*l7-layer=\([a-z0-9_-]*\).*/\1/p')"
lb="$(printf '%s' "$banner" | sed -n 's/.*lb=\([a-z0-9_-]*\).*/\1/p')"

# --- what shares the machine -------------------------------------------------
# Not just the pods under test: a campaign left running in the same namespace is
# a load the measurement cannot see but does feel. A gRPC campaign and an L4
# campaign also produce different sets here, which is what keeps their series
# apart without anyone having to label them.
pods="$(kubectl get pods -n "$NS" --no-headers 2>/dev/null \
    | awk '$3=="Running"{print $1}' | sed 's/-[a-z0-9]\{6,10\}-[a-z0-9]\{5\}$//' | sort -u)"
pod_count="$(printf '%s\n' "$pods" | grep -c .)"
[ "$pod_count" -gt 0 ] || fail "no Running pod in namespace $NS"
pods_id="$(printf '%s\n' "$pods" | sha1sum | cut -c1-8)"

# --- where the client processes sit ------------------------------------------
# The pods run on this node, so their affinity is readable here. Pinning is a
# knob the numbers depend on.
# The set of affinities the load generators sit on, not one of them: a campaign
# runs several clients and a single number would name whichever process pgrep
# happened to return first.
pins="$(for name in bench_dpumesh; do
            for pid in $(pgrep -x "$name" 2>/dev/null); do
                taskset -pc "$pid" 2>/dev/null | sed 's/.*: //'
            done
        done | sort -u | paste -sd+ -)"
[ -n "$pins" ] || pins=none

cat <<OUT
dpu_dpa_threads=$dpa_threads
dpu_rings_per_pod=$rings
dpu_workers=$workers
dpu_l7=$l7
dpu_lb=$lb
dpu_pods=$pod_count
dpu_pods_id=$pods_id
dpu_pin_clients=$pins
dpu_cpus=$(nproc)
OUT
