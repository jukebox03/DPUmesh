#!/usr/bin/env python3
"""Record fixed-load samples without dropping failed benchmark responses."""
import argparse
import json
import subprocess
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('output',type=Path)
parser.add_argument('--repeats',type=int,default=3)
parser.add_argument('--duration',type=int,default=5)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
cases=[('latency64',64,1,1),('bandwidth8192',8192,32,1),('rate8',32,32,8)]
records=[]
for repeat in range(args.repeats):
 for name,request,concurrency,threads in cases:
  command=['bash',str(root/'bench/bench.sh'),'point',str(request),'8',str(concurrency),str(args.duration),'1000',str(threads)]
  result=subprocess.run(command,cwd=root,text=True,capture_output=True,timeout=args.duration+100)
  raw=result.stdout.strip()
  (args.output/f'{name}-{repeat+1}.txt').write_text(raw+'\n'+result.stderr)
  fields={}
  for token in raw.split():
   if '=' in token:
    k,v=token.split('=',1);fields[k]=v
  valid=result.returncode==0 and raw.startswith('OK ') and all(fields.get(k)=='0' for k in ('fail','drops','worker_fail','overflow'))
  record={'case':name,'repeat':repeat+1,'valid':valid,'fields':fields,'raw':raw}
  records.append(record)
  (args.output/'samples.json').write_text(json.dumps(records,indent=2))
  print(f'{name} repeat={repeat+1} valid={valid} p50={fields.get("p50")} p99={fields.get("p99")} mrps={fields.get("mrps")} gbps={fields.get("gbps")}',flush=True)
