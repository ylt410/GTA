#!/usr/bin/env python3
"""Safely summarize bounded schema-1/2 Temporal diagnostics JSON reports.

This is a local, read-only analysis tool. It does not load executable data, access the
network, or infer temporal reconstruction validity from renderer metadata.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import re
import sys
from typing import Any

MAX_FILE_BYTES = 4 * 1024 * 1024
MAX_RECORDS = 2048
MAX_MANIFEST = 256
MAX_UNKNOWN_FIELDS = 64
SUBRESOURCE_COMPARISON_OMIT_MASK = 1 << 4
LEGACY_CAMERA_ENGINE_SHA256 = "11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0"
_HEX16 = re.compile(r"^[0-9a-fA-F]{16}$")
_PROBE_REJECTION_KEY = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")

STAGE_NAMES = {
    100: "HUD render",
    101: "tone-map pass",
    102: "static post-FX depth pass",
    103: "scene render-target lock",
    104: "scene depth-copy lock",
    105: "UI render-target lock",
    106: "deferred-render start",
    107: "deferred-render end",
    108: "forward-render start",
    109: "forward-render end",
}
OP_NAMES = {7: "PRESENT", 26: "stage-change sample", 32: "DRAW"}

ROOT_REQUIRED = {
    "schema", "purpose", "engineSHA256", "baseCommit", "session",
    "sourcePresentCount", "privacy", "limitations", "uniformSlots",
    "uniformHashByteLimit", "stats", "shaderManifest", "records",
}
STAT_FIELDS = {
    "claimed", "published", "matched", "misses", "collisions", "overflow",
    "invalidCamera", "scopeErrors", "tokenOverflow", "truncatedRecords",
    "recordOverflow", "invalidRecord", "threadOverflow", "shaderManifestOverflow",
}
SCHEMA2_STAT_FIELDS = {
    "quotaDroppedRecords", "transientTokenEvictions", "resourceTokenOverflow",
    "candidateTokenOverflow", "transientTokenOverflow", "downsampledFrames",
}
CAMERA_PROBE_FIELDS = {
    "requested", "mode", "session", "grid", "maximumDispatches", "sourceFrames",
    "candidates", "cameraVerifiedFrames", "boundaryFrames", "scheduled", "completed",
    "gpuFailures", "busySkips", "contractRejects", "historyResets", "pending",
    "partialFrame", "timedOut", "readyForTemporal", "limitations", "rejections", "frames",
}
CAMERA_PROBE_FRAME_FIELDS = {
    "frame", "width", "height", "depthToken", "depthFormat", "sceneColorToken",
    "uiColorToken", "cameraVPResidual", "status", "validDepthSamples",
    "backgroundSamples", "invalidDepthSamples", "validMotionSamples",
    "cpuGpuComparedSamples", "cpuGpuMismatchSamples", "maxErrorPixels",
    "meanMotionPixels", "maxMotionPixels", "hudAfterBoundary",
}
RECORD_FIELDS = {
    "frame", "opcode", "sequence", "thread", "stage", "entityClass",
    "candidateEntityToken", "entityToken", "auxiliaryToken", "rootMatrixToken", "paletteToken",
    "scopeDepth", "tagFlags", "camera", "draw", "shaderTokens", "layoutToken",
    "color", "depth", "sourcePresentationTexture", "pixelShaderInputs", "viewport", "uniform",
    "wvpCandidate", "flags",
}
CAMERA_FIELDS = {"viewportToken", "valid", "size", "nearFar", "viewCandidate", "projectionCandidate"}
COLOR_FIELDS = {"tokens", "formats", "widths", "heights", "samples"}
DEPTH_FIELDS = {"token", "format", "width", "height", "samples", "mip", "slice"}
SOURCE_PRESENTATION_FIELDS = {"token", "width", "height", "format"}
PIXEL_INPUT_FIELDS = {"slots", "tokens", "formats", "widths", "heights"}
UNIFORM_FIELDS = {"tokens", "bytesHashed", "firstRows", "hashes"}


class ReportError(ValueError):
    """Invalid or unsupported report input."""


def _pairs_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReportError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ReportError(f"non-standard JSON number: {value}")


def _object(value: Any, where: str) -> dict[str, Any]:
    if type(value) is not dict or any(type(key) is not str for key in value):
        raise ReportError(f"{where} must be a JSON object with string keys")
    return value


def _required(obj: dict[str, Any], keys: set[str], where: str) -> None:
    missing = sorted(keys - obj.keys())
    if missing:
        raise ReportError(f"{where} missing required fields: {', '.join(missing)}")


def _unknown(obj: dict[str, Any], allowed: set[str], where: str, sink: list[str]) -> None:
    for key in sorted(obj.keys() - allowed):
        if len(sink) < MAX_UNKNOWN_FIELDS:
            sink.append(f"{where}.{key}")


def _uint(value: Any, where: str, maximum: int = 0xFFFFFFFF) -> int:
    if type(value) is not int or value < 0 or value > maximum:
        raise ReportError(f"{where} must be an unsigned integer <= {maximum}")
    return value


def _bool(value: Any, where: str) -> bool:
    if type(value) is not bool:
        raise ReportError(f"{where} must be a boolean")
    return value


def _number_or_null(value: Any, where: str) -> float | None:
    if value is None:
        return None
    if type(value) not in (int, float):
        raise ReportError(f"{where} must be a finite number or null")
    try:
        number = float(value)
    except OverflowError as exc:
        raise ReportError(f"{where} must be finite") from exc
    if not math.isfinite(number):
        raise ReportError(f"{where} must be finite")
    return number


def _array(value: Any, length: int, where: str) -> list[Any]:
    if type(value) is not list or len(value) != length:
        raise ReportError(f"{where} must be an array of length {length}")
    return value


def _u32_array(value: Any, length: int, where: str) -> list[int]:
    return [_uint(item, f"{where}[{index}]") for index, item in enumerate(_array(value, length, where))]


def _float_array(value: Any, length: int, where: str) -> list[float | None]:
    return [_number_or_null(item, f"{where}[{index}]") for index, item in enumerate(_array(value, length, where))]


def _finite_number(value: Any, where: str) -> float:
    number = _number_or_null(value, where)
    if number is None:
        raise ReportError(f"{where} must be a finite number")
    return number


def _validate_camera_probe(value: Any, root_session: str, unknown: list[str]) -> dict[str, Any]:
    probe = _object(value, "cameraProbe")
    _required(probe, CAMERA_PROBE_FIELDS, "cameraProbe")
    _unknown(probe, CAMERA_PROBE_FIELDS, "cameraProbe", unknown)
    requested = _bool(probe["requested"], "cameraProbe.requested")
    mode = _string(probe["mode"], "cameraProbe.mode", 64)
    if mode not in ("off", "camera-only-depth-probe"):
        raise ReportError("cameraProbe.mode must be off or camera-only-depth-probe")
    probe_session = _string(probe["session"], "cameraProbe.session", 16)
    if not _HEX16.fullmatch(probe_session) or probe_session != root_session:
        raise ReportError("cameraProbe.session must match the root session")
    grid = _u32_array(probe["grid"], 2, "cameraProbe.grid")
    if grid != [64, 36]:
        raise ReportError("cameraProbe.grid must be [64, 36]")
    maximum_dispatches = _uint(probe["maximumDispatches"], "cameraProbe.maximumDispatches")
    if maximum_dispatches != 12:
        raise ReportError("cameraProbe.maximumDispatches must be 12")

    counter_names = (
        "sourceFrames", "candidates", "cameraVerifiedFrames", "boundaryFrames", "scheduled",
        "completed", "gpuFailures", "busySkips", "contractRejects", "historyResets", "pending",
    )
    counters = {name: _uint(probe[name], f"cameraProbe.{name}", maximum=0xFFFFFFFFFFFFFFFF)
                for name in counter_names}
    if counters["scheduled"] > maximum_dispatches:
        raise ReportError("cameraProbe.scheduled exceeds maximumDispatches")
    partial_frame = _bool(probe["partialFrame"], "cameraProbe.partialFrame")
    if counters["cameraVerifiedFrames"] > counters["sourceFrames"]:
        raise ReportError("cameraProbe.cameraVerifiedFrames exceeds sourceFrames")
    if counters["boundaryFrames"] > counters["sourceFrames"] + (1 if partial_frame else 0):
        raise ReportError("cameraProbe.boundaryFrames exceeds sourceFrames plus partialFrame")
    timed_out = _bool(probe["timedOut"], "cameraProbe.timedOut")
    ready = _bool(probe["readyForTemporal"], "cameraProbe.readyForTemporal")
    if ready:
        raise ReportError("cameraProbe.readyForTemporal must be false")
    limitations = _string(probe["limitations"], "cameraProbe.limitations", 2048)

    rejections = _object(probe["rejections"], "cameraProbe.rejections")
    if len(rejections) > 64:
        raise ReportError("cameraProbe.rejections may contain at most 64 entries")
    rejection_counts: dict[str, int] = {}
    for name, count in rejections.items():
        _string(name, "cameraProbe.rejections key", 64)
        if not _PROBE_REJECTION_KEY.fullmatch(name) or re.fullmatch(r"(?:0x)?[0-9a-fA-F]{8,16}", name):
            raise ReportError("cameraProbe.rejections keys must be bounded reason codes, not addresses or paths")
        rejection_counts[name] = _uint(count, f"cameraProbe.rejections.{name}", maximum=0xFFFFFFFFFFFFFFFF)

    raw_frames = probe["frames"]
    if type(raw_frames) is not list or len(raw_frames) > maximum_dispatches:
        raise ReportError("cameraProbe.frames must contain at most 12 entries")
    frames: list[dict[str, Any]] = []
    previous_frame = 0
    sample_count_names = (
        "validDepthSamples", "backgroundSamples", "invalidDepthSamples", "validMotionSamples",
        "cpuGpuComparedSamples", "cpuGpuMismatchSamples",
    )
    for index, raw_frame in enumerate(raw_frames):
        where = f"cameraProbe.frames[{index}]"
        frame = _object(raw_frame, where)
        _required(frame, CAMERA_PROBE_FRAME_FIELDS, where)
        _unknown(frame, CAMERA_PROBE_FRAME_FIELDS, where, unknown)
        frame_values = {
            name: _uint(frame[name], f"{where}.{name}")
            for name in ("frame", "width", "height", "depthToken", "depthFormat",
                         "sceneColorToken", "uiColorToken")
        }
        if frame_values["frame"] == 0 or frame_values["frame"] <= previous_frame:
            raise ReportError(f"{where}.frame values must be positive and strictly increasing")
        if frame_values["frame"] > counters["sourceFrames"] + (1 if partial_frame else 0):
            raise ReportError(f"{where}.frame exceeds sourceFrames plus partialFrame")
        previous_frame = frame_values["frame"]
        if frame_values["width"] == 0 or frame_values["height"] == 0:
            raise ReportError(f"{where}.width and height must be positive")
        residual = _finite_number(frame["cameraVPResidual"], f"{where}.cameraVPResidual")
        status = _string(frame["status"], f"{where}.status", 32)
        if status not in ("pending", "ok", "gpu-failed", "timed-out"):
            raise ReportError(f"{where}.status is not a supported probe status")
        samples = {name: _uint(frame[name], f"{where}.{name}", maximum=2304)
                   for name in sample_count_names}
        if samples["cpuGpuMismatchSamples"] > samples["cpuGpuComparedSamples"]:
            raise ReportError(f"{where}.cpuGpuMismatchSamples exceeds cpuGpuComparedSamples")
        frame_values.update(samples)
        frame_values["cameraVPResidual"] = residual
        for name in ("maxErrorPixels", "meanMotionPixels", "maxMotionPixels"):
            metric = _finite_number(frame[name], f"{where}.{name}")
            if metric < 0:
                raise ReportError(f"{where}.{name} must be nonnegative")
            frame_values[name] = metric
        frame_values["status"] = status
        frame_values["hudAfterBoundary"] = _bool(frame["hudAfterBoundary"], f"{where}.hudAfterBoundary")
        frames.append(frame_values)

    if counters["scheduled"] != len(frames):
        raise ReportError("cameraProbe.scheduled must equal the number of frame results")
    if counters["scheduled"] != counters["completed"] + counters["gpuFailures"] + counters["pending"]:
        raise ReportError("cameraProbe dispatch ledger does not balance")
    statuses = Counter(frame["status"] for frame in frames)
    if statuses["ok"] != counters["completed"] or statuses["gpu-failed"] != counters["gpuFailures"]:
        raise ReportError("cameraProbe frame statuses do not match completed/GPU-failure counts")
    if statuses["pending"] + statuses["timed-out"] != counters["pending"]:
        raise ReportError("cameraProbe pending/timed-out frame statuses do not match pending count")
    if timed_out != (counters["pending"] > 0):
        raise ReportError("cameraProbe.timedOut must be true exactly when pending work remains")

    if not requested:
        if mode != "off" or any(counters.values()) or partial_frame or timed_out or rejections or frames:
            raise ReportError("an unrequested cameraProbe must be off with zero counters and no frames")
    elif mode != "camera-only-depth-probe":
        raise ReportError("a requested cameraProbe must use camera-only-depth-probe mode")

    return {
        "requested": requested, "mode": mode, "session": probe_session, "grid": grid,
        "maximumDispatches": maximum_dispatches, **counters, "partialFrame": partial_frame,
        "timedOut": timed_out,
        "readyForTemporal": False, "limitations": limitations,
        "rejections": rejection_counts, "frames": frames,
    }


def _string(value: Any, where: str, max_length: int = 4096) -> str:
    if type(value) is not str or len(value) > max_length:
        raise ReportError(f"{where} must be a string of at most {max_length} characters")
    return value


def _validate_record(raw: Any, index: int, unknown: list[str]) -> dict[str, Any]:
    where = f"records[{index}]"
    record = _object(raw, where)
    _required(record, RECORD_FIELDS, where)
    _unknown(record, RECORD_FIELDS, where, unknown)

    for field in ("frame", "opcode", "thread", "stage", "entityClass", "candidateEntityToken",
                  "entityToken", "auxiliaryToken", "rootMatrixToken", "paletteToken", "scopeDepth", "tagFlags",
                  "layoutToken", "flags"):
        _uint(record[field], f"{where}.{field}")
    sequence = _string(record["sequence"], f"{where}.sequence", 16)
    if not _HEX16.fullmatch(sequence):
        raise ReportError(f"{where}.sequence must be a 16-character hexadecimal string")

    camera = _object(record["camera"], f"{where}.camera")
    _required(camera, CAMERA_FIELDS, f"{where}.camera")
    _unknown(camera, CAMERA_FIELDS, f"{where}.camera", unknown)
    _uint(camera["viewportToken"], f"{where}.camera.viewportToken")
    valid = _uint(camera["valid"], f"{where}.camera.valid")
    if valid not in (0, 1):
        raise ReportError(f"{where}.camera.valid must be 0 or 1")
    size = _u32_array(camera["size"], 2, f"{where}.camera.size")
    near_far = _float_array(camera["nearFar"], 2, f"{where}.camera.nearFar")
    view = _float_array(camera["viewCandidate"], 16, f"{where}.camera.viewCandidate")
    projection = _float_array(camera["projectionCandidate"], 16, f"{where}.camera.projectionCandidate")

    _u32_array(record["draw"], 3, f"{where}.draw")
    _u32_array(record["shaderTokens"], 2, f"{where}.shaderTokens")

    color = _object(record["color"], f"{where}.color")
    _required(color, COLOR_FIELDS, f"{where}.color")
    _unknown(color, COLOR_FIELDS, f"{where}.color", unknown)
    colors = {key: _u32_array(color[key], 4, f"{where}.color.{key}") for key in COLOR_FIELDS}

    depth = _object(record["depth"], f"{where}.depth")
    _required(depth, DEPTH_FIELDS, f"{where}.depth")
    _unknown(depth, DEPTH_FIELDS, f"{where}.depth", unknown)
    depth_info = {key: _uint(depth[key], f"{where}.depth.{key}") for key in DEPTH_FIELDS}

    source_presentation = _object(record["sourcePresentationTexture"], f"{where}.sourcePresentationTexture")
    _required(source_presentation, SOURCE_PRESENTATION_FIELDS, f"{where}.sourcePresentationTexture")
    _unknown(source_presentation, SOURCE_PRESENTATION_FIELDS, f"{where}.sourcePresentationTexture", unknown)
    source_presentation_info = {
        key: _uint(source_presentation[key], f"{where}.sourcePresentationTexture.{key}")
        for key in SOURCE_PRESENTATION_FIELDS
    }

    pixel_inputs = _object(record["pixelShaderInputs"], f"{where}.pixelShaderInputs")
    _required(pixel_inputs, PIXEL_INPUT_FIELDS, f"{where}.pixelShaderInputs")
    _unknown(pixel_inputs, PIXEL_INPUT_FIELDS, f"{where}.pixelShaderInputs", unknown)
    pixel = {key: _u32_array(pixel_inputs[key], 4, f"{where}.pixelShaderInputs.{key}") for key in PIXEL_INPUT_FIELDS}
    viewport = _float_array(record["viewport"], 4, f"{where}.viewport")
    wvp = _float_array(record["wvpCandidate"], 16, f"{where}.wvpCandidate")

    uniform = _object(record["uniform"], f"{where}.uniform")
    _required(uniform, UNIFORM_FIELDS, f"{where}.uniform")
    _unknown(uniform, UNIFORM_FIELDS, f"{where}.uniform", unknown)
    uniform_tokens = _u32_array(uniform["tokens"], 4, f"{where}.uniform.tokens")
    _u32_array(uniform["bytesHashed"], 4, f"{where}.uniform.bytesHashed")
    _u32_array(uniform["firstRows"], 4, f"{where}.uniform.firstRows")
    hashes = _array(uniform["hashes"], 4, f"{where}.uniform.hashes")
    for slot, value in enumerate(hashes):
        value = _string(value, f"{where}.uniform.hashes[{slot}]", 16)
        if not _HEX16.fullmatch(value):
            raise ReportError(f"{where}.uniform.hashes[{slot}] must be 16 hex characters")

    return {
        "frame": record["frame"], "opcode": record["opcode"], "stage": record["stage"],
        "candidate": record["candidateEntityToken"], "entity": record["entityToken"],
        "root_matrix": record["rootMatrixToken"], "palette": record["paletteToken"],
        "uniform_tokens": uniform_tokens, "flags": record["flags"],
        "camera": {
            "valid_bit": valid == 1, "viewport": camera["viewportToken"], "size": size,
            "near_far": near_far, "view_candidate": view, "projection_candidate": projection,
        },
        "color": colors, "depth": depth_info, "source_presentation": source_presentation_info,
        "pixel": pixel, "viewport_values": viewport, "wvp": wvp,
    }


def _load_and_validate(data: bytes) -> tuple[dict[str, Any], list[dict[str, Any]], list[str]]:
    if len(data) > MAX_FILE_BYTES:
        raise ReportError(f"input exceeds {MAX_FILE_BYTES} bytes")
    try:
        root_raw = json.loads(data.decode("utf-8"), object_pairs_hook=_pairs_no_duplicates,
                              parse_constant=_reject_constant)
    except (UnicodeDecodeError, ValueError, RecursionError) as exc:
        raise ReportError(f"invalid JSON: {exc}") from exc
    root = _object(root_raw, "root")
    schema = _uint(root.get("schema"), "schema", maximum=0x7FFFFFFF)
    if schema not in (1, 2):
        raise ReportError(f"unsupported schema version {schema}; expected 1 or 2")
    schema2_root_fields = {"baseCommitRole", "buildCommit", "cameraProbe", "cameraLayoutVersion"}
    _required(root, ROOT_REQUIRED | (schema2_root_fields if schema == 2 else set()), "root")
    unknown: list[str] = []
    root_allowed = ROOT_REQUIRED | {"fileOmittedRecords"}
    if schema == 2:
        root_allowed |= schema2_root_fields | {"tokenSemantics"}
    _unknown(root, root_allowed, "root", unknown)
    for field in ("purpose", "engineSHA256", "baseCommit", "privacy", "limitations"):
        _string(root[field], field)
    if len(root["engineSHA256"]) != 64 or not re.fullmatch(r"[0-9a-fA-F]{64}", root["engineSHA256"]):
        raise ReportError("engineSHA256 must be 64 hexadecimal characters")
    if len(root["baseCommit"]) != 40 or not re.fullmatch(r"[0-9a-fA-F]{40}", root["baseCommit"]):
        raise ReportError("baseCommit must be 40 hexadecimal characters")
    if schema == 2:
        _string(root["baseCommitRole"], "baseCommitRole", 512)
        build_commit = _string(root["buildCommit"], "buildCommit", 40)
        if build_commit != "unknown" and (len(build_commit) != 40 or
                                           not re.fullmatch(r"[0-9a-fA-F]{40}", build_commit)):
            raise ReportError("buildCommit must be 40 hexadecimal characters or 'unknown'")
        camera_layout_version = _uint(root["cameraLayoutVersion"], "cameraLayoutVersion")
        if camera_layout_version != 2:
            raise ReportError("cameraLayoutVersion must be 2 for schema 2")
        if "tokenSemantics" in root:
            _string(root["tokenSemantics"], "tokenSemantics", 1024)
    session = _string(root["session"], "session", 16)
    if not _HEX16.fullmatch(session):
        raise ReportError("session must be a 16-character hexadecimal string")
    present_count = _uint(root["sourcePresentCount"], "sourcePresentCount")
    _u32_array(root["uniformSlots"], 4, "uniformSlots")
    _uint(root["uniformHashByteLimit"], "uniformHashByteLimit")
    if "fileOmittedRecords" in root:
        _uint(root["fileOmittedRecords"], "fileOmittedRecords", maximum=MAX_RECORDS)

    stats = _object(root["stats"], "stats")
    stat_fields = STAT_FIELDS | (SCHEMA2_STAT_FIELDS if schema == 2 else set())
    _required(stats, stat_fields, "stats")
    _unknown(stats, stat_fields, "stats", unknown)
    for name in stat_fields:
        _uint(stats[name], f"stats.{name}", maximum=0xFFFFFFFFFFFFFFFF)
    camera_probe = _validate_camera_probe(root["cameraProbe"], session, unknown) if schema == 2 else None

    manifest = root["shaderManifest"]
    if type(manifest) is not list or len(manifest) > MAX_MANIFEST:
        raise ReportError(f"shaderManifest must be an array with at most {MAX_MANIFEST} entries")
    for index, item_raw in enumerate(manifest):
        item = _object(item_raw, f"shaderManifest[{index}]")
        _required(item, {"token", "hash", "label"}, f"shaderManifest[{index}]")
        _unknown(item, {"token", "hash", "label"}, f"shaderManifest[{index}]", unknown)
        _uint(item["token"], f"shaderManifest[{index}].token")
        _string(item["hash"], f"shaderManifest[{index}].hash", 32)
        _string(item["label"], f"shaderManifest[{index}].label", 96)

    raw_records = root["records"]
    if type(raw_records) is not list or len(raw_records) > MAX_RECORDS:
        raise ReportError(f"records must be an array with at most {MAX_RECORDS} entries")
    records = [_validate_record(item, index, unknown) for index, item in enumerate(raw_records)]
    normalized_root = {**root, "schema": schema, "stats": stats,
                       "sourcePresentCount": present_count}
    if schema == 2:
        normalized_root["cameraProbe"] = camera_probe
    return normalized_root, records, unknown


def _share_aliases(records: list[dict[str, Any]], field: str) -> list[dict[str, int]]:
    owners: dict[int, set[int]] = defaultdict(set)
    observations: Counter[int] = Counter()
    for row in records:
        token = row[field]
        owner = row["candidate"]
        if token and owner:
            owners[token].add(owner)
            observations[token] += 1
    shared = [
        {"token": token, "entityCount": len(entities), "observations": observations[token]}
        for token, entities in owners.items() if len(entities) > 1
    ]
    return sorted(shared, key=lambda item: (-item["entityCount"], -item["observations"], item["token"]))[:24]


def analyze_report_bytes(data: bytes) -> dict[str, Any]:
    root, records, unknown = _load_and_validate(data)
    schema = root["schema"]
    stats = root["stats"]
    source_count = root["sourcePresentCount"]
    stage_counts: Counter[int] = Counter(row["stage"] for row in records)
    opcode_counts: Counter[int] = Counter(row["opcode"] for row in records)
    stage_opcode: Counter[tuple[int, int]] = Counter((row["stage"], row["opcode"]) for row in records)
    recorded_frames = {row["frame"] for row in records if row["frame"] > 0}

    camera_candidates = 0
    camera_claimed_valid = 0
    valid_viewports: dict[int, set[tuple[int, int]]] = defaultdict(set)
    repeated_camera_groups: dict[tuple[int, int], set[tuple[Any, ...]]] = defaultdict(set)
    camera_group_observations: Counter[tuple[int, int]] = Counter()
    camera_finite_counts = {"nearFar": 0, "view": 0, "projection": 0}
    for row in records:
        camera = row["camera"]
        finite = (all(v is not None for v in camera["near_far"])
                  and all(v is not None for v in camera["view_candidate"])
                  and all(v is not None for v in camera["projection_candidate"]))
        if all(v is not None for v in camera["near_far"]):
            camera_finite_counts["nearFar"] += 1
        if all(v is not None for v in camera["view_candidate"]):
            camera_finite_counts["view"] += 1
        if all(v is not None for v in camera["projection_candidate"]):
            camera_finite_counts["projection"] += 1
        near, far = camera["near_far"]
        range_ok = near is not None and far is not None and near > 0 and far > near
        size_ok = all(value > 0 for value in camera["size"])
        if camera["valid_bit"]:
            camera_claimed_valid += 1
        if camera["valid_bit"] and finite and range_ok and size_ok:
            camera_candidates += 1
            if camera["viewport"]:
                valid_viewports[camera["viewport"]].add(tuple(camera["size"]))
                signature = (tuple(camera["size"]), tuple(camera["near_far"]),
                             tuple(camera["view_candidate"]), tuple(camera["projection_candidate"]))
                group = (camera["viewport"], row["frame"])
                repeated_camera_groups[group].add(signature)
                camera_group_observations[group] += 1
    stable_viewports = sum(1 for sizes in valid_viewports.values() if len(sizes) == 1)
    changed_viewports = sum(1 for sizes in valid_viewports.values() if len(sizes) > 1)
    stable_repeated_cameras = sum(1 for group, signatures in repeated_camera_groups.items()
                                  if camera_group_observations[group] > 1 and len(signatures) == 1)
    changed_repeated_cameras = sum(1 for group, signatures in repeated_camera_groups.items()
                                   if camera_group_observations[group] > 1 and len(signatures) > 1)

    candidate_frames: dict[int, set[int]] = defaultdict(set)
    for row in records:
        if row["candidate"] and row["frame"]:
            candidate_frames[row["candidate"]].add(row["frame"])
    token_rows: list[dict[str, int]] = []
    for token, frames in candidate_frames.items():
        ordered = sorted(frames)
        longest = current = 1
        for previous, frame in zip(ordered, ordered[1:]):
            current = current + 1 if frame == previous + 1 else 1
            longest = max(longest, current)
        token_rows.append({"candidateEntityToken": token, "recordedFrames": len(frames),
                           "longestConsecutiveFrameRun": longest})
    token_rows.sort(key=lambda item: (-item["recordedFrames"], -item["longestConsecutiveFrameRun"],
                                      item["candidateEntityToken"]))
    entity_frames_covered = {frame for frames in candidate_frames.values() for frame in frames}

    root_matrix_tokens: Counter[int] = Counter()
    root_palette_pairs: Counter[tuple[int, int]] = Counter()
    root_palette_coobservations = 0
    source_presentation_tokens: Counter[int] = Counter()
    source_presentation_sizes: Counter[tuple[int, int]] = Counter()
    for row in records:
        root_token, palette_token = row["root_matrix"], row["palette"]
        if root_token:
            root_matrix_tokens[root_token] += 1
        if root_token and palette_token:
            root_palette_coobservations += 1
            root_palette_pairs[(root_token, palette_token)] += 1
        source_texture = row["source_presentation"]
        if source_texture["token"]:
            source_presentation_tokens[source_texture["token"]] += 1
            if source_texture["width"] and source_texture["height"]:
                source_presentation_sizes[(source_texture["width"], source_texture["height"])] += 1

    depth_known = depth_match = depth_mismatch = 0
    color_pairs = color_matches = 0
    active_color_count = 0
    omitted_subresource_comparisons = 0
    for row in records:
        colors = row["color"]
        attachments = []
        for slot in range(4):
            width, height, samples = colors["widths"][slot], colors["heights"][slot], colors["samples"][slot]
            if colors["tokens"][slot] and width and height and samples:
                attachments.append((width, height, samples))
        active_color_count += len(attachments)
        depth = row["depth"]
        has_depth_candidate = bool(depth["token"] and depth["width"] and depth["height"] and depth["samples"])
        if row["flags"] & SUBRESOURCE_COMPARISON_OMIT_MASK:
            if attachments or has_depth_candidate:
                omitted_subresource_comparisons += 1
            continue
        for a in range(len(attachments)):
            for b in range(a + 1, len(attachments)):
                color_pairs += 1
                if attachments[a] == attachments[b]:
                    color_matches += 1
        if has_depth_candidate and attachments:
            depth_known += 1
            signature = (depth["width"], depth["height"], depth["samples"])
            if signature in attachments:
                depth_match += 1
            else:
                depth_mismatch += 1

    unknown_summary = {
        "countAtLeast": len(unknown), "fields": unknown,
        "truncated": len(unknown) == MAX_UNKNOWN_FIELDS,
    }
    truncation = {
        "fileOmittedRecords": root.get("fileOmittedRecords", 0),
        "collectorTruncatedRecords": stats["truncatedRecords"],
        "collectorRecordOverflow": stats["recordOverflow"],
        "samplingOverflow": stats["overflow"], "tokenOverflow": stats["tokenOverflow"],
        "shaderManifestOverflow": stats["shaderManifestOverflow"],
        "possibleSidebandCollisions": stats["collisions"],
        "claimedButNotPublished": max(0, stats["claimed"] - stats["published"]),
        "publishedButUnmatched": max(0, stats["published"] - stats["matched"]),
        "invalidRecords": stats["invalidRecord"],
        "scopeErrors": stats["scopeErrors"],
    }
    schema_warning = []
    if root.get("fileOmittedRecords", 0) or stats["truncatedRecords"] or stats["overflow"] or stats["tokenOverflow"]:
        schema_warning.append("Some records/tokens may be omitted or truncated; absence of a sample proves nothing.")
    if stats["collisions"]:
        schema_warning.append("Packet-table collisions were observed and may reduce metadata matches.")
    if truncation["claimedButNotPublished"] or truncation["publishedButUnmatched"] or stats["invalidRecord"]:
        schema_warning.append("Packet accounting has gaps; these are metadata-collection signals, not frame-loss or performance measurements.")
    if schema == 2 and stats["transientTokenEvictions"]:
        schema_warning.append("Transient identity mappings were evicted; the same object may receive a new opaque token later, splitting apparent continuity.")

    if schema == 1 and root["engineSHA256"].lower() == LEGACY_CAMERA_ENGINE_SHA256:
        camera_layout_note = (
            "Pinned schema-1 engine camera.viewCandidate is camera-to-world copied from legacy viewport offset 192, not a view matrix. "
            "The candidate values are retained as captured and are not inverted or relabeled; do not treat them as view without verified affine-rigid inversion."
        )
    elif schema == 1:
        camera_layout_note = (
            "Schema 1 has no explicit cameraLayoutVersion. camera.viewCandidate is retained as captured and is not transformed; "
            "verify the source layout before treating it as view."
        )
    else:
        camera_layout_note = (
            "cameraLayoutVersion 2 declares camera.viewCandidate as view at viewport offset 256 and projectionCandidate at offset 320; "
            "the analyzer keeps these as numerical candidates and does not transform them."
        )
    if schema == 1:
        schema_warning.insert(0, camera_layout_note)

    loss_summary = {
        "truncationAndOverflowSignals": truncation,
        "warnings": schema_warning,
        "unknownFieldSummary": unknown_summary,
        "missesMeaning": "Packet lookup misses include uninstrumented/unsampled commands and are not a dropped-frame rate or a performance measurement.",
    }
    if schema == 2:
        loss_summary["intentionalDiagnosticSampling"] = {
            "quotaDroppedRecords": stats["quotaDroppedRecords"],
            "downsampledFrames": stats["downsampledFrames"],
            "note": "These are intentional per-frame diagnostic sampling limits, not missed or dropped game frames and not an FPS measurement.",
        }
        loss_summary["tokenPoolSignals"] = {
            "resourceTokenOverflow": stats["resourceTokenOverflow"],
            "candidateTokenOverflow": stats["candidateTokenOverflow"],
            "transientTokenOverflow": stats["transientTokenOverflow"],
            "transientTokenEvictions": stats["transientTokenEvictions"],
            "note": "Opaque IDs do not reveal raw identities; bounded cache evictions can split token continuity, and zero means unavailable.",
        }

    unique_frame_count = len(recorded_frames)
    camera_rate = (camera_candidates / len(records)) if records else 0.0
    summary = {
        "schema": schema,
        "input": {"records": len(records), "sourcePresentCount": source_count,
                  "distinctRecordedFrameIndices": unique_frame_count,
                  "recordedFrameIndexMin": min(recorded_frames) if recorded_frames else None,
                  "recordedFrameIndexMax": max(recorded_frames) if recorded_frames else None},
        "stageOpcodeCoverage": [
            {"stage": stage, "stageName": STAGE_NAMES.get(stage, "unknown stage"),
             "opcode": opcode, "opcodeName": OP_NAMES.get(opcode, "unknown opcode"),
             "records": count}
            for (stage, opcode), count in sorted(stage_opcode.items())
        ],
        "stageCoverage": [
            {"stage": stage, "name": STAGE_NAMES.get(stage, "unknown stage"), "records": count}
            for stage, count in sorted(stage_counts.items())
        ],
        "opcodeCoverage": [
            {"opcode": opcode, "name": OP_NAMES.get(opcode, "unknown opcode"), "records": count}
            for opcode, count in sorted(opcode_counts.items())
        ],
        "cameraCandidateChecks": {
            "claimedValidRecords": camera_claimed_valid,
            "finiteRangeAndSizeCandidateRecords": camera_candidates,
            "candidateRateAmongRecords": camera_rate,
            "finiteNearFarRecords": camera_finite_counts["nearFar"],
            "finiteViewMatrixRecords": camera_finite_counts["view"],
            "finiteProjectionMatrixRecords": camera_finite_counts["projection"],
            "stableViewportTokenSizeCount": stable_viewports,
            "viewportTokenSizeChanges": changed_viewports,
            "sameViewportSameFrameStableMatrixGroups": stable_repeated_cameras,
            "sameViewportSameFrameChangedMatrixGroups": changed_repeated_cameras,
            "note": "Numeric/range plus repeated same-token/same-frame candidate checks only; camera semantics and projection correctness are not certified.",
            "cameraLayoutNote": camera_layout_note,
        },
        "candidateEntityTokenCoverage": {
            "distinctNonzeroCandidates": len(candidate_frames),
            "candidateRecords": sum(1 for row in records if row["candidate"]),
            "candidateCoveredFrameIndices": len(entity_frames_covered),
            "mostRepeatedCandidates": token_rows[:24],
            "note": "Session-local anonymous candidate token repetition is observational only, not generation-safe object identity or proof of complete draw coverage.",
        },
        "rootMatrixCandidateTokens": {
            "nonzeroObservations": sum(root_matrix_tokens.values()),
            "distinctTokens": len(root_matrix_tokens),
            "recordsAlsoWithPaletteToken": root_palette_coobservations,
            "distinctRootMatrixPaletteTokenPairs": [
                {"rootMatrixToken": root, "paletteToken": palette, "observations": count}
                for (root, palette), count in sorted(root_palette_pairs.items(),
                    key=lambda pair: (-pair[1], pair[0][0], pair[0][1]))[:24]
            ],
            "note": "Root Matrix34 and grmMatrixSet/palette fields are separately observed anonymous candidates; co-occurrence does not prove a GPU palette mapping or temporal history.",
        },
        "sharedTokenAliases": {
            "paletteTokensAcrossCandidateEntities": _share_aliases(records, "palette"),
            "uniformTokensAcrossCandidateEntities": _uniform_aliases(records),
            "note": "Shared tokens are aliases in sampled metadata only; they do not prove shared or incorrect GPU data semantics.",
        },
        "attachmentCompatibilityCandidates": {
            "recordsWithActiveColorAttachments": sum(1 for row in records if any(row["color"]["tokens"][i] for i in range(4))),
            "activeColorAttachmentObservations": active_color_count,
            "colorAttachmentPairsCompared": color_pairs,
            "sameWidthHeightSampleColorPairs": color_matches,
            "recordsWithKnownDepthAndColorCandidates": depth_known,
            "depthWidthHeightSampleMatchAnyColor": depth_match,
            "depthKnownButNoColorMatch": depth_mismatch,
            "omittedSubresourceComparisons": omitted_subresource_comparisons,
            "note": "Dimension/sample-count comparisons only when base mip/slice is known. Records flagged for nonbase mip/slice or skipped subresources are omitted because base width/height cannot establish compatibility; no attachment is identified as main color, scene depth, or temporal input.",
        },
        "sourcePresentationTextureCandidate": {
            "nonzeroObservations": sum(source_presentation_tokens.values()),
            "distinctTokens": len(source_presentation_tokens),
            "observedSizes": [
                {"width": width, "height": height, "observations": count}
                for (width, height), count in sorted(source_presentation_sizes.items())
            ],
            "note": "This is the engine source-presentation texture before Spatial processing, not the CAMetalDrawable/display texture.",
        },
        "collectorStats": {name: stats[name] for name in sorted(stats)},
        "lossAndUnknowns": loss_summary,
        "readyForTemporal": False,
        "temporalReadinessReason": "This report contains no validated per-pixel motion-vector stream for actor motion; camera-only reprojection, matrix, depth, token, or attachment candidates are not proof of actor motion, jitter, disocclusion handling, or complete coverage.",
        "performanceOrQualityClaims": "Not measured; this summary does not calculate performance or image-quality percentages.",
    }
    if schema == 2:
        summary["baseCommit"] = root["baseCommit"]
        summary["baseCommitRole"] = root["baseCommitRole"]
        summary["buildCommit"] = root["buildCommit"]
        summary["cameraLayoutVersion"] = root["cameraLayoutVersion"]
        summary["cameraProbe"] = root["cameraProbe"]
        if "tokenSemantics" in root:
            summary["tokenSemantics"] = root["tokenSemantics"]
    else:
        summary["cameraLayoutWarning"] = camera_layout_note
    return summary


def _uniform_aliases(records: list[dict[str, Any]]) -> list[dict[str, int]]:
    owners: dict[int, set[int]] = defaultdict(set)
    observations: Counter[int] = Counter()
    for row in records:
        if not row["candidate"]:
            continue
        for token in set(row["uniform_tokens"]):
            if token:
                owners[token].add(row["candidate"])
                observations[token] += 1
    shared = [
        {"token": token, "entityCount": len(entities), "observations": observations[token]}
        for token, entities in owners.items() if len(entities) > 1
    ]
    return sorted(shared, key=lambda item: (-item["entityCount"], -item["observations"], item["token"]))[:24]


def analyze_file(path: Path) -> dict[str, Any]:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise ReportError(f"cannot stat input file: {exc}") from exc
    if size > MAX_FILE_BYTES:
        raise ReportError(f"input exceeds {MAX_FILE_BYTES} bytes")
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_FILE_BYTES + 1)
    except OSError as exc:
        raise ReportError(f"cannot read input file: {exc}") from exc
    return analyze_report_bytes(data)


def _human_summary(summary: dict[str, Any]) -> str:
    coverage = ", ".join(f"{item['stage']} {item['stageName']}/op {item['opcode']} {item['opcodeName']}: {item['records']}"
                         for item in summary["stageOpcodeCoverage"]) or "none"
    camera = summary["cameraCandidateChecks"]
    lines = [
        f"Schema {summary['schema']}: {summary['input']['records']} records, "
        f"{summary['input']['sourcePresentCount']} source presents",
        f"Stage/op coverage: {coverage}",
        f"Camera numeric candidates: {camera['finiteRangeAndSizeCandidateRecords']}/"
        f"{summary['input']['records']} (not semantic validation)",
        f"Candidate entities: {summary['candidateEntityTokenCoverage']['distinctNonzeroCandidates']} distinct; "
        f"{summary['candidateEntityTokenCoverage']['candidateCoveredFrameIndices']} frame indices covered",
        "Ready for Temporal: false (no validated motion-vector stream)",
        "No performance or image-quality claims are computed.",
    ]
    if summary["schema"] == 2:
        sampling = summary["lossAndUnknowns"]["intentionalDiagnosticSampling"]
        lines.insert(3, f"Diagnostic sampling: {sampling['quotaDroppedRecords']} quota drops across "
                    f"{sampling['downsampledFrames']} sampled frames (not game-frame loss or FPS).")
        probe = summary["cameraProbe"]
        lines.insert(4, f"Camera-only probe: {probe['mode']}, {probe['completed']} completed, "
                    f"{probe['pending']} pending; it does not enable Temporal readiness.")
        lines.insert(5, f"Camera layout v{summary['cameraLayoutVersion']}: "
                    f"{summary['cameraCandidateChecks']['cameraLayoutNote']}")
    elif summary["schema"] == 1:
        lines.insert(3, f"Camera-layout warning: {summary['cameraLayoutWarning']}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="temporal-diagnostics.json (read-only; maximum 4 MiB)")
    parser.add_argument("--json", action="store_true", help="print the summary as JSON to stdout")
    args = parser.parse_args(argv)
    try:
        summary = analyze_file(args.input)
    except ReportError as exc:
        print(f"analyze_temporal_diagnostics: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False))
    else:
        print(_human_summary(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
