"""A stale dependency job no longer stops the game in rage::sysDependencyScheduler::DependencyWorkerThread (GTA-144).

Player logs (v1.4 to v1.4.2): "<Dependency 0> Error allocating 3722304989 bytes in heap 0" (0xDDDDDDDD), 4294967295 or
3152529250 bytes with 80-127 MB free, from operator new in the worker: a job's scratch size (+112, used when bit 0 of
+110 is set) read from a job descriptor that was already freed (RAGE's BankRelease allocator fills freed memory with
0xDD). The same stale descriptor's job function (+96) is the garbage pointer behind the worker's "Invalid
call_indirect" traps (GTA-89 group D: logs 464, 729, 797).

Before the worker touches a job it now checks it: the job function must be a function of the type the worker calls
(u32 (*)(void*, u64)), and a scratch size must be under 256 MB. A job that fails is dropped (pg_dependency_skipped
logs it) and the worker takes the next one, without touching its parents' counters: those pointers come from the
same freed memory. The job's real run already finished, which is why its memory was freed.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'rage::sysDependencyScheduler::DependencyWorkerThread(void*)'
NUMBER = fn(NAME)
LOAD = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
ANCHOR = ('    var_j0 = var_l1;\n'
          '    var_i0 = i32_load8_u_shared(' + LOAD + '(u64)checked_add_u64(var_j0, 146u));\n'
          '    var_l7 = var_i0;\n')
TABLE = 'instance->w2c_0x5F_indirect_function_table'
GUARD = ('    {  /* GTA-144: a job whose descriptor was freed (Scripts/prepare_dependency_guard.py) */\n'
         '      u64 job = i64_load_shared(' + LOAD + '(u64)checked_add_u64(var_l1, 96u));\n'
         '      u32 scratch = i32_load8_u_shared(' + LOAD + '(u64)checked_add_u64(var_l1, 110u)) & 1u;\n'
         '      u32 size = i32_load_shared(' + LOAD + '(u64)checked_add_u64(var_l1, 112u));\n'
         f'      if (job >= {TABLE}.size || {TABLE}.data[job].func_type != w2c_game_t4 || (scratch && size > 268435456u)) {{\n'
         '        pg_dependency_skipped(instance, var_l1, job, size);\n'
         '        var_l1 = 0;\n'
         '        goto var_L0;\n'
         '      }\n'
         '    }\n')
DECLARATION = 'extern void pg_dependency_skipped(w2c_game *instance, u64 job, u64 function, u32 size);'

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^void w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance, u64 var_p0\) \{$', s, re.M)
    if not found: continue
    done = True
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    if 'pg_dependency_skipped(' in body:
        print(f'dependency guard: f{NUMBER} already patched')
        break
    if body.count(ANCHOR) != 1 or 'var_L0: \n' not in body or 'w2c_game_f101(instance, var_j0, var_j1)' not in body:
        raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(ANCHOR, GUARD + ANCHOR, 1) + s[end:]
    first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
    if DECLARATION not in s: s = s[:first.end()] + '\n' + DECLARATION + s[first.end():]
    path.write_text(s)
    print(f'dependency guard: f{NUMBER} patched in {path.name}')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
