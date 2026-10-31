"""Add guarded, opt-in producer-side temporal diagnostics hooks to pinned wasm2c output.

This is diagnostic instrumentation only: it never edits game.wasm and never enables a
renderer effect. It verifies the pinned engine name section and every target C body before
staging any generated-file changes.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import tempfile

from engine_names import read_names

ROOT = Path(__file__).resolve().parents[1]
PINNED_SHA256 = "11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0"
DECLARATIONS = """\
extern int pg_td_enabled(void);
extern void pg_td_packet(w2c_game* instance, u64 packetAddr);
extern u64 pg_td_entity_enter(w2c_game* instance, u32 kind, u64 data, u32 keyOffset, u64 aux);
extern u64 pg_td_scope_enter(w2c_game* instance, u32 kind, u64 identity, u64 aux);
extern void pg_td_scope_leave(u64 cookie);
"""

# Exact demangled names from the pinned wasm name section. The index is looked up from the
# section at runtime, then checked against the known index as an additional ABI guard.
TARGETS = {
    550: "wasm_gpu::Emit(unsigned int, unsigned int const*, unsigned int)",
    552: "wasm_gpu::EmitData(unsigned int, unsigned int const*, unsigned int, void const*, unsigned long)",
    44394: "CEntityDrawData::Draw()",
    44395: "CEntityDrawDataFm::Draw()",
    44426: "CEntityDrawDataPedBIG::Draw(unsigned char, CEntityDrawDataPedPropsBase*)",
    44415: "CEntityDrawDataStreamPed::Draw(CAddCompositeSkeletonCommand&, CEntityDrawDataPedPropsBase*, unsigned char, unsigned int)",
    44353: "CDrawEntityInstancedDC::ExecuteStatic(rage::dlCmdBase&)",
    44354: "CDrawGrassBatchDC::ExecuteStatic(rage::dlCmdBase&)",
    10704: "rage::rmcDrawable::DrawSkinnedNoShadersTessellationControlled(rage::Matrix34 const&, rage::grmMatrixSet const&, unsigned int, int, rage::rmcDrawableBase::EnumNoShadersSkinned) const",
    35075: "CNewHud::Render()",
    42268: "DeferredRendererWrapper::RenderToneMapPass(DeferredRendererWrapper*, rage::grcRenderTarget*, rage::grcTexture*, rage::Color32, bool)",
    44909: "CRenderPhaseDrawSceneInterface::Static_PostFxDepthPass()",
    43644: "CRenderTargets::LockSceneRenderTargets(bool)",
    43915: "CRenderTargets::LockSceneRenderTargets_DepthCopy()",
    43921: "CRenderTargets::LockUIRenderTargets()",
    42251: "DeferredRendererWrapper::DeferredRenderStart(DeferredRendererWrapper*)",
    42255: "DeferredRendererWrapper::DeferredRenderEnd(DeferredRendererWrapper*)",
    42266: "DeferredRendererWrapper::ForwardRenderStart(DeferredRendererWrapper*)",
    42267: "DeferredRendererWrapper::ForwardRenderEnd(DeferredRendererWrapper*)",
}

ENTITY_HOOKS = {
    # Candidate IDs are u64 and are interned opaquely by the native collector. The fragment
    # field is deliberately unknown here: +64 belongs to a different FragType command layout.
    44394: (1, 56, "CEntityDrawData::Draw()"),
    44395: (2, 0xFFFFFFFF, "CEntityDrawDataFm::Draw()"),
    44426: (3, 320, "CEntityDrawDataPedBIG::Draw(unsigned char, CEntityDrawDataPedPropsBase*)"),
    44415: (4, 144, "CEntityDrawDataStreamPed::Draw(CAddCompositeSkeletonCommand&, CEntityDrawDataPedPropsBase*, unsigned char, unsigned int)"),
}
ENTITY_BODY_GUARDS = {
    44394: (56, "var_l3"),
    44395: (64, "var_l5"),
    44426: (320, "var_l7"),
    44415: (144, "var_l11"),
}
UNKNOWN_ENTITY_HOOKS = {44353: 7, 44354: 8}
STAGE_HOOKS = {
    35075: 100, 42268: 101, 44909: 102, 43644: 103, 43915: 104, 43921: 105,
    42251: 106, 42255: 107, 42266: 108, 42267: 109,
}


def _function_names(names: dict[int, str]) -> dict[str, int]:
    reverse: dict[str, list[int]] = {}
    for index, name in names.items():
        reverse.setdefault(" ".join(name.split()), []).append(index)
    result = {}
    for index, name in TARGETS.items():
        normalized = " ".join(name.split())
        hits = reverse.get(normalized, [])
        if hits != [index]:
            raise RuntimeError(f"Pinned wasm name mismatch for f{index}: expected unique {name!r}, got {hits}")
        result[name] = index
    return result


def _extract_function(source: str, index: int) -> tuple[int, int, str]:
    pattern = re.compile(rf"(?m)^\w[\w *]*\bw2c_game_f{index}\(w2c_game\* instance(?:, [^\n]*)?\) \{{$")
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one generated definition for f{index}, found {len(matches)}")
    start = matches[0].start()
    end = source.find("\n}\n", matches[0].end())
    if end < 0:
        raise RuntimeError(f"Could not find closing brace for f{index}")
    end += 3
    return start, end, source[start:end]


def _one(body: str, old: str, new: str, label: str) -> str:
    count = body.count(old)
    if count != 1:
        raise RuntimeError(f"Body guard failed for {label}: expected one {old!r}, found {count}")
    return body.replace(old, new, 1)


def entity_guard_pattern(index: int) -> str:
    """Require the field loaded from var_p0 to be the same value bound by f44346/f44404."""
    offset, alias = ENTITY_BODY_GUARDS[index]
    return (rf"var_j0 = var_p0;\s*"
            rf"var_j0 = i64_load_shared\([^;]*checked_add_u64\(var_j0, {offset}u\)\);\s*"
            rf"{alias} = var_j0;[\s\S]*?"
            rf"(?:var_j0 = {alias};\s*)?w2c_game_f44346\(instance, var_j0\);\s*"
            rf"var_j0 = {alias};\s*var_i1 = \w+;\s*"
            rf"var_i0 = w2c_game_f44404\(instance, var_j0, var_i1\);")


def _scope(body: str, index: int, kind: int, identity: str, aux: str = "0ull",
           enter_expr: str | None = None) -> str:
    if "FUNC_EPILOGUE;" not in body or body.count("FUNC_EPILOGUE;") != 1:
        raise RuntimeError(f"Body guard failed for f{index}: expected one common FUNC_EPILOGUE")
    if "pg_td_" in body:
        if "pg_td_scope_leave(pgtd_cookie);" in body:
            return body
        raise RuntimeError(f"f{index} contains an unexpected partial temporal hook")
    if enter_expr is None:
        enter_expr = (f"pg_td_scope_enter(instance, {kind}u, (u64)({identity}), "
                      f"(u64)({aux}))")
    body = body.replace("{\n", "{\n  u64 pgtd_cookie = " + enter_expr + ";\n", 1)
    return body.replace("  FUNC_EPILOGUE;", "  pg_td_scope_leave(pgtd_cookie);\n  FUNC_EPILOGUE;", 1)


def patch_sources(generated: Path, names: dict[int, str]) -> dict[Path, str]:
    _function_names(names)
    sources = {path: path.read_text() for path in sorted(generated.glob("game_*.c"))}
    original_sources = dict(sources)
    header = generated / "game-impl.h"
    if not sources or not header.is_file():
        raise RuntimeError(f"Expected wasm2c shards and game-impl.h under {generated}")
    header_text = header.read_text()
    staged: dict[Path, str] = {}

    def edit(index: int, transform):
        owners = [(path, text) for path, text in sources.items()
                  if re.search(rf"(?m)^\w[\w *]*\bw2c_game_f{index}\(w2c_game\* instance(?:, [^\n]*)?\) \{{", text)]
        if len(owners) != 1:
            raise RuntimeError(f"Expected one owner shard for f{index}, found {len(owners)}")
        path, source = owners[0]
        start, end, body = _extract_function(source, index)
        new_body = transform(body)
        sources[path] = source[:start] + new_body + source[end:]

    # The ring packet header (op, words) and payload must be complete before sampling,
    # while the producer still owns the reservation and before head publication.
    for index in (550, 552):
        def packet(body, index=index):
            guard = "var_j0 = 6583408ull;"
            if body.count(guard) != 1:
                raise RuntimeError(f"Body guard failed for packet producer f{index}")
            if "pg_td_packet(instance," in body:
                return body
            if index == 550:
                if "var_l3 = var_j0;" not in body or "memory_copy(instance->w2c_env_memory" not in body:
                    raise RuntimeError("Emit f550 packet reservation/copy shape changed")
                call = "pg_td_packet(instance, var_l3);\n  "
            else:
                if "var_l5 = var_j0;" not in body or "var_B5:;" not in body:
                    raise RuntimeError("EmitData f552 packet reservation/copy shape changed")
                # At the accounting point var_l5 points into the packet body; packet base is
                # retained separately so padding/copy writes cannot change the callback address.
                body = _one(body,
                            "  u32 var_l7 = 0, var_l8 = 0;\n  u64 var_l5 = 0, var_l6 = 0;",
                            "  u32 var_l7 = 0, var_l8 = 0;\n  u64 pgtd_packet_addr = 0, var_l5 = 0, var_l6 = 0;",
                            "EmitData local declaration")
                body = _one(body,
                            "  var_j0 = w2c_game_f551(instance, var_i0);\n  var_l5 = var_j0;\n",
                            "  var_j0 = w2c_game_f551(instance, var_i0);\n  pgtd_packet_addr = var_j0;\n  var_l5 = var_j0;\n",
                            "EmitData reserved packet base capture")
                call = "pg_td_packet(instance, pgtd_packet_addr);\n  "
            return _one(body, guard, call + guard, f"packet callback f{index}")
        edit(index, packet)

    for index, (kind, key_offset, name) in ENTITY_HOOKS.items():
        def entity(body, index=index, kind=kind, key_offset=key_offset, name=name):
            sig = re.search(r"(?m)^void w2c_game_f\d+\(w2c_game\* instance, ([^\n]*)\) \{", body)
            if not sig or "u64 var_p0" not in sig.group(1):
                raise RuntimeError(f"Body guard failed for {name}: expected data in var_p0")
            if not re.search(entity_guard_pattern(index), body):
                raise RuntimeError(f"Body guard failed for {name}: entity-selection path changed")
            if index == 44395 and key_offset != 0xFFFFFFFF:
                raise RuntimeError("Fragment candidate key is not verified for this draw-data type")
            if "pg_td_entity_enter" in body:
                if "pg_td_scope_leave(pgtd_cookie);" in body:
                    return body
                raise RuntimeError(f"{name} contains an unexpected partial hook")
            return _scope(body, index, kind, "0ull", enter_expr=
                          f"pg_td_entity_enter(instance, {kind}u, var_p0, {key_offset}u, 0ull)")
        edit(index, entity)

    for index, kind in UNKNOWN_ENTITY_HOOKS.items():
        edit(index, lambda body, index=index, kind=kind: _scope(
            body, index, kind, "var_p0"))

    for index, kind in STAGE_HOOKS.items():
        def stage(body, index=index, kind=kind):
            sig = re.search(r"(?m)^void w2c_game_f\d+\(w2c_game\* instance([^\n]*)\) \{", body)
            if not sig:
                raise RuntimeError(f"Body guard failed for stage f{index}: signature")
            identity = "var_p0" if "var_p0" in sig.group(1) else "0ull"
            return _scope(body, index, kind, identity)
        edit(index, stage)

    # Candidate-only tag: the verified signature/body passes the Matrix34 const& and
    # grmMatrixSet const& arguments to the downstream draw helper as var_p1/var_p2.
    # This does not assert that grmMatrixSet is a GPU palette resource.
    def palette_candidate(body):
        signature = "void w2c_game_f10704(w2c_game* instance, u64 var_p0, u64 var_p1, u64 var_p2,"
        if signature not in body or "var_j4 = var_p1;" not in body or "var_j5 = var_p2;" not in body:
            raise RuntimeError("Palette-candidate guard failed: Matrix34/grmMatrixSet args changed")
        if "w2c_game_f10703(instance, var_j0, var_j1, var_i2, var_i3, var_j4, var_j5," not in body:
            raise RuntimeError("Palette-candidate guard failed: downstream draw call changed")
        return _scope(body, 10704, 200, "var_p1", "var_p2")
    edit(10704, palette_candidate)

    if "pg_td_packet" not in header_text:
        header_text += "\n/* Temporal diagnostic C-bridge hooks. */\n" + DECLARATIONS
    staged.update({path: text for path, text in sources.items() if text != original_sources[path]})
    staged[header] = header_text
    return staged


def write_staged(staged: dict[Path, str]) -> None:
    """Stage every complete output first, then replace targets only after all guards pass."""
    temporary: list[tuple[Path, Path]] = []
    try:
        for target, content in staged.items():
            fd, name = tempfile.mkstemp(prefix=target.name + ".", suffix=".tmp", dir=target.parent)
            temp = Path(name)
            with os.fdopen(fd, "w") as stream:
                stream.write(content)
            temporary.append((temp, target))
        for temp, target in temporary:
            os.replace(temp, target)
    finally:
        for temp, _ in temporary:
            if temp.exists():
                temp.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wasm", type=Path, required=True)
    parser.add_argument("--generated", type=Path, default=ROOT / "Generated")
    args = parser.parse_args()
    digest = hashlib.sha256(args.wasm.read_bytes()).hexdigest()
    if digest != PINNED_SHA256:
        raise SystemExit(f"Temporal diagnostics require pinned game.wasm SHA256 {PINNED_SHA256}; got {digest}")
    staged = patch_sources(args.generated, read_names(args.wasm))
    write_staged(staged)
    print(f"temporal diagnostics: verified pinned wasm; patched {len(staged) - 1} C shards and game-impl.h")


if __name__ == "__main__":
    main()
