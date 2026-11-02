#!/usr/bin/env python3
"""Compile the production NativeOpus.c against portable host/libopus stubs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = os.environ.get('CC') or shutil.which('cc')
    if not compiler:
        raise SystemExit('A C compiler is required for the NativeOpus production-path test.')
    with tempfile.TemporaryDirectory(prefix='native-opus-production-') as work:
        binary = Path(work) / 'native_opus_production'
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                        '-pthread', 'Tests/native_opus_production.c', '-o', binary], cwd=ROOT, check=True)
        base = os.environ.copy()
        base.pop('PG_NATIVE_OPUS', None)
        base.pop('PG_OPUS_VERIFY', None)
        for mode, additions in [
            ('native', {}),
            ('verify', {'PG_OPUS_VERIFY': '1'}),
            ('wasm', {'PG_NATIVE_OPUS': '0'}),
        ]:
            env = base.copy()
            env.update(additions)
            print(f'+ {mode} NativeOpus production-path test', flush=True)
            subprocess.run([binary], cwd=ROOT, env=env, check=True)


if __name__ == '__main__':
    main()
