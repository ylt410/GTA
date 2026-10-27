"""A resolution change during play no longer stops the game with "Invalid call_indirect" (GTA-89).

A live resize (a resolution change in Settings, a TV connected or gone) restores the graphics device on the main
thread: CApp::UpdateState takes the device-reset lock and the graphics context, then rage::grcDevice::CheckDeviceStatus
runs DeviceRestored, which frees and rebuilds render phases, render targets and constant buffers. By then the main
thread has already handed the render thread the previous frame to draw, so the render thread could be drawing with
what the restore frees. Player logs (464, 560, 613, 614, 374) end in "Invalid call_indirect" right after "FINISHED
DEVICE RESTORE": a destroyed fwRenderPhase walked by CGameScan::KickVisibilityJobs, a freed wheel constant buffer,
grcDevice::SetProgramResources.

When a restore is pending (the byte at 8323016 that CheckDeviceStatus tests), UpdateState now waits for the render
thread to finish (rage::fwRenderThreadInterface::Flush(false)) right after taking the reset lock, as the engine's own
flushes are called, and before it takes the graphics context and CheckDeviceStatus takes the device mutex: the render
thread passes through its own CheckDeviceStatus every frame and needs both. (Flushing inside CheckDeviceStatus, after
the mutex, deadlocked at once in the simulator.) Frames without a pending restore don't wait. Not reproduced in the
simulator (its faster CPU); Harness/scenarios/resize_stress.py checks that nothing hangs or stops.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'CApp::UpdateState(int, int, rage::fwFsm::Event)'
NUMBER = fn(NAME)
FLUSH = fn('rage::fwRenderThreadInterface::Flush(bool)')
LOCK = fn('rage::grcDevice::LockDeviceResetAvailability()')
CONTEXT = fn('rage::grcDevice::LockContext()')
CHECK = fn('rage::grcDevice::CheckDeviceStatus()')
MEMORY = 'wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, '
ANCHOR = f'  w2c_game_f{LOCK}(instance);\n  w2c_game_f{CONTEXT}(instance);\n  var_j0 = 0ull;\n  var_j1 = 599092ull;\n'
NEW = (f'  w2c_game_f{LOCK}(instance);\n'
       f'  if (i32_load8_u_shared({MEMORY}(u64)(8323016ull)) == 1) w2c_game_f{FLUSH}(instance, 0u);  /* GTA-89: a restore is pending */\n'
       f'  w2c_game_f{CONTEXT}(instance);\n  var_j0 = 0ull;\n  var_j1 = 599092ull;\n')

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^u32 w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance[^{]*\{$', s, re.M)
    if not found: continue
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    done = True
    if NEW in body: print(f'restore flush: f{NUMBER} already patched'); break
    at = body.find(ANCHOR)
    if body.count(ANCHOR) != 1 or f'w2c_game_f{CHECK}(instance);' not in body[at:at + 1200]:
        raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(ANCHOR, NEW, 1) + s[end:]
    path.write_text(s)
    print(f'restore flush: f{NUMBER} patched')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
