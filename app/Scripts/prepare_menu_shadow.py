"""Pause menu: no "You made changes but did not apply them" when nothing changed (GTA-96).

Every tab change in the pause menu asks CPauseMenu::DirtyGfxSettings whether the graphics the menu shows
differ from the ones in use, and if so stops with "You made changes but did not apply them. Would you like
to apply them now?". The menu's lowest shadow quality is Normal (setting 1), while the app's lower presets
run shadows at Low or Off (0 or -1, from -shadowQuality), so the two never matched and the question came up
on every tab change; "Yes" then raised the shadows and re-applied the graphics. A shadow quality below the
menu's lowest choice now counts as that choice in the check (Source/EngineHost.c, pg_menu_shadow_floor);
the settings in use stay as they are.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME = 'CPauseMenu::DirtyGfxSettings()'
NUMBER = fn(NAME)
RESOLUTION = fn('CSettingsManager::GetResolutionIndex()')
ANCHOR = f'  var_i0 = w2c_game_f{RESOLUTION}(instance);\n'
MARK = '  pg_menu_shadow_floor(instance, var_l0);  /* GTA-96: the menu has no shadow quality below Normal */\n'
DECLARATION = 'extern void pg_menu_shadow_floor(w2c_game*, u64);\n'

done = False
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    found = re.search(r'^u32 w2c_game_f' + str(NUMBER) + r'\(w2c_game\* instance\) \{$', s, re.M)
    if not found: continue
    end = s.index('\n}\n', found.start())
    body = s[found.start():end]
    done = True
    if MARK in body: print(f'menu shadow: f{NUMBER} already patched'); break
    # The frame (var_l0) holds the settings in use at +576 and the menu's at +128; shadow quality is +16 in each.
    if body.count(ANCHOR) != 1 or 'checked_add_u64(var_j0, 592u)' not in body or 'checked_add_u64(var_j1, 144u)' not in body:
        raise RuntimeError(f'Pinned f{NUMBER} ({NAME}) changed')
    s = s[:found.start()] + body.replace(ANCHOR, MARK + ANCHOR, 1) + s[end:]
    if DECLARATION not in s:
        header = re.search(r'^#include "game-[\w-]*impl\.h"\n', s, re.M)
        if not header: raise RuntimeError(f'{path.name}: no engine header include')
        s = s[:header.end()] + DECLARATION + s[header.end():]
    path.write_text(s)
    print(f'menu shadow: f{NUMBER} patched')
    break
if not done: raise RuntimeError(f'Pinned function f{NUMBER} ({NAME}) not found')
