"""Give memory the engine frees back to iOS (GTA-34; see pg_buddy_* in Source/EngineHost.c).

The engine's streaming and resource pools are buddy allocators carved out of the wasm heap. A freed
block stays dirty memory that counts against the app's limit until it is reused, so a pool keeps its
high-water mark forever (the "physical" pool held 128 MB with nothing in it). The allocator keeps its
bookkeeping in a separate node array and never reads a free block, so its pages can be handed back:

- sysMemBuddyAllocator::Free: the whole pages inside a freed block of 64 KB or more are marked
  reusable (madvise MADV_FREE_REUSABLE, as Apple's malloc does), before the block is freed.
- Allocate and TryAllocate: the pages of every block handed out are marked in use again
  (MADV_FREE_REUSE) before the caller writes to them.
- Defragment moves blocks without Allocate, so it marks the allocator's whole range in use first.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
ALLOCATE = fn('rage::sysMemBuddyAllocator::Allocate(unsigned long, unsigned long, int)')
TRY_ALLOCATE = fn('rage::sysMemBuddyAllocator::TryAllocate(unsigned long, unsigned long, int)')
FREE = fn('rage::sysMemBuddyAllocator::Free(void const*)')
DEFRAGMENT = fn('rage::sysMemBuddyAllocator::Defragment(rage::sysMemDefragmentation&, rage::sysMemDefragmentationFree&, unsigned long)')
PROLOGUE = '  (void)wasm_rt_local_memory_size;\n'
DECLARATIONS = ('extern void pg_buddy_free(w2c_game *instance, u64 allocator, u64 block);\n'
                'extern void pg_buddy_allocated(w2c_game *instance, u64 block, u64 size);\n'
                'extern void pg_buddy_defragment(w2c_game *instance, u64 allocator);')

def body_range(s, head):
    start = s.index(head)
    return start, s.index('\n}\n', start) + 3

path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if f' w2c_game_f{FREE}(w2c_game* instance' in p.read_text())
s = path.read_text()
if 'pg_buddy_free(' in s:
    print('heap return: already patched')
    raise SystemExit
# Allocate and TryAllocate: the original becomes static, a wrapper with the engine's name marks the block in use.
for function in (ALLOCATE, TRY_ALLOCATE):
    head = f'u64 w2c_game_f{function}(w2c_game* instance, u64 var_p0, u64 var_p1, u64 var_p2, u32 var_p3) {{'
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{function} (buddy allocate) changed')
    start, end = body_range(s, head)
    original = s[start:end].replace(head, f'static u64 pg_buddy_original_f{function}(w2c_game* instance, u64 var_p0, u64 var_p1, u64 var_p2, u32 var_p3) {{', 1)
    wrapper = (f'{head}\n  u64 block = pg_buddy_original_f{function}(instance, var_p0, var_p1, var_p2, var_p3);\n'
               f'  pg_buddy_allocated(instance, block, var_p1);\n  return block;\n}}\n')
    s = s[:start] + original + wrapper + s[end:]
for function, head, call in (
        (FREE, f'void w2c_game_f{FREE}(w2c_game* instance, u64 var_p0, u64 var_p1) {{', '  pg_buddy_free(instance, var_p0, var_p1);\n'),
        (DEFRAGMENT, f'u32 w2c_game_f{DEFRAGMENT}(w2c_game* instance, u64 var_p0, u64 var_p1, u64 var_p2, u64 var_p3) {{', '  pg_buddy_defragment(instance, var_p0);\n')):
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{function} changed')
    start, end = body_range(s, head)
    body = s[start:end]
    if body.count(PROLOGUE) < 1: raise RuntimeError(f'Pinned f{function} prologue changed')
    s = s[:start] + body.replace(PROLOGUE, PROLOGUE + call, 1) + s[end:]
first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
s = s[:first.end()] + '\n' + DECLARATIONS + s[first.end():]
path.write_text(s)
print(f'heap return: patched {path.name}')
