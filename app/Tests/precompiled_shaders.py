#!/usr/bin/env python3
"""Fault tests for provenance-checked reuse; no game binaries are required."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

spec = importlib.util.spec_from_file_location('restore', Path(__file__).parents[1] / 'Scripts/restore_precompiled_shaders.py')
restore = importlib.util.module_from_spec(spec)
spec.loader.exec_module(restore)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def bundle(version='16', platform=2, resources=None):
    resources = resources or {}
    info = {'CFBundleIdentifier': 'io.github.playgta5ios.app', 'CFBundleShortVersionString': '1.4.6',
            'CFBundleVersion': version, 'CFBundleExecutable': 'Game'}
    command = struct.pack('<6I', 0x32, 24, platform, 0x100000, 0x1a0000, 0)
    binary = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, len(command), 0, 0) + command
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(restore.APP + 'Info.plist', plistlib.dumps(info))
        archive.writestr(restore.APP + 'Game', binary)
        for name, data in resources.items():
            archive.writestr(restore.APP + name, data)
    return stream.getvalue()

class PrecompiledShaders(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='shader-reuse-test-')
        self.root = Path(self.temp.name)
        self.index_data = json.dumps({'a': {'ok': True, 'stage': 'vs'}, 'b': {'ok': True, 'stage': 'ps'}}).encode()
        self.index = self.root / 'index.json'; self.index.write_bytes(self.index_data)
        self.reflection = b'{"h":"a","st":"vs"}\n{"h":"b","st":"ps"}\n'
        self.resources = {'shaders-iphoneos.metallib': b'fixture-metal', 'shader-reflection.jsonl': self.reflection}
        self.blob = bundle(resources=self.resources)
        self.ipa = self.root / 'baseline.ipa'; self.ipa.write_bytes(self.blob)
        self.out = self.root / 'output'
        self.patches = [mock.patch.object(restore, 'IPA_SHA256', digest(self.blob)),
                        mock.patch.object(restore, 'INDEX_SHA256', digest(self.index_data)),
                        mock.patch.object(restore, 'RESOURCES', {n: (len(b), digest(b)) for n, b in self.resources.items()})]
        for patch in self.patches: patch.start()

    def tearDown(self):
        for patch in reversed(self.patches): patch.stop()
        self.temp.cleanup()

    def test_unchanged_bundle_restores_only_two_resources(self):
        report = restore.restore(self.ipa, self.index, self.out)
        self.assertEqual(report['shader_count'], 2)
        self.assertFalse(report['runtime_or_executable_reused'])
        self.assertEqual(set(x.name for x in self.out.iterdir()), set(self.resources))
        for name, data in self.resources.items(): self.assertEqual((self.out / name).read_bytes(), data)

    def test_verify_only_leaves_no_output(self):
        report = restore.restore(self.ipa, self.index, self.out, verify_only=True)
        self.assertFalse(report['restored']); self.assertFalse(self.out.exists())

    def test_wrong_ipa_and_index_rejected(self):
        self.ipa.write_bytes(self.blob + b'changed')
        with self.assertRaisesRegex(ValueError, 'IPA checksum'): restore.restore(self.ipa, self.index, self.out)
        self.index.write_bytes(b'{}')
        with self.assertRaisesRegex(ValueError, 'Shader index'): restore.restore(self.ipa, self.index, self.out)
        self.assertFalse(self.out.exists())

    def test_wrong_version_and_simulator_rejected(self):
        for raw, message in [(bundle(version='17'), 'build 16'), (bundle(platform=7), 'iOS devices')]:
            with zipfile.ZipFile(io.BytesIO(raw)) as archive:
                with self.assertRaisesRegex(ValueError, message): restore.check_bundle(archive)

    def test_duplicate_zip_entries_rejected(self):
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w') as archive:
            archive.writestr('file', b'a')
            with warnings.catch_warnings():
                warnings.simplefilter('ignore', UserWarning); archive.writestr('file', b'b')
        with zipfile.ZipFile(io.BytesIO(stream.getvalue())) as archive:
            with self.assertRaisesRegex(ValueError, 'exactly one'): restore.one_entry(archive, 'file')

    def test_missing_or_damaged_resource_preserves_existing_output(self):
        self.out.mkdir()
        for n in self.resources: (self.out / n).write_bytes(b'old')
        for invalid in [{'shaders-iphoneos.metallib': self.resources['shaders-iphoneos.metallib']},
                        dict(self.resources, **{'shaders-iphoneos.metallib': b'wrong-length!'}),
                        dict(self.resources, **{'shaders-iphoneos.metallib': b'fixture-metax'})]:
            raw = bundle(resources=invalid); self.ipa.write_bytes(raw)
            with mock.patch.object(restore, 'IPA_SHA256', digest(raw)):
                with self.assertRaises(ValueError): restore.restore(self.ipa, self.index, self.out)
            for n in self.resources: self.assertEqual((self.out / n).read_bytes(), b'old')

    def test_reflection_missing_extra_duplicate_or_wrong_stage_rejected(self):
        index = json.loads(self.index_data)
        for data in [b'{"h":"a","st":"vs"}\n', self.reflection + b'{"h":"a","st":"vs"}\n',
                     self.reflection + b'{"h":"c","st":"cs"}\n', b'{"h":"a","st":"ps"}\n{"h":"b","st":"ps"}\n']:
            path = self.root / 'reflection'; path.write_bytes(data)
            with self.assertRaises(ValueError): restore.check_reflection(path, index)

    def test_changed_shader_abi_rejected(self):
        with self.assertRaisesRegex(ValueError, 'shader ABI'): restore.check_abi(self.root)

if __name__ == '__main__': unittest.main()
