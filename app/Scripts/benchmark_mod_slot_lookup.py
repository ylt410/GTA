#!/usr/bin/env python3
"""Measure production strided mod-slot lookup, not whole-game FPS."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--report', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CC', 'cc')
with tempfile.TemporaryDirectory(prefix='pg-mod-benchmark-build-') as directory:
    executable = Path(directory) / 'mod-slot-benchmark'
    command = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-pthread', str(root / 'Tests/benchmark_mod_slot_lookup.c'), '-o', str(executable)]
    subprocess.run(command, check=True)
    rows = [json.loads(line) for line in subprocess.check_output([str(executable)], text=True).splitlines()]

groups = {}
for row in rows:
    parameter = 'slots'
    key = (row['workload'], row[parameter])
    groups.setdefault(key, {'parameter': parameter, 'rows': []})['rows'].append(row)
summary = []
for (workload, value), group in groups.items():
    baseline = [r for r in group['rows'] if r['variant'] == 'baseline']
    optimized = [r for r in group['rows'] if r['variant'] == 'optimized']
    assert len(baseline) == len(optimized) == 7
    assert len({r['checksum'] for r in group['rows']}) == 1
    before = statistics.median(r['cpu_ms'] for r in baseline)
    after = statistics.median(r['cpu_ms'] for r in optimized)
    summary.append({'workload': workload, group['parameter']: value, 'rounds': 7,
                    'baseline_cpu_ms_median': before, 'optimized_cpu_ms_median': after,
                    'cpu_time_reduction_percent': 100 * (1 - after / before),
                    'baseline_cpu_ms_range': [min(r['cpu_ms'] for r in baseline), max(r['cpu_ms'] for r in baseline)],
                    'optimized_cpu_ms_range': [min(r['cpu_ms'] for r in optimized), max(r['cpu_ms'] for r in optimized)],
                    'baseline_wall_ms_median': statistics.median(r['wall_ms'] for r in baseline),
                    'optimized_wall_ms_median': statistics.median(r['wall_ms'] for r in optimized),
                    'checksum': baseline[0]['checksum'], 'record_stride_bytes': baseline[0]['record_stride_bytes']})
report = {'schema': 1, 'scope': 'isolated sorted mod-slot lookup; NOT local I/O, full-game CPU/GPU/RAM/FPS',
          'environment': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine(),
                          'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
                          'optimization': '-O2', 'timer': 'CLOCK_PROCESS_CPUTIME_ID; wall CLOCK_MONOTONIC',
                          'order': 'seven paired rounds, alternating baseline/optimized order'},
          'memory_note': 'No secondary slot index is allocated (0 additional bytes). This benchmark does not claim reduced process RAM; it does not time or change descriptor caching.',
          'summary': summary, 'raw_samples': rows}
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ('scope', 'environment', 'memory_note', 'summary')}, indent=2))
