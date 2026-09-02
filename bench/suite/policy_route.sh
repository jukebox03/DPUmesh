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
source "$SUITE_DIR/policy_route_judge.sh"
source "$SUITE_DIR/deployed_geometry.sh"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
# A fixture that stands up a workload needs what a workload manifest needs.
LIB_OUT="${LIB_OUT:-$PROJ_ROOT/build/lib}"
IMG_ECHO_GRPC="${IMG_ECHO_GRPC:-bench/echo-grpc:latest}"
BENCH_REACTORS="${BENCH_REACTORS:-8}"
BENCH_NUMA_NODE="${BENCH_NUMA_NODE:-}"
HOST_PCI="${HOST_PCI:-}"
DEPLOYED_GEOMETRY=$(resolve_deployed_geometry "$PROJ_ROOT")
read -r DPUMESH_RINGS_PER_POD DPUMESH_ARM_WORKERS <<<"$DEPLOYED_GEOMETRY"
DPUMESH_ATTEST_SOCKET="${DPUMESH_ATTEST_SOCKET:-/run/dpumesh/attest.sock}"
export NS CTRL_PORT LIB_OUT IMG_ECHO_GRPC BENCH_REACTORS BENCH_NUMA_NODE
export HOST_PCI DPUMESH_RINGS_PER_POD DPUMESH_ARM_WORKERS DPUMESH_ATTEST_SOCKET
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
    for ((port = base; port < base + DPUMESH_ARM_WORKERS; port++)); do
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
# A counter this DPU has never incremented reads as absent, and so does a read
# that failed. They are not the same fact: substituting zero for the second turns
# the next delta negative, which is a counter appearing to run backwards. An
# unreadable endpoint answers NA and the stage that used it says so.
ctl_event() {
    local raw value
    raw=$(ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
        "curl -sf --max-time 5 127.0.0.1:${ADMIN_PORTS[0]}/metrics" 2>/dev/null) || {
        echo NA; return 0; }
    [ -n "$raw" ] || { echo NA; return 0; }
    # A counter with no sample is a count of zero, and a match that produced no
    # line at all leaves nothing for a substitution to act on -- so the default
    # is applied to the value, not to a line that may not exist.
    value=$(sed -n "s/^dmesh_control_events_total{kind=\"$1\",reason=\"$2\"} //p" <<<"$raw" |
        head -n1 | tr -d '[:space:]')
    printf '%s\n' "${value:-0}"
}

# The change between two readings, or NA when either could not be read. A
# reading that is neither NA nor a count is also NA: a stage is better told that
# its counter is unreadable than handed an arithmetic error.
ctl_delta() {  # ctl_delta <before> <after>
    case "$1$2" in ''|*NA*|*[!0-9]*) echo NA; return 0;; esac
    echo $(( $2 - $1 ))
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
    for f in httproute.yaml httproute-timeout.yaml \
             httproute-method.yaml httproute-header.yaml httproute-cross.yaml \
             httproute-weighted.yaml grpcroute.yaml grpcroute-retry.yaml \
             authz-route.yaml authz-network.yaml authz-identity.yaml \
             echo-grpc-broken.yaml server-alt.yaml server-grpc.yaml server.yaml; do
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
    [ -z "${ENDPOINT_FILE:-}" ] || rm -f "$ENDPOINT_FILE"
    note "fixtures removed; replica counts restored"
}
trap cleanup EXIT

### ------------------------------------------------------------ one stage
# stage <id> <fixture-label> <arm> <serve|refuse> <threads> <reconn> [note]
#       [max-admitted] [max-fail]
#
# `max-admitted` bounds how many streams a stage may admit. A refusing stage
# that admits anything admitted it against a policy that refuses, which is the
# one failure traffic alone cannot show: the run fails either way, and only the
# verdict counter says whether it failed for the right reason.
#
# `max-fail` is the failures a serving stage may declare in advance. It is zero
# everywhere except where the feature under test cannot act without observing
# failures first — a circuit breaker ejects an endpoint by counting them — and
# there it is that feature's own threshold, not a tolerance for noise.
PASSES=0; FAILURES=0
# The deltas the last stage measured, for an arm whose gate is one of them
# rather than the traffic. A cross-Service stage cannot use `max-admitted`:
# the caller still reaches its own Service's backend, which no policy refuses,
# so what has to be asserted is that the *destination* refused.
LAST_ADMITTED_DELTA=0; LAST_DENIED_DELTA=0
# A reply carrying no reading is not one fact but two: an instrument that said
# nothing, and one that said why it could not run. Both fail the stage, and
# recording the second as the first loses the reason at the moment it was given.
NO_READING_OBSERVED=""
NO_READING_REASON=""
no_reading() {  # no_reading <reply>
    case "$1" in
        '')   NO_READING_OBSERVED=nodata; NO_READING_REASON="the client did not answer" ;;
        ERR*) NO_READING_OBSERVED=error;  NO_READING_REASON="${1//,/;}" ;;
        *)    NO_READING_OBSERVED=nodata; NO_READING_REASON="unreadable reply: ${1//,/;}" ;;
    esac
}

