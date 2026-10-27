"""Container Handler: the frame always lowers again (GTA-97, report 922: "handler only goes up but not down").

The Handler's frame (CVehicleGadgetHandlerFrame, made in CVehicleFactory::Create) keeps two flags next to each
other: +154 "may raise" and +155 "may lower". The driving task passes a down push to the frame only while +155
is exactly 1 (CTaskVehiclePlayerDriveAutomobile::ProcessDriverInputsForPlayerOnUpdate); a frame contact clears
it, and it is set to 1 again only from 0 (ProcessPreComputeImpacts, ProcessControl). The constructor zeroes +152
and +153 with one 16-bit write and leaves +154 and +155 as the allocator left them, so a Handler made in reused
memory could hold 17, 54 or 255 there: never 1 (no lowering) and never 0 (never set again). Up still worked.
Seen in Scouting the Port in the simulator after mission retries (Harness: frame+155 = 54, down did nothing;
writing 1 there made it lower at once). The constructor now zeroes all four flags with one 32-bit write; the
first frame contact then sets both to 1, as with clean memory.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'CVehicleFactory::Create(rage::fwModelId, eEntityOwnedBy, unsigned int, rage::Matrix34 const*, bool, bool)'
NUMBER = fn(NAME)
VTABLE = '  var_j1 = 6108264ull;\n'   # CVehicleGadgetHandlerFrame's vtable in the pinned game.wasm
MEMORY = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
OLD = f'  i32_store16_shared({MEMORY}(u64)checked_add_u64(var_j0, 152u), var_i1);\n'
NEW = f'  i32_store_shared({MEMORY}(u64)checked_add_u64(var_j0, 152u), var_i1);  /* GTA-97: also +154 and +155 */\n'

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^u64 w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance[^{]*\{$', s, re.M)
    if not found: continue
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    done = True
    if NEW in body: print(f'handler frame: f{NUMBER} already patched'); break
    if body.count(VTABLE) != 1: raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed: Handler frame vtable')
    at = body.index(VTABLE)
    # The flags' 16-bit write is the first one at +152 after the vtable, within the same constructor.
    write = body.find(OLD, at)
    if write < 0 or write - at > 2000: raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed: Handler frame flags')
    body = body[:write] + NEW + body[write + len(OLD):]
    s = s[:found.start()] + body + s[end:]
    path.write_text(s)
    print(f'handler frame: f{NUMBER} patched')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
