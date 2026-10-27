"""Loading a save in game no longer hangs on "Loading Story Mode" (GTA-113).

Leaving a replayed mission (Pause > Game > Replay Mission, then Exit) and Load Game both restart the game
session. Its shutdown flushes every debug net log (CAILogManager::ShutDown -> rage::netLogImpl::Flush), and
Flush waits while a log has a full buffer handed to the log spool thread (+376), poking that thread and
sleeping 5 ms each time round. This build never starts the spool thread (rage::LogSpoolFunction; its "running"
byte at 12313504 stays 0), so once a log had filled its 16 KB buffer, which a few minutes of play does, the
wait never ended: the loading screen stayed up for good (simulator: 3+ minutes with the main thread inside
netLogImpl::Flush; player logs 363, 535, 536). The wait now ends when there is no spool thread to wait for;
the buffer was only ever going to a debug log file.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'rage::netLogImpl::Flush(bool)'
NUMBER = fn(NAME)
MEMORY = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
WAIT = (f'      var_i0 = i32_atomic_load_shared({MEMORY}(u64)checked_add_u64(var_j0, 376u));\n'
        '      if (var_i0) {goto var_L4;}\n')
NEW = (f'      var_i0 = i32_atomic_load_shared({MEMORY}(u64)checked_add_u64(var_j0, 376u));\n'
       f'      if (var_i0 && i32_load8_u_shared({MEMORY}(u64)(12313504ull)) == 1) {{goto var_L4;}}  /* GTA-113: no spool thread, no wait */\n')
SPOOL = '      var_j0 = 12313504ull;\n'   # the loop pokes the spool thread only when this byte is 1

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^void w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance[^{]*\{$', s, re.M)
    if not found: continue
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    done = True
    if NEW in body: print(f'netlog flush: f{NUMBER} already patched'); break
    if body.count(WAIT) != 1 or SPOOL not in body:
        raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(WAIT, NEW, 1) + s[end:]
    path.write_text(s)
    print(f'netlog flush: f{NUMBER} patched')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
