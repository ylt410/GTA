"""Community translations of the app's own text (not the game's: that comes from Rockstar's files).

Every line the app shows goes through AppLanguage.choose("English text"), and App/Localizable.xcstrings
holds the translations keyed by that English text. Translators work on the website's translate page,
which reads the list this script exports, and send back a translation file:

    {"format": "gta5-port-translation", "version": 1, "language": "tr", "translator": "...",
     "appVersion": "1.4.2", "strings": {"English text": "Translated text", ...}}

    python3 Scripts/translations.py export --out-dir DIR         lines.json + one <language>.json each
    python3 Scripts/translations.py check tr.json                same checks as the website
    python3 Scripts/translations.py merge tr.json                writes it into App/Localizable.xcstrings

export also lists lines the code asks for that the catalog doesn't have yet, and catalog lines no
code uses any more. A language that isn't one of the game's 13 shows up in Settings › Language on
its own once merged (AppLanguage reads the catalog's languages); the game's text stays English.
"""
import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[1]
CATALOG = root / 'App/Localizable.xcstrings'
SOURCES = root / 'App'
PROJECT = root / 'project.yml'
FORMAT = 'gta5-port-translation'

# Where each source file's text appears, in the words a player would use.
SCREENS = {
    'NativePortViewController': 'Home screen', 'HomeView': 'Home screen',
    'SettingsViewController': 'Settings', 'SettingsHomeViewController': 'Settings',
    'GameSettings': 'Settings', 'GraphicsOptions': 'Settings › Graphics', 'GyroInput': 'Settings › Gyro',
    'TouchControlsView': 'Touch controls', 'ControlsLayout': 'Touch controls › Edit layout',
    'VirtualPad': 'Touch controls', 'KeyboardMouse': 'Keyboard and mouse',
    'ModKeysView': 'Mod keys (on-screen keys for mod menus)',
    'ModsViewController': 'Settings › Mods', 'ModInstaller': 'Settings › Mods › Install a mod',
    'NativeCheats': 'Settings › Cheats', 'StoryCheats': 'Settings › Cheats',
    'VehicleSpawnViewController': 'Settings › Cheats › Spawn a vehicle',
    'GameFilesViewController': 'Game files', 'GameFiles': 'Game files', 'GameFolder': 'Game files',
    'GameInstall': 'Game files › Unpacking', 'DownloadBrowserViewController': 'Game files › Download',
    'AssetPreloader': 'Home screen › Preparing the game', 'UpdateCheck': 'Update notice',
    'LogUploader': 'Settings › This device › Send log',
}
# Controller button names read the same in every language.
SAME_IN_EVERY_LANGUAGE = {'LB', 'RB', 'LT', 'RT', 'A', 'B', 'X', 'Y'}
PLACEHOLDER = re.compile(r'\{arg\d+\}|%\.\d+f|%@|%d|%lld|%ld')
LANGUAGE_CODE = re.compile(r'^[a-z]{2,3}(-[A-Z][a-z]{3})?(-([A-Z]{2}|\d{3}))?$')
# A Swift string literal on one line ("""-blocks aren't used for app text).
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CHOOSE = re.compile(r'AppLanguage\.choose\(\s*"((?:[^"\\\n]|\\.)*)"\s*\)')
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '"': '"', "'": "'", '\\': '\\'}


def swift_string(body):
    """The value of a Swift literal's body, or None when it interpolates (not a fixed key)."""
    if '\\(' in body:
        return None
    out, i = [], 0
    while i < len(body):
        if body[i] == '\\' and i + 1 < len(body):
            if body[i + 1] == 'u' and body[i + 2:i + 3] == '{':
                end = body.index('}', i)
                out.append(chr(int(body[i + 3:end], 16)))
                i = end + 1
                continue
            out.append(ESCAPES.get(body[i + 1], body[i + 1]))
            i += 2
            continue
        out.append(body[i])
        i += 1
    return ''.join(out)


def load_catalog():
    return json.loads(CATALOG.read_text(encoding='utf-8'))


def save_catalog(catalog):
    CATALOG.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def languages(catalog):
    found = []
    for entry in catalog['strings'].values():
        for code in entry.get('localizations') or {}:
            if code not in found and code != catalog['sourceLanguage']:
                found.append(code)
    return sorted(found)


def translated(entry, code):
    unit = ((entry.get('localizations') or {}).get(code) or {}).get('stringUnit') or {}
    return unit.get('value') if unit.get('state') == 'translated' else None


def app_version():
    match = re.search(r'CFBundleShortVersionString:\s*"?([\d.]+)', PROJECT.read_text())
    return match.group(1) if match else ''


def scan_sources():
    """English text → the screens it appears on; plus the text the code asks for by literal."""
    where, asked = {}, {}
    for path in sorted(SOURCES.glob('*.swift')):
        screen = SCREENS.get(path.stem, re.sub(r'(?<!^)(?=[A-Z])', ' ', path.stem))
        for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            for body in LITERAL.findall(line):
                value = swift_string(body)
                if value is not None and screen not in where.setdefault(value, []):
                    where[value].append(screen)
            for body in CHOOSE.findall(line):
                value = swift_string(body)
                if value is not None:
                    asked.setdefault(value, f'{path.name}:{number}')
    return where, asked


def problems(english, text):
    """What's wrong with one translated line, in words a translator understands. [] = fine."""
    if not text.strip():
        return ['only spaces']
    found = []
    if sorted(PLACEHOLDER.findall(english)) != sorted(PLACEHOLDER.findall(text)):
        found.append('keep every {arg0}-style part exactly as in English: ' + (' '.join(PLACEHOLDER.findall(english)) or 'there are none'))
    if english.count('\n') != text.count('\n'):
        found.append(f'needs {english.count(chr(10))} line breaks, like the English')
    if english[:1].isspace() != text[:1].isspace():
        found.append('starts with a space in English' if english[:1].isspace() else 'remove the space at the start')
    if english[-1:].isspace() != text[-1:].isspace():
        found.append('ends with a space in English' if english[-1:].isspace() else 'remove the space at the end')
    if '—' in text:
        found.append('no long dash (—): use a comma, colon or full stop')
    return found


