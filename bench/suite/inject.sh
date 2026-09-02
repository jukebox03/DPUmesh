#!/usr/bin/env bash
# Does one annotation mesh a workload, and does its absence leave one alone?
#
# Two readings judge every stage, the same way policy_route.sh judges its own:
# what the admitted Pod object actually carries, and what the traffic and the
# DPU's own counters say afterwards. A patch that reads correctly and moves no
# bytes is not an injection result, and bytes that move without the patch are
# not one either.
#
#   I1  the annotated server carries every piece of the patch
#   I2  the unannotated server carries none of it
#   I3  the meshed pair moves data, and the DPU records an inbound verdict
#   I4  the unmeshed pair still moves data, and the DPU records nothing
#   I5  a webhook that cannot answer refuses the Pod, and admits again once
#       it can
#   I6  while the DPU is down, a meshed Pod's connect refuses — warm or born
#       into the outage — the unmeshed road stays open, and a recycled pair
#       serves again once a fresh DPU runs
#   I7  the node refuses a kernel-TCP SYN from an unannotated Pod to a
#       mesh-served port, while the same probe to an unmeshed port connects
#
# I4 is the half that makes the feature safe to turn on: a Pod nobody
# annotated carries no shim and keeps working over kernel TCP. I5 is the half
# that makes the annotation mean something: while the webhook is down the
# namespace creates no Pod at all, because one born unpatched would keep
# working over kernel TCP with no identity, no policy, and no sign anything
# is missing. I6 is the half that makes fail-closed mean the DPU rather than
# the happy path: the mesh is a meshed Pod's only road, so the DPU's death
# must close that road, not open a kernel-TCP detour around it. I7 is the
# other direction of the same claim: the annotation protects the workload's
# own port, so a Pod that never joined the mesh cannot walk around it over
# plain kernel TCP.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
K8S_DIR="$SUITE_DIR/../k8s"
source "$SUITE_DIR/deployed_geometry.sh"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
LIB_OUT="${LIB_OUT:-$PROJ_ROOT/build/lib}"
IMG_PRELOAD_SOCK="${IMG_PRELOAD_SOCK:-bench/preload-sock:latest}"
IMG_CONTROLLER="${IMG_CONTROLLER:-bench/dpumesh-controller:latest}"
BENCH_NUMA_NODE="${BENCH_NUMA_NODE:-0}"
HOST_PCI="${HOST_PCI:-}"
DEPLOYED_GEOMETRY=$(resolve_deployed_geometry "$PROJ_ROOT")
read -r DPUMESH_RINGS_PER_POD DPUMESH_ARM_WORKERS <<<"$DEPLOYED_GEOMETRY"
DPUMESH_ATTEST_SOCKET="${DPUMESH_ATTEST_SOCKET:-/run/dpumesh/attest.sock}"
export NS CTRL_PORT LIB_OUT IMG_PRELOAD_SOCK IMG_CONTROLLER BENCH_NUMA_NODE
export HOST_PCI DPUMESH_RINGS_PER_POD DPUMESH_ARM_WORKERS DPUMESH_ATTEST_SOCKET

DUR="${DUR:-10}"
WARM="${WARM:-100}"
REQ="${REQ:-1024}"
OUT_DIR="${OUT_DIR:-$SUITE_DIR/../report/data/inject-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/stages.csv"
LOG="$OUT_DIR/raw.log"
[ -s "$CSV" ] || echo "stage,subject,expected,observed,verdict,detail" >"$CSV"

SSH_OPTS=(-o ServerAliveInterval=15 -o ServerAliveCountMax=4
          -o ConnectTimeout=10 -o BatchMode=yes)

say()  { printf '\n=== %s\n' "$*" | tee -a "$LOG"; }
note() { printf '    %s\n' "$*" | tee -a "$LOG"; }
field() { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }

PASSES=0; FAILURES=0
record() {
    local id="$1" subject="$2" expected="$3" observed="$4" detail="${5:-}" verdict
    if [ "$expected" = "$observed" ]; then verdict=PASS; PASSES=$((PASSES + 1))
    else verdict=FAIL; FAILURES=$((FAILURES + 1)); fi
    printf '%s,%s,%s,%s,%s,%s\n' "$id" "$subject" "$expected" "$observed" "$verdict" "$detail" >>"$CSV"
    note "$id [$verdict] $subject: expected=$expected observed=$observed ${detail}"
}

