#!/usr/bin/env python3
"""Check the actual config rewrite, spatial sizing and host binding/scaler paths."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(args, **kw):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, timeout=120, **kw)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = {'schema': 1, 'build': '23', 'host': platform.system() + ' ' + platform.machine(),
              'iPhoneGameMeasured': False,
              'scope': 'Actual rewrite and sizing checks, host shader-metadata work, optional MetalFX pixel probe. No full-game CPU/GPU/FPS gain is measured.'}
    source = (ROOT / 'Source/NativeAssets.m').read_text()
    match = re.search(r'\{"AttachmentExtension", "entity attachment extensions", (\d+)\}', source)
    assert match and int(match[1]) == 500, 'The actual attachment pool table must use minimum 500'
    begin = source.index('NSData *PGGameConfigRewrite(')
    end = source.index('\nstatic void gameConfigOverlay(', begin)
    function = source[begin:end]
    report['attachmentPool'] = {'minimum': int(match[1]), 'originalDeviceCapacity': 348,
        'additionalEntryPayloadBytes': (500 - 348) * 160,
        'rewriteSourceSha256': hashlib.sha256(function.encode()).hexdigest()}
    with tempfile.TemporaryDirectory(prefix='build23-features-') as directory:
        work = Path(directory)
        if platform.system() == 'Darwin':
            implementation = work / 'actual-gameconfig.m'
            implementation.write_text('#import <Foundation/Foundation.h>\n#include <stdbool.h>\n' + function)
            executable = work / 'gameconfig-test'
            run(['xcrun', '--sdk', 'macosx', 'clang', '-O2', '-fobjc-arc', '-framework', 'Foundation',
                 implementation, 'Tests/game_config_attachment_pool.m', '-o', executable])
            run([executable, match[1]])
            report['attachmentPool']['actualRewriteTest'] = 'PASS'
            run(['bash', 'Scripts/test_spatial_super_resolution.sh'])
            report['spatialSizing'] = 'PASS'
        else:
            report['attachmentPool']['actualRewriteTest'] = 'SKIPPED: Foundation requires Apple SDK'
            report['spatialSizing'] = 'SKIPPED: Swift sizing executable runs in Apple CI'
        executable = work / 'binding-benchmark'
        run([shutil.which('c++') or 'c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
             'Tests/benchmark_binding_plan.cpp', '-o', executable])
        benchmark = run([executable], text=True, capture_output=True)
        report['bindingMetadata'] = json.loads(benchmark.stdout)
        if platform.system() == 'Darwin':
            executable = work / 'spatial-probe'
            probe_report = work / 'metal-spatial.txt'
            run(['xcrun', '--sdk', 'macosx', 'clang++', '-std=c++17', '-O2', '-fobjc-arc', '-fblocks',
                 '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'MetalFX',
                 'Tests/metal_spatial_upscale_probe.mm', '-o', executable])
            run([executable, '--output', probe_report])
            contents = probe_report.read_text()
            skipped = contents.startswith('SKIPPED:')
            assert skipped or contents.count('PASS ') == 3, 'Three valid scaled readbacks are required'
            report['metalFXSpatial'] = {'status': 'SKIPPED' if skipped else 'PASS', 'details': contents}
        else:
            report['metalFXSpatial'] = {'status': 'SKIPPED', 'reason': 'MetalFX requires an Apple host'}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('Build23 feature checks complete. Whole-game iPhone gain remains unmeasured.', flush=True)

if __name__ == '__main__':
    main()
