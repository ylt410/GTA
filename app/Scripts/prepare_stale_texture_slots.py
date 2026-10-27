"""Textures freed while a shader slot still points at them (GTA-20, GTA-24; see pg_texture_freed in Source/EngineHost.c).

grcEffect_Technique_Pass::Bind writes the textures of each draw into six per-stage slot arrays of 42
pointers (12629008...12631024), and grcDevice::SetProgramResources reads them back and calls each
texture's GetTextureView (vtable +192). Nothing clears a slot when its texture goes away, so after a
device restore (a resolution change) or a level change, a slot that a later shader reads without
Bind refilling it can point at freed memory: the call through its vtable traps as "Invalid
call_indirect" with a garbage index (players' logs show 0, -24 and 0x75746165, which is text).

- The texture and render target destructors, grcTextureDX11::DeleteResources and
  grcRenderTargetDX11::DestroyInternalData clear every slot that holds the object (pg_texture_freed).
- In SetProgramResources, a texture whose vtable lies outside memory, or whose GetTextureView entry
  is not a function of that type, is skipped like an empty slot (pg_texture_view_bad counts it), so a
  slot this misses can't stop the game.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
SET_RESOURCES = fn('rage::grcDevice::SetProgramResources()')
FREES = [fn('rage::grcTextureDX11::~grcTextureDX11()'), fn('rage::grcTextureDX11::DeleteResources()'),
         fn('rage::grcRenderTargetDX11::~grcRenderTargetDX11()'), fn('rage::grcRenderTargetDX11::DestroyInternalData()')]
PROLOGUE = '  (void)wasm_rt_local_memory_size;\n'
LOAD = 'i64_load_shared(wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
VIEW = ('    var_j1 = var_l2;\n    var_j1 = ' + LOAD + '(u64)(var_j1));\n'
        '    var_j1 = ' + LOAD + '(u64)checked_add_u64(var_j1, 192u));\n'
        '    var_j0 = CALL_INDIRECT(instance->w2c_0x5F_indirect_function_table, u64 (*)(void*, u64), w2c_game_t0, var_j1, ')
GUARD = '    if (pg_texture_view_bad(instance, var_l2)) goto var_B20;\n'
TABLE = 'instance->w2c_0x5F_indirect_function_table'
# Right before the call: an entry that isn't a GetTextureView-shaped function is skipped too.
CALL_GUARD = (f'    if (var_j1 >= {TABLE}.size || {TABLE}.data[var_j1].func_type != w2c_game_t0) {{ pg_texture_view_bad(instance, 0); goto var_B20; }}\n')
DECLARATIONS = ('extern void pg_texture_freed(w2c_game *instance, u64 texture);\n'
                'extern u32 pg_texture_view_bad(w2c_game *instance, u64 texture);')

def body_range(s, head):
    start = s.index(head)
    return start, s.index('\n}\n', start) + 3

path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if f' w2c_game_f{SET_RESOURCES}(w2c_game* instance' in p.read_text())
s = path.read_text()
if 'pg_texture_view_bad(' in s:
    print('stale texture slots: already patched')
    raise SystemExit
head = next(h for h in (f'void w2c_game_f{SET_RESOURCES}(w2c_game* instance) {{', f'u32 w2c_game_f{SET_RESOURCES}(w2c_game* instance) {{') if h in s)
start, end = body_range(s, head)
body = s[start:end]
if body.count(VIEW) != 1 or 'var_B20:;' not in body: raise RuntimeError(f'Pinned f{SET_RESOURCES} (GetTextureView call) changed')
CALL = '    var_j0 = CALL_INDIRECT(instance->w2c_0x5F_indirect_function_table, u64 (*)(void*, u64), w2c_game_t0, var_j1, '
s = s[:start] + body.replace(VIEW, GUARD + VIEW.replace(CALL, CALL_GUARD + CALL), 1) + s[end:]
for function in FREES:
    head = next((h for h in (f'u64 w2c_game_f{function}(w2c_game* instance, u64 var_p0) {{', f'void w2c_game_f{function}(w2c_game* instance, u64 var_p0) {{') if h in s), None)
    if not head: raise RuntimeError(f'Pinned f{function} (texture free) not in {path.name}')
    start, end = body_range(s, head)
    body = s[start:end]
    if PROLOGUE not in body: raise RuntimeError(f'Pinned f{function} prologue changed')
    s = s[:start] + body.replace(PROLOGUE, PROLOGUE + '  pg_texture_freed(instance, var_p0);\n', 1) + s[end:]
first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
s = s[:first.end()] + '\n' + DECLARATIONS + s[first.end():]
path.write_text(s)
print(f'stale texture slots: patched {path.name}')
