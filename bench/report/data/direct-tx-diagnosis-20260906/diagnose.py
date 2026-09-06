#!/usr/bin/env python3
"""Read-only binary diagnostics; separate profiled, traced, and clean runs.

Run from the repository root. Credentials stay in memory and are never saved.
Deployment/restoration use copies of the preceding campaign's scripts.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent
RAW = ROOT / 'raw'
PREVIOUS = ROOT.parent / 'direct-tx-rdma-20260905'
RAW.mkdir(exist_ok=True)
REMOTE = '/tmp/direct-tx-diagnosis-20260906'
env = dict(x.split('=', 1) for x in subprocess.check_output(
    ['bash', '-c', 'set -a; source .env; env -0'], text=True).split('\0') if '=' in x)
HOST, PASSWORD = env['DPU_HOST'], env['DPU_PASS']

def local(args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs)

def remote(command, sudo=False, check=True):
    if sudo:
        command = 'sudo -S -p "" bash -c ' + shlex.quote(command)
    p = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                        HOST, command], input=PASSWORD+'\n' if sudo else None,
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and p.returncode:
        raise RuntimeError(f'remote failed ({p.returncode}): {p.stdout}')
    return p.stdout

def save(name, value):
    p = RAW / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(value, indent=2) if not isinstance(value, str) else value)

def state():
    script = '''import glob,json,os
out=[]
for p in glob.glob('/proc/[0-9]*/task/[0-9]*/stat'):
 try:
  s=open(p).read(); name=s[s.index('(')+1:s.rindex(')')]; f=s[s.rindex(')')+2:].split()
  if name.startswith('dmesh-w'):
   out.append(dict(tid=int(p.split('/')[-2]),pid=int(p.split('/')[2]),name=name,ticks=int(f[11])+int(f[12])))
 except (OSError,ValueError): pass
print(json.dumps(dict(clock_hz=os.sysconf('SC_CLK_TCK'),workers=out)))'''
    return json.loads(remote('python3 -c ' + shlex.quote(script)))

def metrics():
    return remote('for port in 4191 4192 4193 4194 4195 4196 4197 4198; do curl -fsS --max-time 3 http://127.0.0.1:$port/metrics || exit; done')

def request(command):
    ip = local(['kubectl','-n','test-bench','get','pod','-l','app=bench-dpumesh-native',
                '-o','jsonpath={.items[0].status.podIP}']).strip()
    with socket.create_connection((ip,9092),timeout=10) as sock:
        sock.settimeout(100)
        sock.sendall((command+'\n').encode()); sock.shutdown(socket.SHUT_WR)
        data=b''
        while True:
            b=sock.recv(65536)
            if not b: break
            data += b
    return data.decode()

def inspect():
    s=state(); save('initial-workers.json',s)
    assert len(s['workers']) == 8
    pid=s['workers'][0]['pid']
    save('initial-pods.json',local(['kubectl','-n','test-bench','get','pods','-o','json']))
    save('initial-workloads.json',local(['kubectl','-n','test-bench','get','deployment/bench-dpumesh-native',
        'deployment/echo-dpumesh-native','service/echo-dpumesh-native','-o','json']))
    out=remote(f'mkdir -p {REMOTE}; sha256sum /proc/{pid}/exe /tmp/dpumesh-before-direct-tx; '
               'perf stat -e cycles,instructions,task-clock -- sleep 0.1; '
               'ls /sys/kernel/tracing; cat /sys/kernel/tracing/uprobe_events',sudo=True)
    save('initial-dpu.txt',out); print(out)
    for arm,path in [('before','/tmp/dpumesh-before-direct-tx'),('after',f'/proc/{pid}/exe')]:
        save(f'{arm}-symbols.txt',remote(f'nm -n {path} | c++filt',sudo=True))
        save(f'{arm}-elf.txt',remote(f'readelf -l {path}',sudo=True))
    print('Saved binary symbols and original workload state.',flush=True)

def deploy(arm,protocol):
    script=(PREVIOUS/'deploy_stage.sh').read_text().replace(
        'receipt=bench/report/data/direct-tx-rdma-20260905','receipt='+str(ROOT))
    path=ROOT/'deploy_stage.sh'; path.write_text(script)
    subprocess.run(['bash',str(path),arm,protocol],check=True)
    ready(arm,protocol)

def ready(arm,protocol):
    for _ in range(30):
        s=state()
        if len(s['workers'])==8: break
        time.sleep(1)
    assert len(s['workers'])==8
    pid=s['workers'][0]['pid']
    save(f'{arm}-{protocol}/workers-{pid}.json',s)
    save(f'{arm}-{protocol}/binary-{pid}.txt',remote(f'sha256sum /proc/{pid}/exe',sudo=True))
    reply=request('RUN 64 64 8 3 200 8')
    save(f'{arm}-{protocol}/warmup-{pid}.txt',reply)
    print('WARMUP',reply,flush=True)
    if not reply.startswith('OK '): raise RuntimeError('Warmup failed')

def sample(arm,protocol,size,mode,kind,rep,duration):
    tag=f'{arm}-{protocol}/{size}-{mode}-{kind}-{rep}'
    if (RAW/(tag+'.json')).exists():
        raise RuntimeError('Refusing to overwrite an existing sample: '+tag)
    rate={64:20000,1024:20000,8192:10000,65536:2000}[size]
    command=(f'OPEN {size} {size} 8 {duration} 200 {rate} const' if mode=='matched'
             else f'RUN {size} {size} 8 {duration} 200 8')
    before=state(); assert len(before['workers'])==8
    tids=','.join(str(w['tid']) for w in before['workers'])
    save(tag+'-metrics-before.txt',metrics())
    profile=None
    path=REMOTE+'/'+tag.replace('/','-')
    def timed_request():
        start=time.monotonic()
        reply=request(command)
        elapsed=time.monotonic()-start
        return reply,elapsed,state()
    if kind=='trace':
        import probe
        probe.setup(arm)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        result=pool.submit(timed_request)
        if kind=='profile':
            # Startup and the first two seconds are excluded from sampling.
            time.sleep(2)
            profile=remote(f'perf record -q -F 99 -e cycles --call-graph fp -t {tids} '
                f'-o {path}.data -- sleep {duration-4}',sudo=True)
            save(tag+'-record.txt',profile)
        elif kind=='stat':
            time.sleep(2)
            save(tag+'-stat.txt',remote('perf stat -x, -e cycles,instructions,cache-misses,'
                f'task-clock,context-switches,cpu-migrations -t {tids} -- sleep {duration-4}',sudo=True))
        elif kind=='trace':
            try:
                time.sleep(2)
                save(tag+'-hist.txt',probe.window(duration-4))
            finally:
                probe.cleanup()
        reply,elapsed,after=result.result()
    save(tag+'-metrics-after.txt',metrics())
    fields=dict(x.split('=',1) for x in reply.split() if '=' in x)
    valid=reply.startswith('OK ') and all(fields.get(k)=='0' for k in ['fail','drops','overflow','worker_fail'])
    row=dict(arm=arm,protocol=protocol,size=size,mode=mode,kind=kind,rep=rep,
             command=command,elapsed=elapsed,cpu_before=before,cpu_after=after,reply=reply,valid=valid)
    save(tag+'.json',row)
    print(tag,reply.strip(),flush=True)
    if not valid: raise RuntimeError('Benchmark failed; saved raw evidence')
    if kind=='profile':
        profile=remote(f'perf report -i {path}.data --stdio --no-children --sort dso,symbol -g none --percent-limit 0',sudo=True)
        save(tag+'-self.txt',profile)
        save(tag+'-children.txt',remote(f'perf report -i {path}.data --stdio --children --percent-limit 0.5',sudo=True))
        remote(f'chmod a+r {path}.data',sudo=True)
        subprocess.run(['scp','-q',HOST+':'+path+'.data',str(RAW/(tag+'.data'))],check=True)

def restore():
    script=(PREVIOUS/'restore_native.sh').read_text().replace(
        'receipt=bench/report/data/direct-tx-rdma-20260905','receipt='+str(ROOT))
    path=ROOT/'restore_native.sh'; path.write_text(script)
    subprocess.run(['bash',str(path)],check=True)

def reports():
    for receipt in sorted(RAW.glob('*/*-profile-*.json')):
        tag=str(receipt.relative_to(RAW).with_suffix(''))
        path=REMOTE+'/'+tag.replace('/','-')
        save(tag+'-self.txt',remote(f'perf report -i {path}.data --stdio --no-children '
             '--sort dso,symbol -g none --percent-limit 0',sudo=True))
        print('REPORT',tag,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('action',choices=['inspect','deploy','ready','sample','restore','reports'])
    p.add_argument('--arm',choices=['before','after'],default='after')
    p.add_argument('--protocol',choices=['grpc','opaque'],default='grpc')
    p.add_argument('--sizes',type=int,nargs='+',default=[64,1024,8192,65536])
    p.add_argument('--mode',choices=['matched','capacity'],default='capacity')
    p.add_argument('--kind',choices=['clean','profile','stat','trace'],default='clean')
    p.add_argument('--reps',type=int,default=1); p.add_argument('--duration',type=int,default=5)
    a=p.parse_args()
    if a.action=='inspect': inspect()
    elif a.action=='deploy': deploy(a.arm,a.protocol)
    elif a.action=='ready': ready(a.arm,a.protocol)
    elif a.action=='restore': restore()
    elif a.action=='reports': reports()
    else:
        if a.kind!='clean': assert a.duration>=8
        for size in a.sizes:
            for rep in range(1,a.reps+1):
                sample(a.arm,a.protocol,size,a.mode,a.kind,rep,a.duration)
                time.sleep(1)
