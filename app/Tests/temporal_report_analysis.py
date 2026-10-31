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
        "engineSHA256": "a" * 64,
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
        value["schema"] = 2
        with self.assertRaisesRegex(analyzer.ReportError, "unsupported schema"):
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
