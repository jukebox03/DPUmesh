#!/bin/bash
# Does the mesh's control plane actually decide anything?
#
# Every other campaign in this tree runs with no policy and no route attached,
# so all of it is served by the default policy — the one the proxy is
# configured with, not one Kubernetes authored. This one attaches real
# resources and reads back whether the data path changed:
#
#   policy   a Server, then AuthorizationPolicies keyed on the caller's
#            identity and on its address, each with a negative control that
#            names a different caller
#   routing  an HTTPRoute on the protocol-aware Service, then a match that
#            cannot match and a backendRef in another Service
#   balance  the endpoint set changed under a Service, and the two balancing
#            grains the two protocol treatments have
#
# A stage is judged by two independent readings: whether the client's requests
# completed, and what the DPU's own counters say the enforcement point decided.
# A stage that fails traffic without a matching verdict is not a policy result.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$SUITE_DIR/../bench.sh"
FIXTURES="$SUITE_DIR/../k8s/policy"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
export NS CTRL_PORT
SCOPE="${1:-all}"
OUT_DIR="${OUT_DIR:-$SUITE_DIR/../report/data/policy-route-$(date +%Y%m%d-%H%M%S)}"
DUR="${DUR:-10}"
WARM="${WARM:-100}"
REQ="${REQ:-1024}"
# The verdict is taken once per connection and remembered on it, so a run that
# opens one connection tests one verdict. Churn is what makes a stage a
# statement about the policy rather than about the connection that predated it.
RECONN="${RECONN:-200}"
# Longer than the inbound policy watch's idle timeout, which is what the
# fail-open stage measures.
IDLE_PROBE="${IDLE_PROBE:-100}"

TRUST_DOMAIN="${DPUMESH_IDENTITY_TRUST_DOMAIN:-linkerd.${LINKERD_TRUST_DOMAIN:-cluster.local}}"
GRPC_METHOD_PREFIX="${GRPC_METHOD_PREFIX:-/grpc.testing.BenchmarkService}"

mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/stages.csv"
LOG="$OUT_DIR/raw.log"
# One output directory may hold several scopes, so the header is written once
# and every scope appends to it.
[ -s "$CSV" ] ||
    echo "stage,fixture,arm,expected,observed,verdict,rcnt,fail,admitted,denied,route_hits,target_mismatch,note" >"$CSV"

SSH_OPTS=(-o ServerAliveInterval=15 -o ServerAliveCountMax=4
          -o ConnectTimeout=10 -o BatchMode=yes)
say()  { printf '\n=== %s\n' "$*" | tee -a "$LOG"; }
note() { printf '    %s\n' "$*" | tee -a "$LOG"; }
field() { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }

### ------------------------------------------------------------ DPU readings
# Every ARM worker runs its own Linkerd runtime with its own admin endpoint, so
# a per-worker counter has to be summed and a process-global one must not be.
ADMIN_PORTS=()
discover_admin_ports() {
    local base="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}" port
    base="${base##*:}"
    for ((port = base; port < base + 8; port++)); do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
            "curl -sf --max-time 3 127.0.0.1:$port/metrics >/dev/null" 2>/dev/null &&
            ADMIN_PORTS+=("$port")
    done
    [ "${#ADMIN_PORTS[@]}" -gt 0 ] || { echo "no DPU admin endpoint answered" >&2; exit 1; }
}

dpu_metrics() {
    local port
    for port in "${ADMIN_PORTS[@]}"; do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
            "curl -sf --max-time 5 127.0.0.1:$port/metrics" 2>/dev/null || true
    done
}

# The control-event family is written from several threads into one
# process-global registry, so every admin endpoint reports the same value.
ctl_event() {
    ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
        "curl -sf --max-time 5 127.0.0.1:${ADMIN_PORTS[0]}/metrics |
         sed -n 's/^dmesh_control_events_total{kind=\"$1\",reason=\"$2\"} //p'" \
        2>/dev/null | tr -d '[:space:]' | head -n1 | sed 's/^$/0/'
}

