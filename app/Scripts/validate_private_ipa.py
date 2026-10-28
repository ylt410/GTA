#!/usr/bin/env python3
"""Check the produced device app before making its private download available."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('ipa', type=Path)
parser.add_argument('--report', type=Path, required=True)
args = parser.parse_args()

with zipfile.ZipFile(args.ipa) as archive:
    if archive.testzip() is not None:
        raise SystemExit('IPA archive CRC failed.')
    infos = [n for n in archive.namelist() if n.startswith('Payload/') and n.count('/') == 2 and n.endswith('.app/Info.plist')]
    if len(infos) != 1:
        raise SystemExit('Expected exactly one app inside Payload.')
    info = plistlib.loads(archive.read(infos[0]))
    assert info['CFBundleVersion'] == '23', 'Wrong app build number'
    assert info['CFBundleShortVersionString'] == '1.4.6', 'Unexpected base app version'
    assert info.get('PGCustomBuildLabel') == 'MetalFX Super Resolution + Stability'
    assert info.get('PGEngineRequiresLSE') is True
    assert info.get('PGEngineCodegen') == 'apple-a11 / tune apple-a17 / LSE'
    app = infos[0].rsplit('/', 1)[0] + '/'
    executable = archive.read(app + info['CFBundleExecutable'])
    assert b'Engine parallelism' in executable, 'Engine core selector missing from the app'
    assert b'native-performance.json' in executable, 'Local whole-frame metrics missing'
    assert b'Native local readahead:' in executable, 'Local advisory readahead missing'
    assert b'full object scans 0' in executable, 'Targeted snapshot retirement missing'
    assert b'Memory policy: physical cap' in executable, 'Separated physical/virtual memory policy missing'
    assert b'Engine virtual mapping:' in executable, 'Actual virtual mapping diagnostic missing'
    assert b'mesh slab memory:' in executable, 'Serial-safe mesh backing reclamation missing'
    assert b'Native synchronization: independently locked' in executable, 'Sharded synchronization marker missing'
    assert b'depth proxies since launch:' in executable, 'Demanded depth-copy work tracking missing'
    assert b'MetalFX spatial super resolution:' in executable, 'Explicit-output super resolution missing'
    assert b'superResolutionMode' in executable, 'Saved super resolution setting missing'
    assert b'entity attachment extensions' in executable, 'AttachmentExtension pool repair missing'
    assert b'Log filter: suppressed' in executable, 'Routine trace filtering missing'
    assert b'logical cores (device %s, %u online, requested %s)' in executable, 'Device core policy log missing'
    magic, cpu, subtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from('<8I', executable)
    assert magic == 0xfeedfacf and cpu == 0x100000c and filetype == 2, 'Not an ARM64 device executable'
    commands, cursor = [], 32
    build_platforms = []
    for _ in range(ncmds):
        command, size = struct.unpack_from('<II', executable, cursor)
        assert size >= 8 and cursor + size <= len(executable)
        if command in (0xc, 0x80000018):
            offset = struct.unpack_from('<I', executable, cursor + 8)[0]
            name = executable[cursor+offset:cursor+size].split(b'\0', 1)[0].decode()
            commands.append({'name': name, 'weak': command == 0x80000018})
        elif command == 0x32:
            build_platforms.append(struct.unpack_from('<I', executable, cursor+8)[0])
        cursor += size
    assert cursor == 32 + sizeofcmds
    assert build_platforms == [2], 'Expected iOS device platform, not an ARM64 simulator'
    metalfx = [x for x in commands if '/MetalFX.framework/' in x['name']]
    assert metalfx and all(x['weak'] for x in metalfx), 'MetalFX must remain weak-linked for older iOS versions'
    for removed in [b'AI frame generation (MetalFX)', b'PGMetalRendererSetFrameGeneration', b'pgfg_motion']:
        assert removed not in executable, 'Removed frame-generation implementation remains'
    assert info.get('UIFileSharingEnabled') is True
    assert info.get('LSSupportsOpeningDocumentsInPlace') is True
    assert info.get('MinimumOSVersion', '16.0').split('.')[0] == '16', 'Accidental minimum OS bump'

digest = hashlib.sha256()
with args.ipa.open('rb') as stream:
    while chunk := stream.read(1 << 20): digest.update(chunk)
report = {'ipa': args.ipa.name, 'bytes': args.ipa.stat().st_size, 'sha256': digest.hexdigest(),
          'bundle_identifier': info['CFBundleIdentifier'], 'version': info['CFBundleShortVersionString'],
          'build': info['CFBundleVersion'], 'minimum_os': info.get('MinimumOSVersion'),
          'arm64_executable': True, 'metal_fx_weak_linked': True, 'frame_generation_removed': True,
          'file_sharing_enabled': True,
          'archive_crc': 'PASS', 'device_launch_tested': False}
args.report.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
