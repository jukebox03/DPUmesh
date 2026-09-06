"""Temporary, campaign-specific uprobes. No production source edits.

ELF executable PT_LOAD offset equals vaddr in both retained binaries.
C commit's fourth AArch64 argument is len (w3). Rust entry probes count only
invocations; no assumptions about the Rust argument/return ABI are needed.
"""
import shlex
import gzip
from diagnose import RAW, remote

GROUP='dtx_20260906'
TRACE='/sys/kernel/tracing'
INSTANCE=TRACE+'/instances/'+GROUP

def events():
    return [name for name in remote(f'ls {INSTANCE}/events/{GROUP}',sudo=True).split()
            if name in ('commit','commit_remote','write','write_vectored')]

def setup(arm):
    path='/tmp/dpumesh-before-direct-tx' if arm=='before' else '/home/jukebox/DPUmesh/doca/build/dpumesh_dpu'
    symbol_path=RAW/f'{arm}-symbols.txt'
    if symbol_path.exists(): symbol_text=symbol_path.read_text()
    else:
        with gzip.open(str(symbol_path)+'.gz','rt') as f:symbol_text=f.read()
    symbols=symbol_text.splitlines()
    selected=[]
    for line in symbols:
        fields=line.split(maxsplit=2)
        if len(fields)!=3: continue
        addr,typ,symbol=fields
        name=None
        if symbol=='dmesh_l7_tx_commit': name='commit'
        elif symbol=='dmesh_l7_tx_commit_remote': name='commit_remote'
        elif symbol.startswith('<dmesh_doca::io::DmeshIo as ') and '>::poll_write::' in symbol: name='write'
        elif symbol.startswith('<dmesh_doca::io::DmeshIo as ') and '>::poll_write_vectored::' in symbol: name='write_vectored'
        if name: selected.append((name,addr))
    assert {'commit','commit_remote','write'} <= {x[0] for x in selected}
    commands=['set -eu',f'mkdir {INSTANCE}']
    for name,addr in selected:
        definition=f'p:{GROUP}/{name} {path}:0x{addr}'
        if name.startswith('commit'): definition+=' len=%x3:u32'
        commands.append(f'echo {shlex.quote(definition)} >> {TRACE}/uprobe_events')
        keys='len,common_pid' if name.startswith('commit') else 'common_pid'
        commands.append(f'echo hist:keys={keys}:size=4096:pause > {INSTANCE}/events/{GROUP}/{name}/trigger')
    try:
        remote('\n'.join(commands),sudo=True)
    except Exception:
        cleanup()
        raise

def window(seconds):
    names=events()
    commands=['set -eu']
    for name in names:
        keys='len,common_pid' if name.startswith('commit') else 'common_pid'
        commands.append(f'echo hist:keys={keys}:size=4096:continue >> {INSTANCE}/events/{GROUP}/{name}/trigger')
    commands.append(f'sleep {seconds}')
    for name in names:
        keys='len,common_pid' if name.startswith('commit') else 'common_pid'
        commands.append(f'echo hist:keys={keys}:size=4096:pause >> {INSTANCE}/events/{GROUP}/{name}/trigger')
    for name in names:
        commands.extend([f'echo EVENT={name}',f'cat {INSTANCE}/events/{GROUP}/{name}/hist'])
    return remote('\n'.join(commands),sudo=True)

def cleanup():
    # Only this campaign's instance and named events, never global trace state.
    remote(f'''if [ -d {INSTANCE} ]; then
for path in {INSTANCE}/events/{GROUP}/*/trigger; do [ ! -f "$path" ] || echo '!hist' > "$path"; done
rmdir {INSTANCE}
fi
for name in commit commit_remote write write_vectored; do
if [ -d {TRACE}/events/{GROUP}/$name ]; then echo "-:{GROUP}/$name" >> {TRACE}/uprobe_events; fi
done''',sudo=True)
