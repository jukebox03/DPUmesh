#!/usr/bin/env python3
import collections,csv,json,pathlib,statistics
root=pathlib.Path(__file__).resolve().parent

def cases(stage):
    groups=collections.defaultdict(list)
    for p in (root/'raw'/stage).glob('*-*-*.json'):
        row=json.loads(p.read_text()); reply=row.get('reply','')
        fields=dict(word.split('=',1) for word in reply.split() if '=' in word)
        valid=reply.startswith('OK ') and int(fields.get('rcnt',0))>0 and all(fields.get(k)=='0' for k in ('fail','drops','overflow','worker_fail'))
        cpu=None
        b,a=row['cpu_before'],row['cpu_after']
        if len(b)==8 and b.keys()==a.keys() and all(a[k]>=b[k] for k in b):
            cpu=sum(a[k]-b[k] for k in b)/100*1e6/int(fields['scheduled'])
        groups[row['size'],row['mode']].append({'valid':valid,'rps':float(fields.get('mrps',0))*1e6,'p50_us':float(fields.get('p50',0)),'p99_us':float(fields.get('p99',0)),'cpu_us_per_scheduled_rpc':cpu})
    return groups
rows=[]
for protocol in ('opaque','grpc'):
    before=cases('before-'+protocol+'-clean');after=cases('after-'+protocol+'-clean')
    for key in sorted(before):
        b=before[key];a=after.get(key,[])
        r={'protocol':protocol,'size':key[0],'mode':key[1],'before_valid':sum(x['valid'] for x in b),'after_valid':sum(x['valid'] for x in a)}
        for metric in ('rps','p50_us','p99_us','cpu_us_per_scheduled_rpc'):
            for stage,values in [('before',b),('after',a)]:
                usable=len(values)==3 and all(x['valid'] and x[metric] is not None for x in values)
                r[stage+'_'+metric]=statistics.median(x[metric] for x in values) if usable else None
                r[stage+'_'+metric+'_min']=min(x[metric] for x in values) if usable else None
                r[stage+'_'+metric+'_max']=max(x[metric] for x in values) if usable else None
            bv,av=r['before_'+metric],r['after_'+metric]
            r[metric+'_change_pct']=(av/bv-1)*100 if bv and av is not None else None
        rows.append(r)
with (root/'comparison.csv').open('w') as f:
    out=csv.DictWriter(f,fieldnames=rows[0].keys());out.writeheader();out.writerows(rows)
(root/'comparison.json').write_text(json.dumps(rows,indent=2))
for r in rows: print(r)

metrics={}
for p in (root/'raw').glob('*-clean/metrics-after.txt'):
    values=collections.defaultdict(list)
    for line in p.read_text().splitlines():
        if line.startswith('dmesh_') and '{' not in line:
            fields=line.split()
            if len(fields)==2: values[fields[0]].append(float(fields[1]))
    metrics[p.parent.name]={k:{'workers':len(v),'sum':sum(v)} for k,v in values.items() if k.startswith('dmesh_tx_') or k in ('dmesh_sessions_active','dmesh_tasks_live','dmesh_registrations_pending','dmesh_worker_dma_tasks_inflight','dmesh_worker_ack_release_depth','dmesh_worker_completion_queue_depth','dmesh_worker_cross_queue_depth','dmesh_worker_stalled_connections','dmesh_worker_emit_pending','dmesh_worker_ack_retry_pending','dmesh_worker_remote_fin_pending')}
(root/'metrics-summary.json').write_text(json.dumps(metrics,indent=2))
