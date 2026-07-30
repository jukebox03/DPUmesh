#!/bin/bash
# Idle re-registration validator: a pod that quiesced must be able to register
# again after the DPU has been idle. Passes when the loopback validator passes
# both before the quiesce and after re-registration.
#
#   ./bench/validators/idle_reregister.sh [IDLE_S] [N] [SIZE]
#
# IDLE_S defaults to 720 s; the mesh must already be deployed.
set -u
IDLE_S="${1:-720}" N="${2:-10000}" SIZE="${3:-1024}"
NS="${NS:-test-bench}"
HERE=$(cd "$(dirname "$0")/.." && pwd)

run_pass() {
    bash "$HERE/bench.sh" loopback "$N" "$SIZE" 0
}

echo "=== idle_reregister: pass 1 (pre-quiesce)"
kubectl scale deployment loopback-dpumesh --replicas=1 -n "$NS" >/dev/null
kubectl wait --for=condition=ready pod -l app=loopback-dpumesh -n "$NS" --timeout=120s >/dev/null || {
    echo "FAIL: loopback pod not ready — is the mesh deployed?"
    exit 1
}
sleep 2
run_pass || { echo "FAIL: baseline loopback pass"; exit 1; }

echo "=== idle_reregister: quiesce loopback pod, idle ${IDLE_S}s"
kubectl scale deployment loopback-dpumesh --replicas=0 -n "$NS" >/dev/null
kubectl wait --for=delete pod -l app=loopback-dpumesh -n "$NS" --timeout=60s >/dev/null 2>&1 || true
sleep "$IDLE_S"

echo "=== idle_reregister: re-register (scale up) + pass 2"
kubectl scale deployment loopback-dpumesh --replicas=1 -n "$NS" >/dev/null
kubectl wait --for=condition=ready pod -l app=loopback-dpumesh -n "$NS" --timeout=120s >/dev/null || {
    echo "FAIL: pod did not become ready after idle — registration rejected?"
    exit 1
}
sleep 2
run_pass || { echo "FAIL: post-idle loopback pass"; exit 1; }
echo "PASS: idle_reregister (idle=${IDLE_S}s)"