### ------------------------------------------------------------ DPU readings
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
# A counter this DPU has never incremented reads as absent, and so does a read
# that failed. Substituting zero for the second turns the next delta negative,
# which is a counter appearing to run backwards; an unreadable endpoint answers
# NA instead.
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

# A reading that is neither NA nor a count is also NA: a stage is better told
# that its counter is unreadable than handed an arithmetic error.
ctl_delta() {  # ctl_delta <before> <after>
    case "$1$2" in ''|*NA*|*[!0-9]*) echo NA; return 0;; esac
    echo $(( $2 - $1 ))
}

### ------------------------------------------------------------ fixtures
render() { envsubst <"$K8S_DIR/$1"; }
apply()  { render "$1" | kubectl apply -n "$NS" -f - >>"$LOG" 2>&1; }
scale()  { kubectl scale deployment/"$1" -n "$NS" --replicas="$2" >>"$LOG" 2>&1 || true; }

DPU_DOWN=0
cleanup() {
    say "cleanup"
    # A campaign that dies between I6's kill and its restore would otherwise
    # leave the rig without a DPU; every later campaign starts by reading its
    # banner.
    if [ "$DPU_DOWN" = 1 ]; then
        note "the DPU this campaign killed is still down; relaunching"
        dpu_start >>"$LOG" 2>&1 || true
    fi
    local d
    for d in inject-echo inject-bench plain-echo plain-bench; do scale "$d" 0; done
    kubectl delete pod inject-probe -n "$NS" --ignore-not-found --wait=false >>"$LOG" 2>&1 || true
    # The registration is fail-closed: left behind with no webhook running, it
    # would refuse every Pod this namespace ever creates again. It leaves with
    # the campaign that deployed it, and so does the webhook it points at.
    kubectl delete mutatingwebhookconfiguration dpumesh-inject >>"$LOG" 2>&1 || true
    kubectl delete deployment dpumesh-webhook -n "$NS" >>"$LOG" 2>&1 || true
    note "injected workloads scaled to zero; webhook and its registration removed"
}
trap cleanup EXIT

