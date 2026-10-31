"""Portable fixture tests for Scripts/prepare_temporal_diagnostics.py."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Scripts"))
import prepare_temporal_diagnostics as patcher


def fixture_function(index: int) -> str:
    if index == 550:
        signature = "void w2c_game_f550(w2c_game* instance, u32 var_p0, u64 var_p1, u32 var_p2) {"
        body = ("  var_l3 = var_j0;\n  memory_copy(instance->w2c_env_memory, instance->w2c_env_memory, var_j0, var_j1, var_j2);\n"
                "  var_j0 = 6583408ull;\n")
    elif index == 552:
        signature = "void w2c_game_f552(w2c_game* instance, u32 var_p0, u64 var_p1, u32 var_p2, u64 var_p3, u64 var_p4) {"
        body = ("  u32 var_l7 = 0, var_l8 = 0;\n  u64 var_l5 = 0, var_l6 = 0;\n"
                "  var_j0 = w2c_game_f551(instance, var_i0);\n  var_l5 = var_j0;\n"
                "  var_j0 = 6583408ull;\n  var_B5:;\n")
    elif index in patcher.ENTITY_HOOKS:
        kind, offset, _ = patcher.ENTITY_HOOKS[index]
        params = {
            44394: "u64 var_p0",
            44395: "u64 var_p0",
            44426: "u64 var_p0, u32 var_p1, u64 var_p2",
            44415: "u64 var_p0, u64 var_p1, u64 var_p2, u32 var_p3, u32 var_p4",
        }[index]
        signature = f"void w2c_game_f{index}(w2c_game* instance, {params}) {{"
        offset, alias = patcher.ENTITY_BODY_GUARDS[index]
        body = ("  var_j0 = var_p0;\n"
                f"  var_j0 = i64_load_shared(wasm_rt_local_memory_base, wasm_rt_local_memory_size, instance->w2c_env_memory, (u64)checked_add_u64(var_j0, {offset}u));\n"
                f"  {alias} = var_j0;\n  w2c_game_f44346(instance, var_j0);\n"
                f"  var_j0 = {alias};\n  var_i1 = var_l0;\n"
                "  var_i0 = w2c_game_f44404(instance, var_j0, var_i1);\n")
    elif index == 10704:
        signature = ("void w2c_game_f10704(w2c_game* instance, u64 var_p0, u64 var_p1, u64 var_p2, "
                    "u32 var_p3, u32 var_p4, u32 var_p5) {")
        body = ("  var_j4 = var_p1;\n  var_j5 = var_p2;\n"
                "  w2c_game_f10703(instance, var_j0, var_j1, var_i2, var_i3, var_j4, var_j5, var_i6, var_i7, var_j8);\n")
    else:
        params = "u64 var_p0" if index not in (35075, 44909, 43915, 43921) else ""
        suffix = f", {params}" if params else ""
        signature = f"void w2c_game_f{index}(w2c_game* instance{suffix}) {{"
        body = ""
    return signature + "\n" + body + "  FUNC_EPILOGUE;\n}\n"


class TemporalDiagnosticsPatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.generated = Path(self.temp.name)
        indices = list(patcher.TARGETS)
        # A single shard is enough to exercise ownership and multi-function patching.
        (self.generated / "game_0.c").write_text("#include \"game-impl.h\"\n\n" +
                                                  "\n".join(fixture_function(i) for i in indices))
        (self.generated / "game-impl.h").write_text("/* generated fixture */\n")
        self.names = {i: name for i, name in patcher.TARGETS.items()}

    def tearDown(self):
        self.temp.cleanup()

    def test_patches_hooks_and_is_idempotent(self):
        first = patcher.patch_sources(self.generated, self.names)
        patched = first[self.generated / "game_0.c"]
        self.assertEqual(patched.count("pg_td_packet(instance,"), 2)
        self.assertIn("pg_td_entity_enter(instance, 1u, var_p0, 56u, 0ull)", patched)
        self.assertIn("pg_td_scope_enter(instance, 200u, (u64)(var_p1), (u64)(var_p2))", patched)
        self.assertEqual(patched.count("pg_td_scope_leave(pgtd_cookie);"), 17)
        self.assertIn("extern void pg_td_packet", first[self.generated / "game-impl.h"])
        for path, content in first.items():
            path.write_text(content)
        second = patcher.patch_sources(self.generated, self.names)
        self.assertEqual(second[self.generated / "game-impl.h"], first[self.generated / "game-impl.h"])
        self.assertEqual((self.generated / "game_0.c").read_text(), first[self.generated / "game_0.c"])
        self.assertFalse(any(path.name.startswith("game_") for path in second))

    def test_guard_failure_does_not_change_fixture_files(self):
        source = self.generated / "game_0.c"
        original = source.read_text().replace("var_l3 = var_j0;", "var_l3 = changed;")
        source.write_text(original)
        before = {path: path.read_text() for path in self.generated.iterdir()}
        with self.assertRaises(RuntimeError):
            patcher.patch_sources(self.generated, self.names)
        after = {path: path.read_text() for path in self.generated.iterdir()}
        self.assertEqual(before, after)

    def test_name_index_mismatch_fails_closed(self):
        names = dict(self.names)
        names[550] = "different engine function"
        with self.assertRaises(RuntimeError):
            patcher.patch_sources(self.generated, names)


if __name__ == "__main__":
    unittest.main()
