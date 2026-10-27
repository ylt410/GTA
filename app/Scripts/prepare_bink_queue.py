"""Drop video commands nobody runs (GTA-72).

bwMovie sends its work (open, play, stop) through a 32-entry queue to the AsyncBink thread. This build
skips that thread (no Bink here, and the game files have no .bik videos), so nothing ever takes from the
queue: every TV switched on or channel changed adds an entry, and once 32 are waiting the game stops with
"Fatal Error: bwMovie command queue flood detected!" (player logs on v1.4: TVs and the drunk scene, from
a 4 GB iPhone 13 to a 17 Pro Max). The queue's Push now returns at once: the commands would never have
run, and what waits for videos already gives up on its own (OPS-52).
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'rage::sysMessageQueue<rage::bwMovie::sAsyncData, 32, true>::Push(rage::bwMovie::sAsyncData const&)'
NUMBER = fn(NAME)
MARK = '  if (!pg_bink_queue_kept()) goto var_Bfunc;  /* GTA-72: no AsyncBink thread reads this queue */\n'
DECLARATION = 'extern u32 pg_bink_queue_kept(void);\n'

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^void w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance, u64 var_p0\) \{$', s, re.M)
    if not found: continue
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    done = True
    if MARK in body: print(f'bink queue: f{NUMBER} already patched'); break
    anchor = '  u64 var_j0, var_j1, var_j2;\n'
    if body.count(anchor) != 1 or 'var_Bfunc:;' not in body or 'i32_atomic_load_shared' not in body:
        raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(anchor, anchor + MARK, 1) + s[end:]
    if DECLARATION not in s:
        header = re.search(r'^#include "game-[\w-]*impl\.h"\n', s, re.M)
        if not header: raise RuntimeError(f'{path.name}: no engine header include')
        s = s[:header.end()] + DECLARATION + s[header.end():]
    path.write_text(s)
    print(f'bink queue: f{NUMBER} patched')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
