"""Turn away script requests the game files can't stream, so they can't jam script loading (GTA-102).

REQUEST_SCRIPT and REQUEST_SCRIPT_WITH_NAME_HASH ask the host first (pg_script_request_allowed and
pg_script_hash_request_allowed in Source/EngineHost.c): apptrackify's request sits on streaming device 0 and is never
turned into a read, and every later request there waits behind it, so no script loads again that session and a
respawn's flush waits for good. A refused request just isn't made.

For those scripts HAS_SCRIPT_LOADED (and its name-hash form) answers true and START_NEW_SCRIPT starts nothing
(thread 0), so a mission that opens the app itself and waits for it to load goes on (The Merryweather Heist waited
for appTrackify for good); the port-made Trackify (GTA-134) does the app's work.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
MEMORY = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
NAME = fn('script_commands::SetupScriptCommands()::scrWrapped_REQUEST_SCRIPT::Call(rage::scrThread::Info&)')
HASH = fn('script_commands::SetupScriptCommands()::scrWrapped_REQUEST_SCRIPT_WITH_NAME_HASH::Call(rage::scrThread::Info&)')
DECLARATION = 'extern u32 pg_script_request_allowed(u64 name); extern u32 pg_script_hash_request_allowed(u32 hash);'
PATCHES = [
    (NAME, f'  var_j0 = i64_load_shared({MEMORY}(u64)(var_j0));\n  w2c_game_f',
           f'  var_j0 = i64_load_shared({MEMORY}(u64)(var_j0));\n  if (!pg_script_request_allowed(var_j0)) return;  /* GTA-102 */\n  w2c_game_f'),
    (HASH, f'  var_i0 = i32_load_shared({MEMORY}(u64)(var_j0));\n  w2c_game_f',
           f'  var_i0 = i32_load_shared({MEMORY}(u64)(var_j0));\n  if (!pg_script_hash_request_allowed(var_i0)) return;  /* GTA-102 */\n  w2c_game_f'),
]
for number, old, new in PATCHES:
    head = re.compile(r'^void w2c_game_f' + str(number) + r'\(w2c_game\* instance, u64 var_p0\) \{$', re.M)
    path = next((p for p in sorted((root / 'Generated').glob('game_*.c')) if head.search(p.read_text())), None)
    if path is None: raise RuntimeError(f'Pinned function f{number} not found')
    s = path.read_text(); found = head.search(s); end = s.index('\n}\n', found.start()); body = s[found.start():end]
    if new in body: print(f'script request guard: f{number} already patched'); continue
    if body.count(old) != 1: raise RuntimeError(f'Pinned f{number} changed')
    s = s[:found.start()] + body.replace(old, new, 1) + s[end:]
    if DECLARATION not in s:
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + DECLARATION + s[first.end():]
    path.write_text(s)
    print(f'script request guard: f{number} patched')

# HAS_SCRIPT_LOADED / HAS_SCRIPT_WITH_NAME_HASH_LOADED answer true, START_NEW_SCRIPT starts nothing, for those scripts.
GUARD_DECLARATION = 'extern u32 pg_script_name_blocked(u64 name); extern u32 pg_script_hash_blocked(u32 hash);'
WRAPS = [
    (fn('script_commands::SetupScriptCommands()::scrWrapped_HAS_SCRIPT_LOADED::Call(rage::scrThread::Info&)'), 'name', 'loaded'),
    (fn('script_commands::SetupScriptCommands()::scrWrapped_HAS_SCRIPT_WITH_NAME_HASH_LOADED::Call(rage::scrThread::Info&)'), 'hash', 'loaded'),
    (fn('rage::scrThread::StartNewScript(rage::scrThread::Info&)'), 'name', 'start'),
]
for number, key, kind in WRAPS:
    head = f'void w2c_game_f{number}(w2c_game* instance, u64 var_p0) {{'
    path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if head in p.read_text())
    s = path.read_text()
    if f'pg_guard_original_f{number}(' in s: print(f'script request guard: f{number} already wrapped'); continue
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{number} changed')
    start = s.index(head); end = s.index('\n}\n', start) + 3
    original = s[start:end].replace(head, f'static void pg_guard_original_f{number}(w2c_game* instance, u64 var_p0) {{', 1)
    read = (f'i64_load_shared({MEMORY}(u64)i64_load_shared({MEMORY}(u64)checked_add_u64(var_p0, 16u)))' if key == 'name' else
            f'i32_load_shared({MEMORY}(u64)i64_load_shared({MEMORY}(u64)checked_add_u64(var_p0, 16u)))')
    test = f'pg_script_name_blocked({read})' if key == 'name' else f'pg_script_hash_blocked({read})'
    answer = '1u' if kind == 'loaded' else '0u'
    wrapper = (f'{head}\n  uint8_t* const wasm_rt_local_memory_base = NULL;\n  uint64_t wasm_rt_local_memory_size = 0;\n'
               f'  (void)wasm_rt_local_memory_base;\n  (void)wasm_rt_local_memory_size;\n'
               f'  if ({test}) {{ i32_store_shared({MEMORY}(u64)i64_load_shared({MEMORY}(u64)(var_p0)), {answer}); return; }}  /* GTA-134 */\n'
               f'  pg_guard_original_f{number}(instance, var_p0);\n}}\n')
    s = s[:start] + original + wrapper + s[end:]
    if GUARD_DECLARATION not in s:
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + GUARD_DECLARATION + s[first.end():]
    path.write_text(s)
    print(f'script request guard: f{number} wrapped ({kind})')
