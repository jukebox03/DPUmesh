#!/usr/bin/env bash
# Real Kubernetes/systemd/Comch lifecycle tests for the per-Pod broker.
#
# Fault scenarios are opt-in (`--execute`) and resolve their exact target from
# Kubernetes Pod UID -> root-private broker state -> PID/starttime -> cgroup.
# A stale state file or an ambiguous label stops the test before any signal is
# sent. Every scenario retains before/after worker snapshots and a machine-
# readable result.env under bench/report/data.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
BENCH="$PROJ_ROOT/bench/bench.sh"
WORKER_PROGRESS="$SUITE_DIR/worker_progress.sh"

if [ -f "$PROJ_ROOT/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    source "$PROJ_ROOT/.env"
    set +a
fi

NS="${NS:-test-bench}"
TARGET_APP="${BROKER_TARGET_APP:-echo-grpc-dpumesh}"
CLIENT_APP="${BROKER_CLIENT_APP:-bench-grpc-dpumesh}"
COLLATERAL_APP="${BROKER_COLLATERAL_APP:-echo-grpc-alt}"
STATE_ROOT="${BROKER_STATE_ROOT:-/run/dpumesh/brokers}"
RECEIPT_ROOT="${BROKER_RECEIPT_ROOT:-$PROJ_ROOT/bench/report/data}"
RECOVERY_TIMEOUT="${BROKER_RECOVERY_TIMEOUT:-120}"
RECOVERY_STABILITY="${BROKER_RECOVERY_STABILITY:-5}"
LOAD_DURATION="${BROKER_LOAD_DURATION:-60}"
CTRL_PORT="${CTRL_PORT:-9092}"
CRI_ENDPOINT="${BROKER_CRI_ENDPOINT:-unix:///run/containerd/containerd.sock}"
IMG_BROKER_PROBE="${BROKER_PROBE_IMAGE:-bench/dpumesh-broker-probe:latest}"
EXECUTE=0
SCENARIO=""
RECEIPT=""
LOAD_PID=""
RECOVERY_SECONDS=""

log() {
    if [ -n "$RECEIPT" ] && [ -d "$RECEIPT" ]; then
        printf '[%s] %s\n' "$(date -Is)" "$*" | tee -a "$RECEIPT/scenario.log" >&2
    else
        printf '[%s] %s\n' "$(date -Is)" "$*" >&2
    fi
}
die() {
    [ "${QUIET_ERRORS:-0}" -eq 1 ] || log "FAIL: $*"
    return 1
}

sudo_host() {
    : "${HOST_PASS:?.env missing HOST_PASS}"
    printf '%s\n' "$HOST_PASS" | sudo -S -p '' "$@"
}

require_tools() {
    local tool
    for tool in kubectl jq timeout nc flock; do
        command -v "$tool" >/dev/null || die "required command not found: $tool"
    done
    [ -x "$WORKER_PROGRESS" ] || die "missing executable $WORKER_PROGRESS"
}

running_pod_json() {
    local app="$1" json count
    json=$(kubectl get pods -n "$NS" -l "app=$app" -o json)
    count=$(jq '[.items[] | select(.metadata.deletionTimestamp == null and .status.phase == "Running")] | length' <<<"$json")
    [ "$count" -eq 1 ] || die "app=$app must resolve to exactly one running Pod (got $count)"
    jq -c '.items[] | select(.metadata.deletionTimestamp == null and .status.phase == "Running")' <<<"$json"
}

pod_name() { running_pod_json "$1" | jq -r .metadata.name; }
pod_uid() { running_pod_json "$1" | jq -r .metadata.uid; }

restart_count() {
    running_pod_json "$1" | jq '[.status.containerStatuses[]?.restartCount] | add // 0'
}

agent_broker_contract_present() {
    kubectl get daemonset dpumesh-node-agent -n "$NS" -o json |
        jq -e 'any(.spec.template.spec.containers[] | select(.name == "workload-agent").args[]?; startswith("--broker-bin="))' >/dev/null
}

state_path() {
    local uid="$1"
    [[ "$uid" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]] ||
        die "refusing non-canonical Pod UID '$uid'"
    printf '%s/%s.state\n' "$STATE_ROOT" "$uid"
}

broker_state() {
    local uid="$1" path
    path=$(state_path "$uid") || return 1
    sudo_host test -f "$path" || die "broker state missing: $path"
    sudo_host cat "$path"
}

process_starttime() {
    local pid="$1" stat rest
    stat=$(sudo_host cat "/proc/$pid/stat" 2>/dev/null) || return 1
    rest="${stat#*) }"
    awk '{print $20}' <<<"$rest"
}

broker_tuple_for_uid() {
    local uid="$1" state pid recorded actual state_uid exe cgroup token mode owner
    state=$(broker_state "$uid") || return 1
    pid=$(jq -er '.pid | select(type == "number" and . > 1)' <<<"$state") ||
        die "broker state has invalid pid for Pod $uid"
    recorded=$(jq -er '.starttime | strings' <<<"$state") ||
        die "broker state has invalid starttime for Pod $uid"
    state_uid=$(jq -er '.pod_uid | strings' <<<"$state") || return 1
    [ "$state_uid" = "$uid" ] || die "state Pod UID mismatch: expected=$uid actual=$state_uid"
    actual=$(process_starttime "$pid") || die "broker pid $pid is not alive"
    [ "$actual" = "$recorded" ] ||
        die "broker PID reuse/stale state: pid=$pid recorded=$recorded actual=$actual"
    exe=$(sudo_host readlink "/proc/$pid/exe") || die "cannot resolve broker executable"
    [ "${exe##*/}" = dmesh_broker ] || die "state pid $pid is not dmesh_broker ($exe)"
    cgroup=$(sudo_host cat "/proc/$pid/cgroup") || die "cannot read broker cgroup"
    token="${uid//-/_}"
    grep -Fq "pod${token}" <<<"$cgroup" ||
        die "broker pid=$pid cgroup does not contain Pod UID $uid"
    grep -Fq '/dpumesh-broker' <<<"$cgroup" ||
        die "broker pid=$pid is not in the per-Pod broker child cgroup"
    mode=$(sudo_host stat -c '%a' "$(state_path "$uid")")
    owner=$(sudo_host stat -c '%u:%g' "$(state_path "$uid")")
    [ "$mode" = 600 ] && [ "$owner" = 0:0 ] ||
        die "broker state is not root-private (owner=$owner mode=$mode)"
    printf '%s|%s|%s\n' "$pid" "$recorded" "$uid"
}

broker_tuple() {
    local uid
    uid=$(pod_uid "$1") || return 1
    broker_tuple_for_uid "$uid"
}

broker_pid() { broker_tuple "$1" | cut -d'|' -f1; }

process_dead() {
    local pid="$1" old_start="$2" current
    current=$(process_starttime "$pid" 2>/dev/null) || return 0
    [ "$current" != "$old_start" ]
}

wait_for_new_broker() {
    local app="$1" uid="$2" old_pid="$3" old_start="$4" old_restarts="$5"
    local start now tuple pid begun restarts stable_since=0 candidate=""
    start=$(date +%s)
    while true; do
        restarts=$(QUIET_ERRORS=1 restart_count "$app" 2>/dev/null || true)
        tuple=$(QUIET_ERRORS=1 broker_tuple_for_uid "$uid" 2>/dev/null || true)
        pid=${tuple%%|*}
        begun=$(cut -d'|' -f2 <<<"$tuple")
        if [[ "$restarts" =~ ^[0-9]+$ ]] && [ "$restarts" -gt "$old_restarts" ] &&
           [ -n "$pid" ] && { [ "$pid" != "$old_pid" ] || [ "$begun" != "$old_start" ]; } &&
           process_dead "$old_pid" "$old_start"; then
            if [ "$candidate" != "$tuple" ]; then
                candidate="$tuple"
                stable_since=$(date +%s)
            fi
            now=$(date +%s)
            if [ $((now - stable_since)) -ge "$RECOVERY_STABILITY" ]; then
                RECOVERY_SECONDS=$((now - start))
                {
                    printf 'status=STABLE\n'
                    printf 'app=%s\n' "$app"
                    printf 'pod_uid=%s\n' "$uid"
                    printf 'old_pid=%s\n' "$old_pid"
                    printf 'old_starttime=%s\n' "$old_start"
                    printf 'new_pid=%s\n' "$pid"
                    printf 'new_starttime=%s\n' "$begun"
                    printf 'restart_count_before=%s\n' "$old_restarts"
                    printf 'restart_count_after=%s\n' "$restarts"
                    printf 'recovery_seconds=%s\n' "$RECOVERY_SECONDS"
                    printf 'stability_seconds=%s\n' "$RECOVERY_STABILITY"
                    printf 'observed_at=%s\n' "$(date -Is)"
                } >"$RECEIPT/recovery.env"
                log "recovered app=$app restarts=$old_restarts->$restarts broker=$old_pid/$old_start->$pid/$begun stable=${RECOVERY_STABILITY}s"
                return 0
            fi
        else
            candidate=""
            stable_since=0
        fi
        now=$(date +%s)
        [ $((now - start)) -lt "$RECOVERY_TIMEOUT" ] ||
            die "timed out waiting for app=$app restart and fresh broker"
        sleep 1
    done
}

pod_ip() { running_pod_json "$1" | jq -r .status.podIP; }

point_smoke() {
    local result
    result=$("$BENCH" point grpc-dpumesh 1024 8 4 10 1000 4)
    printf '%s\n' "$result" | tee -a "$RECEIPT/point.log"
    [[ "$result" == OK* ]] || die "post-fault point did not return OK: $result"
    local field
    for field in fail pending drops reorder worker_fail credit_hold_dropped eq_budget_exhausted; do
        local value
        value=$(sed -n "s/.*[[:space:]]$field=\([^[:space:]]*\).*/\1/p" <<<"$result")
        [ -n "$value" ] && [ "$value" = 0 ] || die "post-fault point $field=${value:-missing}"
    done
}

start_load() {
    local ip
    ip=$(pod_ip "$CLIENT_APP")
    printf 'OPEN 48 48 8 %s 1000 8\n' "$LOAD_DURATION" |
        timeout "$((LOAD_DURATION + 90))s" nc -N "$ip" "$CTRL_PORT" \
        >"$RECEIPT/background-load.txt" 2>&1 &
    LOAD_PID=$!
    sleep 3
    kill -0 "$LOAD_PID" 2>/dev/null || die "background load ended before fault injection"
    log "background load pid=$LOAD_PID duration=${LOAD_DURATION}s"
}

finish_load() {
    [ -n "$LOAD_PID" ] || return 0
    wait "$LOAD_PID" 2>/dev/null || true
    LOAD_PID=""
}

stop_load() {
    [ -n "$LOAD_PID" ] || return 0
    kill "$LOAD_PID" 2>/dev/null || true
    wait "$LOAD_PID" 2>/dev/null || true
    LOAD_PID=""
}

snapshot() {
    local label="$1"
    WORKER_RECEIPT_ROOT="$RECEIPT" "$WORKER_PROGRESS" snapshot "$label" >/dev/null
}

scenario_begin() {
    local scenario="$1" stamp
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    RECEIPT="$RECEIPT_ROOT/broker-lifecycle-${stamp}-${scenario}"
    mkdir -p "$RECEIPT_ROOT"
    [ ! -e "$RECEIPT" ] || die "receipt already exists: $RECEIPT"
    mkdir "$RECEIPT"
    exec 9>"${BROKER_LIFECYCLE_LOCK:-/tmp/dpumesh-broker-lifecycle.lock}"
    flock -n 9 || die "another broker lifecycle scenario is running"
    {
        printf 'scenario=%s\n' "$scenario"
        printf 'started_at=%s\n' "$(date -Is)"
        printf 'namespace=%s\n' "$NS"
        printf 'target_app=%s\n' "$TARGET_APP"
        printf 'client_app=%s\n' "$CLIENT_APP"
        printf 'collateral_app=%s\n' "$COLLATERAL_APP"
        printf 'git_commit=%s\n' "$(git -C "$PROJ_ROOT" rev-parse HEAD)"
    } >"$RECEIPT/scenario.env"
    trap 'stop_load' EXIT INT TERM
    log "scenario=$scenario receipt=$RECEIPT"
}

scenario_pass() {
    {
        printf 'status=PASS\n'
        if [ -n "$RECOVERY_SECONDS" ]; then
            printf 'recovery_seconds=%s\n' "$RECOVERY_SECONDS"
        fi
        printf 'finished_at=%s\n' "$(date -Is)"
    } >"$RECEIPT/result.env"
    log "PASS: $1"
    trap - EXIT INT TERM
    printf '%s\n' "$RECEIPT"
}

scenario_skip() {
    {
        printf 'status=BLOCKED\n'
        printf 'reason=%s\n' "$1"
        printf 'finished_at=%s\n' "$(date -Is)"
    } >"$RECEIPT/result.env"
    log "BLOCKED: $1"
    trap - EXIT INT TERM
    printf '%s\n' "$RECEIPT"
    return 77
}

strict_preflight() {
    require_tools
    agent_broker_contract_present || die "deployed node-agent is missing its required --broker-bin argument"
    local app
    for app in "$TARGET_APP" "$CLIENT_APP" "$COLLATERAL_APP"; do
        running_pod_json "$app" >/dev/null
        broker_tuple "$app" >/dev/null
    done
    local agent_ready
    agent_ready=$(kubectl get daemonset dpumesh-node-agent -n "$NS" -o json |
        jq -r '.status.numberReady // 0')
    [ "$agent_ready" -ge 1 ] || die "node-agent DaemonSet has no ready Pod"
    "$WORKER_PROGRESS" wait-quiescent 10 >/dev/null ||
        die "strict worker metrics/quiescence unavailable; deploy the instrumented DPU build"
}

preflight_command() {
    require_tools
    printf 'namespace=%s\n' "$NS"
    printf 'broker_contract=%s\n' "$(agent_broker_contract_present && echo present || echo missing)"
    local app tuple
    for app in "$TARGET_APP" "$CLIENT_APP" "$COLLATERAL_APP"; do
        printf 'app=%s pod=%s uid=%s restarts=%s' "$app" "$(pod_name "$app")" \
            "$(pod_uid "$app")" "$(restart_count "$app")"
        if tuple=$(broker_tuple "$app" 2>/dev/null); then
            printf ' broker=%s\n' "$tuple"
        else
            printf ' broker=UNAVAILABLE\n'
        fi
    done
    if "$WORKER_PROGRESS" wait-quiescent 3 >/dev/null 2>&1; then
        printf 'strict_quiescence=PASS\n'
    else
        printf 'strict_quiescence=UNAVAILABLE_OR_DIRTY\n'
    fi
}

require_execute() {
    [ "$EXECUTE" -eq 1 ] || die "fault scenario requires --execute"
}

run_b1() {
    require_execute
    scenario_begin b1-broker-sigkill
    strict_preflight
    snapshot before
    local uid tuple old_pid old_start before_restart collateral_restart collateral_tuple
    uid=$(pod_uid "$TARGET_APP")
    tuple=$(broker_tuple_for_uid "$uid")
    IFS='|' read -r old_pid old_start _ <<<"$tuple"
    before_restart=$(restart_count "$TARGET_APP")
    collateral_restart=$(restart_count "$COLLATERAL_APP")
    collateral_tuple=$(broker_tuple "$COLLATERAL_APP")
    start_load
    log "SIGKILL broker pid=$old_pid starttime=$old_start pod_uid=$uid"
    sudo_host kill -KILL "$old_pid"
    wait_for_new_broker "$TARGET_APP" "$uid" "$old_pid" "$old_start" "$before_restart"
    finish_load
    [ "$(restart_count "$COLLATERAL_APP")" = "$collateral_restart" ] ||
        die "collateral app restarted"
    [ "$(broker_tuple "$COLLATERAL_APP")" = "$collateral_tuple" ] ||
        die "collateral broker changed"
    "$WORKER_PROGRESS" wait-quiescent "$RECOVERY_TIMEOUT" >"$RECEIPT/quiescence.csv" ||
        die "DPU/session state did not quiesce after B1"
    point_smoke
    snapshot after
    scenario_pass "broker SIGKILL was contained and recovered"
}

container_pid() {
    local app="$1" pod cid pid uid cgroup token
    pod=$(pod_name "$app")
    uid=$(pod_uid "$app")
    cid=$(kubectl get pod -n "$NS" "$pod" -o json |
        jq -er --arg name "$app" '.status.containerStatuses[] | select(.name == $name) | .containerID')
    cid="${cid#*://}"
    [[ "$cid" =~ ^[0-9a-f]{12,}$ ]] || die "invalid container id for $app"
    pid=$(sudo_host crictl --runtime-endpoint "$CRI_ENDPOINT" inspect "$cid" |
        jq -er '.info.pid | select(. > 1)')
    cgroup=$(sudo_host cat "/proc/$pid/cgroup")
    token="${uid//-/_}"
    grep -Fq "pod${token}" <<<"$cgroup" || die "container pid=$pid escaped target Pod cgroup"
    printf '%s\n' "$pid"
}

run_b2() {
    require_execute
    scenario_begin b2-application-sigkill
    strict_preflight
    snapshot before
    local uid tuple old_pid old_start before_restart app_pid collateral_restart collateral_tuple
    uid=$(pod_uid "$TARGET_APP")
    tuple=$(broker_tuple_for_uid "$uid")
    IFS='|' read -r old_pid old_start _ <<<"$tuple"
    before_restart=$(restart_count "$TARGET_APP")
    collateral_restart=$(restart_count "$COLLATERAL_APP")
    collateral_tuple=$(broker_tuple "$COLLATERAL_APP")
    app_pid=$(container_pid "$TARGET_APP")
    start_load
    log "SIGKILL application pid=$app_pid pod_uid=$uid"
    sudo_host kill -KILL "$app_pid"
    wait_for_new_broker "$TARGET_APP" "$uid" "$old_pid" "$old_start" "$before_restart"
    finish_load
    [ "$(restart_count "$COLLATERAL_APP")" = "$collateral_restart" ] || die "collateral app restarted"
    [ "$(broker_tuple "$COLLATERAL_APP")" = "$collateral_tuple" ] || die "collateral broker changed"
    "$WORKER_PROGRESS" wait-quiescent "$RECOVERY_TIMEOUT" >"$RECEIPT/quiescence.csv" ||
        die "DPU/session state did not quiesce after B2"
    point_smoke
    snapshot after
    scenario_pass "application SIGKILL removed the old broker and registered a fresh one"
}

all_broker_tuples() {
    local app
    for app in "$TARGET_APP" "$CLIENT_APP" "$COLLATERAL_APP"; do
        printf '%s|%s\n' "$app" "$(broker_tuple "$app")"
    done
}

run_probe_job() {
    command -v envsubst >/dev/null || die "envsubst is required for the fresh-registration probe"
    command -v docker >/dev/null || die "docker is required for the fresh-registration probe"
    [ -x "$PROJ_ROOT/build/bin/dmesh_broker_probe" ] ||
        die "build/bin/dmesh_broker_probe is missing; run make bench"

    docker build -q -f "$PROJ_ROOT/bench/docker/broker_probe.Dockerfile" \
        -t "$IMG_BROKER_PROBE" "$PROJ_ROOT" >/dev/null ||
        die "failed to build the dedicated broker probe image"
    local archive node_name
    archive=$(mktemp /tmp/dpumesh-broker-probe.XXXXXX.tar)
    if ! docker save -o "$archive" "$IMG_BROKER_PROBE"; then
        rm -f "$archive"
        die "failed to export the dedicated broker probe image"
    fi
    sudo_host ctr -n k8s.io images rm "docker.io/$IMG_BROKER_PROBE" \
        >/dev/null 2>&1 || true
    if ! sudo_host ctr -n k8s.io images import "$archive" >/dev/null; then
        rm -f "$archive"
        die "failed to import the dedicated broker probe image"
    fi
    rm -f "$archive"

    node_name="${DPUMESH_NODE_NAME:-$(hostname -s)}"
    kubectl delete job dmesh-broker-probe -n "$NS" --ignore-not-found --wait=true >/dev/null
    NS="$NS" NODE_NAME="$node_name" IMG_BROKER_PROBE="$IMG_BROKER_PROBE" \
        envsubst <"$PROJ_ROOT/bench/k8s/broker-probe.yaml" | kubectl apply -f - >/dev/null
    kubectl wait -n "$NS" --for=condition=complete job/dmesh-broker-probe --timeout=120s >/dev/null
    kubectl logs -n "$NS" job/dmesh-broker-probe | tee "$RECEIPT/fresh-registration.log"
    grep -Fq 'teardown=ok' "$RECEIPT/fresh-registration.log" ||
        die "fresh broker registration probe did not finish cleanly"
}

run_b3() {
    require_execute
    scenario_begin b3-agent-re-adoption
    strict_preflight
    snapshot before
    local before after fake_uid fake_state fake_tmp agent_pod load_result
    before=$(all_broker_tuples)
    printf '%s\n' "$before" >"$RECEIPT/brokers-before.txt"
    fake_uid=00000000-0000-0000-0000-000000000000
    fake_state=$(state_path "$fake_uid")
    sudo_host test ! -e "$fake_state" || die "reserved forged state path already exists: $fake_state"
    fake_tmp=$(mktemp /tmp/dpumesh-forged-state.XXXXXX)
    printf '{"pid":1,"starttime":"0","pod_uid":"%s","service":"","cgroup":"/invalid"}\n' \
        "$fake_uid" >"$fake_tmp"
    sudo_host install -o root -g root -m 0600 "$fake_tmp" "$fake_state"
    rm -f "$fake_tmp"
    start_load
    kubectl rollout restart daemonset/dpumesh-node-agent -n "$NS" >/dev/null
    kubectl rollout status daemonset/dpumesh-node-agent -n "$NS" --timeout=120s >/dev/null
    sleep 2
    finish_load
    after=$(all_broker_tuples)
    printf '%s\n' "$after" >"$RECEIPT/brokers-after.txt"
    [ "$after" = "$before" ] || die "one or more broker PID/starttime tuples changed across agent rollout"
    agent_pod=$(pod_name dpumesh-node-agent)
    kubectl logs -n "$NS" "$agent_pod" >"$RECEIPT/agent-after.log"
    grep -Fq 're-adopted broker' "$RECEIPT/agent-after.log" || die "new agent emitted no broker re-adoption evidence"
    sudo_host test ! -e "$fake_state" || {
        sudo_host rm -f "$fake_state"
        die "forged state record survived re-adoption"
    }
    grep -Fq 'discarded broker state 00000000-0000-0000-0000-000000000000.state' \
        "$RECEIPT/agent-after.log" || die "forged state rejection was not logged"
    run_probe_job
    point_smoke
    snapshot after
    scenario_pass "node-agent rollout retained and re-adopted brokers, rejected forged state"
}

run_b4() {
    require_execute
    scenario_begin b4-dpu-restart
    strict_preflight
    snapshot before
    local apps=("$TARGET_APP" "$CLIENT_APP" "$COLLATERAL_APP") app
    local before_restarts="" old_dpu
    for app in "${apps[@]}"; do
        before_restarts+="$app=$(restart_count "$app") "
    done
    printf '%s\n' "$before_restarts" >"$RECEIPT/restarts-before.txt"
    start_load
    : "${DPU_HOST:?.env missing DPU_HOST}" "${DPU_PASS:?.env missing DPU_PASS}"
    old_dpu=$(ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S -p '' pgrep -x dpumesh_dpu | head -1")
    [[ "$old_dpu" =~ ^[0-9]+$ ]] || die "could not identify the DPU process"
    log "SIGKILL dpumesh_dpu pid=$old_dpu"
    ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S -p '' kill -KILL '$old_dpu'"
    finish_load

    # `bench.sh restart` is intentionally valid only when no Pod is meshed.
    # Scale exactly the three gRPC fixtures down, restart the DPU, then restore
    # their original one-replica shape.
    for app in "${apps[@]}"; do kubectl scale deployment/"$app" -n "$NS" --replicas=0 >/dev/null; done
    for app in "${apps[@]}"; do kubectl rollout status deployment/"$app" -n "$NS" --timeout=120s >/dev/null || true; done
    BENCH_DEPLOY_SCOPE=grpc "$BENCH" restart >"$RECEIPT/dpu-restart.log" 2>&1
    for app in "${apps[@]}"; do kubectl scale deployment/"$app" -n "$NS" --replicas=1 >/dev/null; done
    for app in "${apps[@]}"; do kubectl rollout status deployment/"$app" -n "$NS" --timeout=180s >/dev/null; done
    for app in "${apps[@]}"; do broker_tuple "$app" >/dev/null; done
    "$WORKER_PROGRESS" wait-quiescent "$RECOVERY_TIMEOUT" >"$RECEIPT/quiescence.csv" ||
        die "DPU/session state did not quiesce after B4"
    point_smoke
    snapshot after
    scenario_pass "DPU restart produced fresh registrations and clean traffic"
}

run_b5() {
    scenario_begin b5-isolation
    strict_preflight
    snapshot before
    local tuple pid uid status nspid uid_line cap seccomp broker_net host_net root_entries
    uid=$(pod_uid "$TARGET_APP")
    tuple=$(broker_tuple_for_uid "$uid")
    pid=${tuple%%|*}
    status=$(sudo_host cat "/proc/$pid/status")
    uid_line=$(awk '$1=="Uid:" {print $2":"$3":"$4":"$5}' <<<"$status")
    cap=$(awk '$1=="CapEff:" {print $2}' <<<"$status")
    seccomp=$(awk '$1=="Seccomp:" {print $2}' <<<"$status")
    nspid=$(awk '$1=="NSpid:" {print $NF}' <<<"$status")
    [ "$uid_line" = 65532:65532:65532:65532 ] || die "broker UIDs are not fully dropped: $uid_line"
    [ "$cap" = 0000000000000000 ] || die "broker effective capabilities are nonzero: $cap"
    [ "$seccomp" = 2 ] || die "broker seccomp filter is not active: $seccomp"
    [ "$nspid" = 1 ] || die "broker is not PID 1 in its namespace: NSpid=$nspid"
    broker_net=$(sudo_host readlink "/proc/$pid/ns/net")
    host_net=$(sudo_host readlink /proc/1/ns/net)
    [ "$broker_net" != "$host_net" ] || die "broker shares the host network namespace"
    # /proc/PID/root is a magic symlink. GNU find does not descend through a
    # command-line symlink under its default -P policy, which produces a false
    # empty-root result; -L is required to inspect the broker's actual root.
    root_entries=$(sudo_host find -L "/proc/$pid/root" -mindepth 1 -maxdepth 1 \
        -printf '%f\n' | sort)
    [ "$root_entries" = proc ] || die "broker private root exposes more than proc: $root_entries"
    local pod
    pod=$(pod_name "$TARGET_APP")
    kubectl exec -n "$NS" "$pod" -- sh -c 'test ! -e /dev/infiniband' ||
        die "workload container can see /dev/infiniband"
    {
        printf 'uid=%s\ncap_eff=%s\nseccomp=%s\nnspid=%s\nbroker_net=%s\nhost_net=%s\nroot_entries=%s\n' \
            "$uid_line" "$cap" "$seccomp" "$nspid" "$broker_net" "$host_net" "$root_entries"
    } >"$RECEIPT/isolation.env"
    snapshot after
    if [ -z "${B5_PRESSURE_COMMAND:-}" ]; then
        scenario_skip "namespace/capability/device isolation passed; cgroup CPU/OOM pressure arm requires B5_PRESSURE_COMMAND"
        return
    fi
    bash -lc "$B5_PRESSURE_COMMAND" >"$RECEIPT/pressure.log" 2>&1 ||
        die "configured B5 pressure arm failed"
    scenario_pass "broker isolation and configured cgroup pressure arm passed"
}

run_b6() {
    scenario_begin b6-unauthenticated-timeout
    strict_preflight
    snapshot before
    if [ -z "${B6_RAW_PEER_COMMAND:-}" ]; then
        scenario_skip "set B6_RAW_PEER_COMMAND to the raw Comch client that receives a challenge without asserting"
        return
    fi
    local before after
    before=$(ssh "$DPU_HOST" "curl -g -sf --max-time 3 http://127.0.0.1:4191/metrics" |
        awk '$1=="dmesh_control_events_total" && /kind="registration-timeout"/ {s+=$2} END{print s+0}')
    timeout 50s bash -lc "$B6_RAW_PEER_COMMAND" >"$RECEIPT/raw-peer.log" 2>&1 ||
        die "raw peer command did not observe the bounded disconnect"
    after=$(ssh "$DPU_HOST" "curl -g -sf --max-time 3 http://127.0.0.1:4191/metrics" |
        awk '$1=="dmesh_control_events_total" && /kind="registration-timeout"/ {s+=$2} END{print s+0}')
    [ "$after" -gt "$before" ] || die "registration-timeout counter did not advance ($before->$after)"
    run_probe_job
    snapshot after
    scenario_pass "unauthenticated Comch peer disconnected and slot was reused"
}

usage() {
    cat <<'EOF'
usage: broker_lifecycle.sh <command> [--execute]

  preflight       read-only deployment, Pod, broker and worker-metric checks
  b1              SIGKILL one target broker and require bounded recovery
  b2              SIGKILL one target application process and require broker cleanup
  b3              roll the node-agent, preserve brokers, reject forged state, register a new Job
  b4              restart the DPU with a bounded scale-down/restore cycle
  b5              verify namespaces/uid/caps/seccomp/device isolation; pressure arm is configurable
  b6              run a configured raw unauthenticated Comch peer and require timeout/slot reuse

b1..b4 require --execute. Defaults target echo-grpc-dpumesh, drive traffic from
bench-grpc-dpumesh, and use echo-grpc-alt as the collateral Pod.

Configuration: BROKER_TARGET_APP BROKER_CLIENT_APP BROKER_COLLATERAL_APP
               BROKER_RECOVERY_TIMEOUT BROKER_RECOVERY_STABILITY
               BROKER_LOAD_DURATION BROKER_RECEIPT_ROOT BROKER_CRI_ENDPOINT
               BROKER_PROBE_IMAGE
               B5_PRESSURE_COMMAND B6_RAW_PEER_COMMAND
EOF
}

require_tools
SCENARIO="${1:-}"
shift || true
while [ "$#" -gt 0 ]; do
    case "$1" in
        --execute) EXECUTE=1 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

case "$SCENARIO" in
    preflight) preflight_command ;;
    b1) run_b1 ;;
    b2) run_b2 ;;
    b3) run_b3 ;;
    b4) run_b4 ;;
    b5) run_b5 ;;
    b6) run_b6 ;;
    -h|--help|help|'') usage ;;
    *) usage >&2; exit 2 ;;
esac
