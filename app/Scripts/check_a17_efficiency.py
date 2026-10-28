#!/usr/bin/env python3
"""Comparable subsystem measurements; never substitutes for an iPhone game run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / 'Tests/perf_baselines/WasmFutex-build21.c'
BASELINE_SHA = '2c4696e4c4358cac8b3cb97ab1e7eddba6a1e1040b87186b2c60af72f9aa8462'


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, cwd=ROOT,
                          timeout=120, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    assert hashlib.sha256(BASELINE.read_bytes()).hexdigest() == BASELINE_SHA
    compiler = os.environ.get('CC') or shutil.which('cc')
    if not compiler:
        raise SystemExit('A C compiler is required.')
    darwin = platform.system() == 'Darwin'
    report = {
        'schema': 1, 'host': platform.system() + ' ' + platform.machine(),
        'hostLogicalCpus': os.cpu_count(), 'iPhoneGameMeasured': False,
        'scope': 'Host futex request/reply workload and optional host Metal depth-copy probe; not full-game CPU/GPU utilization or FPS',
        'baselineSource': 'build21 commit 140cf81, unchanged global futex queue',
        'baselineSha256': BASELINE_SHA, 'futex': [],
    }
    with tempfile.TemporaryDirectory(prefix='a17-efficiency-') as folder:
        work = Path(folder)
        binaries = {}
        for name, source in [('baseline', BASELINE), ('candidate', ROOT / 'Source/WasmFutex.c')]:
            target = work / name
            run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                 '-D_DARWIN_C_SOURCE' if darwin else '-D_GNU_SOURCE',
                 '-I', 'Tests/futex_support', '-I', 'Source', '-include', 'Tests/futex_support/platform.h',
                 source, 'Tests/futex_shard_benchmark.c', '-o', target])
            binaries[name] = target
        for pairs in [2, 4, 8]:
            samples = {'baseline': [], 'candidate': []}
            # Reverse the order to reduce systematic cold/warm scheduler bias.
            for name in ['baseline', 'candidate', 'candidate', 'baseline']:
                result = run([binaries[name], pairs, 10000], text=True, capture_output=True)
                values = dict(field.split('=', 1) for field in result.stdout.strip().split())
                samples[name].append({k: float(v) for k, v in values.items()})
            before = statistics.median(x['cpu_seconds'] for x in samples['baseline'])
            after = statistics.median(x['cpu_seconds'] for x in samples['candidate'])
            old_wall = statistics.median(x['wall_seconds'] for x in samples['baseline'])
            new_wall = statistics.median(x['wall_seconds'] for x in samples['candidate'])
            item = {'pairs': pairs, 'threads': 2 * pairs, 'roundTrips': pairs * 10000,
                    'cpuSecondsBaseline': before, 'cpuSecondsCandidate': after,
                    'cpuTimeReductionPercent': 100 * (before - after) / before,
                    'wallSecondsBaseline': old_wall, 'wallSecondsCandidate': new_wall,
                    'throughputRatio': old_wall / new_wall, 'samples': samples}
            report['futex'].append(item)
            print(f'Futex {pairs} pairs: CPU {before:.4f}s -> {after:.4f}s; '
                  f'wall {old_wall:.4f}s -> {new_wall:.4f}s; '
                  f'{item["throughputRatio"]:.2f}x round-trip throughput', flush=True)
        if darwin:
            probe = work / 'depth-proxy-probe'
            gpu_report = work / 'gpu.json'
            run(['xcrun', 'clang++', '-std=c++17', '-O2', '-fobjc-arc', '-fblocks',
                 '-framework', 'Foundation', '-framework', 'Metal',
                 'Tests/metal_depth_proxy_probe.mm', '-o', probe])
            run([probe, '--output', gpu_report])
            report['metalDepthCopy'] = json.loads(gpu_report.read_text())
        else:
            report['metalDepthCopy'] = {'status': 'SKIPPED', 'reason': 'Metal requires an Apple host'}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('Subsystem checks complete. No iPhone FPS or total utilization claim.', flush=True)


if __name__ == '__main__':
    main()