# Sum every sample whose line matches, across all worker registries.
metric_sum() {
    awk -v pattern="$2" '$0 ~ pattern { total += $NF } END { print total + 0 }' <<<"$1"
}

### ------------------------------------------------------------ fixtures
render() { envsubst <"$FIXTURES/$1"; }
fixture_apply()  { render "$1" | kubectl apply -f - >>"$LOG" 2>&1; }
fixture_delete() { render "$1" | kubectl delete --ignore-not-found -f - >>"$LOG" 2>&1 || true; }

drop_all_fixtures() {
    local f
    for f in httproute.yaml authz-network.yaml authz-identity.yaml server.yaml; do
        fixture_delete "$f"
    done
}

restore_replicas() {
    kubectl scale deployment/echo-dpumesh-14 -n "$NS" --replicas=1 >>"$LOG" 2>&1 || true
    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=1 >>"$LOG" 2>&1 || true
}

cleanup() {
    say "cleanup"
    drop_all_fixtures
    restore_replicas
    note "fixtures removed; replica counts restored"
}
trap cleanup EXIT

### ------------------------------------------------------------ one stage
# stage <id> <fixture-label> <arm> <serve|refuse> <threads> <reconn> [note] [max-admitted]
#
# `max-admitted` bounds how many streams a stage may admit. A refusing stage
# that admits anything admitted it against a policy that refuses, which is the
# one failure traffic alone cannot show: the run fails either way, and only the
# verdict counter says whether it failed for the right reason.
PASSES=0; FAILURES=0
stage() {
    local id="$1" fixture="$2" arm="$3" expected="$4" threads="${5:-1}" reconn="${6:-$RECONN}" extra="${7:-}" max_admitted="${8:-}"
    local a0 d0 m0 r0 a1 d1 m1 r1 metrics result rcnt fail observed verdict

    a0=$(ctl_event inbound admitted); d0=$(ctl_event inbound denied)
    metrics=$(dpu_metrics)
    m0=$(metric_sum "$metrics" '^dmesh_backend_target_mismatches_total')
    r0=$(metric_sum "$metrics" 'outbound_http_route_request_statuses_total.*route_kind="HTTPRoute"')

    result=$(bash "$BENCH" point "$arm" "$REQ" 8 1 "$DUR" "$WARM" "$threads" "$reconn" 2>/dev/null | tail -n1)
    printf '%s | %s\n' "$id" "$result" >>"$LOG"

    rcnt=$(field "$result" rcnt); fail=$(field "$result" fail)
    : "${rcnt:=0}" "${fail:=0}"
    a1=$(ctl_event inbound admitted); d1=$(ctl_event inbound denied)
    metrics=$(dpu_metrics)
    m1=$(metric_sum "$metrics" '^dmesh_backend_target_mismatches_total')
    r1=$(metric_sum "$metrics" 'outbound_http_route_request_statuses_total.*route_kind="HTTPRoute"')

    if [ "$fail" -eq 0 ] && [ "$rcnt" -gt 0 ]; then observed=serve; else observed=refuse; fi
    if [ "$observed" != "$expected" ]; then
        verdict=FAIL
    elif [ -n "$max_admitted" ] && [ "$((a1 - a0))" -gt "$max_admitted" ]; then
        verdict=FAIL
        extra="$extra; admitted $((a1 - a0)) > $max_admitted"
    else
        verdict=PASS
    fi
    if [ "$verdict" = PASS ]; then PASSES=$((PASSES + 1)); else FAILURES=$((FAILURES + 1)); fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$id" "$fixture" "$arm" "$expected" "$observed" "$verdict" \
        "$rcnt" "$fail" "$((a1 - a0))" "$((d1 - d0))" "$((r1 - r0))" "$((m1 - m0))" \
        "$extra" >>"$CSV"
    note "$id [$verdict] $fixture: expected=$expected observed=$observed rcnt=$rcnt fail=$fail admitted+=$((a1 - a0)) denied+=$((d1 - d0)) route_hits+=$((r1 - r0)) mismatch+=$((m1 - m0))"
}

