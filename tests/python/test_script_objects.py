"""Script String ('S') and text ('T') arguments (G-120): marshalling, the object layouts both mocks build, the
generated signatures, and ui.objective. Offline only; nothing here says the game draws the text (unverified live).

    python -m unittest discover -s sdk/tests/python -p test_script_objects.py -v
"""
import ctypes
import json
import os
import re
import sys
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))

from gta6mod import runtime, script, ui  # noqa: E402
from gta6mod import natives as N  # noqa: E402
from gta6mod.runtime import marshal  # noqa: E402
from gta6mod.runtime.backend import NativeError  # noqa: E402

MOCK_DLLS = [os.path.join(SDK, "build", "gta6mod_mock.dll"), os.path.join(SDK, "build", "luau", "gta6mod_mock.dll")]
MOCK_DLL = max((p for p in MOCK_DLLS if os.path.exists(p)), key=os.path.getmtime, default=None)
HEADER = os.path.join(SDK, "include", "gta6mod", "script_objects.h")
INDEX = os.path.join(SDK, "data", "generated", "natives.index.json")

SET_OBJECTIVE, PRINT_OBJECTIVE, CLEAR_OBJECTIVE = 0x0CB5D1E14473D8A5, 0xE9990552DEC71600, 0xDFF0D417277B41F8
BUSYSPINNER_SET_TEXT = 0x7F78CD75CC4539E4


def mock_supports_objects(path):
    """True when this mock DLL build exports the String records (API 0.4 mock)."""
    try:
        dll = ctypes.CDLL(path)
        return hasattr(dll, "g6_mock_string_type_record")
    except OSError:
        return False


class TestLayoutConstants(unittest.TestCase):
    """marshal.py's offsets are the C header's (both mocks and the runtime build from the header)."""

    def test_header_offsets(self):
        text = open(HEADER, encoding="utf-8").read()
        defs = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"#define (G6_S\w+)\s+(0x[0-9a-fA-F]+|\d+)u?", text)}
        self.assertEqual(defs["G6_SSTR_LENGTH"], marshal.SSTR_LENGTH)
        self.assertEqual(defs["G6_SSTR_CAPACITY"], marshal.SSTR_CAPACITY)
        self.assertEqual(defs["G6_SSTR_STRIDE"], marshal.SSTR_STRIDE)
        self.assertEqual(defs["G6_SSTR_FLAGS"], marshal.SSTR_FLAGS)
        self.assertEqual(defs["G6_SSTR_Q18"], marshal.SSTR_Q18)
        self.assertEqual(defs["G6_SSTR_ELEM"], marshal.SSTR_ELEM)
        self.assertEqual(defs["G6_SSTR_TEXT"], marshal.SSTR_TEXT)
        self.assertEqual(defs["G6_SSTR_OUT_OF_LINE"], marshal.SSTR_OUT_OF_LINE)
        self.assertEqual(defs["G6_SSTR_LITERAL_Q18"], marshal.SSTR_LITERAL_Q18)
        self.assertEqual(defs["G6_STEXT_NATIVE"], marshal.STEXT_NATIVE)
        self.assertEqual(defs["G6_STEXT_SIZE"], marshal.STEXT_SIZE)


class TestPack(unittest.TestCase):
    def test_masks_follow_slots(self):
        p = marshal.pack("wfSfi>i", [(1.0, 2.0, 3.0), 1000.0, "fire", 10.0, 4])
        self.assertEqual(len(p.slots), 6)
        self.assertEqual(p.string_slots, 1 << 3)  # the packed vector takes slots 0-1
        self.assertEqual(p.text_slots, 0)
        self.assertEqual(ctypes.string_at(p.slots[3]), b"fire")
        t = marshal.pack("T>n", ["Lose the cops"])
        self.assertEqual((t.string_slots, t.text_slots), (0, 1))

    def test_none_is_a_null_object(self):
        p = marshal.pack("S>n", [None])
        self.assertEqual((p.slots, p.string_slots), ([0], 1))

    def test_non_str_refused(self):
        with self.assertRaises(TypeError):
            marshal.pack("S>n", [42])
        with self.assertRaises(TypeError):
            marshal.pack("T>n", [b"bytes"])

    def test_utf8(self):
        p = marshal.pack("S>n", ["café ☃"])
        self.assertEqual(ctypes.string_at(p.slots[0]), "café ☃".encode("utf-8"))


