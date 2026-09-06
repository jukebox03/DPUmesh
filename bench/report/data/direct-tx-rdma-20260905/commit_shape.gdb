set pagination off
set print thread-events off
python
import gdb,json,collections
lengths=collections.Counter()
class Commit(gdb.Breakpoint):
    def stop(self):
        n=int(gdb.parse_and_eval('$x3'))
        if n: lengths[n]+=1
        return sum(lengths.values())>=128
Commit('dmesh_l7_tx_commit')
Commit('dmesh_l7_tx_commit_remote')
end
continue
python
print('COMMIT_SHAPE '+json.dumps(dict(sorted(lengths.items()))))
end
detach
quit