### ------------------------------------------------------------ inputs
caller_identity() {
    local sa ns
    sa=$(kubectl get pod -n "$NS" -l app=bench-dpumesh \
          -o jsonpath='{.items[0].spec.serviceAccountName}' 2>/dev/null)
    ns="$NS"
    [ -n "$sa" ] || sa=default
    printf '%s.%s.serviceaccount.identity.%s\n' "$sa" "$ns" "$TRUST_DOMAIN"
}
caller_address() {
    kubectl get pod -n "$NS" -l app=bench-dpumesh \
        -o jsonpath='{.items[0].status.podIP}' 2>/dev/null
}

### ------------------------------------------------------------ campaigns
run_policy() {
    say "policy — inbound authorization on the native Service"
    export POLICY_IDENTITY POLICY_NETWORK
    POLICY_IDENTITY="$(caller_identity)"
    POLICY_NETWORK="$(caller_address)/32"
    note "caller identity: $POLICY_IDENTITY"
    note "caller address:  $POLICY_NETWORK"

    drop_all_fixtures; sleep 5
    stage P0 "no policy" dpumesh serve 1 "$RECONN" "default policy admits"

    fixture_apply server.yaml; sleep 6
    stage P1 "Server, no authorization" dpumesh refuse 1 "$RECONN" "deny by default"

    say "P1b — the watch's idle timeout, probed after ${IDLE_PROBE}s of silence"
    sleep "$IDLE_PROBE"
    # The watch is held for the destination Pod's registration, so silence
    # cannot return it to the configured default. Nothing may be admitted here.
    stage P1b "Server, no authorization (after ${IDLE_PROBE}s idle)" dpumesh refuse 1 "$RECONN" \
        "no stream may be admitted after silence" 0

    fixture_apply authz-identity.yaml; sleep 6
    stage P2 "AuthorizationPolicy: caller identity" dpumesh serve 1 "$RECONN" "identity allows"

    POLICY_IDENTITY="not-the-caller.$NS.serviceaccount.identity.$TRUST_DOMAIN"
    fixture_apply authz-identity.yaml; sleep 6
    stage P3 "AuthorizationPolicy: another identity" dpumesh refuse 1 "$RECONN" \
        "negative control for identity"
    fixture_delete authz-identity.yaml

    fixture_apply authz-network.yaml; sleep 6
    stage P4 "AuthorizationPolicy: caller address" dpumesh serve 1 "$RECONN" "networks clause allows"

    POLICY_NETWORK="10.255.255.0/24"
    fixture_apply authz-network.yaml; sleep 6
    stage P5 "AuthorizationPolicy: another network" dpumesh refuse 1 "$RECONN" \
        "negative control for networks"

    drop_all_fixtures; sleep 6
    stage P6 "no policy" dpumesh serve 1 "$RECONN" "returned to the default"
}

run_route() {
    say "routing — outbound HTTPRoute on the protocol-aware Service"
    export ROUTE_PATH ROUTE_BACKEND
    ROUTE_PATH="$GRPC_METHOD_PREFIX"; ROUTE_BACKEND=echo-grpc-dpumesh

    fixture_delete httproute.yaml; sleep 5
    stage R0 "no route" grpc-dpumesh serve 1 0 "served by the default route"

    fixture_apply httproute.yaml; sleep 6
    stage R1 "HTTPRoute matching the method" grpc-dpumesh serve 1 0 "route_hits must be > 0"

    ROUTE_PATH="/never.matches.anything"
    fixture_apply httproute.yaml; sleep 6
    stage R2 "HTTPRoute matching nothing" grpc-dpumesh refuse 1 0 "no rule matches"

    ROUTE_PATH="$GRPC_METHOD_PREFIX"; ROUTE_BACKEND=echo-dpumesh
    fixture_apply httproute.yaml; sleep 6
    stage R3 "HTTPRoute to another Service" grpc-dpumesh refuse 1 0 \
        "backend registry refuses a foreign Service"

    fixture_delete httproute.yaml; sleep 6
    ROUTE_BACKEND=echo-grpc-dpumesh
    stage R4 "no route" grpc-dpumesh serve 1 0 "returned to the default route"
}

