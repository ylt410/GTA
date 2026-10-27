"""No "LABEL missing" on screen for text the game files don't have.

The game's text lookup (CTextFile::Get, both overloads) answers a label that isn't in the loaded text with
"<LABEL> missing". The game files have no DLC text dictionary, so help boxes that later updates added came up
as raw labels: "HELP_DCK missing" in 59 of 510 player logs, "AM_H_DRIVEBY missing", "AM_H_GARAGEP missing",
"CAR_GAR_NO missing", "WEB_VEH_EXIT missing" and more. The answer is now empty text (Source/EngineHost.c,
pg_missing_text), so those help boxes stay away instead of showing a label nobody can read.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAMES = ['CTextFile::Get(char const*)', 'CTextFile::Get(unsigned int, char const*)']
SPRINTF = fn('char* rage::formatf<char, 64ul>(char (&) [64ul], char const*, ...)')   # what the branches format with
CALL = f'  var_j0 = w2c_game_f{SPRINTF}(instance, var_j0, var_j1, var_j2);\n'
DECLARATION = 'extern void pg_missing_text(u64);\n'

for name in NAMES:
    number = fn(name)
    done = False
    for path in sorted((root / 'Generated').glob('game_*.c')):
        s = path.read_text()
        found = re.search(r'^u64 w2c_game_f' + str(number) + r'\((w2c_game\* instance)[^)]*\) \{$', s, re.M)
        if not found: continue
        end = s.index('\n}\n', found.start())
        body = s[found.start():end]
        done = True
        if 'pg_missing_text(' in body: print(f'missing text: f{number} already patched'); break
        # The missing branch is the last formatf call, right before the shared exit; the per-thread buffer it
        # formats into (at 25482544) is the local the function returns.
        returned = re.search(r'  var_j0 = (var_l\d+);\n  FUNC_EPILOGUE;\n  return var_j0;\n', body + '\n')
        anchor = CALL + '  var_B0:;\n'
        if not returned or body.count(anchor) != 1 or '25482544ull' not in body:
            raise RuntimeError(f'Pinned f{number} ({name}) changed')
        mark = CALL + f'  pg_missing_text({returned.group(1)});  /* "<LABEL> missing" becomes empty text */\n  var_B0:;\n'
        s = s[:found.start()] + body.replace(anchor, mark, 1) + s[end:]
        if DECLARATION not in s:
            header = re.search(r'^#include "game-[\w-]*impl\.h"\n', s, re.M)
            if not header: raise RuntimeError(f'{path.name}: no engine header include')
            s = s[:header.end()] + DECLARATION + s[header.end():]
        path.write_text(s)
        print(f'missing text: f{number} patched')
        break
    if not done: raise RuntimeError(f'Pinned function f{number} ({name}) not found')