stage() {
    local id="$1" fixture="$2" arm="$3" expected="$4" threads="${5:-1}" reconn="${6:-$RECONN}" extra="${7:-}" max_admitted="${8:-}" max_fail="${9:-0}"
    local a0 d0 m0 r0 a1 d1 m1 r1 metrics result rcnt fail observed verdict
    # The CSV is read column-wise, so no field may carry the separator: free
    # text that does shifts every column after it in that row alone, which
    # reads as a stage that reported different things from its neighbours.
    fixture="${fixture//,/;}"; extra="${extra//,/;}"

    a0=$(ctl_event inbound admitted); d0=$(ctl_event inbound denied)
    metrics=$(dpu_metrics)
    m0=$(metric_sum "$metrics" '^dmesh_backend_target_mismatches_total')
    r0=$(metric_sum "$metrics" 'outbound_http_route_request_statuses_total.*route_kind="HTTPRoute"')

    result=$(bash "$BENCH" point "$arm" "$REQ" 8 1 "$DUR" "$WARM" "$threads" "$reconn" 2>/dev/null | tail -n1)
    printf '%s | %s\n' "$id" "$result" >>"$LOG"

    # A missing reading is not a refusal: recording it as one would let an
    # instrument that could not run pass a stage that expects traffic to stop.
    if ! grep -q 'rcnt=' <<<"$result"; then
        no_reading "$result"
        FAILURES=$((FAILURES + 1))
        LAST_ADMITTED_DELTA=0; LAST_DENIED_DELTA=0
        printf '%s,%s,%s,%s,%s,FAIL,,,,,,,%s\n' \
            "$id" "$fixture" "$arm" "$expected" "$NO_READING_OBSERVED" \
            "$NO_READING_REASON" >>"$CSV"
        note "$id [FAIL] $fixture: $NO_READING_REASON — no reading"
        return 0
    fi
    rcnt=$(field "$result" rcnt); fail=$(field "$result" fail)
    : "${rcnt:=0}" "${fail:=0}"
    a1=$(ctl_event inbound admitted); d1=$(ctl_event inbound denied)
    local admitted_delta denied_delta
    admitted_delta=$(ctl_delta "$a0" "$a1"); denied_delta=$(ctl_delta "$d0" "$d1")
    metrics=$(dpu_metrics)
    m1=$(metric_sum "$metrics" '^dmesh_backend_target_mismatches_total')
    r1=$(metric_sum "$metrics" 'outbound_http_route_request_statuses_total.*route_kind="HTTPRoute"')

    if [ "$fail" -le "$max_fail" ] && [ "$rcnt" -gt 0 ]; then observed=serve; else observed=refuse; fi
    if [ "$observed" != "$expected" ]; then
        verdict=FAIL
    elif [ -n "$max_admitted" ] && [ "$admitted_delta" = NA ]; then
        verdict=FAIL
        extra="$extra; the admitted counter was unreadable"
    elif [ -n "$max_admitted" ] && [ "$admitted_delta" -gt "$max_admitted" ]; then
        verdict=FAIL
        extra="$extra; admitted $admitted_delta > $max_admitted"
    else
        verdict=PASS
    fi
    LAST_ADMITTED_DELTA=$admitted_delta; LAST_DENIED_DELTA=$denied_delta
    if [ "$verdict" = PASS ]; then PASSES=$((PASSES + 1)); else FAILURES=$((FAILURES + 1)); fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$id" "$fixture" "$arm" "$expected" "$observed" "$verdict" \
        "$rcnt" "$fail" "$admitted_delta" "$denied_delta" "$((r1 - r0))" "$((m1 - m0))" \
        "$extra" >>"$CSV"
    note "$id [$verdict] $fixture: expected=$expected observed=$observed rcnt=$rcnt fail=$fail admitted+=$admitted_delta denied+=$denied_delta route_hits+=$((r1 - r0)) mismatch+=$((m1 - m0))"
}

# A stage whose subject is a counter rather than the traffic. Some features
# leave the client's reading unchanged whether or not they acted — an absorbed
# failure and a failure that never happened complete the same request — so what
# they did is only visible in the enforcement point's own counter.
counter_stage() {  # counter_stage <id> <label> <arm> <delta> <detail>
    local id="$1" label="$2" arm="$3" delta="$4" detail="$5" observed verdict
    label="${label//,/;}"; detail="${detail//,/;}"
    if [ "$delta" -gt 0 ]; then observed=fired; verdict=PASS; else observed=silent; verdict=FAIL; fi
    if [ "$verdict" = PASS ]; then PASSES=$((PASSES + 1)); else FAILURES=$((FAILURES + 1)); fi
    printf '%s,%s,%s,fired,%s,%s,,,,,,,%s\n' \
        "$id" "$label" "$arm" "$observed" "$verdict" "$detail" >>"$CSV"
    note "$id [$verdict] $label: $detail"
}