# The distribution the client observed, as backend:count pairs. The index is
# the DPU Pod slot that served the reply, so it names one backend across
# repetitions for as long as that registration lives.
lb_point() {
    local arm="$1" threads="$2"
    bash "$BENCH" point "$arm" "$REQ" 8 8 "$DUR" "$WARM" "$threads" 0 2>/dev/null | tail -n1
}
backend_slots() { tr ',' '\n' <<<"$(field "$1" dist)" | sed -n 's/^\([0-9]\+\):.*/\1/p' | sort -u; }

# One point offered in the background, with the balancer's endpoint set read
# while it is in flight: the gauge belongs to a live stack and returns to zero
# once the sessions it served are gone.
# The sampler runs inside a command substitution, so anything it assigns dies
# with that subshell; the reading is handed back through a file instead.
ENDPOINT_FILE="$OUT_DIR/.ready-endpoints"
lb_endpoints() { cat "$ENDPOINT_FILE" 2>/dev/null || echo NA; }
lb_point_sampled() {
    local arm="$1" threads="$2" family="$3" service="$4" out port
    out=$(mktemp)
    ( lb_point "$arm" "$threads" >"$out" 2>&1 & )
    sleep $(( DUR / 3 + 3 ))
    local pattern
    pattern="^outbound_${family}_balancer_endpoints.*endpoint_state=\\\"ready\\\".*parent_name=\\\"${service}\\\""
    printf '%s\n' "$(
        for port in "${ADMIN_PORTS[@]}"; do
            ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
                "curl -sf --max-time 5 127.0.0.1:$port/metrics | grep -E \"$pattern\"" \
                2>/dev/null
        done | awk '{ if ($NF+0 > max) max = $NF+0 } END { print max+0 }'
    )" >"$ENDPOINT_FILE"
    until grep -qE '^(OK|ERR)' "$out" 2>/dev/null; do sleep 2; done
    tail -n1 "$out"; rm -f "$out"
}

lb_record() {
    local id="$1" label="$2" arm="$3" result="$4" slots note_extra="${5:-}"
    slots=$(backend_slots "$result" | tr '\n' ' ')
    note "$id $label: backends={${slots% }} ready_endpoints=$(lb_endpoints) fail=$(field "$result" fail) dist=$(field "$result" dist) $note_extra"
    printf '%s,%s,%s,,,,%s,%s,,,,,backends={%s} ready_endpoints=%s dist=%s %s\n' \
        "$id" "$label" "$arm" "$(field "$result" rcnt)" "$(field "$result" fail)" \
        "${slots% }" "$(lb_endpoints)" "$(field "$result" dist)" "$note_extra" >>"$CSV"
}

# Requests each destination Pod served over one point, as a delta: the metric
# is cumulative and carries labels for Pods that have long since gone.
grpc_pod_requests() {
    dpu_metrics | grep -E '^request_total.*direction="outbound".*echo-grpc-dpumesh' |
        sed 's/.*dst_pod="\([^"]*\)".*} /\1 /' |
        awk '{ t[$1] += $2 } END { for (pod in t) if (t[pod] > 0) print pod, t[pod] }' | sort
}

grpc_backends() {
    local id="$1" channels="$2" before after result used
    before=$(grpc_pod_requests)
    result=$(lb_point_sampled grpc-dpumesh "$channels" http echo-grpc-dpumesh)
    after=$(grpc_pod_requests)
    used=$(join -a1 -a2 -e 0 -o 0,1.2,2.2 <(printf '%s\n' "$before") <(printf '%s\n' "$after") |
           awk '{ if ($3 - $2 > 0) printf "%s=%d ", $1, $3 - $2 }')
    note "$id grpc, $channels channel(s): ready_endpoints=$(lb_endpoints) rcnt=$(field "$result" rcnt) fail=$(field "$result" fail) served: ${used}"
    printf '%s,grpc %s channels,grpc-dpumesh,,,,%s,%s,,,,,ready_endpoints=%s served: %s\n' \
        "$id" "$channels" "$(field "$result" rcnt)" "$(field "$result" fail)" \
        "$(lb_endpoints)" "$used" >>"$CSV"
}

