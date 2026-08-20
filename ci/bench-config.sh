#!/bin/bash
# Print the configuration a measurement was taken under, as KEY=VALUE lines for
# ci/bench-to-json.py to merge into the point.
#
# ci/bench-frozen.txt fixes what the client asks for: sizes, concurrency,
# threads. It cannot fix what the DPU is, and the DPU is what moves the number.
# Two points carrying the same label and a different config_id are measurements
# of two different machines, so the published page breaks its line between them
# instead of drawing a slope that means nothing.
#
# The DPU states its own topology in one line at startup, so that line is the
# source rather than anything inferred from traffic:
#
#   DPU PROXY MODE ON (... N/K/A=32/8/8; ... l7-layer=off, ...)
#
# N is DPA execution units, K rings per pod, A ARM data workers. The line is
# written once per DPU start, which is once per deploy, so the search widens
# until it finds the most recent one.
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
# The pods run on this node, so their affinity is readable here. Pinning is the
# knob that moved these numbers the most in past campaigns.
# The set of affinities the load generators sit on, not one of them: a campaign
# runs several clients and a single number would name whichever process pgrep
# happened to return first.
pins="$(for name in bench_dpumesh bench_sock bench_grpc; do
            for pid in $(pgrep -x "$name" 2>/dev/null); do
                taskset -pc "$pid" 2>/dev/null | sed 's/.*: //'
            done
        done | sort -u | paste -sd+ -)"
[ -n "$pins" ] || pins=none

hashed="nka=$dpa_threads/$rings/$workers l7=$l7 lb=$lb pods=$pods_id pin=$pins cpus=$(nproc)"
config_id="$(printf '%s' "$hashed" | sha1sum | cut -c1-12)"

cat <<OUT
config_id=$config_id
cfg_dpa_threads=$dpa_threads
cfg_rings_per_pod=$rings
cfg_workers=$workers
cfg_l7=$l7
cfg_lb=$lb
cfg_pods=$pod_count
cfg_pods_id=$pods_id
cfg_pin_clients=$pins
cfg_cpus=$(nproc)
OUT
