#!/bin/bash
# Which operation leaves sessions behind? A full surfaces run leaves ~80 with
# the cluster idle; the reductions leave none. Measure after each candidate.
cd /home/jukebox/DPUmesh
set -a; . ./.env; set +a
export NS=test-bench CTRL_PORT=9092 LIB_OUT="$PWD/build/lib"
export IMG_ECHO_GRPC=bench/echo-grpc:latest BENCH_REACTORS=1 BENCH_NUMA_NODE=0
export DPUMESH_RINGS_PER_POD=8 DPUMESH_ATTEST_SOCKET=/run/dpumesh/attest.sock
export GRPC_SERVICE=grpc.testing.BenchmarkService GRPC_METHOD=UnaryCall
export ROUTE_PATH=/grpc.testing.BenchmarkService ROUTE_BACKEND=echo-grpc-dpumesh
TRUST_DOMAIN=linkerd.cluster.local
SSH_OPTS=(-o ServerAliveInterval=15 -o ConnectTimeout=10 -o BatchMode=yes)
act() { ssh "${SSH_OPTS[@]}" -n "$DPU_HOST" '
  for p in 4191 4192 4193 4194 4195 4196 4197 4198; do curl -sf --max-time 3 127.0.0.1:$p/metrics 2>/dev/null || true; done' \
  2>/dev/null | awk '/^dmesh_sessions_active /{a+=$NF} /^dmesh_sessions_opened_total /{o+=$NF} END{printf "active=%-4d opened=%d", a, o}'; }
pt() { bash bench/bench.sh point grpc-dpumesh 1024 8 1 10 100 1 0 2>/dev/null | tail -n1; }
show() { case "$1" in ERR*) echo "ERR";; *) echo "rcnt=$(sed -n 's/.*rcnt=\([0-9]*\).*/\1/p' <<<"$1") fail=$(sed -n 's/.*[^_]fail=\([0-9]*\).*/\1/p' <<<"$1")";; esac; }
ap() { envsubst < "bench/k8s/policy/$1" | kubectl apply -f - >/dev/null 2>&1; sleep 8; }
rm_() { envsubst < "bench/k8s/policy/$1" | kubectl delete --ignore-not-found -f - >/dev/null 2>&1; sleep 8; }

echo "H0 fresh                        [$(act)]"
echo "H1 one plain point              $(show "$(pt)")   [$(act)]"

export ROUTE_METHOD=GET; ap httproute-method.yaml
echo "H2 a no-match stage             $(show "$(pt)")   [$(act)]"
rm_ httproute-method.yaml
echo "H3 route withdrawn              [$(act)]"

export POLICY_IDENTITY="default.$NS.serviceaccount.identity.$TRUST_DOMAIN"
ap server-grpc.yaml
echo "H4 inbound Server, deny         $(show "$(pt)")   [$(act)]"
ap authz-route.yaml
echo "H5 authorization applied        $(show "$(pt)")   [$(act)]"
rm_ authz-route.yaml; rm_ server-grpc.yaml
echo "H6 both withdrawn               [$(act)]"
echo "H7 a plain point again          $(show "$(pt)")   [$(act)]"
