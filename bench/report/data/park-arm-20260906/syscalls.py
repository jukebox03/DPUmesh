#!/usr/bin/env python3
"""Per-RPC syscall counts from the perf trace summaries, per arm and size."""
import csv
from pathlib import Path
ROOT=Path(__file__).resolve().parent
rows=[]
for path in sorted((ROOT/'raw').glob('*/*-capacity-syscalls-1.txt')):
    arm,protocol=path.parent.name.split('-',1);size=int(path.name.split('-')[0])
    reply=path.with_name(path.name.replace('.txt','-reply.txt')).read_text()
    fields=dict(x.split('=',1) for x in reply.split() if '=' in x)
    rps=float(fields['mrps'])*1e6;window=6;rpcs=rps*window
    per={}
    for line in path.read_text().splitlines():
        f=line.split()
        if len(f)>=7 and f[1].isdigit() and f[2].isdigit():
            per.setdefault(f[0],[0,0,0.0]);per[f[0]][0]+=int(f[1]);per[f[0]][1]+=int(f[2]);per[f[0]][2]+=float(f[3])
    for name,(calls,errors,ms) in per.items():
        rows.append(dict(arm=arm,protocol=protocol,size=size,syscall=name,window_rps=round(rps),calls=calls,errors=errors,
                         calls_per_rpc=round(calls/rpcs,4),errors_per_rpc=round(errors/rpcs,4),total_ms=round(ms,1),avg_us=round(ms*1000/calls,3)))
if rows:
    with (ROOT/'syscalls.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    for r in rows:
        if r['calls_per_rpc']>=0.01: print(f"{r['arm']:6s} {r['protocol']:6s} {r['size']:6d} {r['syscall']:12s} calls/RPC={r['calls_per_rpc']:7.3f} errors/RPC={r['errors_per_rpc']:6.3f} avg_us={r['avg_us']:6.2f}")
