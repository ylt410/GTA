"""Give the engine's setjmp/longjmp a native implementation.

Emscripten builds setjmp/longjmp on JavaScript exceptions: longjmp records the target (setThrew)
and calls env._emscripten_throw_longjmp, which throws; the nearest invoke_* import catches it,
restores the stack pointer and returns, and the engine code right after the invoke checks the
recorded target. Generated/Imports.c forwarded invoke_* without catching anything, and
_emscripten_throw_longjmp was unported, so the first longjmp stopped the game. libjpeg reports
every bad JPEG that way (a phone camera photo, GTA-21).

Each invoke_* now keeps a native jump point (_setjmp) on pg_invoke_top for the duration of its
call. w2c_env_0x5Femscripten_throw_longjmp (Source/EngineHost.c) returns there with _longjmp; the
invoke restores the engine's stack pointer and returns 0, as emscripten's JavaScript does.
"""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
path = root / 'Generated/Imports.c'
s = original = path.read_text()
# The unported stub: Source/EngineHost.c defines this import now (generate_imports.py leaves it out).
s = re.sub(r'void w2c_env_0x5Femscripten_throw_longjmp\(struct w2c_env\* a0\) \{\n  pg_missing_import\("env\._emscripten_throw_longjmp"\);\n\}\n\n?', '', s)
CALL = re.compile(r'^  (?P<ret>return )?(?P<call>\(\((?P<type>\w+) \(\*\)\([^)]*\)\)entry\.func\)\([^;]*\));\n', re.M)
def wrap(match):
    kind, call = match['type'], match['call']
    lines = ['  // pg: longjmp (Scripts/prepare_longjmp.py)',
             '  pg_invoke_frame frame = {.outer = pg_invoke_top, .stack = w2c_game_emscripten_stack_get_current(&a0->game)};']
    if kind == 'void':
        lines += ['  if (!_setjmp(frame.jump)) { pg_invoke_top = &frame; ' + call + '; }',
                  '  else w2c_game_0x5Femscripten_stack_restore(&a0->game, frame.stack);',
                  '  pg_invoke_top = frame.outer;']
    else:
        lines += [f'  {kind} result;',
                  '  if (!_setjmp(frame.jump)) { pg_invoke_top = &frame; result = ' + call + '; }',
                  '  else { result = 0; w2c_game_0x5Femscripten_stack_restore(&a0->game, frame.stack); }',
                  '  pg_invoke_top = frame.outer;',
                  '  return result;']
    return '\n'.join(lines) + '\n'
invokes = re.findall(r'^\w+ w2c_env_invoke_\w+\(', s, re.M)
if not invokes: raise RuntimeError('No invoke_* imports in Generated/Imports.c')
s, count = CALL.subn(wrap, s)
done = s.count('// pg: longjmp (Scripts/prepare_longjmp.py)')
if done != len(invokes): raise RuntimeError(f'Imports.c: {done} of {len(invokes)} invoke_* imports catch longjmp')
if 'w2c_env_0x5Femscripten_throw_longjmp' in s: raise RuntimeError('Imports.c still has the _emscripten_throw_longjmp stub')
if s != original: path.write_text(s)
