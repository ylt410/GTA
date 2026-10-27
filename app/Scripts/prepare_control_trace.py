"""Test trace of the controls scripts read (PG_TEST_CONTROLS=1; see pg_control_read in Source/EngineHost.c).

Wraps the script commands IS_CONTROL_PRESSED, GET_CONTROL_VALUE, GET_CONTROL_NORMAL and their
DISABLED_ forms, the UNBOUND_NORMAL stick reads and the JUST_PRESSED/JUST_RELEASED checks: the original becomes static and a wrapper with the engine's name reports each
answer, so a test can see which inputs a minigame polls and what the touch controls give it
(the yoga poses, GTA-37). Off by default: the host returns at once.

Seven of the script commands scripts actually call (IS_CONTROL_PRESSED, IS_CONTROL_JUST_PRESSED and others) read
the input themselves instead of calling the commands above, so those reads were never seen: the race result
screen's wait for IS_CONTROL_JUST_PRESSED 215/216 didn't show in "Prompt: scripts read inputs" (GTA-98, GTA-129).
They are wrapped as well and report through pg_control_script_read, with the control, input and answer read from
the call's arguments. That feeds the test trace and the prompt diagnostics only, not the touch layouts' poll check.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
COMMANDS = [('pad_commands::CommandIsControlPressed(int, int)', 'u32', 0), ('pad_commands::CommandGetControlValue(int, int)', 'u32', 1),
            ('pad_commands::CommandGetControlNormal(int, int)', 'f32', 2), ('pad_commands::CommandIsDisabledControlPressed(int, int)', 'u32', 3),
            ('pad_commands::CommandGetDisabledControlNormal(int, int)', 'f32', 4),
            # Minigames read analog sticks with the unbound forms and buttons with the just-pressed ones
            # (the yoga poses read both sticks this way; GTA-37): they count as polls too.
            ('pad_commands::CommandGetControlUnboundNormal(int, int)', 'f32', 2), ('pad_commands::CommandGetDisabledControlUnboundNormal(int, int)', 'f32', 4),
            ('pad_commands::CommandIsControlJustPressed(int, int)', 'u32', 0), ('pad_commands::CommandIsControlJustReleased(int, int)', 'u32', 0),
            ('pad_commands::CommandIsDisabledControlJustPressed(int, int)', 'u32', 3), ('pad_commands::CommandIsDisabledControlJustReleased(int, int)', 'u32', 3)]
DECLARATION = 'extern void pg_control_read(u32 kind, u32 control, u32 action, f32 value);'

def body_range(s, head):
    start = s.index(head)
    return start, s.index('\n}\n', start) + 3

for name, kind, tag in COMMANDS:
    function = fn(name)
    head = f'{kind} w2c_game_f{function}(w2c_game* instance, u32 var_p0, u32 var_p1) {{'
    path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if head in p.read_text())
    s = path.read_text()
    if f'pg_control_original_f{function}(' in s: print(f'control trace: f{function} already patched'); continue
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{function} ({name}) changed')
    start, end = body_range(s, head)
    original = s[start:end].replace(head, f'static {kind} pg_control_original_f{function}(w2c_game* instance, u32 var_p0, u32 var_p1) {{', 1)
    wrapper = (f'{head}\n  {kind} value = pg_control_original_f{function}(instance, var_p0, var_p1);\n'
               f'  pg_control_read({tag}, var_p0, var_p1, (f32)value);\n  return value;\n}}\n')
    s = s[:start] + original + wrapper + s[end:]
    if DECLARATION not in s:
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + DECLARATION + s[first.end():]
    path.write_text(s)
    print(f'control trace: patched f{function} in {path.name}')

SCRIPT_COMMANDS = [('IS_CONTROL_PRESSED', 'i', 0), ('IS_CONTROL_JUST_PRESSED', 'i', 0), ('IS_CONTROL_JUST_RELEASED', 'i', 0),
                   ('IS_CONTROL_RELEASED', 'i', 0), ('GET_CONTROL_VALUE', 'i', 1), ('GET_CONTROL_UNBOUND_NORMAL', 'f', 2),
                   ('GET_DISABLED_CONTROL_UNBOUND_NORMAL', 'f', 4)]
SCRIPT_DECLARATION = 'extern void pg_control_script_read(u32 kind, u32 control, u32 action, f32 value);'
MEMORY = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '

for command, answer, tag in SCRIPT_COMMANDS:
    function = fn(f'pad_commands::SetupScriptCommands()::scrWrapped_{command}::Call(rage::scrThread::Info&)')
    head = f'void w2c_game_f{function}(w2c_game* instance, u64 var_p0) {{'
    path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if head in p.read_text())
    s = path.read_text()
    if f'pg_control_script_original_f{function}(' in s: print(f'control trace: f{function} ({command}) already patched'); continue
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{function} ({command}) changed')
    start, end = body_range(s, head)
    original = s[start:end].replace(head, f'static void pg_control_script_original_f{function}(w2c_game* instance, u64 var_p0) {{', 1)
    load = 'f32_load_shared' if answer == 'f' else '(f32)(s32)i32_load_shared'
    # rage::scrThread::Info: +0 where the answer goes, +16 the arguments (8 bytes each): control, input.
    wrapper = (f'{head}\n  uint8_t* const wasm_rt_local_memory_base = NULL;\n  uint64_t wasm_rt_local_memory_size = 0;\n'
               f'  (void)wasm_rt_local_memory_base;\n  (void)wasm_rt_local_memory_size;\n'
               f'  pg_control_script_original_f{function}(instance, var_p0);\n'
               f'  u64 args = i64_load_shared({MEMORY}(u64)checked_add_u64(var_p0, 16u));\n'
               f'  u64 answer = i64_load_shared({MEMORY}(u64)(var_p0));\n'
               f'  pg_control_script_read({tag}, i32_load_shared({MEMORY}(u64)(args)), i32_load_shared({MEMORY}(u64)checked_add_u64(args, 8u)),\n'
               f'                         {load}({MEMORY}(u64)(answer)));\n}}\n')
    s = s[:start] + original + wrapper + s[end:]
    if SCRIPT_DECLARATION not in s:
        first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
        s = s[:first.end()] + '\n' + SCRIPT_DECLARATION + s[first.end():]
    path.write_text(s)
    print(f'control trace: patched f{function} ({command}) in {path.name}')