### ------------------------------------------------------------ inputs
caller_identity_of() {
    local sa
    sa=$(kubectl get pod -n "$NS" -l "app=$1" \
          -o jsonpath='{.items[0].spec.serviceAccountName}' 2>/dev/null)
    [ -n "$sa" ] || sa=default
    printf '%s.%s.serviceaccount.identity.%s\n' "$sa" "$NS" "$TRUST_DOMAIN"
}
caller_identity() { caller_identity_of bench-dpumesh; }
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

    # A route may cross Services now, so the backend has to be one that speaks
    # the same protocol — otherwise the arm measures the application, not the
    # route. The policy half of this is the cross scope's X2.
    ROUTE_PATH="$GRPC_METHOD_PREFIX"; ROUTE_BACKEND=echo-grpc-alt
    fixture_apply httproute.yaml; sleep 8
    stage R3 "HTTPRoute to another Service" grpc-dpumesh serve 1 0 \
        "liveness guards the dial, not Service identity"

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
    local arm="$1" threads="$2" family="$3" service="$4" out port point_pid
    out=$(mktemp)
    lb_point "$arm" "$threads" >"$out" 2>&1 &
    point_pid=$!
    sleep $(( DUR / 3 + 3 ))
    local pattern
    pattern="^outbound_${family}_balancer_endpoints.*endpoint_state=\\\"ready\\\".*parent_name=\\\"${service}\\\""
    printf '%s\n' "$(
        for port in "${ADMIN_PORTS[@]}"; do
            ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
                "curl -sf --max-time 5 127.0.0.1:$port/metrics | grep -E \"$pattern\"" \
                2>/dev/null || true
        done | awk '$NF+0 > 0 {
                        value=$NF+0;
                        if (!seen || value < min) min=value;
                        if (!seen || value > max) max=value;
                        seen=1
                    }
                    END {
                        if (!seen) print "NA";
                        else if (min != max) printf "mixed-%d-%d\n", min, max;
                        else print max
                    }'
    )" >"$ENDPOINT_FILE"
    if ! wait "$point_pid"; then :; fi
    tail -n1 "$out"; rm -f "$out"
}

lb_record() {
    local id="$1" label="$2" arm="$3" result="$4" expected_ready="$5"
    local min_backends="$6" max_backends="$7" note_extra="${8:-}"
    local slots dist dist_csv ready backends expected verdict reason
    slots=$(backend_slots "$result" | tr '\n' ' ')
    dist=$(field "$result" dist)
    dist_csv="${dist//,/;}"
    ready=$(lb_endpoints)
    backends=$(printf '%s\n' "$slots" | tr ' ' '\n' | grep -c '[0-9]' || true)
    expected="serve+ready=$expected_ready+backends=$min_backends..$max_backends"
    if policy_route_judge_lb "$result" "$ready" "$expected_ready" \
                              "$backends" "$min_backends" "$max_backends"; then
        verdict=PASS; PASSES=$((PASSES + 1)); reason=""
    else
        verdict=FAIL; FAILURES=$((FAILURES + 1)); reason="$LB_JUDGE_REASON"
    fi
    note "$id [$verdict] $label: backends={${slots% }} ready_endpoints=$ready rcnt=$LB_JUDGE_RCNT fail=$LB_JUDGE_FAIL dist=${dist:-NA} ${reason:+reason=$reason} $note_extra"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,,,,,%s\n' \
        "$id" "${label//,/;}" "$arm" "$expected" "$LB_JUDGE_OBSERVED" "$verdict" \
        "$LB_JUDGE_RCNT" "$LB_JUDGE_FAIL" \
        "backends={${slots% }} ready_endpoints=$ready dist=${dist_csv:-NA} ${reason:+reason=$reason} ${note_extra//,/;}" >>"$CSV"
}

# Requests each destination Pod served over one point, as a delta: the metric
# is cumulative and carries labels for Pods that have long since gone.
grpc_pod_requests() {
    { dpu_metrics | grep -E '^request_total.*direction="outbound".*echo-grpc-dpumesh' || true; } |
        sed 's/.*dst_pod="\([^"]*\)".*} /\1 /' |
        awk '{ t[$1] += $2 } END { for (pod in t) if (t[pod] > 0) print pod, t[pod] }' | sort
}

