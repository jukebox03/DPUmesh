#!/usr/bin/env python3
"""Render the campaign CSVs as Markdown tables for SUMMARY.md."""
import csv
from pathlib import Path
ROOT=Path(__file__).resolve().parent
SIZE={64:'64 B',1024:'1 KiB',8192:'8 KiB',65536:'64 KiB'}

def rows(name):
    path=ROOT/name
    return list(csv.DictReader(path.open())) if path.exists() else []

def pct(v): return f"{float(v):+.2f}%"
def num(v,digits=0): return f"{float(v):,.{digits}f}"

cmp=rows('comparison.csv')
for mode in ['capacity','matched']:
    print(f'\n### {mode}\n')
    print('| protocol | payload | RPC/s lean→lean2 | CPU µs/RPC lean→lean2 | p50 µs lean→lean2 | p99 µs lean→lean2 | B/publication lean→lean2 |')
    print('|---|---:|---:|---:|---:|---:|---:|')
    for r in cmp:
        if r['mode']!=mode: continue
        print(f"| {r['protocol']} | {SIZE[int(r['size'])]} | {num(r['lean_rps'])}→{num(r['lean2_rps'])} ({pct(r['rps_change_percent'])}) "
              f"| {num(r['lean_cpu_us_per_scheduled_rpc'],2)}→{num(r['lean2_cpu_us_per_scheduled_rpc'],2)} ({pct(r['cpu_us_per_scheduled_rpc_change_percent'])}) "
              f"| {num(r['lean_p50_us'])}→{num(r['lean2_p50_us'])} ({pct(r['p50_us_change_percent'])}) "
              f"| {num(r['lean_p99_us'])}→{num(r['lean2_p99_us'])} ({pct(r['p99_us_change_percent'])}) "
              f"| {num(r.get('lean_bytes_per_publication',0))}→{num(r.get('lean2_bytes_per_publication',0))} |")
    print('\nRPC/s 범위(min–max, 3회):\n')
    print('| protocol | payload | lean | lean2 | 겹침 |')
    print('|---|---:|---:|---:|---|')
    for r in cmp:
        if r['mode']!=mode: continue
        b=(float(r['lean_rps_min']),float(r['lean_rps_max']));l=(float(r['lean2_rps_min']),float(r['lean2_rps_max']))
        overlap='yes' if b[0]<=l[1] and l[0]<=b[1] else 'no'
        print(f"| {r['protocol']} | {SIZE[int(r['size'])]} | {num(b[0])}–{num(b[1])} | {num(l[0])}–{num(l[1])} | {overlap} |")

print('\n### drain passes and wakes per RPC (capacity, clean medians)\n')
summary=rows('summary.csv')
print('| protocol | payload | arm | drain calls/RPC | arms/RPC | writer wakes/RPC | retries |')
print('|---|---:|---|---:|---:|---:|---:|')
for r in summary:
    if r['mode']!='capacity' or r['kind']!='clean': continue
    rps=float(r['rps']);n=rps*8
    def per(k): return f"{float(r[k])/n:.3f}" if r.get(k) not in (None,'') else 'NA'
    print(f"| {r['protocol']} | {SIZE[int(r['size'])]} | {r['arm']} | {per('dmesh_worker_drain_calls_total')} | {per('dmesh_worker_arms_total')} | {per('dmesh_tx_writer_wakes_total')} | {r.get('dmesh_tx_retries_total','NA')} |")

print('\n### publication shape (separate trace runs)\n')
print('| protocol | payload | arm | publications | mean bytes | publications/MiB | 64 KiB full |')
print('|---|---:|---|---:|---:|---:|---:|')
for r in sorted(rows('trace-summary.csv'),key=lambda r:(r['protocol'],int(r['size']),r['arm'])):
    print(f"| {r['protocol']} | {SIZE[int(r['size'])]} | {r['arm']} | {num(r['commits'])} | {num(r['mean_commit_bytes'],1)} | {num(r['commits_per_mib'],2)} | {r['full_64k_commits']} |")

print('\n### 64 KiB PMU (separate 12 s runs, middle 8 s)\n')
pmu={r['arm']:r for r in rows('pmu-summary.csv')}
if {'lean','lean2'}<=pmu.keys():
    print('| metric | lean | lean2 | change |')
    print('|---|---:|---:|---:|')
    for k,label,digits in [('cores','worker cores',3),('cycles_per_rpc_estimate','cycles/RPC',0),('instructions_per_rpc_estimate','instructions/RPC',0),('ipc','IPC',3),('cache_misses_per_rpc_estimate','cache-misses/RPC',0),('context_switches_per_rpc_estimate','context-switches/RPC',3)]:
        b=float(pmu['lean'][k]);l=float(pmu['lean2'][k])
        print(f"| {label} | {num(b,digits)} | {num(l,digits)} | {pct((l/b-1)*100)} |")

print('\n### profile categories (exclusive self %, one 12 s window per size)\n')
prof=rows('profile-summary.csv')
if prof:
    keys=['copy_all_layers','atomic_helpers_all_layers','runtime','allocator','kernel','dmesh_poll_write_self','c_commit_self','other']
    print('| protocol | payload | arm | '+' | '.join(keys)+' |')
    print('|---|---:|---|'+'---:|'*len(keys))
    for r in sorted(prof,key=lambda r:(r['protocol'],int(r['size']),r['arm'])):
        print(f"| {r['protocol']} | {SIZE[int(r['size'])]} | {r['arm']} | "+' | '.join(r.get(k) or '' for k in keys)+' |')

print('\n### top symbol movers (lean2 − lean, exclusive self %)\n')
sym=rows('profile-symbols.csv')
if sym:
    from collections import defaultdict
    by=defaultdict(dict)
    for r in sym:
        if r['protocol']=='grpc': by[(int(r['size']),r['symbol'])][r['arm']]=float(r['self_percent'])
    for size in sorted({s for s,_ in by}):
        diffs=sorted(((d.get('lean2',0)-d.get('lean',0),d.get('lean',0),d.get('lean2',0),s) for (sz,s),d in by.items() if sz==size))
        print(f'\n{SIZE[size]} losers (lean→lean2):\n')
        for delta,b,l,s in diffs[:10]: print(f'- {delta:+.2f} ({b:.2f}→{l:.2f}) `{s[:100]}`')
        print(f'\n{SIZE[size]} gainers:\n')
        for delta,b,l,s in diffs[::-1][:8]: print(f'- {delta:+.2f} ({b:.2f}→{l:.2f}) `{s[:100]}`')
