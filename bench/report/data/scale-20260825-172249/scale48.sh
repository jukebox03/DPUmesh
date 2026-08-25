#!/usr/bin/env bash
# MAX_PODS=127 validation: 48 webhook-injected replicas of one preload service
# on a K=2 deployment (ring capacity 128, table cap 127), driven by a meshed
# client. Under the fail-closed shim a replica only turns Ready if its listen()
# registered with the DPU, so "48/48 Ready" is itself the registration proof;
# the DPU log must additionally show no "table full" and traffic must serve
# with fail=0. Scale-down runs in steps of <=8 because >=13 simultaneous
# terminations can wedge the shared SG-DMA egress (known open issue).
set -uo pipefail

PROJ_ROOT=/home/jukebox/DPUmesh
cd "$PROJ_ROOT"
[ -f .env ] && { set -a; source .env; set +a; }

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
LIB_OUT="$PROJ_ROOT/build/lib"
IMG_PRELOAD_SOCK="${IMG_PRELOAD_SOCK:-bench/preload-sock:latest}"
IMG_CONTROLLER="${IMG_CONTROLLER:-bench/dpumesh-controller:latest}"
BENCH_NUMA_NODE="${BENCH_NUMA_NODE:-0}"
DPUMESH_RINGS_PER_POD=2
DPUMESH_ATTEST_SOCKET="${DPUMESH_ATTEST_SOCKET:-/run/dpumesh/attest.sock}"
export NS CTRL_PORT LIB_OUT IMG_PRELOAD_SOCK IMG_CONTROLLER BENCH_NUMA_NODE
export HOST_PCI DPUMESH_RINGS_PER_POD DPUMESH_ATTEST_SOCKET

REPLICAS=48
DUR=10
WARM=100
REQ=1024
THREADS=16
ROUNDS=4

OUT_DIR="$PROJ_ROOT/bench/report/data/scale-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/stages.csv"
LOG="$OUT_DIR/raw.log"
echo "stage,subject,expected,observed,verdict,detail" >"$CSV"
cp "$0" "$OUT_DIR/scale48.sh"

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

dpulog() { bench/bench.sh dpulog "${1:-4000}" 2>/dev/null; }

render() { envsubst <"bench/k8s/$1"; }
apply()  { render "$1" | kubectl apply -n "$NS" -f - >>"$LOG" 2>&1; }
scale()  { kubectl scale deployment/inject-echo -n "$NS" --replicas="$1" >>"$LOG" 2>&1; }

ready_count() {
    kubectl get pod -n "$NS" -l "app=inject-echo" \
      -o jsonpath='{range .items[*]}{range .status.conditions[?(@.type=="Ready")]}{.status}{"\n"}{end}{end}' \
      2>/dev/null | grep -c True
}
restart_sum() {
    kubectl get pod -n "$NS" -l "app=inject-echo" \
      -o jsonpath='{range .items[*]}{range .status.containerStatuses[*]}{.restartCount}{"\n"}{end}{end}' \
      2>/dev/null | awk '{s+=$1} END {print s+0}'
}
wait_ready_n() {  # wait_ready_n <count> <seconds>
    local want="$1" deadline=$(( SECONDS + $2 )) n=0
    while [ "$SECONDS" -lt "$deadline" ]; do
        n=$(ready_count)
        [ "$n" -ge "$want" ] && { echo "$n"; return 0; }
        sleep 5
    done
    echo "$n"; return 1
}
running_pod_ip() {
    kubectl get pod -n "$NS" -l "app=$1" \
        -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' \
        2>/dev/null | head -n1
}
run_traffic() {  # run_traffic <threads>
    local ip to
    ip=$(running_pod_ip inject-bench); [ -n "$ip" ] || { echo "ERR no_pod"; return 0; }
    to=$(( DUR + 90 ))
    printf 'RUN %s %s %s %s %s %s\n' "$REQ" 8 1 "$DUR" "$WARM" "$1" |
        timeout "${to}s" nc -N "$ip" "$CTRL_PORT" 2>/dev/null || echo "ERR nc"
}

ADMIN_PORTS=()
discover_admin_ports() {
    local base="${LINKERD_ADMIN_ADDR:-127.0.0.1:4191}" port
    base="${base##*:}"
    for ((port = base; port < base + 8; port++)); do
        ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
            "curl -sf --max-time 3 127.0.0.1:$port/metrics >/dev/null" 2>/dev/null &&
            ADMIN_PORTS+=("$port")
    done
    [ "${#ADMIN_PORTS[@]}" -gt 0 ]
}
ctl_event() {
    local raw value
    raw=$(ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" \
        "curl -sf --max-time 5 127.0.0.1:${ADMIN_PORTS[0]}/metrics" 2>/dev/null) || {
        echo NA; return 0; }
    [ -n "$raw" ] || { echo NA; return 0; }
    value=$(sed -n "s/^dmesh_control_events_total{kind=\"$1\",reason=\"$2\"} //p" <<<"$raw" |
        head -n1 | tr -d '[:space:]')
    printf '%s\n' "${value:-0}"
}
ctl_delta() {
    case "$1$2" in ''|*NA*|*[!0-9]*) echo NA; return 0;; esac
    echo $(( $2 - $1 ))
}

