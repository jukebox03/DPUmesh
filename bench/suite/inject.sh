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
#
# I4 is the half that makes the feature safe to turn on: the shim falls back to
# kernel TCP, so a Pod nobody annotated is a working Pod.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SUITE_DIR/../.." && pwd)"
K8S_DIR="$SUITE_DIR/../k8s"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
LIB_OUT="${LIB_OUT:-$PROJ_ROOT/build/lib}"
IMG_PRELOAD_SOCK="${IMG_PRELOAD_SOCK:-bench/preload-sock:latest}"
IMG_CONTROLLER="${IMG_CONTROLLER:-bench/dpumesh-controller:latest}"
BENCH_NUMA_NODE="${BENCH_NUMA_NODE:-0}"
HOST_PCI="${HOST_PCI:-}"
DPUMESH_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-2}"
DPUMESH_ATTEST_SOCKET="${DPUMESH_ATTEST_SOCKET:-/run/dpumesh/attest.sock}"
export NS CTRL_PORT LIB_OUT IMG_PRELOAD_SOCK IMG_CONTROLLER BENCH_NUMA_NODE
export HOST_PCI DPUMESH_RINGS_PER_POD DPUMESH_ATTEST_SOCKET

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
    for ((port = base; port < base + 8; port++)); do
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

cleanup() {
    say "cleanup"
    local d
    for d in inject-echo inject-bench plain-echo plain-bench; do scale "$d" 0; done
    note "injected workloads scaled to zero; the webhook registration is left in place"
}
trap cleanup EXIT

wait_running() {  # wait_running <app> <seconds>
    local app="$1" deadline=$(( SECONDS + ${2:-120} ))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ "$(kubectl get pod -n "$NS" -l "app=$app" \
              -o jsonpath='{.items[?(@.status.phase=="Running")].metadata.name}' 2>/dev/null \
              | wc -w)" -ge 1 ] && return 0
        sleep 2
    done
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
    wait_running "$a" 180 || { echo "$a never reached Running" >&2; exit 1; }
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

say "summary"
note "passes=$PASSES failures=$FAILURES"
note "-> $CSV"
[ "$FAILURES" -eq 0 ]