# Ready, not Running: a container that exits and restarts keeps its Pod in
# phase Running, so waiting on the phase hands the traffic stage a server that
# never started and its refusal reads as a mesh verdict.
wait_ready() {  # wait_ready <app> <seconds>
    local app="$1" deadline=$(( SECONDS + ${2:-120} ))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ "$(kubectl get pod -n "$NS" -l "app=$app" \
              -o jsonpath='{range .items[*]}{range .status.conditions[?(@.type=="Ready")]}{.status}{end}{end}' \
              2>/dev/null | grep -c True)" -ge 1 ] && return 0
        sleep 2
    done
    note "$app never became Ready:"
    kubectl get pod -n "$NS" -l "app=$app" 2>&1 | tee -a "$LOG"
    kubectl logs -n "$NS" -l "app=$app" --tail=20 2>&1 | tee -a "$LOG"
    return 1
}
# A Deployment mid-replacement has more than one Pod, and a terminating one
# still answers a list. Read the object that is actually serving.
pod_json() {
    kubectl get pod -n "$NS" -l "app=$1" -o json 2>/dev/null |
        python3 -c '
import json, sys
items = json.load(sys.stdin)["items"]
live = [p for p in items
        if not (p.get("metadata") or {}).get("deletionTimestamp")
        and (p.get("status") or {}).get("phase") == "Running"]
print(json.dumps((live or items)[0]))'
}
running_pod_ip() {
    kubectl get pod -n "$NS" -l "app=$1" \
        -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' \
        2>/dev/null | head -n1
}
run_traffic() {  # run_traffic <app> <threads>
    local ip to
    ip=$(running_pod_ip "$1"); [ -n "$ip" ] || { echo "ERR no_pod($1)"; return 0; }
    to=$(( DUR + 90 ))
    printf 'RUN %s %s %s %s %s %s\n' "$REQ" 8 1 "$DUR" "$WARM" "${2:-1}" |
        timeout "${to}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null || echo "ERR nc"
}
classify() {  # classify <result> — serve | refuse | nodata
    local fail rcnt
    fail=$(field "$1" fail); rcnt=$(field "$1" rcnt)
    : "${fail:=1}" "${rcnt:=0}"
    if ! grep -q 'rcnt=' <<<"$1"; then echo nodata
    elif [ "$fail" -eq 0 ] && [ "$rcnt" -gt 0 ]; then echo serve
    else echo refuse; fi
}
restart_count() {  # container restarts of the app's first Pod, NA when unreadable
    local n
    n=$(kubectl get pod -n "$NS" -l "app=$1" \
        -o jsonpath='{.items[0].status.containerStatuses[0].restartCount}' 2>/dev/null)
    echo "${n:-NA}"
}
# The shim logs every refused connect to the Pod's stderr; the count tells a
# refusal apart from a client that never reached its connect at all.
refusal_lines() {
    kubectl logs -n "$NS" -l "app=$1" --tail=300 2>/dev/null |
        grep -c 'dmesh_preload.*refus' || true
}
# A bare TCP connect from inside a Pod: the kernel road itself, no shim, no
# payload. Prints connect | refuse | timeout | nodata. An exec that cannot
# run at all also reads refuse, which is why every use pairs with a control
# probe that must connect.
kernel_probe() {  # kernel_probe <src-app> <dst-ip> <dst-port>
    local src status=0
    src=$(kubectl get pod -n "$NS" -l "app=$1" \
              -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
    { [ -n "$src" ] && [ -n "$2" ]; } || { echo nodata; return 0; }
    kubectl exec -n "$NS" "$src" -- \
        timeout 3 bash -c "exec 3<>/dev/tcp/$2/$3" >/dev/null 2>&1 || status=$?
    case "$status" in
        0)   echo connect;;
        124) echo timeout;;
        *)   echo refuse;;
    esac
}

### ------------------------------------------------------------ the DPU process
# The kill matches full command lines, exactly as bench.sh stop_dpu does, so a
# renamed main thread cannot ride out the outage. The relaunch reuses the
# launcher the deploy wrote to /tmp — the same file that started the process
# being replaced — so the fresh DPU differs from the dead one in nothing but
# its PID.
dpu_pid() {
    ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" "pgrep -f '[d]pumesh_dpu' | head -1" \
        2>/dev/null || true
}
dpu_kill() {
    ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
        "echo '$DPU_PASS' | sudo -S bash -c \"pids=\\\$(pgrep -f '[d]pumesh_dpu'); [ -z \\\"\\\$pids\\\" ] || kill -9 \\\$pids\"" \
        >>"$LOG" 2>&1 || true
}
dpu_start() {  # prints the fresh PID, or NO_PID
    ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
        "echo '$DPU_PASS' | sudo -S bash /tmp/start_dpu_bench.sh" 2>>"$LOG" |
        sed 's/^\[sudo\][^:]*: *//' | tail -n1
}
wait_dpu_banner() {  # the launcher truncates the log, so the banner is fresh
    local deadline=$(( SECONDS + 360 ))
    while [ "$SECONDS" -lt "$deadline" ]; do
        (cd "$PROJ_ROOT" && timeout 60 bench/bench.sh dpulog 400 2>/dev/null) |
            grep -q 'DPU PROXY MODE ON' && return 0
        sleep 3
    done
    return 1
}

