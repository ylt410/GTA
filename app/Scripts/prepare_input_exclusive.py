"""Race result screens take Continue and Retry again (GTA-98: the Stock Car Race's "1st Place ... Retry X / Continue A").

The result screen makes its inputs exclusive every frame: SET_INPUT_EXCLUSIVE for 215 ENDSCREEN_ACCEPT, 216
ENDSCREEN_EXPAND and 200 FRONTEND_PAUSE_ALTERNATE, then reads 215 and 216 with IS_CONTROL_JUST_PRESSED, which
ignores disabled inputs. Making an input exclusive disables every other input that shares one of its sources
(CControl::SetInputExclusive -> CControl::DisableInputsByActiveSources). Here input 200's only source is a
keyboard key 0 (unbound) for any device, and about 70 inputs carry unbound keyboard slots of their own, 215 and
216 among them, so the third call disabled Continue and Retry along with every frontend input: A and X (touch
or controller) reached the inputs as "disabled pressed" and the screen never moved (simulator, Harness
scenarios/race_result.py). The function already skips unbound keyboard/mouse sources (device index -4, key 0);
it now skips every unbound keyboard source, so making one exclusive disables nothing. Pad buttons and real keys
keep their exclusivity (215 still disables FRONTEND_ACCEPT, which shares A).
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'CControl::DisableInputsByActiveSources(rage::atFixedArray<rage::ioSource, 192> const&, rage::ioValue::DisableOptions const&)'
NUMBER = fn(NAME)
MEMORY = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
# ioSource is {device, parameter, device index}; device 0 is IOMS_KEYBOARD, parameter 0 KEY_NULL, index -4 keyboard/mouse.
UNBOUND = (f'        var_j0 = var_l5;\n'
           f'        var_i0 = i32_load_shared({MEMORY}(u64)checked_add_u64(var_j0, 8u));\n'
           f'        var_i1 = 4294967292u;\n'
           f'        var_i0 = var_i0 == var_i1;\n'
           f'        if (var_i0) {{\n'
           f'          var_j0 = var_l5;\n'
           f'          var_i0 = i32_load_shared({MEMORY}(u64)checked_add_u64(var_j0, 4u));\n'
           f'          var_i0 = !(var_i0);\n'
           f'          if (var_i0) {{goto var_B7;}}\n')
NEW = UNBOUND.replace('        var_i0 = var_i0 == var_i1;\n',
                      f'        var_i0 = var_i0 == var_i1 || i32_load_shared({MEMORY}(u64)(var_j0)) == 0u;  /* GTA-98: any unbound keyboard source */\n', 1)

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^void w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance[^{]*\{$', s, re.M)
    if not found: continue
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    done = True
    if NEW in body: print(f'input exclusive: f{NUMBER} already patched'); break
    if body.count(UNBOUND) != 1: raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(UNBOUND, NEW, 1) + s[end:]
    path.write_text(s)
    print(f'input exclusive: f{NUMBER} patched')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
