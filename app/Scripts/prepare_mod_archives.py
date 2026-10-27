"""Mods inside archives (GTA-69): after the engine opens an archive, the host may point its entries at mod files.

rage::fiPackfile::ReInit(name, ...) reads an .rpf's header and decrypts its table of contents into memory,
at mount (from Init) and again whenever the streaming system reloads it
(the RPF7 entry array at +32, names at +16). The original becomes static and a wrapper with the engine's
name calls it, then pg_packfile_mounted(instance, packfile, name, result), where the host can rewrite the
entries of files the player replaced (Mods/<archive>.rpf/<path inside>), pointing them at data the file
layer serves past the archive's real end (Source/NativeAssets.m).

Add-on packs made with OpenIV are OPEN archives: their table of contents isn't encrypted (header key id
"OPEN"), but the engine runs AES on every table whose key isn't the NG one. AES::Decrypt(keyId, ...) is
wrapped to leave OPEN data alone; files inside OPEN packs are already marked unencrypted.
"""
from pathlib import Path
from engine_names import fn
root = Path(__file__).resolve().parents[1]
DECLARATION = 'extern void pg_packfile_mounted(w2c_game *instance, u64 packfile, u64 name, u32 result);'

# Builds before 2026-10-17 hooked Init; the streaming system later re-reads tables through ReInit
# (strPackfileStreamingModule::Load), which dropped the patch. Undo the Init hook if present.
init = fn('rage::fiPackfile::Init(char const*, bool, rage::fiPackfile::CacheMode, char*)')
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    if f'pg_packfile_init_original_f{init}(' not in s: continue
    head = f'u32 w2c_game_f{init}(w2c_game* instance, u64 var_p0, u64 var_p1, u32 var_p2, u32 var_p3, u64 var_p4) {{'
    start = s.index(head); end = s.index('\n}\n', start) + 3
    s = s[:start] + s[end:]
    s = s.replace(f'static u32 pg_packfile_init_original_f{init}(', f'u32 w2c_game_f{init}(', 1)
    path.write_text(s)
    print(f'mod archives: removed the old Init hook from {path.name}')

function = fn('rage::fiPackfile::ReInit(char const*, bool, void*)')
head = f'u32 w2c_game_f{function}(w2c_game* instance, u64 var_p0, u64 var_p1, u32 var_p2, u64 var_p3) {{'
path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if head in p.read_text())
s = path.read_text()
if f'pg_packfile_reinit_original_f{function}(' in s:
    print(f'mod archives: f{function} already patched')
else:
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{function} (fiPackfile::ReInit) changed')
    start = s.index(head); end = s.index('\n}\n', start) + 3
    original = s[start:end].replace(head, f'static u32 pg_packfile_reinit_original_f{function}(w2c_game* instance, u64 var_p0, u64 var_p1, u32 var_p2, u64 var_p3) {{', 1)
    wrapper = (f'{head}\n  u32 result = pg_packfile_reinit_original_f{function}(instance, var_p0, var_p1, var_p2, var_p3);\n'
               f'  pg_packfile_mounted(instance, var_p0, var_p1, result);\n  return result;\n}}\n')
    s = s[:start] + original + wrapper + s[end:]
    if DECLARATION not in s:
        first = s.index('\n', s.index('#include')) + 1
        s = s[:first] + DECLARATION + '\n' + s[first:]
    path.write_text(s)
    print(f'mod archives: patched f{function} in {path.name}')

OPEN = 0x4E45504F
decrypt = fn('rage::AES::Decrypt(unsigned int, unsigned int, void*, unsigned int)')
head = f'void w2c_game_f{decrypt}(w2c_game* instance, u32 var_p0, u64 var_p1, u32 var_p2) {{'
path = next(p for p in sorted((root / 'Generated').glob('game_*.c')) if head in p.read_text())
s = path.read_text()
if f'pg_aes_decrypt_original_f{decrypt}(' in s:
    print(f'mod archives: f{decrypt} already patched')
else:
    if s.count(head) != 1: raise RuntimeError(f'Pinned f{decrypt} (AES::Decrypt) changed')
    start = s.index(head); end = s.index('\n}\n', start) + 3
    original = s[start:end].replace(head, f'static void pg_aes_decrypt_original_f{decrypt}(w2c_game* instance, u32 var_p0, u64 var_p1, u32 var_p2) {{', 1)
    wrapper = (f'{head}\n  if (var_p0 == {OPEN}u) return; /* OPEN archive: not encrypted */\n'
               f'  pg_aes_decrypt_original_f{decrypt}(instance, var_p0, var_p1, var_p2);\n}}\n')
    path.write_text(s[:start] + original + wrapper + s[end:])
    print(f'mod archives: patched f{decrypt} in {path.name}')