# What the patch is supposed to have put on a Pod, read back off the object the
# API server admitted. Prints `injected` or `bare`, and the first missing piece.
inspect_pod() {  # inspect_pod <app> <expected-ports> <expected-service>
    pod_json "$1" | INJECT_PORTS="$2" INJECT_SERVICE="$3" python3 -c '
import json, os, sys
pod = json.load(sys.stdin)
metadata = pod.get("metadata") or {}
spec = pod.get("spec") or {}
labels = metadata.get("labels") or {}
annotations = metadata.get("annotations") or {}
container = (spec.get("containers") or [{}])[0]
environment = {e.get("name"): e.get("value") for e in container.get("env") or []}
mounts = {m.get("mountPath") for m in container.get("volumeMounts") or []}
volumes = {v.get("name") for v in spec.get("volumes") or []}
affinity = json.dumps(spec.get("affinity") or {})

checks = {
    "control-plane-ns": labels.get("linkerd.io/control-plane-ns") == "linkerd",
    "skip-inbound-ports":
        annotations.get("config.linkerd.io/skip-inbound-ports") == os.environ["INJECT_PORTS"],
    "privileged": (container.get("securityContext") or {}).get("privileged") is True,
    "pci": bool(environment.get("DPUMESH_PCI_ADDR")),
    "infiniband": "/dev/infiniband" in mounts,
    "library": "/usr/local/lib/libdpumesh.so.5" in mounts,
    "attest": "/run/dpumesh" in mounts,
    "preload-lib": "/usr/local/lib/libdmesh_preload.so" in mounts,
    "preload-var": environment.get("NUMA_TARGET_LD_PRELOAD")
                   == "/usr/local/lib/libdmesh_preload.so",
    "volumes": {"dpumesh-infiniband", "dpumesh-library", "dpumesh-attest"} <= volumes,
    "node-affinity": "dpumesh.io/dpu" in affinity,
}
service = os.environ["INJECT_SERVICE"]
if service:
    checks["identity"] = environment.get("DPUMESH_SERVICE") == service

present = [name for name, ok in checks.items() if ok]
missing = [name for name, ok in checks.items() if not ok]
if not missing:
    print("injected")
elif not present:
    print("bare")
else:
    print("partial:" + ",".join(missing))
'
}

### ------------------------------------------------------------ campaign
say "webhook — deploy the registration and its serving certificate"
apply webhook.yaml
kubectl rollout status deployment/dpumesh-webhook -n "$NS" --timeout=120s >>"$LOG" 2>&1 || {
    echo "the webhook did not become ready" >&2; exit 1; }
CA=$(kubectl get mutatingwebhookconfiguration dpumesh-inject \
        -o jsonpath='{.webhooks[0].clientConfig.caBundle}' 2>/dev/null || true)
record I0 "caBundle published" present "$([ -n "$CA" ] && echo present || echo absent)"

say "workloads — the same Deployment, with and without the annotation"
apply injected.yaml
for d in inject-echo plain-echo inject-bench plain-bench; do scale "$d" 1; done
for a in inject-echo plain-echo inject-bench plain-bench; do
    wait_ready "$a" 180 || { echo "$a never became Ready" >&2; exit 1; }
done

# Running is not serving: a meshed Pod still has to register with the DPU, and
# its Service endpoint still has to propagate, before the first connection can
# be routed.
sleep "${SETTLE:-25}"

discover_admin_ports
note "DPU admin ports: ${ADMIN_PORTS[*]}"

say "I1/I2 — what the admitted Pod objects carry"
record I1 inject-echo injected "$(inspect_pod inject-echo 9101 inject-sock)"
record I2 plain-echo  bare     "$(inspect_pod plain-echo  9102 '')"

say "I3 — the meshed pair"
A0=$(ctl_event inbound admitted)
RESULT=$(run_traffic inject-bench 1)
printf 'I3 | %s\n' "$RESULT" >>"$LOG"
A1=$(ctl_event inbound admitted)
FAIL=$(field "$RESULT" fail); RCNT=$(field "$RESULT" rcnt)
: "${FAIL:=1}" "${RCNT:=0}"
if ! grep -q 'rcnt=' <<<"$RESULT"; then OBS=nodata
elif [ "$FAIL" -eq 0 ] && [ "$RCNT" -gt 0 ]; then OBS=serve; else OBS=refuse; fi
record I3 "meshed traffic" serve "$OBS" "rcnt=$RCNT fail=$FAIL admitted+=$(ctl_delta "$A0" "$A1")"
# A meshed stream is one the DPU decided on. Traffic without a verdict came
# from a Pod the patch did not actually mesh.
record I3b "meshed verdict" yes "$(d=$(ctl_delta "$A0" "$A1"); [ "$d" != NA ] && [ "$d" -gt 0 ] && echo yes || echo no)"

