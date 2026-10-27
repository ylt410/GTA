"""The pause menu takes input after 2 s even if its "ready" call hasn't come (GTA-22; pg_pause_input_ready in Source/EngineHost.c).

CPauseMenu::UpdateInput handles nothing until a byte at 19566904 is 1, which only the menu's
ActionScript sets ("ready to render") through CScaleformMgr::ExecuteCallbackMethod. Here that call
came 15-25 s after opening (the map pane waits for map images to stream), so Start, B and the D-pad
did nothing and players thought the game had frozen. The menu itself is built within a frame or two,
so after 2 s its input runs anyway. CPauseMenu::Open notes the time (pg_pause_opened).
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
UPDATE_INPUT = fn('CPauseMenu::UpdateInput(bool)')
OPEN = fn('CPauseMenu::Open(unsigned int, bool, eMenuScreen, bool)')
LOAD = '  var_i0 = i32_load8_u_shared(wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, (u64)(var_j0));\n'
READY = '  var_j0 = 19566904ull;\n' + LOAD
PROLOGUE = '  (void)wasm_rt_local_memory_size;\n'
DECLARATIONS = 'extern u32 pg_pause_input_ready(u32 ready);\nextern void pg_pause_opened(void);'

def body_range(s, head):
    start = s.index(head)
    return start, s.index('\n}\n', start) + 3

def source(function):
    return next(p for p in sorted((root / 'Generated').glob('game_*.c')) if re.search(rf'^\w+ w2c_game_f{function}\(w2c_game\* instance', p.read_text(), re.M))

for function, change in ((UPDATE_INPUT, lambda body: body.replace(READY, READY + '  var_i0 = pg_pause_input_ready(var_i0);\n') if body.count(READY) >= 1 else None),
                         (OPEN, lambda body: body.replace(PROLOGUE, PROLOGUE + '  pg_pause_opened();\n', 1) if PROLOGUE in body else None)):
    path = source(function)
    s = path.read_text()
    head = re.search(rf'^\w+ w2c_game_f{function}\(w2c_game\* instance[^\n]*\{{$', s, re.M).group(0)
    start, end = body_range(s, head)
    body = s[start:end]
    if 'pg_pause_input_ready(' in body or 'pg_pause_opened(' in body: print(f'pause ready: f{function} already patched'); continue
    new = change(body)
    if new is None: raise RuntimeError(f'Pinned f{function} changed')
    s = s[:start] + new + s[end:]
    if DECLARATIONS not in s:
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + DECLARATIONS + s[first.end():]
    path.write_text(s)
    print(f'pause ready: patched f{function} in {path.name}')