class TestObjects(unittest.TestCase):
    """The pure-Python builder (MockBackend) lays objects out as the game's readers expect."""

    def test_string_layout(self):
        tr, er = (ctypes.c_uint64 * 17)(), (ctypes.c_uint64 * 17)()
        o = marshal.ScriptObjects(ctypes.addressof(tr), ctypes.addressof(er))
        a = o.string("café".encode("utf-8"))
        self.assertEqual(ctypes.c_void_p.from_address(a).value, ctypes.addressof(tr))
        self.assertEqual(ctypes.c_void_p.from_address(a + 0x20).value, ctypes.addressof(er))
        self.assertEqual(ctypes.c_int32.from_address(a + 8).value, 5)
        self.assertEqual(ctypes.c_uint32.from_address(a + 0xC).value, 6)
        self.assertEqual(ctypes.c_uint32.from_address(a + 0x10).value, 1)
        self.assertEqual(ctypes.c_uint8.from_address(a + 0x14).value, 0)
        self.assertEqual(ctypes.c_uint64.from_address(a + 0x18).value, 9)
        self.assertEqual(ctypes.string_at(a + 0x28), "café".encode("utf-8"))
        self.assertEqual(marshal.read_script_string(a), "café")
        self.assertIsNone(marshal.read_script_string(0))

    def test_out_of_line_flag(self):
        # the reader follows +0x28 when bit 2 of +0x14 is set (how the game marks out-of-line text)
        text = ctypes.create_string_buffer(b"elsewhere")
        obj = (ctypes.c_uint8 * 0x40)()
        a = ctypes.addressof(obj)
        ctypes.c_int32.from_address(a + 8).value = 9
        ctypes.c_uint8.from_address(a + 0x14).value = 4
        ctypes.c_void_p.from_address(a + 0x28).value = ctypes.addressof(text)
        self.assertEqual(marshal.read_script_string(a), "elsewhere")

    def test_text_object_read_through_the_virtual(self):
        o = marshal.ScriptObjects(0, 0)
        a = o.text(b"Lose the cops")
        native = ctypes.c_void_p.from_address(a + 0x18).value
        vtable = ctypes.c_void_p.from_address(native).value
        self.assertTrue(ctypes.c_void_p.from_address(vtable + 0x18).value)
        self.assertEqual(marshal.read_script_text(a), "Lose the cops")
        self.assertEqual(marshal.read_script_text(0), "")  # null object -> "" like 0x142055010

    def test_bad_masks(self):
        o = marshal.ScriptObjects(0, 0)
        with self.assertRaises(ValueError):
            o.convert([1, 2], 1, 1)
        with self.assertRaises(ValueError):
            o.convert([1], 0b10, 0)


