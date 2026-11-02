#!/usr/bin/env python3
"""Guard the production build against restoring the retired diagnostic probe."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RETIRED_FILES = (
    "App/TemporalDiagnosticsViewController.swift",
    "Scripts/analyze_temporal_diagnostics.py",
    "Scripts/prepare_temporal_diagnostics.py",
    "Source/NativeTemporalCameraProbe.h",
    "Source/NativeTemporalCameraProbe.mm",
    "Source/NativeTemporalDiagnostics.cpp",
    "Source/NativeTemporalDiagnostics.h",
    "Source/NativeTemporalDiagnosticsPlatform.h",
    "Source/NativeTemporalDiagnosticsPlatform.mm",
    "Source/NativeTemporalEngine.inc",
    "Source/NativeTemporalProbeKernel.h",
    "Source/NativeTemporalReprojection.h",
    "Tests/temporal_camera_probe.mm",
    "Tests/temporal_diagnostics.cpp",
    "Tests/temporal_diagnostics_patch.py",
    "Tests/temporal_engine_bridge.c",
    "Tests/temporal_packet_production.py",
    "Tests/temporal_report_analysis.py",
    "Tests/temporal_reprojection.cpp",
    "../docs/TEMPORAL_CAMERA_PROTOTYPE_2026-10-31.md",
    "../docs/TEMPORAL_DIAGNOSTICS_2026-10-31.md",
)
TOKENS = (
    "PGTemporalDiagnostics",
    "PGTDProbe",
    "PGTDRecordMetal",
    "PGTDConsumePacket",
    "NativeTemporalDiagnostics",
    "NativeTemporalCameraProbe",
    "TemporalDiagnosticsViewController",
    "PGDiagnosticBuildSHA",
    "PGTDEnabled",
    "PGTDInvalidateThread",
    "TEMPORAL_DIAGNOSTICS_BUILD",
)

remaining_files = [name for name in RETIRED_FILES if (ROOT / name).exists()]
if remaining_files:
    raise SystemExit("Retired temporal probe files remain: " + ", ".join(remaining_files))

source_roots = (ROOT / "App", ROOT / "Scripts", ROOT / "Source")
violations = []
for source_root in source_roots:
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix not in {".h", ".c", ".cc", ".cpp", ".m", ".mm", ".swift", ".py", ".yml", ".sh"}:
            continue
        # The archive validator must name retired symbols to reject them.
        if path == Path(__file__) or path == ROOT / "Scripts/validate_private_ipa.py":
            continue
        try:
            content = path.read_text(errors="ignore")
        except OSError:
            continue
        for token in TOKENS:
            if token in content:
                violations.append(f"{path.relative_to(ROOT)}: {token}")
if violations:
    raise SystemExit("Retired temporal probe hooks remain:\n" + "\n".join(violations))

print("PASS: no runtime temporal camera/depth probe or diagnostic hooks remain")
