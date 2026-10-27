#!/usr/bin/env python3
"""Reuse only the verified engine shaders from this account's working build.

This optional path does not reuse an executable or any runtime code. It avoids
retranslating the same 4,808 shaders when the input and shader ABI are unchanged.
Only unchanged engine shader resources are reused; native host code is rebuilt.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASE_COMMIT = '2843334856991f88056184fb3c269652658052bd'
IPA_SHA256 = 'fc2fca21dbaeeaabee7d85babd318efd99588263cd2e2dd01802ed62567ee885'
INDEX_SHA256 = '11c331895051b30126980ef666bd8935314ab400348b17c56ef8fe8983f50ee0'
APP = 'Payload/PlayGTA5Native.app/'
ABI_FILES = {
    'ShaderTool/pgmsl.cc': '76e68792034995f595f835da6d456d13f51d45a9c88bdc0e08474d18349c4690',
    'Scripts/build_metal_shaders.py': 'b820e2cb73e631735d3083b69142d91dab4792f92a78d698cc2b7ce208e626af',
    'Source/MetalRenderer/MRInternal.h': '49660fb666cd08df10a8197adc22608b20b408d3791977d570cd61bbe065a36d',
    'Source/MetalRenderer/MRShaders.mm': '31c9c8663418c7dc42beb96f43db71006e3037d311a19b29007815b0cd5021bc',
}
RESOURCES = {
    'shaders-iphoneos.metallib': (75103568, '0f941dc5a816d23f12fca14dab37b345cbd92568cec5b17177523a20d8cb0865'),
    'shader-reflection.jsonl': (2606209, 'b1119c7e9d6c7f00f407492b21d28847ce9ba9da29a041e25ff54a2e76a2b4db'),
}

def require(condition, message):
    if not condition:
        raise ValueError(message)

def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while data := stream.read(1 << 20):
            digest.update(data)
    return digest.hexdigest()

def check_abi(root):
    for name, expected in ABI_FILES.items():
        path = root / name
        require(path.is_file() and sha256(path) == expected,
                f'Cannot reuse shaders: shader ABI source changed: {name}')

def one_entry(archive, name):
    matches = [item for item in archive.infolist() if item.filename == name]
    require(len(matches) == 1, f'Expected exactly one archived {name}')
    require(not matches[0].flag_bits & 1, f'Encrypted ZIP entry: {name}')
    return matches[0]

def check_bundle(archive):
    info = plistlib.loads(archive.read(one_entry(archive, APP + 'Info.plist')))
    expected = {'CFBundleIdentifier': 'io.github.playgta5ios.app',
                'CFBundleShortVersionString': '1.4.6', 'CFBundleVersion': '16'}
    require(all(info.get(key) == value for key, value in expected.items()),
            'Precompiled shaders must come from the verified original v1.4.6 build 16')
    executable = info.get('CFBundleExecutable')
    require(isinstance(executable, str) and executable and '/' not in executable,
            'Invalid baseline executable name')
    with archive.open(one_entry(archive, APP + executable)) as stream:
        header = stream.read(32)
        require(len(header) == 32, 'Truncated baseline executable')
        magic, cpu, _, kind, count, size, _, _ = struct.unpack('<8I', header)
        require(magic == 0xfeedfacf and cpu == 0x100000c and kind == 2,
                'Baseline app is not an ARM64 device executable')
        require(count <= 4096 and size <= 1 << 20, 'Invalid Mach-O command table')
        commands = stream.read(size)
        require(len(commands) == size, 'Truncated baseline load commands')
    position, platform = 0, None
    for _ in range(count):
        require(position + 8 <= len(commands), 'Truncated Mach-O command')
        command, length = struct.unpack_from('<II', commands, position)
        require(length >= 8 and position + length <= len(commands), 'Invalid Mach-O command')
        if command == 0x32:
            require(length >= 24, 'Truncated build-version command')
            platform = struct.unpack_from('<I', commands, position + 8)[0]
        position += length
    require(position == size and platform == 2, 'Baseline must target iOS devices, not the simulator')

def check_reflection(path, index):
    expected = {name: value['stage'] for name, value in index.items()
                if not name.startswith('_') and value.get('ok') and value.get('stage') in ('vs', 'ps', 'cs')}
    actual = {}
    with path.open() as stream:
        for line in stream:
            record = json.loads(line)
            name, stage = record.get('h'), record.get('st')
            require(name not in actual, 'Duplicate shader reflection record')
            require(isinstance(name, str) and stage in ('vs', 'ps', 'cs'), 'Invalid shader reflection record')
            actual[name] = stage
    require(actual == expected, 'Precompiled shader hashes/stages do not match the supplied shader index')
    return len(actual)

def restore(ipa, index_path, destination, root=ROOT, verify_only=False):
    check_abi(root)
    require(index_path.is_file() and sha256(index_path) == INDEX_SHA256, 'Shader index is not the supported game build')
    require(ipa.is_file() and sha256(ipa) == IPA_SHA256, 'Baseline IPA checksum mismatch')
    index = json.loads(index_path.read_text())
    # Stage both files and finish their CRC/SHA checks before publishing either.
    with tempfile.TemporaryDirectory(prefix='verified-engine-shaders-') as work:
        work = Path(work)
        with zipfile.ZipFile(ipa) as archive:
            check_bundle(archive)
            for name, (size, digest) in RESOURCES.items():
                member = one_entry(archive, APP + name)
                require(member.file_size == size, f'Wrong precompiled resource size: {name}')
                target = work / name
                with archive.open(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output, 1 << 20)
                require(sha256(target) == digest, f'Precompiled resource checksum mismatch: {name}')
        count = check_reflection(work / 'shader-reflection.jsonl', index)
        if not verify_only:
            destination.mkdir(parents=True, exist_ok=True)
            for name in RESOURCES:
                # Copies are complete before replace, even across temp volumes.
                target = destination / ('.' + name + '.verified')
                try:
                    shutil.copyfile(work / name, target)
                    target.replace(destination / name)
                finally:
                    target.unlink(missing_ok=True)
    return {'baseline_commit': BASE_COMMIT, 'baseline_ipa_sha256': IPA_SHA256,
            'shader_index_sha256': INDEX_SHA256, 'shader_count': count,
            'shader_abi_unchanged': True, 'resources': {
                name: {'bytes': size, 'sha256': digest} for name, (size, digest) in RESOURCES.items()},
            'restored': not verify_only, 'runtime_or_executable_reused': False}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ipa', type=Path, required=True)
    parser.add_argument('--index', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'GeneratedResources')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    try:
        report = restore(args.ipa, args.index, args.out, verify_only=args.verify_only)
    except (ValueError, OSError, KeyError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        raise SystemExit(f'Precompiled shader validation failed: {error}')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"Verified {report['shader_count']} unchanged engine shaders; " +
          ('restored two resources.' if report['restored'] else 'validation only.'))

if __name__ == '__main__':
    main()
