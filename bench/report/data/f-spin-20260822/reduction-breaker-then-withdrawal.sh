#!/bin/bash
# S13 -> S14 in isolation: the breaker annotations, a failing endpoint inside
# the Service, traffic that makes the breaker act, then the withdrawal.
cd /home/jukebox/DPUmesh
export NS=test-bench CTRL_PORT=9092 LIB_OUT="$PWD/build/lib"
export IMG_ECHO_GRPC=bench/echo-grpc:latest BENCH_REACTORS=1 BENCH_NUMA_NODE=0
export DPUMESH_RINGS_PER_POD=8 DPUMESH_ATTEST_SOCKET=/run/dpumesh/attest.sock
export HOST_PCI="${HOST_PCI:-$(grep -oP '(?<=^HOST_PCI=).*' .env 2>/dev/null | tr -d '"')}"
p() { bash bench/bench.sh point grpc-dpumesh 1024 8 1 10 100 1 0 2>/dev/null | tail -n1 | cut -c1-56; }

echo "C0 baseline                          : $(p)"
kubectl annotate service echo-grpc-dpumesh -n $NS --overwrite \
  balancer.linkerd.io/failure-accrual=consecutive \
  balancer.linkerd.io/failure-accrual-consecutive-max-failures=3 \
  balancer.linkerd.io/failure-accrual-consecutive-min-penalty=10s >/dev/null 2>&1
sleep 8
echo "C1 breaker annotations only          : $(p)"
envsubst < bench/k8s/policy/echo-grpc-broken.yaml | kubectl apply -f - >/dev/null 2>&1
kubectl rollout status deployment/echo-grpc-broken -n $NS --timeout=180s >/dev/null 2>&1
sleep 25
echo "C2 = S13, failing endpoint present   : $(p)"
envsubst < bench/k8s/policy/echo-grpc-broken.yaml | kubectl delete --ignore-not-found -f - >/dev/null 2>&1
kubectl wait --for=delete pod -n $NS -l run=echo-grpc-broken --timeout=90s >/dev/null 2>&1
sleep 20
echo "C3 = S14, endpoint withdrawn         : $(p)"
sleep 45
echo "C4 65s later                         : $(p)"
kubectl annotate service echo-grpc-dpumesh -n $NS \
  balancer.linkerd.io/failure-accrual- \
  balancer.linkerd.io/failure-accrual-consecutive-max-failures- \
  balancer.linkerd.io/failure-accrual-consecutive-min-penalty- >/dev/null 2>&1
sleep 8
echo "C5 annotations removed               : $(p)"
