#!/usr/bin/env python3
"""Reproducible CPU/payload microbenchmarks, not iPhone frame-time claims.

Prints raw samples as JSON. Setup/verification stays outside each executable's
timed interval. No timing thresholds are used as CI correctness gates.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = '72f81231b12828b5fb001f609b6dcb723207e230'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rounds', type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100:
        parser.error('--rounds must be between 1 and 100')
    cc = os.environ.get('CC') or shutil.which('cc')
    cxx = os.environ.get('CXX') or shutil.which('c++')
    if not cc or not cxx:
        raise SystemExit('A C and C++ compiler are required.')
    report = {
        'scope': 'portable CPU helpers and payload capacity; not whole-App CPU/GPU/RSS',
        'baseline_commit': BASELINE,
        'platform': platform.platform(),
        'machine': platform.machine(),
        'c_compiler': subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
        'cxx_compiler': subprocess.check_output([cxx, '--version'], text=True).splitlines()[0],
        'rounds': args.rounds,
        'samples': [],
    }
    with tempfile.TemporaryDirectory(prefix='gta-native-bench-') as directory:
        work = Path(directory)
        specifications = [
            ('archive', cc, 'c11', 'Tests/benchmark_archive_lookup.c', []),
            ('shadow', cxx, 'c++17', 'Tests/benchmark_copy_on_write_bytes.cpp', []),
            ('mesh_baseline', cxx, 'c++17', 'Tests/benchmark_mesh_slab_reclaim.cpp',
             ['-DMR_MESH_SLAB_ALLOCATOR_BASELINE']),
            ('mesh_watermark', cxx, 'c++17', 'Tests/benchmark_mesh_slab_reclaim.cpp', []),
        ]
        executables = {}
        for name, compiler, standard, source, definitions in specifications:
            target = work / name
            subprocess.run([compiler, '-std=' + standard, '-O2', '-Wall', '-Wextra',
                            '-Werror', '-pthread', *definitions, str(ROOT / source),
                            '-o', str(target)], check=True)
            executables[name] = target
        for round_index in range(args.rounds):
            jobs = [('archive', []), ('shadow', [])]
            # Alternate which allocator is measured first across rounds.
            mesh_names = ['mesh_baseline', 'mesh_watermark']
            if round_index % 2:
                mesh_names.reverse()
            for pending in (64, 1024, 32768):
                jobs.extend((name, [str(pending), '1000']) for name in mesh_names)
            for name, arguments in jobs:
                output = subprocess.check_output([executables[name], *arguments], text=True)
                for line in output.splitlines():
                    sample = json.loads(line)
                    sample.update(benchmark=name, round=round_index + 1)
                    report['samples'].append(sample)
            # Includes allocation, sorting, traversal and patch application in
            # the real old/new production functions, not just the matcher.
            output = subprocess.check_output(
                [sys.executable, ROOT / 'Tests/archive_production.py', '--benchmark', '--runs', '7'],
                text=True)
            for line in output.splitlines():
                if line == 'archive production integration passed':
                    continue
                sample = json.loads(line)
                sample.update(benchmark='archive_production', round=round_index + 1)
                report['samples'].append(sample)
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