grpc_backends() {
    local id="$1" channels="$2" expected_backends="$3"
    local before after result used ready backends verdict reason expected
    local metrics take_before take_after take_delta
    before=$(grpc_pod_requests)
    metrics=$(dpu_metrics)
    take_before=$(metric_sum "$metrics" '^dmesh_backend_take_errors_total')
    result=$(lb_point_sampled grpc-dpumesh "$channels" http echo-grpc-dpumesh)
    after=$(grpc_pod_requests)
    metrics=$(dpu_metrics)
    take_after=$(metric_sum "$metrics" '^dmesh_backend_take_errors_total')
    take_delta=$((take_after - take_before))
    used=$(join -a1 -a2 -e 0 -o 0,1.2,2.2 <(printf '%s\n' "$before") <(printf '%s\n' "$after") |
           awk '{ if ($3 - $2 > 0) printf "%s=%d ", $1, $3 - $2 }')
    ready=$(lb_endpoints)
    backends=$(printf '%s\n' "$used" | tr ' ' '\n' | grep -c '=' || true)
    expected="serve+ready=$expected_backends+backends=$expected_backends"
    if policy_route_judge_lb "$result" "$ready" "$expected_backends" \
                              "$backends" "$expected_backends" "$expected_backends"; then
        if [ "$take_delta" -eq 0 ]; then
            verdict=PASS; PASSES=$((PASSES + 1)); reason=""
        else
            verdict=FAIL; FAILURES=$((FAILURES + 1))
            reason="backend take errors increased by $take_delta"
        fi
    else
        verdict=FAIL; FAILURES=$((FAILURES + 1)); reason="$LB_JUDGE_REASON"
    fi
    note "$id [$verdict] grpc, $channels channel(s): ready_endpoints=$ready rcnt=$LB_JUDGE_RCNT fail=$LB_JUDGE_FAIL take_errors+=$take_delta served: ${used:-none} ${reason:+reason=$reason}"
    printf '%s,grpc %s channels,grpc-dpumesh,%s,%s,%s,%s,%s,,,,,ready_endpoints=%s take_errors+=%s served: %s %s\n' \
        "$id" "$channels" "$expected" "$LB_JUDGE_OBSERVED" "$verdict" \
        "$LB_JUDGE_RCNT" "$LB_JUDGE_FAIL" "$ready" "$take_delta" "${used:-none}" \
        "${reason:+reason=$reason}" >>"$CSV"
}

wait_ready_replicas() {  # wait_ready_replicas <deployment> <count> <timeout>
    local deployment="$1" count="$2" timeout="$3"
    kubectl wait -n "$NS" \
        --for="jsonpath={.status.readyReplicas}=$count" \
        "deployment/$deployment" --timeout="$timeout" >>"$LOG" 2>&1
}

run_lb() {
    say "balancing — connection grain, endpoint changes, and the request grain"
    local result rep threads union

    say "L1 — how many of the three backends one client reaches"
    for threads in 1 2 4 6; do
        for rep in 1 2 3; do
            result=$(lb_point_sampled dpumesh "$threads" tcp echo-dpumesh)
            lb_record "L1" "opaque, $threads threads, rep $rep" dpumesh \
                "$result" 3 1 "$([ "$threads" -lt 3 ] && echo "$threads" || echo 3)"
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
        lb_record "L2" "two backends, rep $rep" dpumesh "$result" 2 1 2
        union="$union $(backend_slots "$result" | tr '\n' ' ')"
    done
    note "L2 backend slots observed over three repetitions (p2c characterization, not a fairness gate): $(tr ' ' '\n' <<<"$union" | sed -n '/^[0-9]\+$/p' | sort -u | tr '\n' ' ')"

    say "L3 — the backend restored"
    kubectl scale deployment/echo-dpumesh-14 -n "$NS" --replicas=1 >>"$LOG" 2>&1
    wait_ready_replicas echo-dpumesh-14 1 120s
    sleep 20
    union=""
    for rep in 1 2 3; do
        result=$(lb_point_sampled dpumesh 6 tcp echo-dpumesh)
        lb_record "L3" "three backends, rep $rep" dpumesh "$result" 3 1 3
        union="$union $(backend_slots "$result" | tr '\n' ' ')"
    done
    note "L3 backend slots observed over three repetitions (p2c characterization, not a fairness gate): $(tr ' ' '\n' <<<"$union" | sed -n '/^[0-9]\+$/p' | sort -u | tr '\n' ' ')"

    say "L4 — the request grain against two protocol-aware backends"
    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=2 >>"$LOG" 2>&1
    wait_ready_replicas echo-grpc-dpumesh 2 150s
    sleep 25
    # One client channel is one DMA session, and a session owns one backend
    # channel. Offering the same load over one channel and over four is what
    # separates "the balancer holds both endpoints" from "one request may go
    # anywhere".
    grpc_backends L4a 1 2
    grpc_backends L4b 4 2

    say "L5 — back to one backend"
    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=1 >>"$LOG" 2>&1
    wait_ready_replicas echo-grpc-dpumesh 1 90s
    sleep 20
    grpc_backends L5 1 1
}

# Linkerd features the DPU-hosted proxy has never been pointed at. Each stage
# is a claim the stack already makes; the arm either closes it or turns it into
# an open item. The two protocol-aware stages that need a failing backend get
# one: BENCH_FAIL_EVERY makes the echo server answer INTERNAL on a schedule,
# because a retry policy and a circuit breaker are both statements about what
# happens when a backend fails.
grpc_fail_every_on() {  # grpc_fail_every_on <deployment> <n>
    kubectl set env "deployment/$1" -n "$NS" "BENCH_FAIL_EVERY=$2" >>"$LOG" 2>&1
    kubectl rollout status "deployment/$1" -n "$NS" --timeout=180s >>"$LOG" 2>&1 || true
    sleep 20
}
grpc_fail_every() { grpc_fail_every_on echo-grpc-dpumesh "$1"; }

