"""Decode the game's Opus audio with native libopus (Scripts/build_opus.py, Source/NativeOpus.c).

The engine's opus_decode (inlined into rage::audDecoderOpus::Update) parses each packet into frames
and calls opus_decode_frame once per frame; as translated wasm that decoder is a large share of the
audio thread. pg_opus_frame sees each call first: it decodes the frame with a native libopus decoder
kept for that engine decoder and answers 1, so the wasm body doesn't run. It answers 0 to let the
wasm decoder run (PG_NATIVE_OPUS=0, or the verify mode's own comparison call).
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
FUNCTION = fn('opus_decode_frame')
HEAD = f'u32 w2c_game_f{FUNCTION}(w2c_game* instance, u64 var_p0, u64 var_p1, u32 var_p2, u64 var_p3, u32 var_p4) {{\n'
HOOK = '  { u32 pg_r; if (pg_opus_frame(instance, var_p0, var_p1, var_p2, var_p3, var_p4, &pg_r)) return pg_r; }\n'
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    if HEAD not in s: continue
    start = s.index(HEAD); end = s.index('\n}\n', start); body = s[start:end]
    if 'pg_opus_frame(' in body: print('native Opus: already patched'); break
    if body.count('  FUNC_PROLOGUE;\n') != 1: raise RuntimeError(f'Pinned f{FUNCTION} (opus_decode_frame) changed')
    s = s[:start] + body.replace('  FUNC_PROLOGUE;\n', HOOK + '  FUNC_PROLOGUE;\n', 1) + s[end:]
    first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
    s = s[:first.end()] + '\nextern u32 pg_opus_frame(w2c_game *instance, u64 st, u64 data, u32 len, u64 pcm, u32 frame_size, u32 *result);' + s[first.end():]
    path.write_text(s)
    print(f'native Opus: patched {path.name}')
    break
else:
    raise RuntimeError(f'f{FUNCTION} (opus_decode_frame) not found')
