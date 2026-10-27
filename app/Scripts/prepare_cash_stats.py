"""Watch the characters' cash stats (and guard them, see pg_stat_set_int in EngineHost.c).

Players of other ports reported Story saves loading with every character's money at 0: right after
a save loads, a script sets SP0/SP1/SP2_TOTAL_CASH to 0. stats_commands::CommandStatSetInt is the
script command every such write goes through; pg_stat_set_int sees each write first, logs the cash
ones, and answers 1 for a write it drops.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
FUNCTION = fn('stats_commands::CommandStatSetInt(int, int, bool)')
HEAD = f'u32 w2c_game_f{FUNCTION}(w2c_game* instance, u32 var_p0, u32 var_p1, u32 var_p2) {{'
OLD = '  var_j0 = instance->w2c_g0;\n  var_j1 = 96ull;\n'
NEW = '  if (pg_stat_set_int(instance, var_p0, var_p1)) { FUNC_EPILOGUE; return 1u; }\n' + OLD
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    if HEAD not in s: continue
    start = s.index(HEAD); end = s.index('\n}\n', start); body = s[start:end]
    if 'pg_stat_set_int(' in body: print('cash stats: already patched'); break
    if body.count(OLD) != 1 or '25469128ull' not in body: raise RuntimeError(f'Pinned f{FUNCTION} (CommandStatSetInt) changed')
    s = s[:start] + body.replace(OLD, NEW, 1) + s[end:]
    first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
    s = s[:first.end()] + '\nextern u32 pg_stat_set_int(w2c_game *instance, u32 stat, u32 value);' + s[first.end():]
    path.write_text(s)
    print(f'cash stats: patched {path.name}')
    break
else:
    raise RuntimeError(f'f{FUNCTION} (CommandStatSetInt) not found')