run_surfaces() {  # run_surfaces [grpc-only]
    local surface_scope="${1:-all}"
    say "surfaces — Linkerd features running in a proxy nobody has aimed at them"
    export ROUTE_PATH ROUTE_BACKEND ROUTE_TIMEOUT ROUTE_RETRY_LIMIT
    export ROUTE_METHOD ROUTE_HEADER_NAME ROUTE_HEADER_VALUE
    export GRPC_SERVICE GRPC_METHOD POLICY_IDENTITY
    ROUTE_PATH="$GRPC_METHOD_PREFIX"; ROUTE_BACKEND=echo-grpc-dpumesh
    GRPC_SERVICE="${GRPC_METHOD_PREFIX#/}"
    GRPC_METHOD="${GRPC_UNARY_METHOD:-UnaryCall}"

    drop_all_fixtures; sleep 6
    stage S0 "no route" grpc-dpumesh serve 1 0 "baseline for the surfaces scope"

    say "S1/S2 — HTTPRoute request timeouts"
    ROUTE_TIMEOUT=10s
    fixture_apply httproute-timeout.yaml; sleep 6
    stage S1 "timeout 10s" grpc-dpumesh serve 1 0 "a generous timeout changes nothing"
    ROUTE_TIMEOUT=1ms
    fixture_apply httproute-timeout.yaml; sleep 6
    # The client's latency includes the DMA round trip on both sides, which the
    # route timeout does not cover, so what the client completes says little
    # here. The route's own counter is the reading.
    local t0 t1
    t0=$(metric_sum "$(dpu_metrics)" 'route_request_statuses_total.*error="REQUEST_TIMEOUT"')
    stage S2 "timeout 1ms" grpc-dpumesh refuse 1 0 "shorter than the proxy's own request"
    t1=$(metric_sum "$(dpu_metrics)" 'route_request_statuses_total.*error="REQUEST_TIMEOUT"')
    counter_stage S2v "the route timed requests out" grpc-dpumesh "$((t1 - t0))" \
        "REQUEST_TIMEOUT+=$((t1 - t0))"
    fixture_delete httproute-timeout.yaml; sleep 6

    say "S3 — retries, against a backend that fails on purpose"
    # Every fiftieth call. Sparse enough that three retries clear it, and
    # frequent enough that a run without them fails: a rate a bounded retry
    # cannot absorb would prove nothing about the retry.
    #
    # Both arms are GRPCRoutes, differing only in the annotations. A gRPC
    # failure is an HTTP 200 carrying `grpc-status`, and an HTTPRoute's retry
    # conditions are HTTP status ranges, so the condition has nowhere to live
    # on one — the same annotations there reach the proxy as a retry policy
    # with no condition, which is attached to every request and can never fire.
    grpc_fail_every 50
    fixture_apply grpcroute.yaml; sleep 6
    stage S3a "1-in-50 failures, no retry policy" grpc-dpumesh refuse 1 0 \
        "the failures reach the client"
    ROUTE_RETRY_LIMIT=3
    fixture_apply grpcroute-retry.yaml; sleep 6
    # An absorbed failure and a failure that never happened are the same
    # request to the client, so the route's own counter is what says the retry
    # acted rather than the backend having a quiet run.
    local y0 y1
    y0=$(metric_sum "$(dpu_metrics)" '^outbound_grpc_route_retry_requests_total')
    stage S3b "1-in-50 failures, retry limit 3" grpc-dpumesh serve 1 0 \
        "the retry policy absorbs them"
    y1=$(metric_sum "$(dpu_metrics)" '^outbound_grpc_route_retry_requests_total')
    counter_stage S3v "the route retried the failures" grpc-dpumesh "$((y1 - y0))" \
        "retry_requests+=$((y1 - y0))"
    fixture_delete grpcroute-retry.yaml
    grpc_fail_every 0

    say "S4/S5 — method matching"
    ROUTE_METHOD=POST
    fixture_apply httproute-method.yaml; sleep 6
    stage S4 "method POST" grpc-dpumesh serve 1 0 "gRPC is POST"
    ROUTE_METHOD=GET
    fixture_apply httproute-method.yaml; sleep 6
    stage S5 "method GET" grpc-dpumesh refuse 1 0 "no rule matches"
    fixture_delete httproute-method.yaml; sleep 6

    say "S6/S7 — header matching"
    ROUTE_HEADER_NAME=content-type; ROUTE_HEADER_VALUE=application/grpc
    fixture_apply httproute-header.yaml; sleep 6
    stage S6 "header content-type: application/grpc" grpc-dpumesh serve 1 0 \
        "every gRPC request carries it"
    ROUTE_HEADER_VALUE=application/json
    fixture_apply httproute-header.yaml; sleep 6
    stage S7 "header content-type: application/json" grpc-dpumesh refuse 1 0 \
        "no rule matches"
    fixture_delete httproute-header.yaml; sleep 6

    say "S8/S9 — GRPCRoute"
    fixture_apply grpcroute.yaml; sleep 6
    stage S8 "GRPCRoute on the called method" grpc-dpumesh serve 1 0 \
        "$GRPC_SERVICE/$GRPC_METHOD"
    GRPC_METHOD=NoSuchCall
    fixture_apply grpcroute.yaml; sleep 6
    stage S9 "GRPCRoute on another method" grpc-dpumesh refuse 1 0 "no rule matches"
    fixture_delete grpcroute.yaml
    GRPC_METHOD="${GRPC_UNARY_METHOD:-UnaryCall}"; sleep 6

    say "S10-S12 — authorization whose subject is a route"
    POLICY_IDENTITY="$(caller_identity_of bench-grpc-dpumesh)"
    note "caller identity: $POLICY_IDENTITY"
    fixture_apply server-grpc.yaml; sleep 8
    stage S10 "Server on the protocol-aware Service, no authorization" \
        grpc-dpumesh refuse 1 0 "deny by default" 0
    fixture_apply authz-route.yaml; sleep 8
    stage S11 "AuthorizationPolicy targeting the GRPCRoute" grpc-dpumesh serve 1 0 \
        "the route authorizes the caller"
    POLICY_IDENTITY="not-the-caller.$NS.serviceaccount.identity.$TRUST_DOMAIN"
    fixture_apply authz-route.yaml; sleep 8
    stage S12 "the same policy naming another identity" grpc-dpumesh refuse 1 0 \
        "negative control for route authorization"
    fixture_delete authz-route.yaml; fixture_delete server-grpc.yaml; sleep 6

    say "S13/S14 — failure accrual"
    # A breaker acts inside one Service's balancer: it ejects an endpoint and
    # the traffic lands on the endpoint beside it. So the failing backend joins
    # the Service under test rather than sitting behind a route weight, where
    # ejecting it would leave its share of the requests with nowhere to go.
    kubectl annotate service echo-grpc-dpumesh -n "$NS" --overwrite \
        balancer.linkerd.io/failure-accrual=consecutive \
        balancer.linkerd.io/failure-accrual-consecutive-max-failures=3 \
        balancer.linkerd.io/failure-accrual-consecutive-min-penalty=10s >>"$LOG" 2>&1
    fixture_apply echo-grpc-broken.yaml
    kubectl rollout status deployment/echo-grpc-broken -n "$NS" --timeout=180s >>"$LOG" 2>&1 || true
    sleep 25

    local before after healthy broken
    before=$(grpc_service_requests echo-grpc-dpumesh)
    # The breaker cannot act without observing failures: its threshold is three
    # consecutive ones, and the ejection takes effect against what is already in
    # flight. The declared bound is that, well under a run of several thousand;
    # which endpoint stopped serving is S13v's reading, not this one's.
    stage S13 "a failing endpoint beside a healthy one" grpc-dpumesh serve 1 0 \
        "the breaker ejects it and the client keeps serving" "" 10
    after=$(grpc_service_requests echo-grpc-dpumesh)
    # An endpoint that served nothing has no sample, and under `pipefail` an
    # empty grep would end the campaign rather than the reading.
    healthy=$({ served_delta "$before" "$after" | tr ' ' '\n' |
                grep -E 'echo-grpc-dpumesh-[a-z0-9]+-[a-z0-9]+=' || true; } |
              sed 's/.*=//' | sort -rn | head -1)
    broken=$({ served_delta "$before" "$after" | tr ' ' '\n' |
               grep -E 'echo-grpc-broken-' || true; } | sed 's/.*=//' | head -1)
    : "${healthy:=0}" "${broken:=0}"
    note "S13v ejected: healthy=$healthy broken=$broken"
    printf 'S13v,the breaker ejected the failing endpoint,grpc-dpumesh,ejected,%s,%s,,,,,,,healthy=%s broken=%s\n' \
        "$([ "$healthy" -gt "$((broken * 4))" ] && echo ejected || echo held)" \
        "$([ "$healthy" -gt "$((broken * 4))" ] && echo PASS || echo FAIL)" \
        "$healthy" "$broken" >>"$CSV"
    if [ "$healthy" -gt "$((broken * 4))" ]; then PASSES=$((PASSES + 1)); else FAILURES=$((FAILURES + 1)); fi

    fixture_delete echo-grpc-broken.yaml
    kubectl wait --for=delete pod -n "$NS" -l run=echo-grpc-broken --timeout=90s >>"$LOG" 2>&1 || true
    sleep 20
    stage S14 "the failing endpoint withdrawn" grpc-dpumesh serve 1 0 \
        "the Service is healthy again"
    kubectl annotate service echo-grpc-dpumesh -n "$NS" \
        balancer.linkerd.io/failure-accrual- \
        balancer.linkerd.io/failure-accrual-consecutive-max-failures- \
        balancer.linkerd.io/failure-accrual-consecutive-min-penalty- >>"$LOG" 2>&1 || true

    [ "$surface_scope" = grpc-only ] && return 0

    say "S15 — HTTP/1.1 through the protocol-aware path"
    # The stack has always handled HTTP/1; nothing in this tree had ever sent
    # it any. A verdict is what separates "the bytes arrived" from "the
    # protocol-aware path carried them".
    stage S15 "HTTP/1.1 request-response" http1 serve 1 0 \
        "the only HTTP/1 workload in the tree"
}

