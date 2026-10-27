#!/usr/bin/env python3
"""Verify actual device compiler output before compiling the large engine."""
import argparse
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-mcpu=apple-a11', '-mtune=apple-a17']
PROBE = '''
unsigned pg_increment(unsigned *p) { return __atomic_fetch_add(p, 1u, 5); }
unsigned long long pg_increment64(unsigned long long *p) {
 return __atomic_fetch_add(p, 1ull, 5);
}
int pg_compare(unsigned *p, unsigned expected, unsigned value) {
 return __atomic_compare_exchange_n(p, &expected, value, 0, 5, 5);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    # Validate the same flags that xcodegen assigns to every generated shard.
    assert 'compilerFlags: -w ' + ' '.join(FLAGS) in (ROOT / 'project.yml').read_text()
    result = {'schema': 1, 'engine_flags': FLAGS,
              'minimum_engine_cpu': 'Apple A11 (LSE); checked before engine launch',
              'whole_game_fps_tested': False}
    if platform.system() != 'Darwin':
        result.update(status='DEFERRED', reason='Apple Clang/device SDK required; enforced in the IPA job')
    else:
        sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
        compiler = ['xcrun', '--sdk', 'iphoneos', 'clang']
        result['compiler'] = subprocess.check_output(compiler + ['--version'], text=True).splitlines()[0]
        with tempfile.TemporaryDirectory(prefix='a17-codegen-') as directory:
            source = Path(directory) / 'probe.c'
            source.write_text(PROBE)
            assembly = {}
            for name, flags in [('baseline', []), ('candidate', FLAGS)]:
                command = compiler + ['-target', 'arm64-apple-ios16.0', '-isysroot', sdk,
                                      '-O2', '-Werror', '-fno-optimize-sibling-calls', '-frounding-math',
                                      *flags, '-S', str(source), '-o', '-']
                assembly[name] = subprocess.check_output(command, text=True)
            # Do not accept a build flag that is ignored or still emits retry
            # loops for these shared-memory operations.
            baseline_exclusive = re.findall(r'\b(?:ldaxr|stlxr)\b', assembly['baseline'])
            candidate_lse = re.findall(r'\b(?:ldaddal|casal)\b', assembly['candidate'])
            assert baseline_exclusive, 'Baseline no longer uses exclusive loops; re-evaluate the claimed codegen change'
            assert candidate_lse.count('ldaddal') == 2 and candidate_lse.count('casal') == 1, assembly['candidate']
            assert not re.search(r'\b(?:ldaxr|stlxr)\b', assembly['candidate'])
            result.update(status='PASS', baseline_exclusive_instructions=len(baseline_exclusive),
                          candidate_lse_instructions=candidate_lse,
                          baseline_assembly=assembly['baseline'], candidate_assembly=assembly['candidate'])
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if not k.endswith('_assembly')}, indent=2))


if __name__ == '__main__':
    main()
