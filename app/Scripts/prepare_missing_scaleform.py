"""Give scripts a blank stand-in for Scaleform movies the game files don't have (GTA-43).

The pinned scripts are newer than the game files. The barber (hairdo_shop_sp) asks for
COLOUR_SWITCHER_01/02, which aren't there: CScaleformMgr::CreateMovie fails, REQUEST_SCALEFORM_MOVIE
answers 0, a retry got a slot that never loaded, and the shop timed out after a few seconds and dropped
back to "press to interact" without its menu. The script waits for a real handle, so answering
"loaded" for movie 0 isn't enough.

graphics_commands::CommandRequestScaleformMovieCommon now remembers its flags on entry
(pg_scaleform_request_begin), and when CreateMovie fails, pg_scaleform_missing logs the name, rewrites
the script's name string to midsized_message (in every game version, blank until a script asks it to
show a message) and requests that instead. The script gets a movie that loads; its colour switcher
calls mean nothing to that movie, so nothing is drawn, and the menu opens.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
REQUEST = str(fn('graphics_commands::CommandRequestScaleformMovieCommon(char const*, bool, bool)'))
SIGNATURE = f'u32 w2c_game_f{REQUEST}(w2c_game* instance, u64 var_p0, u32 var_p1, u32 var_p2)'
ENTRY = '  var_j0 = instance->w2c_g0;\n  var_j1 = 160ull;\n'
FAILED = '  if (var_i0) {\n    var_i0 = 0u;\n    var_p2 = var_i0;\n    goto var_B2;\n  }\n'
# Earlier versions of this patch (stand-in-less), upgraded in place.
OLD_ENTRY = '  if (pg_scaleform_known_missing(instance, var_p0)) { FUNC_EPILOGUE; return 0u; }\n'
OLD_FAILED = '  if (var_i0) {\n    pg_scaleform_missing(instance, var_p0);\n    var_i0 = 0u;\n    var_p2 = var_i0;\n    goto var_B2;\n  }\n'
NEW_ENTRY = '  pg_scaleform_request_begin(instance, var_p0, var_p1, var_p2);\n'
NEW_FAILED = '  if (var_i0) {\n    var_p2 = pg_scaleform_missing(instance, var_p0);\n    goto var_B2;\n  }\n'
OLD_LOADED = '  if (var_p0 == 0 && pg_scaleform_failed_recently()) { FUNC_EPILOGUE; return 1u; }\n'
done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search('^' + re.escape(SIGNATURE) + r' \{$', s, re.M)
    if not found: continue
    s = s.replace(OLD_LOADED, '')
    start, end = found.start(), s.index('\n}\n', found.start())
    body = s[start:end].replace(OLD_ENTRY, '').replace(OLD_FAILED, FAILED)
    if NEW_ENTRY not in body:
        if body.count(ENTRY) != 1 or body.count(FAILED) != 1:
            raise RuntimeError(f'Pinned f{REQUEST} (CommandRequestScaleformMovieCommon) changed')
        body = body.replace(ENTRY, NEW_ENTRY + ENTRY, 1).replace(FAILED, NEW_FAILED, 1)
    s = s[:start] + body + s[end:]
    include = '#include "game-missing-scaleform-impl.h"'
    # Another patch may already have replaced the game-impl.h include with its own header.
    if include not in s: s = re.sub(r'^#include "game-[\w-]*impl\.h"$', lambda m: m.group(0) + '\n' + include, s, count=1, flags=re.M)
    path.write_text(s)
    done = True
    break
if not done: raise RuntimeError(f'Pinned function f{REQUEST} not found')
(root / 'Generated/game-missing-scaleform-impl.h').write_text(
    '/* Included right after a game-*impl.h header (game-impl.h has no include guard). */\n'
    'extern void pg_scaleform_request_begin(w2c_game *instance, u64 name, u32 a, u32 b);\n'
    'extern u32 pg_scaleform_missing(w2c_game *instance, u64 name);\n')
print('missing scaleform: patched')
