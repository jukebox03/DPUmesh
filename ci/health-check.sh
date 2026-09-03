#!/bin/bash
# Ask whether the campaign deployed on this node still answers, and print one
# JSON object describing what was found on stdout. Human-readable progress goes
# to stderr, so `ci/health-check.sh > record.json` gives a record and a running
# commentary at the same time.
#
# Exit 0 when there is nothing wrong: a campaign answered, none is deployed, or
# this machine is busy with a measurement someone started by hand. Exit 1 when a
# campaign is deployed and the path is not usable.
#
# The busy case exists because the request below is real load and the clients'
# control servers are serial accept loops: a probe that arrives during a hand-run
# campaign perturbs it, and then times out and calls it a fault. Two guards keep
# that from happening -- the load average, and a PING that a client occupied with
# another RUN cannot answer.
#
# What is deliberately NOT here is a number. The smoke request exists to prove
# the path carries bytes; its latency is a single sample against whatever
# happens to be deployed, and recording it would start a performance series
# that says nothing about a chosen configuration. Performance is measured by
# hand, from bench/suite/, by someone who chose the configuration.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NS="${NS:-test-bench}"
# Above this 1-minute load average the machine is taken to be working, and this
# run records that instead of adding to it.
HEALTH_MAX_LOAD="${HEALTH_MAX_LOAD:-3.0}"
# The order paths are tried in. The first one that is part of the deployed
# campaign answers for it; the rest are simply not in this campaign.
PATHS="${HEALTH_PATHS:-grpc-dpumesh dpumesh preload}"

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
    for cast in (int, float):
        try:
            out[key] = cast(value)
            break
        except ValueError:
            continue
    else:
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

# First guard. Reading the DPU's log and sending a request both cost the machine
# something, so this comes before either of them.
load="$(cut -d' ' -f1 /proc/loadavg)"
add load "$load"
if awk -v l="$load" -v max="$HEALTH_MAX_LOAD" 'BEGIN { exit !(l + 0 > max + 0) }'; then
    say "1-minute load is $load, over the $HEALTH_MAX_LOAD limit; this machine is working"
    add status busy
    emit 0
fi

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
    # Second guard. A client already serving a RUN leaves this connection in its
    # backlog and says nothing, which from here is indistinguishable from a
    # wedged one -- so silence is recorded, not failed. A client that is really
    # wedged stays silent, and reads on the page as consecutive busy runs.
    case "$("$ROOT/bench/bench.sh" ping "$sol" 2>&1 </dev/null | tail -1)" in
        *"ERR no_pod("*)
            say "$sol is not part of this campaign"
            continue ;;
        OK*) ;;
        *"ERR silent"*)
            say "$sol did not answer PING: it is occupied, or it is wedged"
            add status busy
            add busy_path "$sol"
            emit 0 ;;
        *)
            # The port refused the connection, or the target is not a name this
            # harness knows. Neither is ambiguous, and neither is busy.
            say "$sol is deployed and its control port is not usable"
            add status no_answer
            add answered "$sol"
            emit 1 ;;
    esac

    # The window between the PING and this line is small and unguarded: a
    # campaign started inside it is reported as a fault.
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
