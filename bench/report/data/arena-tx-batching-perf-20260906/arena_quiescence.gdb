set pagination off
set print thread-events off
set language c
python
import gdb, json
layout=json.load(open('/tmp/arena-tx-batching-perf-20260906/arena-offsets.json'))
seen=set()
inferior=gdb.selected_inferior()
def pointer(addr):
    return int.from_bytes(bytes(inferior.read_memory(addr,8)),'little')
def count(head):
    n=0
    while head:
        if head in seen: raise RuntimeError('duplicate/cyclic free chunk')
        seen.add(head); n+=1
        if n>layout['pool_chunks']: raise RuntimeError('free chunk count exceeds pool')
        head=pointer(head) # struct px_chunk.next is the first field
    return n
px=0
mags={}
for t in inferior.threads():
    t.switch()
    cur=int(gdb.parse_and_eval('(unsigned long)px_cur_worker'))
    if cur: px=pointer(pointer(cur+layout['worker_objs'])+layout['objs_proxy'])
    head=int(gdb.parse_and_eval('(unsigned long)tls_chunk_mag'))
    n=count(head)
    declared=int(gdb.parse_and_eval('(int)tls_chunk_mag_n'))
    if n!=declared: raise RuntimeError('magazine count mismatch')
    if n: mags[str(t.ptid[1])]=n
if not px: raise RuntimeError('no proxy worker found')
shared=count(pointer(px+layout['proxy_chunk_free']))
result={'pool_chunks':layout['pool_chunks'],'shared_free':shared,'thread_magazines':mags,'free_total':len(seen),'live_chunks':layout['pool_chunks']-len(seen)}
print('ARENA_QUIESCENCE '+json.dumps(result,sort_keys=True))
if len(seen)!=layout['pool_chunks']: raise RuntimeError('arena chunks remain owned')
end
detach
quit