cleanup() {
    say "cleanup"
    kubectl scale deployment/inject-echo deployment/inject-bench -n "$NS" --replicas=0 >>"$LOG" 2>&1 || true
    # failurePolicy: Fail — a registration left behind with no webhook running
    # would refuse every Pod this namespace ever creates again.
    kubectl delete mutatingwebhookconfiguration dpumesh-inject >>"$LOG" 2>&1 || true
    kubectl delete deployment dpumesh-webhook -n "$NS" >>"$LOG" 2>&1 || true
    note "workloads scaled to zero; webhook and its registration removed"
}
trap cleanup EXIT

### S0 — the deployed geometry is the one under test
say "S0 — DPU banner"
BANNER=""
for lines in 4000 40000 200000; do
    BANNER=$(dpulog "$lines" | grep 'DPU PROXY MODE ON' | tail -1)
    [ -n "$BANNER" ] && break
done
note "banner: $BANNER"
K=$(sed -n 's/.*N\/K\/A=[0-9]*\/\([0-9]*\)\/[0-9]*.*/\1/p' <<<"$BANNER")
NKA=$(grep -o 'N/K/A=[0-9/]*' <<<"$BANNER" | head -1)
record S0 "deployed K" 2 "${K:-unknown}" "$NKA"

### S1 — webhook up
say "S1 — webhook"
apply webhook.yaml
if kubectl rollout status deployment/dpumesh-webhook -n "$NS" --timeout=120s >>"$LOG" 2>&1; then
    record S1 "webhook serving" ready ready
else
    record S1 "webhook serving" ready notready
    exit 1
fi

### S2 — 48 injected replicas all turn Ready (fail-closed: Ready => registered)
say "S2 — scale inject-echo to $REPLICAS"
apply injected.yaml
kubectl scale deployment/inject-bench -n "$NS" --replicas=1 >>"$LOG" 2>&1
scale "$REPLICAS"
T0=$SECONDS
N=$(wait_ready_n "$REPLICAS" 900) || true
ELAPSED=$(( SECONDS - T0 ))
RESTARTS=$(restart_sum)
record S2 "replicas Ready" "$REPLICAS" "$N" "elapsed=${ELAPSED}s restarts=$RESTARTS"
kubectl get pod -n "$NS" -l app=inject-echo -o wide >>"$LOG" 2>&1
[ "$N" -ge "$REPLICAS" ] || { note "not all replicas came up; aborting traffic"; exit 1; }
wait_ready_bench=$(kubectl wait pod -n "$NS" -l app=inject-bench --for=condition=Ready --timeout=120s 2>&1) || true
note "$wait_ready_bench"

### S3 — the table admitted them all
say "S3 — table admission"
TF=$(dpulog 200000 | grep -c 'table full')
record S3 "table-full refusals" 0 "$TF"

### S4 — traffic serves over the whole backend set, fail=0, with mesh verdicts
say "S4 — traffic ($ROUNDS rounds x $THREADS conns, round-robin over $REPLICAS backends)"
discover_admin_ports && note "DPU admin ports: ${ADMIN_PORTS[*]}" || note "no admin port"
A0=$(ctl_event inbound admitted)
TOTAL_RCNT=0; BAD=0
sleep 20   # registration settle, endpoint propagation
for r in $(seq 1 "$ROUNDS"); do
    RESULT=$(run_traffic "$THREADS")
    printf 'S4 round %s | %s\n' "$r" "$RESULT" >>"$LOG"
    FAIL=$(field "$RESULT" fail); RCNT=$(field "$RESULT" rcnt)
    : "${FAIL:=1}" "${RCNT:=0}"
    note "round $r: rcnt=$RCNT fail=$FAIL"
    if ! grep -q 'rcnt=' <<<"$RESULT" || [ "$FAIL" != 0 ] || [ "$RCNT" -le 0 ]; then BAD=$((BAD+1)); fi
    TOTAL_RCNT=$(( TOTAL_RCNT + RCNT ))
done
A1=$(ctl_event inbound admitted)
record S4 "traffic rounds clean" "0" "$BAD" "rcnt_total=$TOTAL_RCNT admitted+=$(ctl_delta "$A0" "$A1")"

### S5 — nothing broke while 49 pods were live
say "S5 — data-path health at full scale"
ERRS=$(dpulog 200000 | grep -cE 'px_dma_err|batch failed|table full')
record S5 "DPU error lines" 0 "$ERRS"

### S6 — gradual drain (steps of <=8; >=13 simultaneous terminations can wedge egress)
say "S6 — drain in steps"
for n in 40 32 24 16 8 0; do
    scale "$n"
    sleep 20
    note "scaled to $n (ready=$(ready_count))"
done
sleep 10
ERRS=$(dpulog 200000 | grep -cE 'px_dma_err|batch failed')
record S6 "drain error lines" 0 "$ERRS"

### receipts
say "receipts"
kubectl logs deployment/dpumesh-webhook -n "$NS" --tail=400 >"$OUT_DIR/webhook.log" 2>&1 || true
dpulog 4000 >"$OUT_DIR/dpu-tail.log" 2>&1 || true
say "campaign: $PASSES pass, $FAILURES fail (out: $OUT_DIR)"
[ "$FAILURES" -eq 0 ]
