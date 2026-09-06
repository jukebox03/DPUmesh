#!/usr/bin/env python3
"""Same-rig A/B: lean (fixed-overhead TX) versus lean2 (no driver poll per pass,
one wake-fd read). The lean binary is the one the previous campaign preserved
on the BlueField; lean2 is the working tree built by `bench/bench.sh build`.
The rig is left as found: the direct binary in the L4 profile with the
original native workloads.
"""
import argparse, importlib.util, json, re, shutil, subprocess, threading, time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('base',ROOT.parent/'tx-fixed-overhead-20260906/campaign.py')
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
d=b.d
RAW=ROOT/'raw'; RAW.mkdir(exist_ok=True)
b.RAW=d.RAW=RAW; d.ROOT=ROOT
REMOTE='/tmp/park-arm-20260906'; b.REMOTE=d.REMOTE=REMOTE
PREV_REMOTE='/tmp/tx-fixed-overhead-20260906'
LEAN_SHA='1577c3ccc7f4261a930a06a8534da00a776e85c5540c10d690cd50cfef72e270'
save=b.save

def setup():
    s=d.state();save('initial-workers.json',s);assert len(s['workers'])==8
    pid=s['workers'][0]['pid']
    initial=d.remote(f'sha256sum /proc/{pid}/exe',sudo=True);save('initial-binary.txt',initial)
    assert initial.split()[0]==b.DIRECT_SHA,initial
    save('initial-pods.json',d.local(['kubectl','-n','test-bench','get','pods','-o','json']))
    save('initial-workloads.json',d.local(['kubectl','-n','test-bench','get','deployment/bench-dpumesh-native',
        'deployment/echo-dpumesh-native','service/echo-dpumesh-native','-o','json']))
    launch=d.remote('cat /tmp/start-dpumesh.sh')
    runtime={k:v.strip("'") for k,v in re.findall(r"(DPUMESH_[A-Z_0-9]+)=('[^']*'|[^ ]*)",launch)}
    save('initial-runtime.json',runtime)
    assert not runtime.get('DPUMESH_L7_SVC') and not runtime.get('DPUMESH_L7_OPAQUE_SVC'),runtime
    d.remote(f'mkdir -p {REMOTE}; chown "$SUDO_USER" {REMOTE}; cp {PREV_REMOTE}/lean {REMOTE}/lean; '
             f'cp {b.DPU_REPO}/doca/build/dpumesh_dpu {REMOTE}/lean2; chmod 755 {REMOTE}/lean {REMOTE}/lean2; '
             f'cp {PREV_REMOTE}/arena-offsets.json {REMOTE}/arena-offsets.json',sudo=True)
    text=d.remote(f'sha256sum {b.OLD}/direct {PREV_REMOTE}/lean {REMOTE}/lean {REMOTE}/lean2 {b.DPU_REPO}/doca/build/dpumesh_dpu',sudo=True)
    save('binaries.txt',text);hashes=b.sha_table(text)
    assert hashes[f'{b.OLD}/direct']==b.DIRECT_SHA and hashes[f'{REMOTE}/lean']==LEAN_SHA,hashes
    assert hashes[f'{REMOTE}/lean2'] not in (LEAN_SHA,b.DIRECT_SHA,b.BATCH_SHA),hashes
    assert hashes[f'{REMOTE}/lean2']==hashes[f'{b.DPU_REPO}/doca/build/dpumesh_dpu']
    for arm in ['lean','lean2']:
        save(f'{arm}-symbols.txt',d.remote(f'nm -n {REMOTE}/{arm} | c++filt',sudo=True))
        save(f'{arm}-elf.txt',d.remote(f'readelf -l {REMOTE}/{arm}',sudo=True))
    # The C proxy structures are unchanged; the arena layout is the previous campaign's.
    layout=(ROOT.parent/'tx-fixed-overhead-20260906/raw/arena-offsets.json').read_text()
    save('arena-offsets.json',layout)
    gdb=(ROOT.parent/'tx-fixed-overhead-20260906/arena_quiescence.gdb').read_text().replace(PREV_REMOTE,REMOTE)
    (ROOT/'arena_quiescence.gdb').write_text(gdb)
    subprocess.run(['scp','-q',str(ROOT/'arena_quiescence.gdb'),f'{d.HOST}:{REMOTE}/arena_quiescence.gdb'],check=True)
    import socket
    for port in [28086,28087,28088]:
        with socket.socket() as sock:
            assert sock.connect_ex(('192.168.100.1',port)) != 0, 'control relay port already in use'
    shutil.copyfile('/tmp/direct-linkerd-cp-relay.py',ROOT/'control-relay.py')
    log=open(RAW/'control-relay.log','w')
    proc=subprocess.Popen(['python3',str(ROOT/'control-relay.py')],stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    time.sleep(1);assert proc.poll() is None
    save('relay-process.json',dict(pid=proc.pid,starttime=Path(f'/proc/{proc.pid}/stat').read_text().split()[21]))
    print('Preserved both binaries and started control relay.',flush=True)

def setup3():
    """Add the lean3 build (pinned maintenance timer, selective clears) as a third arm."""
    d.remote(f'cp {b.DPU_REPO}/doca/build/dpumesh_dpu {REMOTE}/lean3; chmod 755 {REMOTE}/lean3',sudo=True)
    text=d.remote(f'sha256sum {b.OLD}/direct {PREV_REMOTE}/lean {REMOTE}/lean {REMOTE}/lean2 {REMOTE}/lean3 {b.DPU_REPO}/doca/build/dpumesh_dpu',sudo=True)
    save('binaries.txt',text);hashes=b.sha_table(text)
    assert hashes[f'{REMOTE}/lean3'] not in (hashes[f'{REMOTE}/lean2'],LEAN_SHA,b.DIRECT_SHA,b.BATCH_SHA),hashes
    assert hashes[f'{REMOTE}/lean3']==hashes[f'{b.DPU_REPO}/doca/build/dpumesh_dpu']
    save('lean3-symbols.txt',d.remote(f'nm -n {REMOTE}/lean3 | c++filt',sudo=True))
    save('lean3-elf.txt',d.remote(f'readelf -l {REMOTE}/lean3',sudo=True))
    # The relay from setup() is still serving unless restore() ended it.
    data=json.loads((RAW/'relay-process.json').read_text());stat=Path(f'/proc/{data["pid"]}/stat')
    if stat.exists() and stat.read_text().split()[21]==data['starttime']:
        print('Preserved lean3; control relay still running.',flush=True);return
    import socket
    for port in [28086,28087,28088]:
        with socket.socket() as sock:
            assert sock.connect_ex(('192.168.100.1',port)) != 0, 'control relay port already in use'
    log=open(RAW/'control-relay-3.log','w')
    proc=subprocess.Popen(['python3',str(ROOT/'control-relay.py')],stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    time.sleep(1);assert proc.poll() is None
    save('relay-process.json',dict(pid=proc.pid,starttime=Path(f'/proc/{proc.pid}/stat').read_text().split()[21]))
    print('Preserved lean3 and restarted control relay.',flush=True)

def syscalls(arm,protocol,size,duration=12):
    """perf trace summary over the middle of one run, plus callchains for the writes."""
    tag=f'{arm}-{protocol}/{size}-capacity-syscalls-1'
    if (RAW/(tag+'-reply.txt')).exists(): raise RuntimeError('exists: '+tag)
    s=d.state();tids=','.join(str(w['tid']) for w in s['workers'])
    result={}
    def go(): result['reply']=d.request(f'RUN {size} {size} 8 {duration} 200 8')
    t=threading.Thread(target=go);t.start();time.sleep(2)
    result['summary']=d.remote(f'perf trace -s -t {tids} -- sleep {duration-6} 2>&1',sudo=True,check=False)
    t.join()
    save(tag+'-reply.txt',result['reply']);save(tag+'.txt',result['summary'])
    print(tag,result['reply'][:90],flush=True)
    assert result['reply'].startswith('OK ')
    time.sleep(1)
    tag=f'{arm}-{protocol}/{size}-capacity-callchain-1'
    events=' '.join(f'-e {e}' for e in ['syscalls:sys_enter_write','syscalls:sys_enter_read','syscalls:sys_enter_epoll_pwait','syscalls:sys_enter_ppoll'])
    t=threading.Thread(target=go);t.start();time.sleep(2)
    result['chains']=d.remote(f'perf record -q {events} -g -t {tids} -o {REMOTE}/{arm}-{size}-chains.data -- sleep 4 2>&1; '
        f'perf report -i {REMOTE}/{arm}-{size}-chains.data --stdio --no-children --sort sym -g caller,0.5,callee,function,percent --percent-limit 2 2>&1 | head -600',sudo=True,check=False)
    t.join()
    save(tag+'-reply.txt',result['reply']);save(tag+'.txt',result['chains'])
    print(tag,result['reply'][:90],flush=True)
    assert result['reply'].startswith('OK ')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('action',choices=['setup','setup3','deploy','sample','syscalls','restore']);p.add_argument('--arm',choices=['lean','lean2','lean3'],default='lean2');p.add_argument('--protocol',choices=['grpc','opaque'],default='grpc');p.add_argument('--sizes',nargs='+',type=int,default=[64,1024,8192,65536]);p.add_argument('--mode',choices=['capacity','matched'],default='capacity');p.add_argument('--kind',choices=['clean','stat','profile'],default='clean');p.add_argument('--reps',type=int,default=3);p.add_argument('--start-rep',type=int,default=1);p.add_argument('--duration',type=int,default=8)
    a=p.parse_args()
    if a.action=='setup':setup()
    elif a.action=='setup3':setup3()
    elif a.action=='deploy':b.deploy(a.arm,a.protocol)
    elif a.action=='restore':b.restore()
    elif a.action=='syscalls':
        for size in a.sizes: syscalls(a.arm,a.protocol,size)
    else:
        if a.kind!='clean': assert a.duration>=8
        for size in a.sizes:
            for rep in range(a.start_rep,a.start_rep+a.reps):
                d.sample(a.arm,a.protocol,size,a.mode,a.kind,rep,a.duration)
                row=json.loads((RAW/f'{a.arm}-{a.protocol}/{size}-{a.mode}-{a.kind}-{rep}.json').read_text())
                fields=dict(x.split('=',1) for x in row['reply'].split() if '=' in x)
                assert int(fields.get('rcnt','0'))>0 and fields.get('reorder')=='0',row['reply']
                time.sleep(1)