run_lb() {
    say "balancing — connection grain, endpoint changes, and the request grain"
    local result rep threads union

    say "L1 — how many of the three backends one client reaches"
    for threads in 1 2 4 6; do
        for rep in 1 2 3; do
            rm -f "$ENDPOINT_FILE"
            result=$(lb_point dpumesh "$threads")
            lb_record "L1" "opaque, $threads threads, rep $rep" dpumesh "$result"
        done
    done

    # Scaling a backend down while traffic flows faults the engine-shared DMA
    # context, so the endpoint set is changed against an idle data path and
    # allowed to settle before the next point is offered.
    say "L2 — one backend withdrawn from the Service"
    kubectl scale deployment/echo-dpumesh-14 -n "$NS" --replicas=0 >>"$LOG" 2>&1
    kubectl wait --for=delete pod -n "$NS" -l app=echo-dpumesh-14 --timeout=90s >>"$LOG" 2>&1 || true
    sleep 15
    union=""
    for rep in 1 2 3; do
        result=$(lb_point_sampled dpumesh 6 tcp echo-dpumesh)
        lb_record "L2" "two backends, rep $rep" dpumesh "$result"
        union="$union $(backend_slots "$result" | tr '\n' ' ')"
    done
    note "L2 backend slots seen over three repetitions: $(tr ' ' '\n' <<<"$union" | sort -u | tr '\n' ' ')"

    say "L3 — the backend restored"
    kubectl scale deployment/echo-dpumesh-14 -n "$NS" --replicas=1 >>"$LOG" 2>&1
    kubectl wait --for=condition=Ready pod -n "$NS" -l app=echo-dpumesh-14 --timeout=120s >>"$LOG" 2>&1 || true
    sleep 20
    union=""
    for rep in 1 2 3; do
        result=$(lb_point_sampled dpumesh 6 tcp echo-dpumesh)
        lb_record "L3" "three backends, rep $rep" dpumesh "$result"
        union="$union $(backend_slots "$result" | tr '\n' ' ')"
    done
    note "L3 backend slots seen over three repetitions: $(tr ' ' '\n' <<<"$union" | sort -u | tr '\n' ' ')"

    say "L4 — the request grain against two protocol-aware backends"
    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=2 >>"$LOG" 2>&1
    kubectl wait --for=condition=Ready pod -n "$NS" -l app=echo-grpc-dpumesh --timeout=150s >>"$LOG" 2>&1 || true
    sleep 25
    # One client channel is one DMA session, and a session owns one backend
    # channel. Offering the same load over one channel and over four is what
    # separates "the balancer holds both endpoints" from "one request may go
    # anywhere".
    grpc_backends L4a 1
    grpc_backends L4b 4

    say "L5 — back to one backend"
    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=1 >>"$LOG" 2>&1
    kubectl wait --for=delete pod -n "$NS" -l app=echo-grpc-dpumesh --timeout=90s >>"$LOG" 2>&1 || true
    sleep 20
    rm -f "$ENDPOINT_FILE"
    result=$(lb_point grpc-dpumesh 1)
    lb_record "L5" "grpc, one backend" grpc-dpumesh "$result"
}

### ------------------------------------------------------------ main
: "${DPU_HOST:?.env missing DPU_HOST}"
discover_admin_ports
say "DPU admin endpoints: ${ADMIN_PORTS[*]}"
say "output: $OUT_DIR"

case "$SCOPE" in
    policy) run_policy ;;
    route)  run_route ;;
    lb)     run_lb ;;
    all)    run_policy; run_route; run_lb ;;
    *) echo "usage: $0 [all|policy|route|lb]" >&2; exit 1 ;;
esac

say "stages: $PASSES pass, $FAILURES fail"
say "csv: $CSV"
[ "$FAILURES" -eq 0 ]
