#!/bin/bash
set -euo pipefail
set -a
source .env
set +a
stage=${1:?before or after}
mode=${2:?opaque or grpc}
case "$stage/$mode" in before/opaque|before/grpc|after/opaque|after/grpc) ;; *) exit 2;; esac
receipt=bench/report/data/direct-tx-rdma-20260905
kubectl -n test-bench scale deployment/bench-dpumesh-native deployment/echo-dpumesh-native --replicas=0
kubectl -n test-bench wait --for=delete pod -l 'app in (bench-dpumesh-native,echo-dpumesh-native)' --timeout=90s
sleep 12
ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S -p '' pkill -KILL -f '^(/tmp/dpumesh-before-direct-tx|./dpumesh_dpu)( |$)'" || true
sleep 5
scp -q "$DPU_HOST:/tmp/start-dpumesh-before-direct.sh" /tmp/direct-stage-launch.sh
python3 - "$stage" "$mode" <<'PY'
import sys
p='/tmp/direct-stage-launch.sh';s=open(p).read()
s=s.replace('bash -c "cd ', 'bash -c "set -a; source /tmp/direct-tx-l7.env; set +a; cd ')
selected='DPUMESH_L7_OPAQUE_SVC=test-bench/echo-dpumesh-native DPUMESH_L7_SVC=' if sys.argv[2]=='opaque' else 'DPUMESH_L7_OPAQUE_SVC= DPUMESH_L7_SVC=test-bench/echo-dpumesh-native'
s=s.replace('DPUMESH_L7_OPAQUE_SVC= DPUMESH_L7_SVC=',selected)
if sys.argv[1]=='before': s=s.replace('./dpumesh_dpu ', '/tmp/dpumesh-before-direct-tx ')
# Same release code and log level; retain banner and error evidence.
s=s.replace("-l '40'", "-l '30'")
open(p,'w').write(s)
PY
scp -q /tmp/direct-stage-launch.sh "$DPU_HOST:/tmp/direct-stage-launch.sh"
ssh "$DPU_HOST" "echo '$DPU_PASS' | sudo -S -p '' bash /tmp/direct-stage-launch.sh" || true
NS=test-bench RINGS=8 IMG_ECHO=bench/echo-dpumesh:native IMG_BENCH=bench/bench-dpumesh:native envsubst < bench/k8s/native-hw.yaml > /tmp/direct-stage-manifest.yaml
python3 - "$mode" <<'PY'
import json,sys,yaml
p='/tmp/direct-stage-manifest.yaml'; docs=list(yaml.safe_load_all(open(p)))
for d in docs:
 if d['kind']=='Deployment':
  t=d['spec']['template'];t['metadata']['labels']['linkerd.io/control-plane-ns']='linkerd'
  t['metadata']['annotations']={'config.linkerd.io/skip-inbound-ports':'9091,9092'}
  c=t['spec']['containers'][0]
  server=d['metadata']['name'].startswith('echo')
  binary=('echo_grpc' if server else 'bench_grpc') if sys.argv[1]=='grpc' else ('echo_dpumesh' if server else 'bench_dpumesh')
  c['command']=['/usr/bin/taskset','-c','10-17' if server else '4-9','/usr/local/bin/'+binary]
  if sys.argv[1]=='grpc':
   c['image']='docker.io/bench/'+('echo-grpc' if d['metadata']['name'].startswith('echo') else 'bench-grpc')+':direct-tx'
   c['env'] += [{'name':'BENCH_TRANSPORT','value':'dmesh'},{'name':'BENCH_REACTORS','value':'8'}]
 if d['kind']=='Service' and sys.argv[1]=='grpc':
  d['spec']['ports'][0].update(port=9091,targetPort=9091,name='grpc')
open(p,'w').write(yaml.safe_dump_all(docs))
PY
cp /tmp/direct-stage-manifest.yaml "$receipt/raw/$stage-$mode-manifest.yaml"
kubectl apply -f /tmp/direct-stage-manifest.yaml
kubectl -n test-bench rollout status deployment/echo-dpumesh-native --timeout=240s
kubectl -n test-bench rollout status deployment/bench-dpumesh-native --timeout=240s
