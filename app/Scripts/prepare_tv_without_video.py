"""Let scripts follow a TV show that has no video.

A TV channel plays its playlist on the game clock: CTVPlaylistManager::GetVideoInfo picks the
show that is on now and how far into it the channel is, and GET_CURRENT_TV_CLIP_NAMEHASH answers
from that. GET_CURRENT_TV_SHOW_PLAY_TIME, GET_CURRENT_TV_SHOW_DURATION and
IS_TVSHOW_CURRENTLY_PLAYING look at the show whose video is loaded instead (the cached show), and
answer 0 / false when there is none. The game files have no .bik videos and this build can't play
Bink anyway, so no show ever has a video: in Michael's mission Friend Request the TV stayed blank
and the script waited forever for the Lifeinvader presentation to reach the moment Jay Norris
takes out the phone (GTA-22).

These three now go through pg_tv_current_show (Source/EngineHost.c): the cached show when there is
one, else the channel's current show from the playlist (the one GetCurrentShowNameHash reports),
so the play time runs on with the channel and the show ends on time.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
NAME_HASH = fn('CTVPlaylistManager::GetCurrentShowNameHash()')
FUNCTIONS = {str(fn(name)): name for name in [
    'audio_commands::SetupScriptCommands()::scrWrapped_GET_CURRENT_TV_SHOW_PLAY_TIME::Call(rage::scrThread::Info&)',
    'audio_commands::SetupScriptCommands()::scrWrapped_GET_CURRENT_TV_SHOW_DURATION::Call(rage::scrThread::Info&)',
    'CTVPlaylistManager::IsTVShowCurrentlyPlaying(unsigned int)',
]}
MARK = 'pg_tv_current_show('
DECLARATION = 'extern u64 pg_tv_current_show(w2c_game *instance, u64 cached, u64 current, u64 channel);\n'
LOAD = '  var_j0 = {cached}ull;\n  var_j0 = i64_load_shared(wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, (u64)(var_j0));\n'

def body_of(s, number):
    found = re.search(r'^\S.* w2c_game_f' + str(number) + r'\(w2c_game\* instance[^;\n]*\{$', s, re.M)
    if not found: return None
    return found.start(), s.index('\n}\n', found.start())

# GetCurrentShowNameHash: "if the channel (first address) is on, the show at the second address".
channel = info = None
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    span = body_of(s, NAME_HASH)
    if not span: continue
    addresses = re.findall(r'var_j0 = (\d+)ull;', s[span[0]:span[1]])
    if len(addresses) != 2: raise RuntimeError(f'Pinned f{NAME_HASH} (GetCurrentShowNameHash) changed')
    channel, info = addresses
    break
if not channel: raise RuntimeError(f'Pinned function f{NAME_HASH} not found')

done = set()
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = original = path.read_text()
    for number, name in FUNCTIONS.items():
        span = body_of(s, number)
        if not span: continue
        start, end = span
        body = s[start:end]
        done.add(number)
        if MARK in body: continue
        # The cached show's address (CTVPlaylistManager's current cached show) is the first load.
        cached = re.search(r'  var_j0 = (\d+)ull;\n  var_j0 = i64_load_shared\(', body)
        if not cached or body.count(LOAD.format(cached=cached[1])) != 1:
            raise RuntimeError(f'Pinned f{number} ({name}) changed')
        new = f'  var_j0 = pg_tv_current_show(instance, {cached[1]}ull, {info}ull, {channel}ull);\n'
        s = s[:start] + body.replace(LOAD.format(cached=cached[1]), new, 1) + s[end:]
    if s != original:
        # Declared right after the file's engine header (other patchers may have swapped it for theirs).
        if DECLARATION not in s:
            header = re.search(r'^#include "game-[\w-]*impl\.h"\n', s, re.M)
            if not header: raise RuntimeError(f'{path.name}: no engine header include')
            s = s[:header.end()] + DECLARATION + s[header.end():]
        path.write_text(s)
missing = set(FUNCTIONS) - done
if missing: raise RuntimeError('Pinned functions not found: ' + ', '.join(f'f{f}' for f in sorted(missing)))
