"""Skip entities that were deleted while still on a render list (GTA-18; see Source/RenderListGuard.c).

rage::fwRenderListBuilder::AddToDrawListEntityListCore walks a render list the visibility scan filled
earlier in the frame and calls each entity's draw handler. fwBasePool::Delete fills a deleted entity's
slot with 0xDD, so an entity deleted after the scan sends that call through a pointer of 0xDD bytes.
pg_render_entity_gone sees each entity before its draw handler is read and answers 1 for one that was
deleted; pg_pool_deleted and pg_render_scan_started give it the deletions and the frame they came in.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
CORE = fn('rage::fwRenderListBuilder::AddToDrawListEntityListCore(int, int, int, int, int, rage::fwRenderListBuilder::AddEntityFlags const&, unsigned int)')
DELETE = fn('rage::fwBasePool::Delete(void*)')
PRESCAN = fn('rage::fwWorldMgr::PreScan()')
PROLOGUE = '  (void)wasm_rt_local_memory_size;\n'
LOAD = 'i64_load_shared(wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
ENTITY = ('    var_j0 = var_l24;\n    var_j0 = ' + LOAD + '(u64)(var_j0));\n    var_l24 = var_j0;\n'
          '    var_j0 = ' + LOAD + '(u64)checked_add_u64(var_j0, 72u));\n')
HOOKS = {
    CORE: ('u32 w2c_game_f{}(w2c_game* instance, u32 var_p0, u32 var_p1, u32 var_p2, u32 var_p3, u32 var_p4, u64 var_p5, u32 var_p6) {{',
           ENTITY, ENTITY.replace('    var_l24 = var_j0;\n', '    var_l24 = var_j0;\n    if (pg_render_entity_gone(instance, var_j0)) goto var_B5;\n', 1),
           'extern u32 pg_render_entity_gone(w2c_game *instance, u64 entity);'),
    DELETE: ('void w2c_game_f{}(w2c_game* instance, u64 var_p0, u64 var_p1) {{', PROLOGUE,
             PROLOGUE + '  pg_pool_deleted(instance, var_p0, var_p1);\n',
             'extern void pg_pool_deleted(w2c_game *instance, u64 pool, u64 item);'),
    PRESCAN: ('void w2c_game_f{}(w2c_game* instance) {{', PROLOGUE, PROLOGUE + '  pg_render_scan_started();\n',
              'extern void pg_render_scan_started(void);'),
}
for function, (head, old, new, declaration) in HOOKS.items():
    head = head.format(function)
    for path in sorted((root / 'Generated').glob('game_*.c')):
        s = path.read_text()
        if head not in s: continue
        start = s.index(head); end = s.index('\n}\n', start); body = s[start:end]
        if new in body: print(f'render list guard: f{function} already patched'); break
        if body.count(old) < 1 or (function == CORE and body.count(old) != 1): raise RuntimeError(f'Pinned f{function} changed')
        s = s[:start] + body.replace(old, new, 1) + s[end:]
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + declaration + s[first.end():]
        path.write_text(s)
        print(f'render list guard: patched f{function} in {path.name}')
        break
    else:
        raise RuntimeError(f'f{function} not found')
