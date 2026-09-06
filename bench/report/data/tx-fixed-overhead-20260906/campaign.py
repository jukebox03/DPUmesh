#!/usr/bin/env python3
"""Same-rig A/B: retained arena-batch binary versus the fixed-overhead build.

Arms: `batch` is the b62194f1 binary the previous campaign preserved on the
BlueField; `lean` is the working tree built by `bench/bench.sh build`.
Credentials stay in memory. The rig is left as found: the direct binary in the
L4 profile with the original native workloads.
"""
import argparse, importlib.util, json, os, re, shutil, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('base',ROOT.parent/'direct-tx-diagnosis-20260906/diagnose.py')
d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
d.ROOT=ROOT; d.RAW=ROOT/'raw'; d.REMOTE='/tmp/tx-fixed-overhead-20260906'
RAW=d.RAW; REMOTE=d.REMOTE
OLD='/tmp/arena-tx-batching-perf-20260906'
BATCH_SHA='b62194f1ba65886438c1b380e8346518c4fd4ad148afaca38f59ddaf77af36eb'
DIRECT_SHA='2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e'
DPU_REPO='/home/jukebox/DPUmesh'
DPU_RECEIPT=f'{DPU_REPO}/bench/report/data/{ROOT.name}'

def save(name,value): d.save(name,value)
def sha_table(text):
    return {line.split()[1]:line.split()[0] for line in text.splitlines() if len(line.split())==2}
def wait_workers():
    for _ in range(60):
        s=d.state()
        if len(s['workers'])==8: return s
        time.sleep(1)
    raise RuntimeError('worker startup timed out')

