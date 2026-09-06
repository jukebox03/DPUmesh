#!/usr/bin/env python3
"""Compact diagnostic comparison; bars are medians, not confidence intervals."""
import csv
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

root=Path(__file__).resolve().parent
summary=list(csv.DictReader((root/'summary.csv').open()))
traces=list(csv.DictReader((root/'trace-summary.csv').open()))
sizes=[64,1024,8192,65536];labels=['64 B','1 KiB','8 KiB','64 KiB']
def row(arm,size):
    return next(r for r in summary if r['protocol']=='grpc' and r['arm']==arm and
                int(r['size'])==size and r['mode']=='capacity' and r['kind']=='clean')
def trace(arm,size,mode):
    return next(r for r in traces if r['protocol']=='grpc' and r['arm']==arm and
                int(r['size'])==size and r['mode']==mode)
x=np.arange(4);width=.35
fig,axes=plt.subplots(1,3,figsize=(12.5,3.8),layout='constrained')
colors=['#27628c','#d17a29']
for j,(metric,label) in enumerate([('rps','Throughput'),('cpu_us_per_scheduled_rpc','CPU / RPC')]):
    vals=[100*(float(row('after',s)[metric])/float(row('before',s)[metric])-1) for s in sizes]
    bars=axes[0].bar(x+(j-.5)*width,vals,width,label=label,color=colors[j])
    axes[0].bar_label(bars,fmt='%+.1f',fontsize=8,padding=2)
axes[0].axhline(0,color='#999999',linewidth=.7)
axes[0].set(title='Clean capacity runs (3 repetitions)',ylabel='Change (%)',ylim=(-7,8))
for j,mode in enumerate(['capacity','matched']):
    vals=[float(trace('after',s,mode)['commits_per_mib'])/float(trace('before',s,mode)['commits_per_mib']) for s in sizes]
    bars=axes[1].bar(x+(j-.5)*width,vals,width,label=mode.title(),color=colors[j])
    axes[1].bar_label(bars,fmt='%.2f',fontsize=8,padding=2)
axes[1].axhline(1,color='#999999',linewidth=.7)
axes[1].set(title='Commit count per transmitted MiB',ylabel='After / before',ylim=(0,3.8))
vals=[float(row('after',s)['dmesh_tx_budget_wait_total']) for s in sizes]
bars=axes[2].bar(x,vals,color=colors[0],width=.55)
axes[2].bar_label(bars,fmt='%.0f',fontsize=9,padding=3)
axes[2].set(title='After: quota waits per 5 s run',ylabel='Wait counter delta (median)',ylim=(0,max(vals)*1.25))
for ax in axes:
    ax.set_xticks(x,labels);ax.spines[['top','right']].set_visible(False)
    ax.grid(axis='y',alpha=.15);ax.set_axisbelow(True)
axes[0].legend(frameon=False,fontsize=8);axes[1].legend(frameon=False,fontsize=8)
fig.suptitle('Direct TX diagnosis: gRPC, unchanged binaries',fontsize=13)
fig.savefig(root/'diagnosis.png',dpi=180)
fig.savefig(root/'diagnosis.svg')
