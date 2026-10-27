"""Two engine heaps that reserve far more of the 4 GB address space than they use can be made smaller (GTA-124).

Player logs (358 with "Engine memory held"): the Replay recorder heap (196 MB, 172 MB with @useDecreasedReplayMemory)
never held more than 2 MB, and heap 2 (220 MB, a sysMemSimpleAllocator) at most 58 MB. On 6-8 GB iPhones the engine's
address space is what runs out (GTA-70, GTA-145), so their reservations matter more than their memory.

InitGameHeap_t's constructor reserves both with sysMemVirtualAllocate and builds a sysMemSimpleAllocator over each. Both sizes
now go through pg_engine_heap_size (Source/EngineHost.c), which keeps them unless the switch is on (PG_SMALL_ENGINE_HEAPS=1,
Settings > Experimental): then Replay 16 MB and heap 2 128 MB, about 270 MB more room for the resource heap. Replay
recording never runs on the port (its threads are skipped), so the Replay heap is unused either way.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
BOOT = fn('InitGameHeap_t::InitGameHeap_t()')		# the heaps set up at startup (game_62.c)
REPLAY_RESERVE = '  var_j0 = var_i2 ? var_j0 : var_j1;\n  var_j0 = w2c_game_f2934(instance, var_j0);\n'
REPLAY_SIZE = '  var_i2 = var_i4 ? var_i2 : var_i3;\n  var_i3 = 8u;\n'
HEAP2_RESERVE = '  var_j0 = 230686720ull;\n  var_j0 = w2c_game_f2934(instance, var_j0);\n'
HEAP2_END = '    var_j2 = 230686720ull;\n    var_j1 += var_j2;\n'
HEAP2_SIZE = '    var_i2 = 230686720u;\n    var_i3 = 2u;\n'
PATCHES = [
    (REPLAY_RESERVE, '  var_j0 = var_i2 ? var_j0 : var_j1;\n  var_j0 = pg_engine_heap_size(0u, var_j0);  /* GTA-124 */\n  var_j0 = w2c_game_f2934(instance, var_j0);\n'),
    (REPLAY_SIZE, '  var_i2 = var_i4 ? var_i2 : var_i3;\n  var_i2 = (u32)pg_engine_heap_size(0u, var_i2);\n  var_i3 = 8u;\n'),
    (HEAP2_RESERVE, '  var_j0 = pg_engine_heap_size(2u, 230686720ull);  /* GTA-124 */\n  var_j0 = w2c_game_f2934(instance, var_j0);\n'),
    (HEAP2_END, '    var_j2 = pg_engine_heap_size(2u, 230686720ull);\n    var_j1 += var_j2;\n'),
    (HEAP2_SIZE, '    var_i2 = (u32)pg_engine_heap_size(2u, 230686720ull);\n    var_i3 = 2u;\n'),
]
DECLARATION = 'extern u64 pg_engine_heap_size(u32 heap, u64 size);'
if fn('rage::sysMemVirtualAllocate(unsigned long, bool)') != 2934 or fn('rage::sysMemSimpleAllocator::sysMemSimpleAllocator(void*, int, int, bool)') != 2950:
    raise RuntimeError('Pinned allocator functions changed')
path = root / 'Generated' / 'game_62.c'
s = path.read_text()
found = re.search(r'^void w2c_game_f' + str(BOOT) + r'\(w2c_game\* instance\) \{$', s, re.M)
if not found: raise RuntimeError(f'Pinned boot setup f{BOOT} not in {path.name}')
end = s.index('\n}\n', found.start())
body = s[found.start():end]
if 'pg_engine_heap_size(' in body:
    print('engine heap sizes: already patched')
    raise SystemExit
for old, new in PATCHES:
    if body.count(old) != 1: raise RuntimeError(f'Pinned f{BOOT} changed near: {old.strip().splitlines()[0]}')
    body = body.replace(old, new, 1)
s = s[:found.start()] + body + s[end:]
first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
s = s[:first.end()] + '\n' + DECLARATION + s[first.end():]
path.write_text(s)
print(f'engine heap sizes: patched f{BOOT}')
