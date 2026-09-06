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
colors={'batch':'#607d8b','lean':'#007c91'}
x=np.arange(len(cap))
for ax,metric,title,scale in [(axes[0,0],'rps','Capacity throughput (kRPC/s), higher is better',1000),(axes[0,1],'cpu_us_per_scheduled_rpc','Capacity worker CPU (us/RPC), lower is better',1)]:
 for arm,offset in [('batch',-.18),('lean',.18)]:
  y=np.array([float(r[arm+'_'+metric])/scale for r in cap]);lo=np.array([float(r[arm+'_'+metric+'_min'])/scale for r in cap]);hi=np.array([float(r[arm+'_'+metric+'_max'])/scale for r in cap])
  ax.bar(x+offset,y,.34,color=colors[arm],label=arm,yerr=[y-lo,hi-y],capsize=3)
 ax.set_title(title);ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
axes[0,0].legend(frameon=False)
ax=axes[1,0]
for mode,items,offset,color in [('capacity',cap,-.07,'#a8661e'),('matched rate',matched,.07,'#007c91')]:
 y=[float(r['cpu_us_per_scheduled_rpc_change_percent']) for r in items]
 ax.scatter(x+offset,y,color=color,label=mode,s=45)
 for xi,yi in zip(x+offset,y):ax.annotate(f'{yi:+.1f}%',(xi,yi),xytext=(0,7 if mode=='capacity' else -13),textcoords='offset points',ha='center',fontsize=8)
ax.axhline(0,color='#888',lw=1);ax.set_title('Worker CPU/RPC change, lean vs batch (%), lower is better');ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.legend(frameon=False)
ax=axes[1,1]
for mode,items,offset,color in [('capacity',cap,-.07,'#a8661e'),('matched rate',matched,.07,'#007c91')]:
 y=[float(r['p99_us_change_percent']) for r in items]
 ax.scatter(x+offset,y,color=color,label=mode,s=45)
 for xi,yi in zip(x+offset,y):ax.annotate(f'{yi:+.1f}%',(xi,yi),xytext=(0,7 if mode=='capacity' else -13),textcoords='offset points',ha='center',fontsize=8)
ax.axhline(0,color='#888',lw=1);ax.set_title('p99 change, lean vs batch (%), lower is better');ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.legend(frameon=False)
fig.suptitle('Fixed-overhead TX (lean) vs arena batching (batch) — same BlueField, N/K/A = 32/8/8\nClean runs: medians and min–max of 3 repeats; profile/trace/PMU runs excluded from performance results',fontsize=14)
fig.savefig(ROOT/'comparison.png',dpi=180);fig.savefig(ROOT/'comparison.svg')
