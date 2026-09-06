"""Separate return-probe windows measure successfully submitted batch sizes."""
import gzip
import shlex
from campaign import RAW,REMOTE,d
GROUP='atxb_20260906'
TRACE='/sys/kernel/tracing'
INSTANCE=TRACE+'/instances/'+GROUP

def raw_text(path):
    if path.exists():
        return path.read_text()
    with gzip.open(str(path)+'.gz', 'rt') as source:
        return source.read()

def names():
    return [name for name in d.remote(f'ls {INSTANCE}/events/{GROUP}',sudo=True).split() if name in ['commit','commit_remote']]

def setup(arm):
    wanted={'dmesh_l7_tx_commit':'commit','dmesh_l7_tx_commit_remote':'commit_remote'} if arm=='direct' else {'dmesh_l7_tx_batch_flush':'commit'}
    selected=[]
    # Map virtual symbol addresses to ELF file offsets, including non-PIE binaries.
    loads=[]
    lines=raw_text(RAW/f'{arm}-elf.txt').splitlines()
    for i,line in enumerate(lines):
        f=line.split()
        if f and f[0]=='LOAD':
            off,va=int(f[1],16),int(f[2],16)
            size=int(lines[i+1].split()[0],16)
            loads.append((va,va+size,off))
    for line in raw_text(RAW/f'{arm}-symbols.txt').splitlines():
        f=line.split(maxsplit=2)
        if len(f)!=3 or f[2] not in wanted: continue
        va=int(f[0],16)
        offset=next(off+va-start for start,end,off in loads if start<=va<end)
        selected.append((wanted[f[2]],offset))
    assert len(selected)==len(wanted),(arm,selected)
    commands=['set -eu',f'mkdir {INSTANCE}']
    for name,offset in selected:
        definition=f'r:{GROUP}/{name} {REMOTE}/{arm}:0x{offset:x} len=$retval:s32'
        commands += [f'echo {shlex.quote(definition)} >> {TRACE}/uprobe_events',
                     f'echo hist:keys=len,common_pid:size=4096:pause > {INSTANCE}/events/{GROUP}/{name}/trigger']
    try:d.remote('\n'.join(commands),sudo=True)
    except Exception:cleanup();raise

def window(seconds):
    events=names();commands=['set -eu']
    for name in events:commands.append(f'echo hist:keys=len,common_pid:size=4096:continue >> {INSTANCE}/events/{GROUP}/{name}/trigger')
    commands.append(f'sleep {seconds}')
    for name in events:commands.append(f'echo hist:keys=len,common_pid:size=4096:pause >> {INSTANCE}/events/{GROUP}/{name}/trigger')
    for name in events:commands += [f'echo EVENT={name}',f'cat {INSTANCE}/events/{GROUP}/{name}/hist']
    return d.remote('\n'.join(commands),sudo=True)

def cleanup():
    d.remote(f'''if [ -d {INSTANCE} ]; then
for path in {INSTANCE}/events/{GROUP}/*/trigger; do [ ! -f "$path" ] || echo '!hist' > "$path"; done
rmdir {INSTANCE}
fi
for name in commit commit_remote; do
if [ -d {TRACE}/events/{GROUP}/$name ]; then echo "-:{GROUP}/$name" >> {TRACE}/uprobe_events; fi
done''',sudo=True)
