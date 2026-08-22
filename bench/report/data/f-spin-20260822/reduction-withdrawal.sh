#!/bin/bash
# Minimal reproducer for findings 1.2: which withdrawal breaks the L7 path, and
# for how long.
cd /home/jukebox/DPUmesh
export NS=test-bench CTRL_PORT=9092 LIB_OUT="$PWD/build/lib"
export IMG_ECHO_GRPC=bench/echo-grpc:latest BENCH_REACTORS=1 BENCH_NUMA_NODE=0
export DPUMESH_RINGS_PER_POD=8 DPUMESH_ATTEST_SOCKET=/run/dpumesh/attest.sock
export HOST_PCI="${HOST_PCI:-$(grep -oP '(?<=^HOST_PCI=).*' .env 2>/dev/null | tr -d '"')}"
p() { bash bench/bench.sh point grpc-dpumesh 1024 8 1 5 100 1 0 2>/dev/null | tail -n1 | cut -c1-46; }

echo "A0 baseline                         : $(p)"

echo "--- A: withdraw a SECOND backend of the Service"
envsubst < bench/k8s/policy/echo-grpc-broken.yaml | kubectl apply -f - >/dev/null 2>&1
kubectl rollout status deployment/echo-grpc-broken -n $NS --timeout=180s >/dev/null 2>&1
sleep 20
echo "A1 with the broken backend up       : $(p)"
envsubst < bench/k8s/policy/echo-grpc-broken.yaml | kubectl delete --ignore-not-found -f - >/dev/null 2>&1
kubectl wait --for=delete pod -n $NS -l run=echo-grpc-broken --timeout=90s >/dev/null 2>&1
sleep 20
echo "A2 just after withdrawal            : $(p)"
sleep 45
echo "A3 65s after withdrawal             : $(p)"

echo "--- B: replace the Service's OWN Pod"
kubectl rollout restart deployment/echo-grpc-dpumesh -n $NS >/dev/null 2>&1
kubectl rollout status deployment/echo-grpc-dpumesh -n $NS --timeout=200s >/dev/null 2>&1
sleep 20
echo "B1 after its own Pod was replaced   : $(p)"
sleep 45
echo "B2 65s later                        : $(p)"
