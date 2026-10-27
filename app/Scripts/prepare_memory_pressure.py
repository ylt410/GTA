"""Hook the pinned engine's available-memory query into the native host budget."""
from pathlib import Path
from engine_names import fn
root=Path(__file__).resolve().parents[1]
def source(head):
    for path in sorted((root/'Generated').glob('game_*.c')):
        if head in path.read_text():return path
    raise RuntimeError('Pinned function not found: '+head)
head='u64 w2c_game_f%d(w2c_game* instance) {'%fn('rage::sysMemTotalFreeMemory()')
p=source(head)
s=p.read_text()
start=s.index(head)
end=s.index('\n}\n',start)
f=s[start:end]
old='  FUNC_EPILOGUE;\n  return var_j0;'
new='  var_j0 = pg_streaming_available_memory(var_j0);\n'+old
if new not in f:
    if f.count(old)!=1:raise RuntimeError('Pinned available-memory query changed')
    f=f.replace(old,new)
    s=s[:start]+f+s[end:]
s=s.replace('#include "game-impl.h"','#include "game-memory-budget-impl.h"',1)
header=root/'Generated/game-memory-budget-impl.h'
header.write_text('#include "game-impl.h"\nextern uint64_t pg_streaming_available_memory(uint64_t engine_available);\n')
if p.read_text()!=s:p.write_text(s)

# The streaming allocator also has a separate video/resource budget. Its
# ordinary path can bypass the system-memory query above; clamp both answers.
head='u64 w2c_game_f%d(w2c_game* instance, u64 var_p0) {'%fn('rage::grcResourceCache::GetStreamingMemory() const')
p=source(head)
s=p.read_text()
start=s.index(head)
end=s.index('\n}\n',start)
f=s[start:end]
# This query is a signed remaining budget: a negative answer requests eviction.
f=f.replace('pg_streaming_available_memory(var_j0)', 'pg_streaming_resource_memory(var_j0)')
resource_new='  var_j0 = pg_streaming_resource_memory(var_j0);\n'+old
if resource_new not in f:
    if f.count(old)!=1:raise RuntimeError('Pinned streaming-memory query changed')
    f=f.replace(old,resource_new)
    s=s[:start]+f+s[end:]
else:
    s=s[:start]+f+s[end:]
declaration='#include "game-memory-budget-decl.h"'
if declaration not in s:
    anchor='#include "game-heap-impl.h"'
    if anchor not in s:raise RuntimeError('Run prepare_heap_diagnostics first')
    s=s.replace(anchor,anchor+'\n'+declaration,1)
(root/'Generated/game-memory-budget-decl.h').write_text('#include <stdint.h>\nextern uint64_t pg_streaming_resource_memory(uint64_t engine_available);\n')
if p.read_text()!=s:p.write_text(s)