say "I4 — the unmeshed control"
B0=$(ctl_event inbound admitted)
RESULT=$(run_traffic plain-bench 1)
printf 'I4 | %s\n' "$RESULT" >>"$LOG"
B1=$(ctl_event inbound admitted)
FAIL=$(field "$RESULT" fail); RCNT=$(field "$RESULT" rcnt)
: "${FAIL:=1}" "${RCNT:=0}"
if ! grep -q 'rcnt=' <<<"$RESULT"; then OBS=nodata
elif [ "$FAIL" -eq 0 ] && [ "$RCNT" -gt 0 ]; then OBS=serve; else OBS=refuse; fi
record I4 "unmeshed traffic" serve "$OBS" "rcnt=$RCNT fail=$FAIL admitted+=$(ctl_delta "$B0" "$B1")"
record I4b "unmeshed verdict" no "$(d=$(ctl_delta "$B0" "$B1"); [ "$d" != NA ] && [ "$d" -gt 0 ] && echo yes || echo no)"

say "I5 — fail-closed: while the webhook cannot answer, the namespace creates no Pod"
probe() {  # probe — one unannotated Pod; prints admit or refuse, logs the server's words
    local out
    if out=$(kubectl run inject-probe -n "$NS" --image="docker.io/${IMG_PRELOAD_SOCK}" \
                 --image-pull-policy=Never --restart=Never \
                 --command -- sleep 300 2>&1); then
        printf 'probe | %s\n' "$out" >>"$LOG"; echo admit
    else
        printf 'probe | %s\n' "$out" >>"$LOG"
        # A refusal that does not name this webhook is some other failure
        # wearing the expected verdict.
        grep -q 'inject.dpumesh.io' <<<"$out" && echo refuse || echo refuse-other
    fi
}
scale dpumesh-webhook 0
kubectl wait pod -n "$NS" -l app=dpumesh-webhook --for=delete --timeout=60s >>"$LOG" 2>&1 || true
record I5a "creation while webhook is down" refuse "$(probe)"
kubectl delete pod inject-probe -n "$NS" --ignore-not-found --wait=true >>"$LOG" 2>&1 || true

scale dpumesh-webhook 1
kubectl rollout status deployment/dpumesh-webhook -n "$NS" --timeout=120s >>"$LOG" 2>&1 || true
record I5b "creation once it answers" admit "$(probe)"
kubectl delete pod inject-probe -n "$NS" --ignore-not-found --wait=false >>"$LOG" 2>&1 || true

say "I6 — fail-closed: while the DPU is down, a meshed Pod's connect refuses"
# The kill is the scenario, not an accident: the DPU dies under Pods that hold
# live channels into it. Two shapes of meshed Pod must both refuse — one whose
# channel was up when the DPU died, and one born into the outage with no
# channel to build — while the unmeshed pair keeps its ordinary kernel road,
# which is what separates enforcement from a broken node.
RESULT=$(run_traffic inject-bench 1)
printf 'I6a | %s\n' "$RESULT" >>"$LOG"
record I6a "meshed serve before the kill" serve "$(classify "$RESULT")" \
       "rcnt=$(field "$RESULT" rcnt) fail=$(field "$RESULT" fail)"

RESTARTS0=$(restart_count inject-bench)
DPU_DOWN=1
dpu_kill
sleep 3
record I6b "dpumesh_dpu after the kill" absent \
       "$([ -z "$(dpu_pid)" ] && echo absent || echo present)"

