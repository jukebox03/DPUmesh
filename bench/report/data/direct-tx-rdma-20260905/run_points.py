#!/usr/bin/env python3
"""Same client, geometry and cases for both binaries; retain every raw reply."""
import argparse, json, pathlib, socket, subprocess, time
p=argparse.ArgumentParser();p.add_argument('stage');p.add_argument('--client',default='bench-dpumesh-native');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parent
ip=subprocess.check_output(['kubectl','-n','test-bench','get','pod','-l','app='+a.client,'-o','jsonpath={.items[0].status.podIP}'],text=True).strip()
# The shell loads rig credentials without printing them.
def remote(cmd):
    return subprocess.check_output(['bash','-c','set -a; source .env; set +a; ssh "$DPU_HOST" "$1"','receipt',cmd],text=True)
def cpu():
    script="import glob,json; d={};\nfor p in glob.glob('/proc/[0-9]*/task/[0-9]*/stat'):\n try:\n  s=open(p).read(); name=s[s.index('(')+1:s.rindex(')')]; f=s[s.rindex(')')+2:].split();\n  if name.startswith('dmesh-w'): d[p]=int(f[11])+int(f[12])\n except (OSError,ValueError): pass\nprint(json.dumps(d))"
    import shlex
    return json.loads(remote('python3 -c '+shlex.quote(script)))
def request(cmd):
    with socket.create_connection((ip,9092),timeout=10) as s:
        s.settimeout(90);s.sendall((cmd+'\n').encode());s.shutdown(socket.SHUT_WR);out=b''
        while True:
            b=s.recv(8192)
            if not b: break
            out+=b
    return out.decode()
raw=root/'raw'/a.stage;raw.mkdir(parents=True,exist_ok=True)
(raw/'client-affinity.txt').write_text(subprocess.check_output(['kubectl','-n','test-bench','exec','deployment/'+a.client,'--','taskset','-pc','1'],text=True))
(raw/'server-affinity.txt').write_text(subprocess.check_output(['kubectl','-n','test-bench','exec','deployment/echo-dpumesh-native','--','taskset','-pc','1'],text=True))
(raw/'pods.json').write_text(subprocess.check_output(['kubectl','-n','test-bench','get','pods','-o','json'],text=True))
(raw/'metrics-before.txt').write_text(remote('for port in 4191 4192 4193 4194 4195 4196 4197 4198; do curl -fsS --max-time 3 http://127.0.0.1:$port/metrics; done'))
(raw/'warmup.txt').write_text(request('RUN 64 64 8 3 200 8'))
for size,rate in [(64,20000),(1024,20000),(8192,10000),(65536,2000)]:
    for mode in ['matched','capacity']:
        cmd=f'OPEN {size} {size} 8 5 200 {rate} const' if mode=='matched' else f'RUN {size} {size} 8 5 200 8'
        for rep in range(1,4):
            before=cpu();t=time.time();out=request(cmd);elapsed=time.time()-t;after=cpu()
            row={'stage':a.stage,'size':size,'mode':mode,'rep':rep,'command':cmd,'elapsed':elapsed,'cpu_before':before,'cpu_after':after,'reply':out}
            (raw/f'{size}-{mode}-{rep}.json').write_text(json.dumps(row,indent=2))
            print(a.stage,size,mode,rep,out.strip(),flush=True)
            time.sleep(.5)
(raw/'dpu.log').write_text(remote('cat /tmp/dpumesh_dpu_bench.log'))

(raw/'metrics-after.txt').write_text(remote('for port in 4191 4192 4193 4194 4195 4196 4197 4198; do curl -fsS --max-time 3 http://127.0.0.1:$port/metrics; done'))
