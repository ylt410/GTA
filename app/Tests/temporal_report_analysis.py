"""Synthetic safety and summary tests for the offline temporal-report analyzer."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Scripts"))
import analyze_temporal_diagnostics as analyzer


def record(frame: int, candidate: int, *, palette: int = 0, uniform: int = 0,
           root_matrix: int = 0, flags: int = 0, stage: int = 101,
           opcode: int = 32, camera_valid: int = 1) -> dict:
    return {
        "frame": frame,
        "opcode": opcode,
        "sequence": f"{frame:016x}",
        "thread": 1,
        "stage": stage,
        "entityClass": 1 if candidate else 0,
        "candidateEntityToken": candidate,
        "entityToken": candidate + 100 if candidate else 0,
        "auxiliaryToken": 0,
        "rootMatrixToken": root_matrix,
        "paletteToken": palette,
        "scopeDepth": 2 if candidate else 1,
        "tagFlags": 0,
        "camera": {
            "viewportToken": 8,
            "valid": camera_valid,
            "size": [1600, 900],
            "nearFar": [0.1, 1000.0],
            "viewCandidate": [0.0] * 16,
            "projectionCandidate": [1.0] * 16,
        },
        "draw": [3, 36, 1],
        "shaderTokens": [11, 12],
        "layoutToken": 13,
        "color": {
            "tokens": [21, 22, 0, 0],
            "formats": [1, 1, 0, 0],
            "widths": [1280, 1280, 0, 0],
            "heights": [720, 720, 0, 0],
            "samples": [1, 1, 0, 0],
        },
        "depth": {"token": 31, "format": 2, "width": 1280, "height": 720,
                  "samples": 1, "mip": 0, "slice": 0},
        "sourcePresentationTexture": {"token": 40, "width": 1600, "height": 900, "format": 1},
        "pixelShaderInputs": {
            "slots": [0, 1, 2, 3], "tokens": [0, 0, 0, 0],
            "formats": [0, 0, 0, 0], "widths": [0, 0, 0, 0], "heights": [0, 0, 0, 0],
        },
        "viewport": [0.0, 0.0, 1280.0, 720.0],
        "uniform": {
            "tokens": [uniform, 0, 0, 0], "bytesHashed": [64, 0, 0, 0],
            "firstRows": [4, 0, 0, 0], "hashes": ["0123456789abcdef"] * 4,
        },
        "wvpCandidate": [1.0] * 16,
        "flags": flags,
    }


def report(records: list[dict] | None = None) -> dict:
    return {
        "schema": 1,
        "purpose": "diagnostics only",
        "engineSHA256": "11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0",
        "baseCommit": "b" * 40,
        "session": "0123456789abcdef",
        "sourcePresentCount": 3,
        "privacy": "camera data is game space",
        "limitations": "sampled candidate IDs",
        "uniformSlots": [1, 2, 3, 0],
        "uniformHashByteLimit": 4096,
        "stats": {
            "claimed": 4, "published": 4, "matched": 3, "misses": 17,
            "collisions": 0, "overflow": 0, "invalidCamera": 0,
            "scopeErrors": 0, "tokenOverflow": 0, "truncatedRecords": 0,
            "recordOverflow": 0,
            "invalidRecord": 0, "threadOverflow": 0, "shaderManifestOverflow": 0,
        },
        "shaderManifest": [{"token": 11, "hash": "cafebabe", "label": "scene_vs"}],
        "records": records if records is not None else [
            record(1, 7, palette=9, root_matrix=6, uniform=17),
            record(2, 7, palette=9, root_matrix=6, uniform=17),
            record(2, 8, palette=9, root_matrix=7, uniform=17, flags=16,
                   stage=105, opcode=7),
        ],
    }


def probe_frame(frame: int = 1, *, status: str = "ok") -> dict:
    return {
        "frame": frame, "width": 1600, "height": 900, "depthToken": 31,
        "depthFormat": 2, "sceneColorToken": 21, "uiColorToken": 22,
        "cameraVPResidual": 0.000001, "status": status,
        "validDepthSamples": 1200, "backgroundSamples": 500, "invalidDepthSamples": 604,
        "validMotionSamples": 900, "cpuGpuComparedSamples": 850,
        "cpuGpuMismatchSamples": 3, "maxErrorPixels": 0.25,
        "meanMotionPixels": 0.75, "maxMotionPixels": 4.0,
        "hudAfterBoundary": True,
    }


def camera_probe(requested: bool = True) -> dict:
    return {
        "requested": requested,
        "mode": "camera-only-depth-probe" if requested else "off",
        "session": "0123456789abcdef",
        "grid": [64, 36], "maximumDispatches": 12,
        "sourceFrames": 60 if requested else 0,
        "candidates": 4 if requested else 0,
        "cameraVerifiedFrames": 8 if requested else 0,
        "boundaryFrames": 7 if requested else 0,
        "scheduled": 3 if requested else 0,
        "completed": 1 if requested else 0,
        "gpuFailures": 1 if requested else 0, "busySkips": 1 if requested else 0,
        "contractRejects": 0, "historyResets": 2 if requested else 0,
        "pending": 1 if requested else 0,
        "partialFrame": False, "timedOut": requested, "readyForTemporal": False,
        "limitations": "Camera-only reprojection; not actor motion or reconstruction readiness.",
        "rejections": {"no-scene-depth": 1} if requested else {},
        "frames": [probe_frame(2, status="timed-out"), probe_frame(4),
                   probe_frame(5, status="gpu-failed")] if requested else [],
    }


def report_schema2() -> dict:
    value = report()
    value["schema"] = 2
    value["cameraLayoutVersion"] = 2
    value["baseCommitRole"] = "baseline source revision"
    value["buildCommit"] = "c" * 40
    value["tokenSemantics"] = (
        "Opaque anonymous token numbers are not raw identities; transient mapping eviction can split continuity."
    )
    value["cameraProbe"] = camera_probe()
    value["stats"].update({
        "quotaDroppedRecords": 25,
        "transientTokenEvictions": 5,
        "resourceTokenOverflow": 0,
        "candidateTokenOverflow": 0,
        "transientTokenOverflow": 0,
        "downsampledFrames": 2,
    })
    return value


def encode(value: dict) -> bytes:
    return json.dumps(value, allow_nan=True).encode("utf-8")


class TemporalReportAnalysisTest(unittest.TestCase):
    def test_candidate_continuity_aliases_and_attachment_candidates(self):
        summary = analyzer.analyze_report_bytes(encode(report()))
        entities = summary["candidateEntityTokenCoverage"]
        self.assertEqual(entities["distinctNonzeroCandidates"], 2)
        self.assertEqual(entities["candidateCoveredFrameIndices"], 2)
        self.assertEqual(entities["mostRepeatedCandidates"][0]["candidateEntityToken"], 7)
        self.assertEqual(entities["mostRepeatedCandidates"][0]["longestConsecutiveFrameRun"], 2)

        aliases = summary["sharedTokenAliases"]
        self.assertEqual(aliases["paletteTokensAcrossCandidateEntities"][0]["token"], 9)
        self.assertEqual(aliases["uniformTokensAcrossCandidateEntities"][0]["token"], 17)
        root_matrix = summary["rootMatrixCandidateTokens"]
        self.assertEqual(root_matrix["distinctTokens"], 2)
        self.assertEqual(root_matrix["recordsAlsoWithPaletteToken"], 3)
        self.assertEqual(root_matrix["distinctRootMatrixPaletteTokenPairs"], [
            {"rootMatrixToken": 6, "paletteToken": 9, "observations": 2},
            {"rootMatrixToken": 7, "paletteToken": 9, "observations": 1},
        ])
        source_texture = summary["sourcePresentationTextureCandidate"]
        self.assertEqual(source_texture["distinctTokens"], 1)
        self.assertIn("before Spatial", source_texture["note"])
        self.assertIn("not the CAMetalDrawable", source_texture["note"])
        attachments = summary["attachmentCompatibilityCandidates"]
        self.assertEqual(attachments["sameWidthHeightSampleColorPairs"], 2)
        self.assertEqual(attachments["recordsWithKnownDepthAndColorCandidates"], 2)
        self.assertEqual(attachments["depthWidthHeightSampleMatchAnyColor"], 2)
        self.assertEqual(attachments["omittedSubresourceComparisons"], 1)
        self.assertFalse(summary["readyForTemporal"])
        self.assertIn("motion-vector", summary["temporalReadinessReason"])
        self.assertIn("camera-to-world", summary["cameraLayoutWarning"])
        self.assertIn("offset 192", summary["cameraLayoutWarning"])
        self.assertIn("not inverted", summary["cameraLayoutWarning"])
        self.assertIn("not a dropped-frame rate", summary["lossAndUnknowns"]["missesMeaning"])
        self.assertNotIn("fps", json.dumps(summary).lower())

    def test_camera_missing_values_are_not_counted_as_valid(self):
        value = report([record(1, 0, camera_valid=1)])
        value["records"][0]["camera"]["viewCandidate"] = [None] * 16
        summary = analyzer.analyze_report_bytes(encode(value))
        camera = summary["cameraCandidateChecks"]
        self.assertEqual(camera["claimedValidRecords"], 1)
        self.assertEqual(camera["finiteRangeAndSizeCandidateRecords"], 0)
        self.assertEqual(camera["finiteViewMatrixRecords"], 0)

    def test_missing_camera_object_is_rejected(self):
        value = report([record(1, 0)])
        del value["records"][0]["camera"]
        with self.assertRaisesRegex(analyzer.ReportError, "missing required fields: camera"):
            analyzer.analyze_report_bytes(encode(value))

    def test_bad_array_size_and_schema_are_rejected(self):
        value = report([record(1, 1)])
        value["records"][0]["color"]["widths"] = [1, 2]
        with self.assertRaisesRegex(analyzer.ReportError, "length 4"):
            analyzer.analyze_report_bytes(encode(value))
        value = report([])
        value["schema"] = 3
        with self.assertRaisesRegex(analyzer.ReportError, "unsupported schema"):
            analyzer.analyze_report_bytes(encode(value))

    def test_schema2_probe_sampling_and_build_metadata(self):
        summary = analyzer.analyze_report_bytes(encode(report_schema2()))
        self.assertEqual(summary["schema"], 2)
        self.assertEqual(summary["buildCommit"], "c" * 40)
        self.assertEqual(summary["baseCommitRole"], "baseline source revision")
        self.assertEqual(summary["cameraLayoutVersion"], 2)
        self.assertIn("offset 256", summary["cameraCandidateChecks"]["cameraLayoutNote"])
        self.assertIn("offset 320", summary["cameraCandidateChecks"]["cameraLayoutNote"])
        self.assertIn("transient mapping eviction", summary["tokenSemantics"])
        probe = summary["cameraProbe"]
        self.assertEqual(probe["mode"], "camera-only-depth-probe")
        self.assertFalse(probe["readyForTemporal"])
        self.assertEqual(probe["frames"][0]["validDepthSamples"], 1200)
        self.assertEqual(probe["rejections"]["no-scene-depth"], 1)
        sampling = summary["lossAndUnknowns"]["intentionalDiagnosticSampling"]
        self.assertEqual(sampling["quotaDroppedRecords"], 25)
        self.assertEqual(sampling["downsampledFrames"], 2)
        self.assertIn("not missed or dropped game frames", sampling["note"])
        self.assertEqual(summary["collectorStats"]["transientTokenEvictions"], 5)
        self.assertTrue(any("Transient identity mappings were evicted" in warning
                            for warning in summary["lossAndUnknowns"]["warnings"]))
        self.assertFalse(summary["readyForTemporal"])
        self.assertIn("motion-vector", summary["temporalReadinessReason"])

    def test_schema2_default_off_and_unknown_build_commit(self):
        value = report_schema2()
        value["buildCommit"] = "unknown"
        value["cameraProbe"] = camera_probe(requested=False)
        summary = analyzer.analyze_report_bytes(encode(value))
        self.assertEqual(summary["buildCommit"], "unknown")
        self.assertFalse(summary["cameraProbe"]["requested"])
        self.assertFalse(summary["cameraProbe"]["partialFrame"])
        self.assertEqual(summary["cameraProbe"]["frames"], [])
        self.assertEqual(summary["cameraProbe"]["sourceFrames"], 0)
        value["cameraProbe"]["partialFrame"] = True
        with self.assertRaisesRegex(analyzer.ReportError, "unrequested cameraProbe"):
            analyzer.analyze_report_bytes(encode(value))

    def test_schema2_partial_source_frame_boundary_is_allowed_only_when_marked(self):
        value = report_schema2()
        probe = value["cameraProbe"]
        probe["partialFrame"] = True
        probe["boundaryFrames"] = probe["sourceFrames"] + 1
        probe["frames"][-1]["frame"] = probe["sourceFrames"] + 1
        summary = analyzer.analyze_report_bytes(encode(value))
        self.assertTrue(summary["cameraProbe"]["partialFrame"])
        self.assertEqual(summary["cameraProbe"]["frames"][-1]["frame"], 61)

        value["cameraProbe"]["partialFrame"] = False
        with self.assertRaisesRegex(analyzer.ReportError, "boundaryFrames exceeds"):
            analyzer.analyze_report_bytes(encode(value))

        value["cameraProbe"]["boundaryFrames"] = value["cameraProbe"]["sourceFrames"]
        with self.assertRaisesRegex(analyzer.ReportError, "frame exceeds sourceFrames plus partialFrame"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["partialFrame"] = True
        value["cameraProbe"]["cameraVerifiedFrames"] = value["cameraProbe"]["sourceFrames"] + 1
        with self.assertRaisesRegex(analyzer.ReportError, "cameraVerifiedFrames exceeds"):
            analyzer.analyze_report_bytes(encode(value))

    def test_schema2_dispatch_ledger_and_timeout_are_consistent(self):
        value = report_schema2()
        value["cameraProbe"]["pending"] = 0
        value["cameraProbe"]["timedOut"] = False
        with self.assertRaisesRegex(analyzer.ReportError, "ledger does not balance"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["timedOut"] = False
        with self.assertRaisesRegex(analyzer.ReportError, "timedOut must be true"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["scheduled"] = 2
        with self.assertRaisesRegex(analyzer.ReportError, "scheduled must equal"):
            analyzer.analyze_report_bytes(encode(value))

    def test_schema2_rejects_missing_stats_bad_build_and_probe_contract(self):
        value = report_schema2()
        del value["stats"]["candidateTokenOverflow"]
        with self.assertRaisesRegex(analyzer.ReportError, "stats missing required fields"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        del value["cameraLayoutVersion"]
        with self.assertRaisesRegex(analyzer.ReportError, "missing required fields: cameraLayoutVersion"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraLayoutVersion"] = 1
        with self.assertRaisesRegex(analyzer.ReportError, "cameraLayoutVersion must be 2"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["buildCommit"] = "not-a-commit"
        with self.assertRaisesRegex(analyzer.ReportError, "buildCommit"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["readyForTemporal"] = True
        with self.assertRaisesRegex(analyzer.ReportError, "must be false"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["frames"][0]["validMotionSamples"] = 2305
        with self.assertRaisesRegex(analyzer.ReportError, "<= 2304"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        del value["cameraProbe"]["frames"][0]["cpuGpuMismatchSamples"]
        with self.assertRaisesRegex(analyzer.ReportError, "cpuGpuMismatchSamples"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["frames"] *= 7
        with self.assertRaisesRegex(analyzer.ReportError, "at most 12"):
            analyzer.analyze_report_bytes(encode(value))

    def test_schema2_rejects_bad_probe_session_counts_and_commit_role(self):
        value = report_schema2()
        value["cameraProbe"]["session"] = "ffffffffffffffff"
        with self.assertRaisesRegex(analyzer.ReportError, "match the root session"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["scheduled"] = 13
        with self.assertRaisesRegex(analyzer.ReportError, "exceeds maximumDispatches"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["baseCommitRole"] = 123
        with self.assertRaisesRegex(analyzer.ReportError, "baseCommitRole"):
            analyzer.analyze_report_bytes(encode(value))

        value = report_schema2()
        value["cameraProbe"]["rejections"] = {"/tmp/private-pointer": 1}
        with self.assertRaisesRegex(analyzer.ReportError, "reason codes"):
            analyzer.analyze_report_bytes(encode(value))

    def test_nonfinite_and_duplicate_keys_are_rejected(self):
        value = report([record(1, 1)])
        value["records"][0]["viewport"][0] = float("nan")
        with self.assertRaisesRegex(analyzer.ReportError, "non-standard JSON number"):
            analyzer.analyze_report_bytes(encode(value))
        duplicate = b'{"schema":1,"schema":1}'
        with self.assertRaisesRegex(analyzer.ReportError, "duplicate JSON object key"):
            analyzer.analyze_report_bytes(duplicate)
        huge = report([record(1, 1)])
        huge["records"][0]["viewport"][0] = 10 ** 400
        with self.assertRaisesRegex(analyzer.ReportError, "must be finite"):
            analyzer.analyze_report_bytes(encode(huge))

    def test_boolean_is_not_accepted_as_an_integer_token(self):
        value = report([record(1, 1)])
        value["records"][0]["stage"] = True
        with self.assertRaisesRegex(analyzer.ReportError, "unsigned integer"):
            analyzer.analyze_report_bytes(encode(value))

    def test_unknown_fields_and_truncation_signals_are_reported(self):
        value = report([record(1, 2)])
        value["newPlatformField"] = "future"
        value["records"][0]["futureTag"] = 3
        value["fileOmittedRecords"] = 4
        value["stats"]["truncatedRecords"] = 2
        summary = analyzer.analyze_report_bytes(encode(value))
        unknown = summary["lossAndUnknowns"]["unknownFieldSummary"]
        self.assertEqual(unknown["countAtLeast"], 2)
        self.assertIn("root.newPlatformField", unknown["fields"])
        self.assertIn("records[0].futureTag", unknown["fields"])
        self.assertEqual(summary["lossAndUnknowns"]["truncationAndOverflowSignals"]["fileOmittedRecords"], 4)
        self.assertTrue(summary["lossAndUnknowns"]["warnings"])

    def test_record_limit_and_input_limit(self):
        value = report([record(1, 1)] * (analyzer.MAX_RECORDS + 1))
        with self.assertRaisesRegex(analyzer.ReportError, "at most 2048"):
            analyzer.analyze_report_bytes(encode(value))
        with self.assertRaisesRegex(analyzer.ReportError, "exceeds"):
            analyzer.analyze_report_bytes(b" " * (analyzer.MAX_FILE_BYTES + 1))


if __name__ == "__main__":
    unittest.main()