def read_translation(path, code=None):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if data.get('format') != FORMAT or not isinstance(data.get('strings'), dict):
        sys.exit(f'{path}: not a translation file from the translate page')
    sent = str(data.get('language', ''))
    if sent.startswith('x-') and not code:
        sys.exit(f'{path}: {data.get("languageName") or sent!r} was added by name on the page. Pick its code '
                 f'(the .lproj name Apple uses, e.g. "ckb") and run again with --code')
    data['language'] = code or sent
    if not LANGUAGE_CODE.match(data['language']):
        sys.exit(f'{path}: language code {data["language"]!r} must look like "tr", "pt-BR" or "zh-Hant"')
    return data


def app_lines(catalog):
    """English text → where it appears, for every line the app can show today: catalog lines the
    code still uses, then lines the code asks for that the catalog doesn't have yet."""
    where, asked = scan_sources()
    wanted = lambda key: key.strip() and key not in SAME_IN_EVERY_LANGUAGE
    lines = {key: where[key] for key in catalog['strings'] if wanted(key) and key in where}
    lines.update((key, where[key]) for key in asked if wanted(key) and key not in lines)
    return lines


def check(data, known):
    """(lines to write, errors, lines the app doesn't have)."""
    lines, errors, unknown = {}, [], []
    for english, text in data['strings'].items():
        if not isinstance(text, str) or not text:
            continue
        if english not in known:
            unknown.append(english)
            continue
        for problem in problems(english, text):
            errors.append(f'{english!r}: {problem}')
        lines[english] = text
    return lines, errors, unknown


def summary(data, catalog, known, lines, unknown):
    code = data['language']
    done = lambda key: key in catalog['strings'] and translated(catalog['strings'][key], code) is not None
    before = sum(1 for key in known if done(key))
    after = sum(1 for key in known if key in lines or done(key))
    new = ' (new language)' if code not in languages(catalog) else ''
    skipped = f', {len(unknown)} lines the app no longer has skipped' if unknown else ''
    return (f"{code}{new} by {data.get('translator') or 'someone'}: {len(lines)} lines{skipped}; "
            f'{before} → {after} of {len(known)} translated')


def screen_order(item):
    """Grouped by screen in the order a player meets them, so related lines sit together."""
    first = ['Home screen', 'Game files', 'Settings', 'Touch controls']
    place = item[1][0]
    top = next((i for i, name in enumerate(first) if place.startswith(name)), len(first))
    return top, place


def cmd_export(args):
    catalog = load_catalog()
    known = app_lines(catalog)
    codes = languages(catalog)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    write = lambda name, value: (out / name).write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')
    write('lines.json', {'format': FORMAT + '-source', 'version': 1, 'appVersion': app_version(),
                         'generated': datetime.now(timezone.utc).strftime('%Y-%m-%d'), 'languages': codes,
                         'lines': [{'en': key, 'where': places} for key, places in sorted(known.items(), key=screen_order)]})
    for code in codes:
        write(f'{code}.json', {key: value for key in known if key in catalog['strings']
                               and (value := translated(catalog['strings'][key], code)) is not None})
    print(f'{out}: {len(known)} lines, {len(codes)} languages, app {app_version()}')
    for key in known:
        if key not in catalog['strings']:
            print(f'not in the catalog yet: {key!r}', file=sys.stderr)
    for key in catalog['strings']:
        if key not in known and key not in SAME_IN_EVERY_LANGUAGE:
            print(f'in the catalog but not in the code: {key!r}', file=sys.stderr)


def cmd_check(args):
    catalog = load_catalog()
    known = app_lines(catalog)
    data = read_translation(args.file, args.code)
    lines, errors, unknown = check(data, known)
    print(summary(data, catalog, known, lines, unknown))
    for error in errors:
        print('  ' + error)
    return 1 if errors else 0


def cmd_merge(args):
    catalog = load_catalog()
    known = app_lines(catalog)
    data = read_translation(args.file, args.code)
    lines, errors, unknown = check(data, known)
    print(summary(data, catalog, known, lines, unknown))
    if errors:
        for error in errors:
            print('  ' + error)
        print('Not merged: fix these lines first (or ask the translator).')
        return 1
    code = data['language']
    for english, text in lines.items():
        entry = catalog['strings'].setdefault(english, {'extractionState': 'manual', 'localizations': {
            'en': {'stringUnit': {'state': 'translated', 'value': english}}}})
        entry.setdefault('localizations', {})[code] = {'stringUnit': {'state': 'translated', 'value': text}}
    save_catalog(catalog)
    print(f'Merged into {CATALOG.relative_to(root)}. Thank {data.get("translator") or "the translator"} in the release notes.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    export = commands.add_parser('export', help='write the lines for the translate page')
    export.add_argument('--out-dir', required=True, help='folder for lines.json and the language files')
    export.set_defaults(run=cmd_export)
    for name, run in (('check', cmd_check), ('merge', cmd_merge)):
        command = commands.add_parser(name)
        command.add_argument('file', help='translation file from the translate page')
        command.add_argument('--code', help='language code to use instead of the file\'s (needed for "x-" languages added by name)')
        command.set_defaults(run=run)
    args = parser.parse_args()
    sys.exit(args.run(args) or 0)


if __name__ == '__main__':
    main()
