#!/usr/bin/env python3
"""Compile and run production BC paths against the pinned bcdec decoder."""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BCDEC_COMMIT = '80859ed3b7afb1c527a2a99d70c61457bea72d0c'


def run(args, cwd):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def pinned_header(temp):
    configured = os.environ.get('PG_BCDEC_HEADER')
    candidates = [Path(configured)] if configured else []
    candidates += [ROOT / 'Tools/bcdec/bcdec.h']
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    archive = temp / 'bcdec.tar.gz'
    url = f'https://codeload.github.com/iOrange/bcdec/tar.gz/{BCDEC_COMMIT}'
    print(f'+ download pinned bcdec {BCDEC_COMMIT}', flush=True)
    with urllib.request.urlopen(url, timeout=60) as response:
        archive.write_bytes(response.read())
    expected = f'bcdec-{BCDEC_COMMIT}/bcdec.h'
    with tarfile.open(archive, 'r:gz') as package:
        member = next((m for m in package.getmembers() if m.name == expected and m.isfile()), None)
        if member is None:
            raise RuntimeError('Pinned bcdec archive did not contain the expected header.')
        stream = package.extractfile(member)
        if stream is None:
            raise RuntimeError('Could not read the pinned bcdec header.')
        header = temp / 'bcdec.h'
        header.write_bytes(stream.read())
        return header


def main():
    compiler = os.environ.get('CC') or shutil.which('cc')
    if not compiler:
        raise SystemExit('A native C compiler is required for the BC production tests.')
    with tempfile.TemporaryDirectory(prefix='bc-texture-production-') as directory:
        temp = Path(directory)
        tree = temp / 'tree'
        for folder in ['Source', 'Tests', 'Tools/bcdec']:
            (tree / folder).mkdir(parents=True, exist_ok=True)
        for relative in [
            'Source/BCTextureDecoder.c', 'Source/BCTextureDecoder.h', 'Source/BCTextureDecoderReference.inc',
            'Source/BC1ToASTC.c', 'Source/BC1ToASTC.h', 'Tests/bc_textures.c',
            'Tests/bc_texture_allocation_guard.c',
        ]:
            shutil.copy2(ROOT / relative, tree / relative)
        shutil.copy2(pinned_header(temp), tree / 'Tools/bcdec/bcdec.h')

        decoder = temp / 'bc_textures'
        run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra',
             '-Werror', '-Wno-misleading-indentation', '-Wno-unused-function',
             'Tests/bc_textures.c', 'Source/BCTextureDecoder.c', '-lm', '-o', decoder], tree)
        run([decoder], tree)

        allocation_guard = temp / 'bc_texture_allocation_guard'
        run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-Wno-misleading-indentation', '-Wno-unused-function',
             'Tests/bc_texture_allocation_guard.c', '-o', allocation_guard], tree)
        run([allocation_guard], tree)


if __name__ == '__main__':
    main()