if [ -z "$(dpu_pid)" ]; then
    RESULT=$(run_traffic inject-bench 1)
    printf 'I6c | %s\n' "$RESULT" >>"$LOG"
    record I6c "warm meshed connect while the DPU is down" refuse "$(classify "$RESULT")" \
           "rcnt=$(field "$RESULT" rcnt) fail=$(field "$RESULT" fail) refusals=$(refusal_lines inject-bench) restarts=$RESTARTS0:$(restart_count inject-bench)"

    scale inject-bench 0
    kubectl wait pod -n "$NS" -l app=inject-bench --for=delete --timeout=90s >>"$LOG" 2>&1 || true
    scale inject-bench 1
    if wait_ready inject-bench 90; then
        RESULT=$(run_traffic inject-bench 1)
        printf 'I6d | %s\n' "$RESULT" >>"$LOG"
        record I6d "meshed Pod born in the outage" refuse "$(classify "$RESULT")" \
               "rcnt=$(field "$RESULT" rcnt) fail=$(field "$RESULT" fail) refusals=$(refusal_lines inject-bench)"
    else
        record I6d "meshed Pod born in the outage" refuse not-ready
    fi

    RESULT=$(run_traffic plain-bench 1)
    printf 'I6e | %s\n' "$RESULT" >>"$LOG"
    record I6e "unmeshed pair while the DPU is down" serve "$(classify "$RESULT")" \
           "rcnt=$(field "$RESULT" rcnt) fail=$(field "$RESULT" fail)"
fi

say "I6 — restore: a fresh DPU, then the meshed pair recycled against it"
# Order matters twice here. The Pods leave first: each one holds channel state
# into a process that no longer exists, and the relaunch admits only what
# registers with it. The serve check then proves the refusals above were
# enforcement, not a rig this stage had already broken.
scale inject-bench 0; scale inject-echo 0
for a in inject-bench inject-echo; do
    kubectl wait pod -n "$NS" -l "app=$a" --for=delete --timeout=120s >>"$LOG" 2>&1 || true
done
PID=$(dpu_start)
case "$PID" in
    ''|NO_PID)
        # Without a fresh process the old log's old banner is still there, so
        # waiting on it would vouch for a DPU that is not running.
        note "dpumesh_dpu did not relaunch";;
    *)
        DPU_DOWN=0; note "dpumesh_dpu relaunched (PID: $PID)"
        wait_dpu_banner || note "no startup banner before the deadline";;
esac
scale inject-echo 1; scale inject-bench 1
for a in inject-echo inject-bench; do
    wait_ready "$a" 180 || note "$a not Ready after the restore"
done
sleep "${SETTLE:-25}"
ADMIN_PORTS=()
discover_admin_ports
D0=$(ctl_event inbound admitted)
RESULT=$(run_traffic inject-bench 1)
printf 'I6f | %s\n' "$RESULT" >>"$LOG"
D1=$(ctl_event inbound admitted)
record I6f "meshed serve after restore" serve "$(classify "$RESULT")" \
       "rcnt=$(field "$RESULT" rcnt) fail=$(field "$RESULT" fail) admitted+=$(ctl_delta "$D0" "$D1")"

say "I7 — the kernel road: an unannotated Pod cannot reach a mesh-served port"
# inject-echo serves 9101 over DMA, and the node agent's ingress guard rejects
# kernel-TCP SYNs to that (address, port) pair on its membership cadence.
# plain-echo's 9102 is an ordinary kernel listener, so the same probe from the
# same Pod is the method control: only the mesh-served pair may refuse. The
# probe runs after the I6 restore, so a refusal here also shows the guard
# re-covered a recycled Pod at its fresh address; the wait below is bounded by
# that cadence, and how long it took is part of the record. That the guard
# never touches the DMA road is what the whole campaign's meshed traffic
# stages show.
PLAIN_IP=$(running_pod_ip plain-echo)
MESH_IP=$(running_pod_ip inject-echo)
record I7a "kernel probe to the unmeshed port" connect \
       "$(kernel_probe plain-bench "$PLAIN_IP" 9102)"
GUARD_T0=$SECONDS
OBS=$(kernel_probe plain-bench "$MESH_IP" 9101)
while [ "$OBS" != refuse ] && [ $(( SECONDS - GUARD_T0 )) -lt 45 ]; do
    sleep 5
    OBS=$(kernel_probe plain-bench "$MESH_IP" 9101)
done
record I7b "kernel probe to the mesh-served port" refuse "$OBS" \
       "settled=$(( SECONDS - GUARD_T0 ))s"

say "summary"
note "passes=$PASSES failures=$FAILURES"
note "-> $CSV"
[ "$FAILURES" -eq 0 ]