class TestBindings(unittest.TestCase):
    def test_generated_signatures(self):
        idx = {n["name"]: n for n in json.load(open(INDEX, encoding="utf-8"))["natives"]}
        self.assertEqual(idx["_UILOG_SET_CACHED_OBJECTIVE"]["sig"], "T>n")
        self.assertEqual(idx["START_SCRIPT_FIRE"]["sig"], "wfSfi>i")
        self.assertEqual(idx["_BUSYSPINNER_SET_TEXT"]["sig"], "S>n")
        sigs = [n["sig"].split(">")[0] for n in idx.values() if n.get("sig")]
        self.assertEqual(sum(s.count("S") for s in sigs), 31)
        self.assertEqual(sum(s.count("T") for s in sigs), 9)

    def test_c_wrapper_sets_the_mask(self):
        h = open(os.path.join(SDK, "include", "gta6mod", "natives.h"), encoding="utf-8").read()
        body = h[h.index("G6_UILOG__UILOG_SET_CACHED_OBJECTIVE("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("c.text_slots |= 1u << c.argc;", body)
        self.assertIn("{0, 0, {0}, {0}, 0, 0}", h)


class TestPythonMock(unittest.TestCase):
    def setUp(self):
        self.rt = runtime.use_mock(quiet=True)
        self.b = self.rt.backend
        script.reset()

    def test_string_and_text_reach_the_handlers(self):
        N.HUD._BUSYSPINNER_SET_TEXT("Saving...")
        N.UILOG._UILOG_SET_CACHED_OBJECTIVE("Lose the cops")
        N.UILOG._UILOG_PRINT_CACHED_OBJECTIVE(7500)
        self.assertIn(("INFO", "mock", "busyspinner: Saving..."), self.b.logs)
        self.assertEqual(self.b.world.objectives_shown, [(0, "Lose the cops", 7500)])
        # the caller's slots were passed as pointers to text, the handler got objects (not the same address)
        h, slots = self.b.calls[1]
        self.assertEqual(h, SET_OBJECTIVE)
        self.assertEqual(ctypes.string_at(slots[0]), b"Lose the cops")

    def test_objective_truncated_like_the_game(self):
        N.UILOG._UILOG_SET_CACHED_OBJECTIVE("x" * 600)
        self.assertEqual(len(self.b.world.objective_text), 511)

    def test_ui_objective_sequence(self):
        @script.register("obj")
        def main():
            ui.objective("Lose the cops", 500)
            yield script.wait(100)
            yield script.wait(600)
            ui.objective("Second", -1)
            yield script.wait(200)
            ui.objective("Third", 300)
            ui.objective("Fourth", 1000)   # replaces Third: Third's clear (at +300) must not fire
            yield script.wait(400)
        script.run_for(2400)
        w = self.b.world
        self.assertEqual([t for _, t, _ in w.objectives_shown], ["Lose the cops", "Second", "Third", "Fourth"])
        self.assertEqual([ms for _, _, ms in w.objectives_shown], [500, -1, 300, 1000])
        prints = [i for i, (h, _) in enumerate(self.b.calls) if h == PRINT_OBJECTIVE]
        clears = [i for i, (h, _) in enumerate(self.b.calls) if h == CLEAR_OBJECTIVE]
        self.assertEqual(len(prints), 4)
        self.assertEqual(len(clears), 2)  # after Lose the cops (500 ms) and after Fourth (1000 ms)
        self.assertLess(prints[0], clears[0])
        self.assertLess(clears[0], prints[1])
        self.assertGreater(clears[1], prints[3])
        self.assertEqual(w.objective_text, "")

    def test_ui_objective_rejects_non_text(self):
        with self.assertRaises(TypeError):
            ui.objective(123)  # type: ignore[arg-type]

    def test_frame_functions_cannot_show_objectives(self):
        @ui.on_frame("obj")
        def frame(_f):
            try:
                ui.objective("no")
            except NativeError as e:
                seen.append(e)
        seen = []
        self.b.ui.frame()
        self.assertEqual(len(seen), 1)
        ui.remove("obj")


@unittest.skipUnless(MOCK_DLL and mock_supports_objects(MOCK_DLL), "build an API 0.4 gta6mod_mock.dll first")
class TestCMock(unittest.TestCase):
    """The C mock converts string_slots / text_slots like the runtime (script_objects.h)."""

    def setUp(self):
        self.rt = runtime.use_cabi(MOCK_DLL)
        self.b = self.rt.backend
        self.seen = []

    def _register(self, h, reader):
        def fn(call):
            self.seen.append((call.args[0], reader(call.args[0]), call.args[1]))
        self.b.mock_register_native(h, fn)

    def test_string_object(self):
        dll = ctypes.CDLL(MOCK_DLL)
        type_rec = ctypes.addressof(ctypes.c_uint64.in_dll(dll, "g6_mock_string_type_record"))
        self._register(0xA000000000000001, marshal.read_script_string)
        self.rt.call(0xA000000000000001, "Si>n", "t", "observed", ("café", 7))
        addr, text, other = self.seen[0]
        self.assertEqual(text, "café")
        self.assertEqual(other, 7)
        self.assertEqual(ctypes.c_void_p.from_address(addr).value, type_rec)
        self.assertEqual(ctypes.c_uint64.from_address(addr + 0x18).value, 9)

    def test_text_object(self):
        self._register(0xA000000000000002, marshal.read_script_text)
        self.rt.call(0xA000000000000002, "Ti>n", "t", "observed", ("Lose the cops", 1))
        self.assertEqual(self.seen[0][1], "Lose the cops")

    def test_null_and_bad_masks(self):
        self._register(0xA000000000000003, lambda a: a)
        self.rt.call(0xA000000000000003, "Si>n", "t", "observed", (None, 2))
        self.assertEqual(self.seen[0][0], 0)
        with self.assertRaises(NativeError) as cm:
            self.b.invoke(0xA000000000000003, [0, 0], 1, 1)  # overlapping masks
        self.assertEqual(cm.exception.status, 1)
        with self.assertRaises(NativeError):
            self.b.invoke(0xA000000000000003, [0], 0b10, 0)  # beyond argc

    def test_api_03_caller_has_no_masks(self):
        # an API 0.3 caller: size without the two masks; the pointer reaches the handler unchanged
        from gta6mod.runtime.cabi import G6NativeCall
        self._register(0xA000000000000004, lambda a: a)
        call = G6NativeCall()
        call.size = G6NativeCall.string_slots.offset
        call.argc = 2
        buf = ctypes.create_string_buffer(b"raw")
        call.args[0] = ctypes.addressof(buf)
        call.string_slots = 1
        self.assertEqual(self.b.api.native_invoke_hash(0xA000000000000004, ctypes.byref(call)), 0)
        self.assertEqual(self.seen[0][0], ctypes.addressof(buf))


if __name__ == "__main__":
    unittest.main()
