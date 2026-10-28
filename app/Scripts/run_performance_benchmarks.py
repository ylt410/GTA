#!/usr/bin/env python3
"""Measure isolated old/new paths; never substitute them for full-game results."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, timeout=180):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    return subprocess.check_output(list(map(str, command)), cwd=ROOT, text=True, timeout=timeout)


def json_lines(output):
    return [json.loads(line) for line in output.splitlines() if line.startswith('{')]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    cxx = os.environ.get('CXX') or shutil.which('c++')
    compiler = run([cxx, '--version']).splitlines()[0]
    result = {
        'schema': 1,
        'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'environment': {'system': platform.system(), 'release': platform.release(),
                        'machine': platform.machine(), 'compiler': compiler, 'optimization': '-O2'},
        'baseline': 'Per-workload original source paths; binding metadata follows the build22 loop. Isolated algorithm replay, not a full-game run.',
        'whole_game': {'tested': False, 'cpu_reduction_percent': None, 'gpu_reduction_percent': None,
                       'memory_reduction_percent': None,
                       'reason': 'No user iPhone or complete locally runnable game data is attached. Host microbenchmarks cannot establish these numbers.'},
        'measurements': {},
    }
    with tempfile.TemporaryDirectory(prefix='native-performance-bench-') as directory:
        work = Path(directory)
        for name, source, extra in [
            ('constant_upload', 'Tests/benchmark_constant_upload.cpp', []),
            ('binding_plan', 'Tests/benchmark_binding_plan.cpp', []),
            ('upload_ring_retention', 'Tests/upload_ring_retention.cpp', ['--benchmark']),
        ]:
            executable = work / name
            run([cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', source, '-o', executable])
            rows = json_lines(run([executable, *extra]))
            if not rows:
                raise RuntimeError('Missing measured output: ' + name)
            result['measurements'][name] = rows
        lookup = work / 'slot-lookup.json'
        run([shutil.which('python3') or 'python3', 'Scripts/benchmark_mod_slot_lookup.py', '--report', lookup])
        result['measurements']['mod_slot_lookup'] = json.loads(lookup.read_text())
        if platform.system() == 'Darwin':
            result['environment']['cpu'] = run(['sysctl', '-n', 'machdep.cpu.brand_string']).strip()
            for name, source in [('render_target_metal', 'Tests/metal_pass_benchmark.mm'),
                                 ('constant_upload_metal', 'Tests/benchmark_constant_upload_metal.mm')]:
                executable = work / name
                run(['xcrun', '--sdk', 'macosx', 'clang++', '-std=c++20', '-O2', '-fobjc-arc',
                     '-framework', 'Foundation', '-framework', 'Metal', source, '-o', executable])
                output = run([executable])
                result['measurements'][name] = json.loads(output)
        else:
            result['measurements']['metal'] = {'status': 'SKIPPED', 'reason': 'This executor has no macOS Metal runtime.'}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print('Measured report: ' + str(args.report), flush=True)
    print('Whole-game CPU/GPU/memory reductions remain unmeasured; see per-workload scope.', flush=True)


if __name__ == '__main__':
    main()
