#!/usr/bin/env python3
"""Recompute CPU, per-case counter deltas, and trace distributions from receipts."""
from collections import defaultdict, Counter
import csv
import gzip
import json
from pathlib import Path
import re
import statistics

ROOT=Path(__file__).resolve().parent
RAW=ROOT/'raw'

def raw_text(path):
    if path.exists(): return path.read_text()
    with gzip.open(str(path)+'.gz','rt') as f:return f.read()

def raw_paths(pattern):
    return sorted(set(RAW.glob(pattern)) | {p.with_suffix('') for p in RAW.glob(pattern+'.gz')})

def metrics(path):
    values=defaultdict(list)
    for line in raw_text(path).splitlines():
        if line.startswith('dmesh_') and '{' not in line:
            key,val=line.split(); values[key].append(float(val))
    return {k:sum(v) for k,v in values.items() if len(v)==8}

def write_csv(name,rows):
    if not rows: return
    fields=list(dict.fromkeys(k for row in rows for k in row))
    with (ROOT/name).open('w') as out:
        w=csv.DictWriter(out,fieldnames=fields);w.writeheader();w.writerows(rows)

rows=[]
for path in sorted(RAW.glob('*/*-*-*-*.json')):
    raw=json.loads(path.read_text())
    if 'reply' not in raw: continue
    fields=dict(x.split('=',1) for x in raw['reply'].split() if '=' in x)
    row={k:raw[k] for k in ['arm','protocol','size','mode','kind','rep','valid','elapsed']}
    b=raw['cpu_before'];a=raw['cpu_after']
    before={w['tid']:w['ticks'] for w in b['workers']}
    after={w['tid']:w['ticks'] for w in a['workers']}
    assert len(before)==8 and before.keys()==after.keys() and b['clock_hz']==a['clock_hz']
    row['cpu_seconds']=sum(after[k]-v for k,v in before.items())/b['clock_hz']
    row['scheduled']=int(fields['scheduled'])
    row['rps']=float(fields['mrps'])*1e6
    row['p50_us']=float(fields['p50']);row['p99_us']=float(fields['p99'])
    row['cpu_us_per_scheduled_rpc']=row['cpu_seconds']*1e6/row['scheduled']
    mb=metrics(path.with_name(path.stem+'-metrics-before.txt'))
    ma=metrics(path.with_name(path.stem+'-metrics-after.txt'))
    for k in ma.keys() & mb.keys():
        if k.endswith('_total') and k.startswith(('dmesh_tx_','dmesh_worker_')):
            row[k]=ma[k]-mb[k]
    rows.append(row)
write_csv('samples.csv',rows)
groups=defaultdict(list)
for row in rows:
    if row['valid']: groups[tuple(row[k] for k in ['arm','protocol','size','mode','kind'])].append(row)
summary=[]
for key,items in sorted(groups.items()):
    out=dict(zip(['arm','protocol','size','mode','kind'],key));out['reps']=len(items)
    for metric in ['rps','p50_us','p99_us','cpu_seconds','cpu_us_per_scheduled_rpc',
                   'dmesh_tx_budget_wait_total','dmesh_tx_writer_wakes_total',
                   'dmesh_tx_publications_total','dmesh_tx_reserve_attempts_total','dmesh_tx_arena_copy_bytes_total',
                   'dmesh_tx_retries_total','dmesh_tx_accepted_bytes_total',
                   'dmesh_worker_drain_calls_total','dmesh_worker_arms_total']:
        vals=[x[metric] for x in items if metric in x]
        if len(vals)==len(items):
            out[metric]=statistics.median(vals)
            out[metric+'_min']=min(vals);out[metric+'_max']=max(vals)
    summary.append(out)
write_csv('summary.csv',summary)

histograms=[];traces=[]
for path in sorted(RAW.glob('*/*-hist.txt')):
    receipt=path.with_name(path.name.replace('-hist.txt','.json'))
    if not receipt.exists(): continue  # A running window has not finished yet.
    raw=json.loads(receipt.read_text())
    counts=defaultdict(Counter);event=None;drops=0
    for line in path.read_text().splitlines():
        if line.startswith('EVENT='): event=line.split('=',1)[1]
        m=re.search(r'\{.*?\}\s+hitcount:\s*(\d+)',line)
        if m:
            length=re.search(r'len:\s*(-?\d+)',line)
            counts[event][int(length[1]) if length else 0]+=int(m[1])
        m=re.search(r'Dropped:\s*(\d+)',line)
        if m: drops+=int(m[1])
    base={k:raw[k] for k in ['arm','protocol','size','mode','rep']}
    total=Counter()
    for ev,dist in counts.items():
        if ev.startswith('commit'): total.update(dist)
        for length,count in sorted(dist.items()):
            histograms.append(dict(base,event=ev,length=length,count=count))
    nonempty=sum(v for k,v in total.items() if k>0)
    nbytes=sum(k*v for k,v in total.items() if k>0)
    traces.append(dict(base,write_calls=None,
        vectored_calls=None,commits=nonempty,
        zero_returns=total[0],terminal_returns=sum(v for k,v in total.items() if k<0),bytes=nbytes,mean_commit_bytes=nbytes/nonempty if nonempty else 0,
        commits_per_mib=nonempty/(nbytes/1048576) if nbytes else 0,
        full_64k_commits=total[65536],histogram_dropped=drops))
