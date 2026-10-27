"""A render phase destroyed by a device restore no longer stops the game in CGameScan::KickVisibilityJobs (GTA-89).

A live resize (a resolution change in Settings, a TV connected or gone) restores the graphics device, and
CViewportManager::DeviceReset deletes and rebuilds the render phases. One can stay in the phase list (global
11547128) with the bare rage::fwRenderPhase vtable that ~fwRenderPhase leaves; KickVisibilityJobs calls its slot +64,
past that vtable, and traps as "Invalid call_indirect" with table index 4227208 (12 player logs, v1.3.4 to v1.4.3;
the v1.4.3 render-thread flush in prepare_restore_flush.py didn't cover it: this runs on the main thread).

KickVisibilityJobs now starts with pg_drop_destroyed_phases (Source/EngineHost.c), which drops such entries from the
list, so the later loops over it (this one and the render thread's) never see them.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'CGameScan::KickVisibilityJobs()'
NUMBER = fn(NAME)
PROLOGUE = '  (void)wasm_rt_local_memory_size;\n'
CALL = '  pg_drop_destroyed_phases(instance);  /* GTA-89 */\n'
DECLARATION = 'extern void pg_drop_destroyed_phases(w2c_game *instance);'

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^void w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance\) \{$', s, re.M)
    if not found: continue
    done = True
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    if CALL in body: print(f'destroyed phases: f{NUMBER} already patched'); break
    if PROLOGUE not in body or '11547128ull' not in body: raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(PROLOGUE, PROLOGUE + CALL, 1) + s[end:]
    first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
    if DECLARATION not in s: s = s[:first.end()] + '\n' + DECLARATION + s[first.end():]
    path.write_text(s)
    print(f'destroyed phases: f{NUMBER} patched in {path.name}')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
