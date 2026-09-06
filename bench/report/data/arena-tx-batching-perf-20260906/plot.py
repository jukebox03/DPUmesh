#!/usr/bin/env python3
import csv
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
ROOT=Path(__file__).resolve().parent
rows=list(csv.DictReader((ROOT/'comparison.csv').open()))
cap=[r for r in rows if r['mode']=='capacity'];matched=[r for r in rows if r['mode']=='matched']
labels=[('gRPC ' if r['protocol']=='grpc' else 'opaque ')+{64:'64B',1024:'1KiB',8192:'8KiB',65536:'64KiB'}[int(r['size'])] for r in cap]
fig,axes=plt.subplots(2,2,figsize=(13,8),constrained_layout=True)
colors={'direct':'#607d8b','batch':'#007c91'}
x=np.arange(len(cap))
for ax,metric,title,scale in [(axes[0,0],'rps','Capacity throughput (kRPC/s), higher is better',1000),(axes[0,1],'cpu_us_per_scheduled_rpc','Capacity worker CPU (us/RPC), lower is better',1)]:
 for arm,offset in [('direct',-.18),('batch',.18)]:
  y=np.array([float(r[arm+'_'+metric])/scale for r in cap]);lo=np.array([float(r[arm+'_'+metric+'_min'])/scale for r in cap]);hi=np.array([float(r[arm+'_'+metric+'_max'])/scale for r in cap])
  ax.bar(x+offset,y,.34,color=colors[arm],label=arm,yerr=[y-lo,hi-y],capsize=3)
 ax.set_title(title);ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
axes[0,0].legend(frameon=False)
ax=axes[1,0]
for mode,items,offset,color in [('capacity',cap,-.07,'#a8661e'),('matched rate',matched,.07,'#007c91')]:
 y=[float(r['p99_us_change_percent']) for r in items]
 ax.scatter(x+offset,y,color=color,label=mode,s=45)
 for xi,yi in zip(x+offset,y):ax.annotate(f'{yi:+.1f}%',(xi,yi),xytext=(0,7 if mode=='capacity' else -13),textcoords='offset points',ha='center',fontsize=8)
ax.axhline(0,color='#888',lw=1);ax.set_title('p99 change (%), lower is better');ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.legend(frameon=False)
ax=axes[1,1]
traces=list(csv.DictReader((ROOT/'trace-summary.csv').open()))
for arm,offset in [('direct',-.18),('batch',.18)]:
 y=[]
 for r in cap:
  hit=next(t for t in traces if t['arm']==arm and t['protocol']==r['protocol'] and t['size']==r['size'])
  y.append(float(hit['commits_per_mib']))
 ax.bar(x+offset,y,.34,label=arm,color=colors[arm])
ax.set_yscale('log');ax.set_title('Successful publications/MiB (separate trace runs)');ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
fig.suptitle('Arena TX batching vs direct TX — same BlueField, N/K/A = 32/8/8\nClean runs: medians and min–max of 3 repeats; traces excluded from performance results',fontsize=14)
fig.savefig(ROOT/'comparison.png',dpi=180);fig.savefig(ROOT/'comparison.svg')
