"""Script mods (.asi) run on the game's script thread (GTA-69).

Script Hook V runs its scripts inside the game's own script update. Here rage::scrThread::Run(int),
which runs each game script once per frame on the main game thread, is wrapped: before the game's
"main" script runs, pg_asi_script_tick(instance, thread) gives the mods their turn. (The controller
update thread, where the touch controls' per-frame hook runs, works alongside the main thread; game
commands that change the world deadlocked there.)
"""
from pathlib import Path
from engine_names import fn
root = Path(__file__).resolve().parents[1]
function = fn('rage::scrThread::Run(int)')
head = f'u32 w2c_game_f{function}(w2c_game* instance, u64 var_p0, u32 var_p1) {{'
DECLARATION = 'extern void pg_asi_script_tick(w2c_game *instance, u64 thread);'
path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if head in p.read_text())
s = path.read_text()
if f'pg_script_run_original_f{function}(' in s:
    print(f'asi script tick: f{function} already patched')
else:
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{function} (scrThread::Run) changed')
    start = s.index(head); end = s.index('\n}\n', start) + 3
    original = s[start:end].replace(head, f'static u32 pg_script_run_original_f{function}(w2c_game* instance, u64 var_p0, u32 var_p1) {{', 1)
    wrapper = (f'{head}\n  pg_asi_script_tick(instance, var_p0);\n'
               f'  return pg_script_run_original_f{function}(instance, var_p0, var_p1);\n}}\n')
    s = s[:start] + original + wrapper + s[end:]
    if DECLARATION not in s:
        first = s.index('\n', s.index('#include')) + 1
        s = s[:first] + DECLARATION + '\n' + s[first:]
    path.write_text(s)
    print(f'asi script tick: patched f{function} in {path.name}')
