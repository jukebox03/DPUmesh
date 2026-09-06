#!/bin/bash
set -euo pipefail
receipt=/home/jukebox/DPUmesh/bench/report/data/direct-tx-diagnosis-20260906
kubectl -n test-bench scale deployment/bench-dpumesh-native deployment/echo-dpumesh-native --replicas=0
kubectl -n test-bench wait --for=delete pod -l 'app in (bench-dpumesh-native,echo-dpumesh-native)' --timeout=90s
sleep 12
./bench/bench.sh restart
NS=test-bench RINGS=8 IMG_ECHO=bench/echo-dpumesh:native IMG_BENCH=bench/bench-dpumesh:native envsubst < bench/k8s/native-hw.yaml > "$receipt/raw/restored-native-manifest.yaml"
kubectl apply -f "$receipt/raw/restored-native-manifest.yaml"
kubectl -n test-bench rollout status deployment/echo-dpumesh-native --timeout=240s
kubectl -n test-bench rollout status deployment/bench-dpumesh-native --timeout=240s
sleep 8
./bench/bench.sh point 64 64 8 3 100 8 > "$receipt/raw/restored-native-smoke.txt"
kubectl -n test-bench get pods -o wide > "$receipt/raw/restored-native-pods.txt"
set -a
source .env
set +a
ssh "$DPU_HOST" "pid=\$(pgrep -x dpumesh_dpu); echo '$DPU_PASS' | sudo -S -p '' sha256sum /proc/\$pid/exe; head -n 40 /tmp/dpumesh_dpu_bench.log" > "$receipt/raw/restored-native-dpu.txt"