# Requests each destination Pod served over one point, by Service. The metric
# is cumulative and carries labels for Pods that are long gone, so every
# reading here is a delta.
grpc_service_requests() {  # grpc_service_requests <service>
    # A Service that has served nothing yet has no samples at all, and under
    # `pipefail` an empty grep would end the campaign rather than the reading.
    { dpu_metrics | grep -E "^request_total.*direction=\"outbound\".*$1" || true; } |
        sed 's/.*dst_pod="\([^"]*\)".*} /\1 /' |
        awk '{ t[$1] += $2 } END { for (pod in t) if (t[pod] > 0) print pod, t[pod] }' | sort
}
served_delta() {  # served_delta <before> <after>
    join -a1 -a2 -e 0 -o 0,1.2,2.2 <(printf '%s\n' "$1") <(printf '%s\n' "$2") |
        awk '{ if ($3 - $2 > 0) printf "%s=%d ", $1, $3 - $2 }'
}

# Routing across Services. The order of the two halves is the security
# argument: a route may cross, and the destination's own policy is what grades
# the stream that arrives there.
run_cross() {
    say "cross-Service routing — a route whose backend is another Service"
    export ROUTE_PATH ROUTE_BACKEND ROUTE_WEIGHT_A ROUTE_WEIGHT_B
    ROUTE_PATH="$GRPC_METHOD_PREFIX"

    drop_all_fixtures; sleep 6
    stage X0 "no route" grpc-dpumesh serve 1 0 "baseline for the cross scope"

    say "X1 — the route reaches the other Service"
    ROUTE_BACKEND=echo-grpc-alt
    fixture_apply httproute-cross.yaml; sleep 8
    local before after served
    before=$(grpc_service_requests echo-grpc-alt)
    stage X1 "backendRef in another Service" grpc-dpumesh serve 1 0 \
        "target_mismatch must stay flat"
    after=$(grpc_service_requests echo-grpc-alt)
    served=$(served_delta "$before" "$after")
    note "X1 served by echo-grpc-alt: ${served:-none}"
    printf 'X1b,attribution,grpc-dpumesh,served,%s,%s,,,,,,,served: %s\n' \
        "$([ -n "$served" ] && echo served || echo none)" \
        "$([ -n "$served" ] && echo PASS || echo FAIL)" "${served:-none}" >>"$CSV"
    if [ -n "$served" ]; then PASSES=$((PASSES + 1)); else FAILURES=$((FAILURES + 1)); fi

    say "X2 — the destination's own policy grades the stream that arrives"
    fixture_apply server-alt.yaml; sleep 12
    # The caller's Service has no Server at all. If admission were still graded
    # by the Service the client addressed, this would be admitted.
    stage X2 "Server on the destination Service, no authorization" \
        grpc-dpumesh refuse 1 0 "graded by the callee"
    # Traffic that stops is not a policy result. What makes this one is the
    # enforcement point saying it refused, which the process-global admitted
    # counter cannot show here: the caller legitimately reaches its own
    # Service's backend, and that has no policy to refuse it.
    printf 'X2v,the destination refused,grpc-dpumesh,refused,%s,%s,,,%s,%s,,,denied+=%s admitted+=%s\n' \
        "$([ "$LAST_DENIED_DELTA" != NA ] && [ "$LAST_DENIED_DELTA" -gt 0 ] && echo refused || echo silent)" \
        "$([ "$LAST_DENIED_DELTA" != NA ] && [ "$LAST_DENIED_DELTA" -gt 0 ] && echo PASS || echo FAIL)" \
        "$LAST_ADMITTED_DELTA" "$LAST_DENIED_DELTA" \
        "$LAST_DENIED_DELTA" "$LAST_ADMITTED_DELTA" >>"$CSV"
    note "X2v [$([ "$LAST_DENIED_DELTA" != NA ] && [ "$LAST_DENIED_DELTA" -gt 0 ] && echo PASS || echo FAIL)] the destination refused: denied+=$LAST_DENIED_DELTA"
    if [ "$LAST_DENIED_DELTA" != NA ] && [ "$LAST_DENIED_DELTA" -gt 0 ]; then
        PASSES=$((PASSES + 1)); else FAILURES=$((FAILURES + 1)); fi
    fixture_delete server-alt.yaml; sleep 8
    stage X2b "the destination Server withdrawn" grpc-dpumesh serve 1 0 \
        "the route works again"

    say "X3 — weighted backendRefs across two Services"
    ROUTE_WEIGHT_A=50; ROUTE_WEIGHT_B=50
    fixture_apply httproute-weighted.yaml; sleep 8
    local before_a after_a before_b after_b served_a served_b
    before_a=$(grpc_service_requests echo-grpc-dpumesh)
    before_b=$(grpc_service_requests echo-grpc-alt)
    stage X3 "weighted 50/50 across two Services" grpc-dpumesh serve 4 0 \
        "both destinations must serve"
    after_a=$(grpc_service_requests echo-grpc-dpumesh)
    after_b=$(grpc_service_requests echo-grpc-alt)
    served_a=$(served_delta "$before_a" "$after_a")
    served_b=$(served_delta "$before_b" "$after_b")
    note "X3 split: echo-grpc-dpumesh{${served_a:-none}} echo-grpc-alt{${served_b:-none}}"
    printf 'X3b,split,grpc-dpumesh,both,%s,%s,,,,,,,A{%s} B{%s}\n' \
        "$([ -n "$served_a" ] && [ -n "$served_b" ] && echo both || echo one)" \
        "$([ -n "$served_a" ] && [ -n "$served_b" ] && echo PASS || echo FAIL)" \
        "${served_a:-none}" "${served_b:-none}" >>"$CSV"
    if [ -n "$served_a" ] && [ -n "$served_b" ]; then PASSES=$((PASSES + 1))
    else FAILURES=$((FAILURES + 1)); fi

    fixture_delete httproute-weighted.yaml; sleep 6
    stage X4 "no route" grpc-dpumesh serve 1 0 "returned to the default route"
}