def setup():
    # The rig as found: workers, running binary, workloads and the L4 launch line.
    s=d.state();save('initial-workers.json',s);assert len(s['workers'])==8
    pid=s['workers'][0]['pid']
    initial=d.remote(f'sha256sum /proc/{pid}/exe',sudo=True);save('initial-binary.txt',initial)
    assert initial.split()[0]==DIRECT_SHA,initial
    save('initial-pods.json',d.local(['kubectl','-n','test-bench','get','pods','-o','json']))
    save('initial-workloads.json',d.local(['kubectl','-n','test-bench','get','deployment/bench-dpumesh-native',
        'deployment/echo-dpumesh-native','service/echo-dpumesh-native','-o','json']))
    launch=d.remote('cat /tmp/start-dpumesh.sh')
    runtime={k:v.strip("'") for k,v in re.findall(r"(DPUMESH_[A-Z_0-9]+)=('[^']*'|[^ ]*)",launch)}
    save('initial-runtime.json',runtime)
    assert not runtime.get('DPUMESH_L7_SVC') and not runtime.get('DPUMESH_L7_OPAQUE_SVC'),runtime
    # Both arms, preserved outside the build tree.
    # The directory stays writable by the unprivileged build steps below.
    d.remote(f'mkdir -p {REMOTE}; chown "$SUDO_USER" {REMOTE}; cp {OLD}/batch {REMOTE}/batch; cp {DPU_REPO}/doca/build/dpumesh_dpu {REMOTE}/lean; chmod 755 {REMOTE}/batch {REMOTE}/lean',sudo=True)
    text=d.remote(f'sha256sum {OLD}/direct {OLD}/batch {REMOTE}/batch {REMOTE}/lean {DPU_REPO}/doca/build/dpumesh_dpu',sudo=True)
    save('binaries.txt',text);hashes=sha_table(text)
    assert hashes[f'{OLD}/direct']==DIRECT_SHA and hashes[f'{REMOTE}/batch']==BATCH_SHA,hashes
    assert hashes[f'{REMOTE}/lean'] not in (BATCH_SHA,DIRECT_SHA),hashes
    assert hashes[f'{REMOTE}/lean']==hashes[f'{DPU_REPO}/doca/build/dpumesh_dpu']
    for arm in ['batch','lean']:
        save(f'{arm}-symbols.txt',d.remote(f'nm -n {REMOTE}/{arm} | c++filt',sudo=True))
        save(f'{arm}-elf.txt',d.remote(f'readelf -l {REMOTE}/{arm}',sudo=True))
    # Arena layout for the quiescence check, compiled from the synced C source.
    d.remote(f'mkdir -p {DPU_RECEIPT}')
    subprocess.run(['scp','-q',str(ROOT/'arena_offsets.c'),f'{d.HOST}:{DPU_RECEIPT}/arena_offsets.c'],check=True)
    layout=d.remote(f'cd {DPU_REPO} && cc -O2 -D_GNU_SOURCE -DDOCA_ALLOW_EXPERIMENTAL_API -DDMESH_L7_RUNTIME_OWNER=1 '
        '-Iinclude -I. -Ilinkerd/include $(pkg-config --cflags doca-common doca-comch doca-dpa doca-dma) '
        f'-ffunction-sections -fdata-sections -Wl,--gc-sections {DPU_RECEIPT}/arena_offsets.c -o {REMOTE}/arena-offsets '
        f'$(pkg-config --libs doca-common) && {REMOTE}/arena-offsets | tee {REMOTE}/arena-offsets.json')
    save('arena-offsets.json',layout)
    previous=(ROOT.parent/'arena-tx-batching-perf-20260906/raw/arena-offsets.json').read_text()
    assert json.loads(layout.strip().splitlines()[-1])==json.loads(previous.strip().splitlines()[-1]),(layout,previous)
    subprocess.run(['scp','-q',str(ROOT/'arena_quiescence.gdb'),f'{d.HOST}:{REMOTE}/arena_quiescence.gdb'],check=True)
    # Temporary control relay; the controller relay on 28089 is pre-existing.
    import socket
    for port in [28086,28087,28088]:
        with socket.socket() as sock:
            assert sock.connect_ex(('192.168.100.1',port)) != 0, 'control relay port already in use'
    shutil.copyfile('/tmp/direct-linkerd-cp-relay.py',ROOT/'control-relay.py')
    log=open(RAW/'control-relay.log','w')
    proc=subprocess.Popen(['python3',str(ROOT/'control-relay.py')],stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    time.sleep(1);assert proc.poll() is None
    save('relay-process.json',dict(pid=proc.pid,starttime=Path(f'/proc/{proc.pid}/stat').read_text().split()[21]))
    print('Preserved both binaries, verified arena layout, started control relay.',flush=True)

def stop_workloads():
    subprocess.run(['kubectl','-n','test-bench','scale','deployment/bench-dpumesh-native','deployment/echo-dpumesh-native','--replicas=0'],check=True)
    subprocess.run(['kubectl','-n','test-bench','wait','--for=delete','pod','-l','app in (bench-dpumesh-native,echo-dpumesh-native)','--timeout=90s'],check=True)
    time.sleep(12)
    workers=d.state()['workers']; pids=sorted({w['pid'] for w in workers})
    assert len(pids)<=1,pids
    if pids:
        try: text=d.metrics()
        except RuntimeError:
            initial_pid=json.loads((RAW/'initial-workers.json').read_text())['workers'][0]['pid']
            runtime=json.loads((RAW/'initial-runtime.json').read_text())
            assert pids[0]==initial_pid and not runtime.get('DPUMESH_L7_SVC') and not runtime.get('DPUMESH_L7_OPAQUE_SVC')
            text=''
            save(f'quiescence-{pids[0]}-metrics-unavailable.txt','Initial L4 runtime has no Linkerd admin metrics listener. Arena is checked separately.\n')
        if text: save(f'quiescence-{pids[0]}-metrics.txt',text)
        required=['dmesh_sessions_active','dmesh_registrations_pending','dmesh_tasks_live',
                  'dmesh_worker_completion_queue_depth','dmesh_worker_cross_queue_depth',
                  'dmesh_worker_dma_tasks_inflight','dmesh_worker_dma_retry_batches',
                  'dmesh_worker_dma_stalled','dmesh_worker_stalled_connections',
                  'dmesh_worker_emit_pending','dmesh_worker_ack_release_depth',
                  'dmesh_worker_ack_retry_pending','dmesh_worker_remote_fin_pending']
        values={k:[] for k in required}
        for line in text.splitlines():
            f=line.split()
            if len(f)==2 and f[0] in values: values[f[0]].append(float(f[1]))
        save(f'quiescence-{pids[0]}.json',values)
        if text: assert all(len(v)==8 and not any(v) for v in values.values()),values
        arena=d.remote(f'timeout 30 gdb -q -batch /proc/{pids[0]}/exe -p {pids[0]} -x {REMOTE}/arena_quiescence.gdb',sudo=True)
        save(f'quiescence-{pids[0]}-arena.txt',arena)
        result=json.loads(next(line.split(' ',1)[1] for line in arena.splitlines() if line.startswith('ARENA_QUIESCENCE ')))
        assert result['live_chunks']==0 and result['free_total']==result['pool_chunks'],result
        d.remote('kill -KILL '+str(pids[0]),sudo=True)
    time.sleep(5)

def launch(arm,protocol):
    script=d.remote('cat /tmp/start-dpumesh-before-direct.sh')
    assert './dpumesh_dpu ' in script
    script=script.replace('./dpumesh_dpu ',f'{REMOTE}/{arm} ')
    script=script.replace('bash -c "cd ','bash -c "set -a; source /tmp/direct-tx-l7.env; set +a; cd ')
    selected=('DPUMESH_L7_OPAQUE_SVC=test-bench/echo-dpumesh-native DPUMESH_L7_SVC='
              if protocol=='opaque' else 'DPUMESH_L7_OPAQUE_SVC= DPUMESH_L7_SVC=test-bench/echo-dpumesh-native')
    assert 'DPUMESH_L7_OPAQUE_SVC= DPUMESH_L7_SVC=' in script
    script=script.replace('DPUMESH_L7_OPAQUE_SVC= DPUMESH_L7_SVC=',selected).replace("-l '40'","-l '30'")
    path=RAW/f'launch-{arm}-{protocol}.sh';path.write_text(script)
    subprocess.run(['scp','-q',str(path),d.HOST+':'+REMOTE+'/launch.sh'],check=True)
    d.remote(f'bash {REMOTE}/launch.sh',sudo=True)

def manifest(protocol):
    import yaml
    env=os.environ|dict(NS='test-bench',RINGS='8',IMG_ECHO='bench/echo-dpumesh:native',IMG_BENCH='bench/bench-dpumesh:native')
    text=subprocess.check_output(['envsubst'],input=Path('bench/k8s/native-hw.yaml').read_text(),text=True,env=env)
    docs=list(yaml.safe_load_all(text))
    for obj in docs:
        if obj['kind']=='Deployment':
            t=obj['spec']['template'];t['metadata']['labels']['linkerd.io/control-plane-ns']='linkerd'
            t['metadata']['annotations']={'config.linkerd.io/skip-inbound-ports':'9091,9092'}
            c=t['spec']['containers'][0];server=obj['metadata']['name'].startswith('echo')
            binary=('echo_grpc' if server else 'bench_grpc') if protocol=='grpc' else ('echo_dpumesh' if server else 'bench_dpumesh')
            c['command']=['/usr/bin/taskset','-c','10-17' if server else '4-9','/usr/local/bin/'+binary]
            if protocol=='grpc':
                c['image']='docker.io/bench/'+('echo-grpc' if server else 'bench-grpc')+':direct-tx'
                c['env'] += [{'name':'BENCH_TRANSPORT','value':'dmesh'},{'name':'BENCH_REACTORS','value':'8'}]
        if obj['kind']=='Service' and protocol=='grpc': obj['spec']['ports'][0].update(port=9091,targetPort=9091,name='grpc')
    return yaml.safe_dump_all(docs)

def deploy(arm,protocol):
    stop_workloads();launch(arm,protocol)
    path=RAW/f'{arm}-{protocol}-manifest.yaml';path.write_text(manifest(protocol))
    subprocess.run(['kubectl','apply','-f',str(path)],check=True)
    for name in ['echo','bench']:
        subprocess.run(['kubectl','-n','test-bench','rollout','status',f'deployment/{name}-dpumesh-native','--timeout=240s'],check=True)
    s=wait_workers();pid=s['workers'][0]['pid']
    save(f'{arm}-{protocol}/workers-{pid}.json',s)
    binary=d.remote(f'sha256sum /proc/{pid}/exe',sudo=True)
    save(f'{arm}-{protocol}/binary-{pid}.txt',binary)
    expected=sha_table((RAW/'binaries.txt').read_text())[f'{REMOTE}/{arm}']
    assert binary.split()[0]==expected,(arm,binary)
    save(f'{arm}-{protocol}/pods-{pid}.json',d.local(['kubectl','-n','test-bench','get','pods','-l','app in (bench-dpumesh-native,echo-dpumesh-native)','-o','json']))
    save(f'{arm}-{protocol}/banner-{pid}.txt',d.remote("grep -h 'Connection-affine topology active\\|DPU PROXY MODE ON' /tmp/dpumesh_dpu_bench.log | tail -2"))
    # The controller feed must name the new destination Pod before the first
    # dial; a warmup that races it is refused ("no live registration"), so give
    # the feed a moment and retry the warmup rather than measure into a race.
    time.sleep(5)
    for attempt in range(1,5):
        assert d.state()['workers'],'DPU runtime is gone'
        reply=d.request('RUN 64 64 8 3 200 8');save(f'{arm}-{protocol}/warmup-{pid}-{attempt}.txt',reply)
        print('WARMUP',arm,protocol,attempt,reply,flush=True)
        if reply.startswith('OK ') and 'fail=0' in reply: break
        time.sleep(5)
    else:
        raise RuntimeError('warmup never succeeded')

def restore():
    save('final-l7-metrics.txt',d.metrics())
    save('final-l7-workers.json',d.state())
    save('final-l7-dpu.log',d.remote('tail -n 120 /tmp/dpumesh_dpu_bench.log'))
    stop_workloads()
    # The rig as found: the direct binary at its build-tree location, L4 profile.
    d.remote(f'cp {OLD}/direct {DPU_REPO}/doca/build/dpumesh_dpu.restore; chmod 755 {DPU_REPO}/doca/build/dpumesh_dpu.restore; mv {DPU_REPO}/doca/build/dpumesh_dpu.restore {DPU_REPO}/doca/build/dpumesh_dpu',sudo=True)
    subprocess.run(['./bench/bench.sh','restart'],stdout=open(RAW/'restore-restart.log','w'),stderr=subprocess.STDOUT,check=True)
    initial=json.loads((RAW/'initial-workloads.json').read_text())
    objects=[]
    for obj in initial['items']:
        objects.append(dict(apiVersion=obj['apiVersion'],kind=obj['kind'],metadata={k:v for k,v in obj['metadata'].items() if k in ['name','namespace','labels','annotations']},spec=obj['spec']))
    path=RAW/'restore-workloads.json';path.write_text(json.dumps(dict(apiVersion='v1',kind='List',items=objects)))
    subprocess.run(['kubectl','apply','-f',str(path)],check=True)
    for name in ['echo','bench']:
        subprocess.run(['kubectl','-n','test-bench','rollout','status',f'deployment/{name}-dpumesh-native','--timeout=240s'],check=True)
    time.sleep(8);wait_workers()
    reply=d.request('RUN 64 64 8 3 100 8');save('restored-native-smoke.txt',reply)
    assert reply.startswith('OK ') and 'fail=0' in reply
    current=json.loads(d.local(['kubectl','-n','test-bench','get','deployment/bench-dpumesh-native','deployment/echo-dpumesh-native','service/echo-dpumesh-native','-o','json']))
    save('restored-workloads.json',current)
    expected={(o['kind'],o['metadata']['name']):o for o in initial['items']}
    for obj in current['items']:
        old=expected[(obj['kind'],obj['metadata']['name'])]
        if obj['kind']=='Deployment': assert obj['spec']['template']==old['spec']['template'] and obj['spec']['replicas']==old['spec']['replicas']
        else: assert obj['spec']==old['spec']
    pid=d.state()['workers'][0]['pid'];binary=d.remote(f'sha256sum /proc/{pid}/exe',sudo=True)
    save('restored-binary.txt',binary);assert binary.split()[0]==DIRECT_SHA,binary
    data=json.loads((RAW/'relay-process.json').read_text());pid=data['pid']
    if Path(f'/proc/{pid}/stat').exists():
        assert Path(f'/proc/{pid}/stat').read_text().split()[21]==data['starttime']
        os.kill(pid,15)
    print('Restored original native workload templates, Service and direct binary. '+reply,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=['setup','deploy','sample','restore']);p.add_argument('--arm',choices=['batch','lean'],default='lean');p.add_argument('--protocol',choices=['grpc','opaque'],default='grpc');p.add_argument('--sizes',nargs='+',type=int,default=[64,1024,8192,65536]);p.add_argument('--mode',choices=['capacity','matched'],default='capacity');p.add_argument('--kind',choices=['clean','stat','trace','profile'],default='clean');p.add_argument('--reps',type=int,default=3);p.add_argument('--start-rep',type=int,default=1);p.add_argument('--duration',type=int,default=8)
    a=p.parse_args()
    if a.action=='setup':setup()
    elif a.action=='deploy':deploy(a.arm,a.protocol)
    elif a.action=='restore':restore()
    else:
        if a.kind!='clean': assert a.duration>=8
        for size in a.sizes:
            for rep in range(a.start_rep,a.start_rep+a.reps):
                d.sample(a.arm,a.protocol,size,a.mode,a.kind,rep,a.duration)
                row=json.loads((RAW/f'{a.arm}-{a.protocol}/{size}-{a.mode}-{a.kind}-{rep}.json').read_text())
                fields=dict(x.split('=',1) for x in row['reply'].split() if '=' in x)
                assert int(fields.get('rcnt','0'))>0 and fields.get('reorder')=='0',row['reply']
                time.sleep(1)
