#!/usr/bin/env python3
"""Main three-repeat A/B (lean2 relative to lean) and the end-of-campaign lean re-baseline."""
import csv,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parent
rows=list(csv.DictReader((ROOT/'samples.csv').open()))
ARMS=['lean','lean2']
metrics=['rps','cpu_seconds','cpu_us_per_scheduled_rpc','p50_us','p99_us','dmesh_tx_retries_total','dmesh_tx_writer_wakes_total','dmesh_tx_accepted_bytes_total','dmesh_tx_arena_copy_bytes_total','dmesh_tx_publications_total','dmesh_tx_reserve_attempts_total','dmesh_worker_drain_calls_total','dmesh_worker_arms_total']
results=[]
for protocol in ['grpc','opaque']:
 for size in ([64,1024,8192,65536] if protocol=='grpc' else [64,1024]):
  for mode in ['capacity','matched']:
   arms={a:sorted([r for r in rows if r['arm']==a and r['protocol']==protocol and int(r['size'])==size and r['mode']==mode and r['kind']=='clean' and r['valid']=='True'],key=lambda r:int(r['rep']))[:3] for a in ARMS}
   if not all(len(v)==3 for v in arms.values()):continue
   out=dict(protocol=protocol,size=size,mode=mode)
   for metric in metrics:
    for arm,samples in arms.items():
     if all(r.get(metric) not in [None,''] for r in samples):
      vals=[float(r[metric]) for r in samples];out[arm+'_'+metric]=statistics.median(vals);out[arm+'_'+metric+'_min']=min(vals);out[arm+'_'+metric+'_max']=max(vals)
    a,b=out.get('lean_'+metric),out.get('lean2_'+metric)
    if a and b is not None:out[metric+'_change_percent']=(b/a-1)*100
   for arm in ARMS:
    if out.get(arm+'_dmesh_tx_publications_total'):
     vals=[float(r['dmesh_tx_accepted_bytes_total'])/float(r['dmesh_tx_publications_total']) for r in arms[arm]]
     out[arm+'_bytes_per_publication']=statistics.median(vals)
   results.append(out)
if results:
 fields=list(dict.fromkeys(k for r in results for k in r))
 with (ROOT/'comparison.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(results)
 for r in results:
  print(r['protocol'],r['size'],r['mode'],'RPS',round(r['lean_rps']),round(r['lean2_rps']),f"{r['rps_change_percent']:+.2f}%",
        'CPU/RPC',round(r['lean_cpu_us_per_scheduled_rpc'],2),round(r['lean2_cpu_us_per_scheduled_rpc'],2),f"{r['cpu_us_per_scheduled_rpc_change_percent']:+.2f}%",
        'p50',f"{r['p50_us_change_percent']:+.2f}%",'p99',f"{r['p99_us_change_percent']:+.2f}%",
        'B/pub',round(r.get('lean_bytes_per_publication',0)),round(r.get('lean2_bytes_per_publication',0)))
late=[r for r in rows if r['arm']=='lean' and r['protocol']=='grpc' and r['size']=='65536' and r['kind']=='clean' and r['mode']=='capacity' and int(r['rep'])>3]
if late:
 print('64KiB repeated lean baseline:',[round(float(r['rps'])) for r in late],'CPU/RPC',[round(float(r['cpu_us_per_scheduled_rpc']),2) for r in late])