write_csv('commit-histograms.csv',histograms);write_csv('trace-summary.csv',traces)

symbols=[];profiles=[]
for path in raw_paths('*/*-self.txt'):
    receipt=path.with_name(path.name.replace('-self.txt','.json'))
    if not receipt.exists(): continue
    raw=json.loads(receipt.read_text())
    base={k:raw[k] for k in ['arm','protocol','size','mode','rep']}
    grouped=Counter();categories=Counter()
    for line in raw_text(path).splitlines():
        m=re.match(r'\s*([\d.]+)%\s+(.*?)\s+\[([.k])\]\s+(.*)',line)
        if not m: continue
        percent=float(m[1]);symbol=re.split(r'\s{2,}',m[4].strip())[0];grouped[symbol]+=percent
        category='other'
        if m[3]=='k': category='kernel'
        elif re.search(r'memcpy|memmove',symbol): category='copy_all_layers'
        elif re.search(r'^__aarch64_(ldadd|cas|swp|ldclr|ldset)',symbol): category='atomic_helpers_all_layers'
        elif 'DmeshIo as ' in symbol and '::poll_write' in symbol: category='dmesh_poll_write_self'
        elif 'dmesh_l7_tx_commit' in symbol: category='c_commit_self'
        elif 'tokio::runtime' in symbol or 'tokio::task' in symbol or 'mio::' in symbol: category='runtime'
        elif '_rjem_' in symbol or re.search(r'^(__libc_)?(malloc|free|realloc|calloc)$',symbol): category='allocator'
        categories[category]+=percent
    for symbol,percent in grouped.most_common():
        symbols.append(dict(base,symbol=symbol,self_percent=round(percent,4)))
    profiles.append(dict(base,**{k:round(v,4) for k,v in categories.items()},reported_sum=round(sum(categories.values()),4)))
write_csv('profile-symbols.csv',symbols);write_csv('profile-summary.csv',profiles)

pmu=[]
for path in sorted(RAW.glob('*/*-stat-*-stat.txt')):
    receipt=path.with_name(path.name.replace('-stat.txt','.json'))
    if not receipt.exists(): continue
    raw=json.loads(receipt.read_text())
    fields=dict(w.split('=',1) for w in raw['reply'].split() if '=' in w)
    rps=float(fields['mrps'])*1e6
    seconds=float(raw['command'].split()[4])-4
    events={}
    for parts in csv.reader(path.open()):
        if len(parts)>4 and parts[0] and not parts[0].startswith('<'):
            assert float(parts[4])==100.0, 'PMU multiplexing needs explicit handling'
            events[parts[2]]=float(parts[0])
    base={k:raw[k] for k in ['arm','protocol','size','mode','rep']}
    base.update(window_seconds=seconds,whole_run_rps=rps,estimated_window_rpcs=rps*seconds,
        ipc=events['instructions']/events['cycles'],cores=events['task-clock']/(1000*seconds))
    for event in ['cycles','instructions','cache-misses','context-switches','cpu-migrations']:
        base[event.replace('-','_')]=events[event]
        base[event.replace('-','_')+'_per_rpc_estimate']=events[event]/(rps*seconds)
    pmu.append(base)
write_csv('pmu-samples.csv',pmu)
pmu_groups=defaultdict(list)
for row in pmu: pmu_groups[tuple(row[k] for k in ['arm','protocol','size','mode'])].append(row)
pmu_summary=[]
for key,items in sorted(pmu_groups.items()):
    out=dict(zip(['arm','protocol','size','mode'],key));out['reps']=len(items)
    for metric in ['ipc','cores','cycles_per_rpc_estimate','instructions_per_rpc_estimate',
                   'cache_misses_per_rpc_estimate','context_switches_per_rpc_estimate']:
        vals=[x[metric] for x in items]
        out[metric]=statistics.median(vals);out[metric+'_min']=min(vals);out[metric+'_max']=max(vals)
    pmu_summary.append(out)
write_csv('pmu-summary.csv',pmu_summary)
print(f'{len(rows)} samples; {len(traces)} trace windows')
