"""Let the engine write into a "memory:" file whose name has a 0x pointer.

fiDevice::MakeMemoryFileName names a block of memory as "memory:$%p,size,flags:name". This
build's printf writes %p with a 0x prefix. fiDeviceMemory::Open skips the "0x" before it reads the
address, but fiDeviceMemory::Create (the same file opened for writing) does not: it reads the "0"
and stops at the "x", so every write went to address 0. A phone camera photo is written that way
(CHighQualityScreenshot::StoreScreenShotInBuffer): the JPEG landed on the engine's own data at
the start of memory instead of in the photo, and the photo's low quality copy then found no JPEG
("Not a JPEG file: starts with 0x00 0x5f") and stopped the game (GTA-21). Create now skips the
"0x" the same way Open does (pg_memory_file_address_start in Source/EngineHost.c).
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
FUNCTION = str(fn('rage::fiDeviceMemory::Create(char const*) const'))
MARK = 'pg_memory_file_address_start('
OLD = '  var_j0 = var_p1;\n  var_j1 = 8ull;\n  var_j0 += var_j1;\n  var_p0 = var_j0;\n  var_L1: \n'
NEW = '  var_j0 = var_p1;\n  var_j1 = pg_memory_file_address_start(instance, var_j0);\n  var_j0 += var_j1;\n  var_p0 = var_j0;\n  var_L1: \n'
done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^u64 w2c_game_f' + FUNCTION + r'\(w2c_game\* instance, u64 var_p0, u64 var_p1\) \{$', s, re.M)
    if not found: continue
    start, end = found.start(), s.index('\n}\n', found.start())
    body = s[start:end]
    if MARK not in body:
        # "memory:" (3032900) is compared first; the address starts after "memory:$".
        if body.count(OLD) != 1 or '3032900ull' not in body:
            raise RuntimeError(f'Pinned f{FUNCTION} (fiDeviceMemory::Create) changed')
        s = s[:start] + body.replace(OLD, NEW, 1) + s[end:]
        include = '#include "game-memory-file-impl.h"'
        if include not in s: s = s.replace('#include "game-impl.h"', include, 1)
        path.write_text(s)
    done = True
    break
if not done: raise RuntimeError(f'Pinned function f{FUNCTION} not found')
(root / 'Generated/game-memory-file-impl.h').write_text(
    '#include "game-impl.h"\n'
    'extern u64 pg_memory_file_address_start(w2c_game *instance, u64 name);\n')
