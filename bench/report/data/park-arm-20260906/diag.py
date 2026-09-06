#!/usr/bin/env python3
"""Syscall profile of the lean driver loop at gRPC 64 B capacity.

Reuses the previous campaign's deploy/restore; the lean and batch binaries
stay where that campaign left them on the BlueField.
"""
import importlib.util, json, re, shutil, subprocess, sys, threading, time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
RAW=ROOT/'raw'; RAW.mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('campaign',ROOT.parent/'tx-fixed-overhead-20260906/campaign.py')
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
c.RAW=c.d.RAW=RAW; d=c.d
PREV=ROOT.parent/'tx-fixed-overhead-20260906/raw'
OUT='/tmp/park-arm-20260906'

def prepare():
    s=d.state();c.save('initial-workers.json',s);assert len(s['workers'])==8
    pid=s['workers'][0]['pid']
    initial=d.remote(f'sha256sum /proc/{pid}/exe',sudo=True);c.save('initial-binary.txt',initial)
    assert initial.split()[0]==c.DIRECT_SHA,initial
    c.save('initial-pods.json',d.local(['kubectl','-n','test-bench','get','pods','-o','json']))
    c.save('initial-workloads.json',d.local(['kubectl','-n','test-bench','get','deployment/bench-dpumesh-native',
        'deployment/echo-dpumesh-native','service/echo-dpumesh-native','-o','json']))
    launch=d.remote('cat /tmp/start-dpumesh.sh')
    runtime={k:v.strip("'") for k,v in re.findall(r"(DPUMESH_[A-Z_0-9]+)=('[^']*'|[^ ]*)",launch)}
    c.save('initial-runtime.json',runtime)
    for name in ['binaries.txt','lean-symbols.txt.gz','lean-elf.txt.gz']:
        shutil.copyfile(PREV/name,RAW/name)
    d.remote(f'mkdir -p {OUT}; test -x {c.REMOTE}/lean && test -f {c.REMOTE}/arena_quiescence.gdb')
    import socket
    for port in [28086,28087,28088]:
        with socket.socket() as sock:
            assert sock.connect_ex(('192.168.100.1',port)) != 0, 'control relay port already in use'
    shutil.copyfile('/tmp/direct-linkerd-cp-relay.py',ROOT/'control-relay.py')
    log=open(RAW/'control-relay.log','w')
    proc=subprocess.Popen(['python3',str(ROOT/'control-relay.py')],stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    time.sleep(1);assert proc.poll() is None
    c.save('relay-process.json',dict(pid=proc.pid,starttime=Path(f'/proc/{proc.pid}/stat').read_text().split()[21]))
    print('prepared',flush=True)

def run_with(label,command,remote_cmd,delay=3):
    result={}
    def go(): result['reply']=d.request(command)
    t=threading.Thread(target=go);t.start()
    time.sleep(delay)
    result['probe']=d.remote(remote_cmd,sudo=True,check=False)
    t.join()
    c.save(f'{label}-reply.txt',result['reply']);c.save(f'{label}.txt',result['probe'])
    print(label,result['reply'][:100],flush=True)
    assert result['reply'].startswith('OK ')
    return result

def trace(size=64):
    s=d.state();tids=','.join(str(w['tid']) for w in s['workers']);pid=s['workers'][0]['pid']
    c.save('trace-workers.json',s)
    # 1. Syscall summary per worker thread, 6 s inside a 12 s run.
    run_with(f'strace-{size}',f'RUN {size} {size} 8 12 200 8',
             f'perf trace -s -t {tids} -- sleep 6 2>&1')
    time.sleep(2)
    # 2. Who issues write/read/epoll: tracepoints with call chains, 4 s window.
    events=' '.join(f'-e {e}' for e in ['syscalls:sys_enter_write','syscalls:sys_enter_read','syscalls:sys_enter_epoll_pwait','syscalls:sys_enter_epoll_wait','syscalls:sys_enter_ppoll'])
    run_with(f'tp-{size}',f'RUN {size} {size} 8 12 200 8',
             f'perf record -q {events} -g -t {tids} -o {OUT}/tp-{size}.data -- sleep 4 2>&1; '
             f'perf report -i {OUT}/tp-{size}.data --stdio --no-children --sort comm,sym -g caller,0.5,callee --percent-limit 1 2>&1 | head -400')
    time.sleep(2)
    # 3. Driver loop counters over one clean 8 s run for the record.
    c.save(f'metrics-before-{size}.txt',d.metrics())
    before=d.state()
    reply=d.request(f'RUN {size} {size} 8 8 200 8');after=d.state()
    c.save(f'metrics-after-{size}.txt',d.metrics())
    c.save(f'clean-{size}.json',dict(reply=reply,cpu_before=before,cpu_after=after))
    print('clean',reply[:80],flush=True)

if __name__=='__main__':
    action=sys.argv[1]
    if action=='prepare':prepare()
    elif action=='deploy':c.deploy('lean','grpc')
    elif action=='trace':trace(int(sys.argv[2]) if len(sys.argv)>2 else 64)
    elif action=='restore':c.restore()
