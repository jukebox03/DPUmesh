#!/bin/bash
# Ask whether the campaign deployed on this node still answers, and print one
# JSON object describing what was found on stdout. Human-readable progress goes
# to stderr, so `ci/health-check.sh > record.json` gives a record and a running
# commentary at the same time.
#
# Exit 0 when there is nothing wrong: either a campaign answered, or none is
# deployed. Exit 1 when a campaign is deployed and the path is not usable.
#
# What is deliberately NOT here is a number. The smoke request exists to prove
# the path carries bytes; its latency is a single sample against whatever
# happens to be deployed, and recording it would rebuild the performance series
# this file replaced. Performance is measured by hand, from bench/suite/, by
# someone who chose the configuration.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS="${NS:-test-bench}"
# The order paths are tried in. The first one that is part of the deployed
# campaign answers for it; the rest are simply not in this campaign.
PATHS="${HEALTH_PATHS:-grpc-dpumesh dpumesh grpc-tcp tcp grpc-envoy}"

say() { echo "$*" >&2; }
fields=()
add() { fields+=("$1=$2"); }

add ts "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
add host "$(hostname)"
add commit "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

emit() {   # emit <exit-code>
    python3 - "${fields[@]}" <<'PY'
import json, sys
out = {}
for arg in sys.argv[1:]:
    key, _, value = arg.partition("=")
    if value in ("", "-"):
        out[key] = None
        continue
    try:
        out[key] = int(value)
    except ValueError:
        out[key] = value
if "clients" in out and out["clients"]:
    out["clients"] = out["clients"].split(",")
json.dump(out, sys.stdout, sort_keys=True)
print()
PY
    exit "$1"
}

if ! kubectl get pods -n "$NS" >/dev/null 2>&1; then
    say "namespace $NS is not reachable"
    add status unreachable
    emit 1
fi

clients="$(kubectl get pods -n "$NS" --no-headers 2>/dev/null \
    | awk '$3=="Running" && $1 ~ /^bench-/ {sub(/-[a-z0-9]+-[a-z0-9]+$/,"",$1); print $1}' | sort | paste -sd, -)"
add clients "${clients:--}"

# An empty namespace between experiments is a normal state on a research
# machine, not a fault.
if [ -z "$clients" ]; then
    say "no bench client is deployed; nothing to check"
    add status idle
    emit 0
fi
say "deployed clients: ${clients//,/ }"

# The DPU's own startup line. Losing it means the DPU is down or its log is gone,
# which is a fault whenever a campaign is deployed.
if state="$("$ROOT/ci/dpu-state.sh" 2>/dev/null)"; then
    while read -r line; do [ -n "$line" ] && add "${line%%=*}" "${line#*=}"; done <<<"$state"
    say "DPU: $(tr '\n' ' ' <<<"$state")"
else
    say "the DPU did not state its topology; it is down, or its log is gone"
    add status no_dpu
    emit 1
fi

for sol in $PATHS; do
    raw="$("$ROOT/bench/bench.sh" point "$sol" 48 48 1 3 100 1 2>&1 </dev/null | tail -1)"
    case "$raw" in
        OK*)
            say "$sol answered"
            add status ok
            add answered "$sol"
            emit 0 ;;
        *"ERR no_pod("*)
            say "$sol is not part of this campaign" ;;
        *)
            say "$sol is deployed and did not answer: $raw"
            add status no_answer
            add answered "$sol"
            emit 1 ;;
    esac
done

say "a campaign is deployed but no known path is part of it"
add status no_path
emit 1