# Per-request backend selection. The negative result this replaces is in the
# balancing scope: one client channel reached one Pod and four channels split
# unevenly, because a session owned one backend channel and spread came from
# how many channels the client happened to open.
run_fanout() {
    say "fan-out — one client channel across two backends of one Service"
    drop_all_fixtures; sleep 6

    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=2 >>"$LOG" 2>&1
    wait_ready_replicas echo-grpc-dpumesh 2 180s
    sleep 25

    local metrics take_before take_after before after served pods result channels
    local expected observed verdict
    metrics=$(dpu_metrics)
    take_before=$(metric_sum "$metrics" '^dmesh_backend_take_errors_total')

    for channels in 1 4; do
        before=$(grpc_service_requests echo-grpc-dpumesh)
        result=$(lb_point grpc-dpumesh "$channels")
        if ! grep -q 'rcnt=' <<<"$result"; then
            no_reading "$result"
            note "F1 $channels channel(s): $NO_READING_REASON — no reading"
            printf 'F1-%sch,one Service two backends,grpc-dpumesh,2,%s,FAIL,,,,,,,%s\n' \
                "$channels" "$NO_READING_OBSERVED" "$NO_READING_REASON" >>"$CSV"
            FAILURES=$((FAILURES + 1))
            continue
        fi
        after=$(grpc_service_requests echo-grpc-dpumesh)
        served=$(served_delta "$before" "$after")
        pods=$(printf '%s\n' "$served" | tr ' ' '\n' | grep -c '=' || true)
        metrics=$(dpu_metrics)
        take_after=$(metric_sum "$metrics" '^dmesh_backend_take_errors_total')
        note "F1 $channels channel(s): backends=$pods fail=$(field "$result" fail) take_errors+=$((take_after - take_before)) served: ${served:-none}"
        # One channel reaching both backends is the whole item: spread must
        # come from the route, not from how many channels a client opens.
        expected=2
        [ "$pods" -ge 2 ] && observed=2 || observed="$pods"
        if [ "$observed" = "$expected" ] && [ "$(field "$result" fail)" = 0 ] &&
           [ "$((take_after - take_before))" -eq 0 ]; then
            verdict=PASS; PASSES=$((PASSES + 1))
        else
            verdict=FAIL; FAILURES=$((FAILURES + 1))
        fi
        printf 'F1-%sch,one Service two backends,grpc-dpumesh,%s,%s,%s,%s,%s,,,,%s,served: %s\n' \
            "$channels" "$expected" "$observed" "$verdict" \
            "$(field "$result" rcnt)" "$(field "$result" fail)" \
            "$((take_after - take_before))" "${served:-none}" >>"$CSV"
        take_before=$take_after
    done

    say "F1 — back to one backend"
    kubectl scale deployment/echo-grpc-dpumesh -n "$NS" --replicas=1 >>"$LOG" 2>&1
    wait_ready_replicas echo-grpc-dpumesh 1 120s
    sleep 20
    stage F1r "one backend restored" grpc-dpumesh serve 1 0 "the Service is healthy again"
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
    surfaces) run_surfaces ;;
    grpc-surfaces) run_surfaces grpc-only ;;
    cross)  run_cross ;;
    fanout) run_fanout ;;
    all)    run_policy; run_route; run_cross; run_fanout; run_surfaces; run_lb ;;
    *) echo "usage: $0 [all|policy|route|cross|fanout|surfaces|grpc-surfaces|lb]" >&2; exit 1 ;;
esac

if ! csv_error=$(policy_route_csv_valid "$CSV" 2>&1); then
    note "CSV [FAIL]: $csv_error"
    FAILURES=$((FAILURES + 1))
fi
say "stages: $PASSES pass, $FAILURES fail"
say "csv: $CSV"
[ "$FAILURES" -eq 0 ]
