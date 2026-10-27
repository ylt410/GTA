"""Streaming health (GTA-116; Source/NativeStreamingHealth.c): the engine tells the app when it requests,
loads and unloads a streamed object, so the Health line can say how long textures and map data wait.

Each of these rage::strStreamingInfoManager methods gets one call at its start, with the object's index
(the strIndex argument, var_p1 after the manager's this pointer):
    RequestObject(strIndex, int)   pg_stream_requested
    SetObjectToLoaded(strIndex)    pg_stream_loaded
    RemoveObject(strIndex, bool)   pg_stream_removed
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
HOOKS = (('rage::strStreamingInfoManager::RequestObject(rage::strIndex, int)', 'pg_stream_requested'),
         ('rage::strStreamingInfoManager::SetObjectToLoaded(rage::strIndex)', 'pg_stream_loaded'),
         ('rage::strStreamingInfoManager::RemoveObject(rage::strIndex, bool)', 'pg_stream_removed'))
PROLOGUE = '  (void)wasm_rt_local_memory_size;\n'
DECLARATIONS = '\n'.join(f'extern void {call}(u32 index);' for _, call in HOOKS)

def source(function):
    return next(p for p in sorted((root / 'Generated').glob('game_*.c')) if re.search(rf'^\w+ w2c_game_f{function}\(w2c_game\* instance', p.read_text(), re.M))

for name, call in HOOKS:
    function = fn(name)
    path = source(function)
    s = path.read_text()
    head = re.search(rf'^\w+ w2c_game_f{function}\(w2c_game\* instance, u64 var_p0, u32 var_p1[,)][^\n]*\{{$', s, re.M)
    if not head: raise RuntimeError(f'Pinned f{function} ({name}) changed: expected (this, strIndex, ...)')
    start = head.start()
    end = s.index('\n}\n', start) + 3
    body = s[start:end]
    if f'{call}(' in body: print(f'streaming health: f{function} already patched'); continue
    if PROLOGUE not in body: raise RuntimeError(f'Pinned f{function} ({name}) changed: no prologue')
    s = s[:start] + body.replace(PROLOGUE, PROLOGUE + f'  {call}(var_p1);\n', 1) + s[end:]
    if DECLARATIONS not in s:
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + DECLARATIONS + s[first.end():]
    path.write_text(s)
    print(f'streaming health: patched f{function} ({call}) in {path.name}')
