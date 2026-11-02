#!/usr/bin/env python3
"""Fast regression gate before the expensive native engine compilation."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(args):
    print('+ ' + ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True)

def main():
    cc = os.environ.get('CC') or shutil.which('cc')
    cxx = os.environ.get('CXX') or shutil.which('c++')
    if not cc or not cxx:
        raise SystemExit('A C and C++ compiler are required for the preflight.')
    tests = [
        ('native_log_filter', cc, 'c11', 'Tests/native_log_filter.c'),
        ('audio_decoder_cache', cc, 'c11', 'Tests/audio_decoder_cache.c'),
        ('audio_continuity', cc, 'c11', 'Tests/audio_continuity.c'),
        ('spatial_upscale_policy', cxx, 'c++17', 'Tests/spatial_upscale_policy.cpp'),
        ('memory_budget', cc, 'c11', 'Tests/memory_budget.c'),
        ('engine_configuration', cc, 'c11', 'Tests/engine_configuration.c'),
        ('packed_asset_cache', cc, 'c11', 'Tests/packed_asset_cache.c'),
        ('packed_asset_migration', cc, 'c11', 'Tests/packed_asset_migration.c'),
        ('local_asset_reader', cc, 'c11', 'Tests/local_asset_reader.c'),
        ('metal_immediate_cache', cxx, 'c++17', 'Tests/metal_immediate_cache.cpp'),
        ('metal_binding_plan', cxx, 'c++17', 'Tests/metal_binding_plan.cpp'),
        ('metal_layout_identity', cxx, 'c++17', 'Tests/metal_layout_identity.cpp'),
        ('texture_upload_policy', cxx, 'c++17', 'Tests/texture_upload_policy.cpp'),
        ('constant_upload', cxx, 'c++17', 'Tests/metal_constant_upload.cpp'),
        ('render_target_cache', cxx, 'c++17', 'Tests/metal_render_target_cache.cpp'),
        ('upload_ring_retention', cxx, 'c++20', 'Tests/upload_ring_retention.cpp'),
        ('asset_range_search', cc, 'c11', 'Tests/asset_range_search.c'),
        ('frame_metrics', cxx, 'c++17', 'Tests/frame_metrics.cpp'),
        ('gpu_frame_timing', cxx, 'c++17', 'Tests/gpu_frame_timing.cpp'),
        ('renderer_completion_owners', cxx, 'c++17', 'Tests/renderer_completion_owners.cpp'),
        ('mesh_slab_allocator', cxx, 'c++17', 'Tests/metal_mesh_slab_allocator.cpp'),
        ('depth_proxy_range', cxx, 'c++17', 'Tests/metal_depth_proxy_range.cpp'),
        ('archive_lookup', cc, 'c11', 'Tests/archive_lookup.c'),
        ('http_range', cc, 'c11', 'Tests/http_range.c'),
        ('copy_on_write_bytes', cxx, 'c++17', 'Tests/copy_on_write_bytes.cpp'),
        ('mesh_slab_reclaim', cxx, 'c++17', 'Tests/mesh_slab_reclaim.cpp'),
        ('mesh_slab_equivalence', cxx, 'c++17', 'Tests/mesh_slab_equivalence.cpp'),
    ]
    with tempfile.TemporaryDirectory(prefix='fanport-preflight-') as work:
        for name, compiler, standard, source in tests:
            target = Path(work) / name
            feature = '-D_DARWIN_C_SOURCE' if platform.system() == 'Darwin' else '-D_POSIX_C_SOURCE=200809L'
            run([compiler, '-std='+standard, feature, '-O2', '-Wall', '-Wextra',
                 '-pthread', '-I', 'Source', source, '-o', target])
            run([target])
        # Exercise ASTC transcoding and BC decoding as native host code. The
        # decoder runner obtains the exact pinned bcdec header in its temp tree
        # when prepare.py has not populated Tools yet; CC is deliberately not
        # inherited here, so this is not accidentally an iOS cross-build.
        native_cc = shutil.which('cc')
        if not native_cc:
            raise SystemExit('A native host C compiler is required for BC texture preflight.')
        astc = Path(work) / 'bc-astc-into'
        feature = '-D_DARWIN_C_SOURCE' if platform.system() == 'Darwin' else '-D_POSIX_C_SOURCE=200809L'
        run([native_cc, '-std=c11', feature, '-O2', '-Wall', '-Wextra', '-Werror',
             'Tests/bc_astc_into.c', 'Source/BC1ToASTC.c', '-o', astc])
        run([astc])
        run([shutil.which('python3') or 'python3', 'Tests/bc_texture_production.py'])
        run([shutil.which('python3') or 'python3', 'Tests/precompiled_shaders.py'])
        # Exercise the actual production CPU paths without requiring a Metal
        # device; SDK compilation still runs below in the macOS build job.
        run([shutil.which('python3') or 'python3', 'Tests/archive_production.py'])
        run([shutil.which('python3') or 'python3', 'Tests/compressed_shadow_production.py'])
        run([shutil.which('python3') or 'python3', 'Tests/asset_worker_production.py'])
        run([shutil.which('python3') or 'python3', 'Tests/native_opus_production.py'])
        run([shutil.which('python3') or 'python3', 'Tests/no_temporal_probe.py'])
        futex = Path(work) / 'futex-waits'
        feature = '-D_DARWIN_C_SOURCE' if platform.system() == 'Darwin' else '-D_GNU_SOURCE'
        run([cc, '-std=c11', feature, '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DPG_FUTEX_TESTING',
             '-I', 'Tests/futex_support', '-I', 'Source', '-include', 'Tests/futex_support/platform.h',
             'Source/WasmFutex.c', 'Tests/futex_waits.c', '-o', futex])
        run([futex])
        for path in ['App/GameSettings.swift', 'App/GraphicsOptions.swift', 'App/SettingsViewController.swift',
                     'Source/NativeGraphics.h', 'Source/MetalRenderer/MetalRenderer.h',
                     'Source/MetalRenderer/MRRenderer.mm', 'Scripts/prepare.py']:
            content = (ROOT / path).read_text()
            for removed in ['frameGeneration', 'FrameGeneration', 'FRAME_SCENE_DONE', 'prepare_frame_scene', 'AI frame generation']:
                if removed in content:
                    raise SystemExit(f'Removed frame-generation code remains: {path}: {removed}')
        if platform.system() == 'Darwin':
            swift = shutil.which('swiftc')
            if not swift:
                raise SystemExit('Swift compiler required for the iOS preflight.')
            run(['bash', 'Tests/run_bike_input_axes.sh'])
            target = Path(work) / 'cache-maintenance'
            run([swift, '-parse-as-library', 'App/CacheMaintenance.swift',
                 'Tests/cache_maintenance.swift', '-o', target])
            run([target])
            # Parse every app file before spending time generating/compiling
            # the engine; UIKit/API type checking remains in the real SDK build.
            run([swift, '-frontend', '-parse', *sorted(ROOT.glob('App/**/*.swift'))])
            simulator_sdk = subprocess.check_output(
                ['xcrun', '--sdk', 'iphonesimulator', '--show-sdk-path'], text=True).strip()
            # Type-check the production touch controls with the engine stubs and
            # regression scenario. Keep this in preflight so the harness can't
            # silently drift out of sync with PGControlHints or production APIs.
            # Concatenate the harness with its source so its extension can inspect
            # TouchControlsView's private state, as the simulator runner does.
            controls_harness = Path(work) / 'touch-controls-regression.swift'
            controls_harness.write_text(
                (ROOT / 'App/TouchControlsView.swift').read_text() + '\n' +
                (ROOT / 'Tests/airplane_controls.swift').read_text())
            run([swift, '-typecheck', '-parse-as-library', '-swift-version', '5',
                 '-module-name', 'TouchControlsRegression', '-target', 'arm64-apple-ios16.0-simulator',
                 '-sdk', simulator_sdk, str(controls_harness), 'App/VirtualPad.swift',
                 'App/BikeInputAxes.swift'])
            sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
            run([swift, '-typecheck', '-parse-as-library', '-swift-version', '5',
                 '-module-name', 'PlayGTA5Native', '-target', 'arm64-apple-ios16.0', '-sdk', sdk,
                 '-import-objc-header', 'Source/Native-Bridging-Header.h',
                 '-I', 'Source', '-I', 'App/GameInstaller', *sorted(ROOT.glob('App/**/*.swift'))])
            run(['xcrun', '--sdk', 'iphoneos', 'clang++', '-fsyntax-only', '-std=c++20',
                 '-fobjc-arc', '-fblocks', '-target', 'arm64-apple-ios16.0', '-isysroot', sdk,
                 '-I', 'Source', 'Source/MetalRenderer/MRRenderer.mm'])
            run(['xcrun', '--sdk', 'iphoneos', 'clang++', '-fsyntax-only', '-std=c++20',
                 '-fobjc-arc', '-fblocks', '-target', 'arm64-apple-ios16.0', '-isysroot', sdk,
                 '-I', 'Source', 'Source/MetalRenderer/MRFrameReports.mm'])
        else:
            print('Swift/UIKit checks deferred to the same macOS build job.', flush=True)
    print('Performance/cache preflight passed.', flush=True)

if __name__ == '__main__':
    main()
