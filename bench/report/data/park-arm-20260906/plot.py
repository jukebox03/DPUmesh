#!/usr/bin/env python3
import csv
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
ROOT=Path(__file__).resolve().parent
rows=list(csv.DictReader((ROOT/'comparison3.csv').open()))
cap=[r for r in rows if r['mode']=='capacity'];matched=[r for r in rows if r['mode']=='matched']
labels=[('gRPC ' if r['protocol']=='grpc' else 'opaque ')+{64:'64B',1024:'1KiB',8192:'8KiB',65536:'64KiB'}[int(r['size'])] for r in cap]
arms=['lean','lean2','lean3'];colors={'lean':'#607d8b','lean2':'#4f9fb0','lean3':'#007c91'}
fig,axes=plt.subplots(2,2,figsize=(13,8),constrained_layout=True)
x=np.arange(len(cap))
for ax,metric,title,scale in [(axes[0,0],'rps','Capacity throughput (kRPC/s), higher is better',1000),(axes[0,1],'cpu','Capacity worker CPU (us/RPC), lower is better',1)]:
 for i,arm in enumerate(arms):
  y=np.array([float(r[f'{arm}_{metric}'])/scale for r in cap])
  lo=np.array([float(r[f'{arm}_{metric}_min'])/scale for r in cap]);hi=np.array([float(r[f'{arm}_{metric}_max'])/scale for r in cap])
  ax.bar(x+(i-1)*.26,y,.24,color=colors[arm],label=arm,yerr=[y-lo,hi-y],capsize=2)
 ax.set_title(title);ax.set_xticks(x,labels,rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True)
axes[0,0].legend(frameon=False)
for ax,items,title in [(axes[1,0],cap,'CPU/RPC change vs lean (%), capacity'),(axes[1,1],matched,'CPU/RPC change vs lean (%), matched rate')]:
 for arm,offset,color in [('lean2',-.07,colors['lean2']),('lean3',.07,colors['lean3'])]:
  y=[(float(r[f'{arm}_cpu'])/float(r['lean_cpu'])-1)*100 for r in items]
  ax.scatter(np.arange(len(items))+offset,y,color=color,label=arm,s=45)
  for xi,yi in zip(np.arange(len(items))+offset,y):ax.annotate(f'{yi:+.1f}%',(xi,yi),xytext=(0,7 if arm=='lean3' else -13),textcoords='offset points',ha='center',fontsize=8)
 ax.axhline(0,color='#888',lw=1);ax.set_title(title);ax.set_xticks(np.arange(len(items)),[('gRPC ' if r['protocol']=='grpc' else 'opaque ')+{64:'64B',1024:'1KiB',8192:'8KiB',65536:'64KiB'}[int(r['size'])] for r in items],rotation=25,ha='right');ax.grid(axis='y',alpha=.2);ax.legend(frameon=False)
fig.suptitle('Driver park/arm cost: lean → lean2 (no driver poll per pass) → lean3 (+pinned timer, selective clears)\nSame BlueField, N/K/A = 32/8/8; medians and min–max of 3 clean repeats',fontsize=13)
fig.savefig(ROOT/'comparison.png',dpi=180);fig.savefig(ROOT/'comparison.svg')
