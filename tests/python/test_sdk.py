"""SDK tests (stdlib unittest; no pytest needed).

    python -m unittest discover -s sdk/tests/python -v

Optional inputs (tests skip when absent):
  G6_DUMP      path to the user's own dump folder (game_scarlett_master.exe + *.rpf)
  sdk/build/gta6mod_mock.dll   built C mock runtime (see sdk/README.md)
"""
import ctypes
import importlib
import json
import os
import runpy
import sys
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))
sys.path.insert(0, os.path.join(SDK, "generators"))
# The public repository ships the generated outputs only; the generators and the evidence
# database (re-db) they read are private. Tests that need them skip with this reason.
HAVE_GENERATORS = os.path.isdir(os.path.join(SDK, "generators"))
HAVE_REDB = os.path.exists(os.path.join(SDK, "..", "re-db", "builds", "1.0.2.1", "anchors.json"))

from gta6mod import joaat, runtime, script  # noqa: E402
from gta6mod.runtime import marshal  # noqa: E402
from gta6mod.runtime.backend import Backend, BuildInfo, NativeError  # noqa: E402
from gta6mod.types import Vector3  # noqa: E402
from gta6mod import natives as N  # noqa: E402

DUMP = os.environ.get("G6_DUMP")
MOCK_DLL = os.path.join(SDK, "build", "gta6mod_mock.dll")
HAVE_GEN = os.path.isfile(os.path.join(SDK, "generators", "gen_bindings.py"))
VECTOR_EVIDENCE = os.path.join(SDK, "..", "re-db", "builds", "1.0.2.1", "natives", "script-vm.vector_params.json")


class TestJoaat(unittest.TestCase):
    def test_vectors(self):
        self.assertEqual(joaat("adder"), 0xB779A091)          # research/02 test vector
        self.assertEqual(joaat("ADDER"), joaat("adder"))       # GET_HASH_KEY lower-cases
        self.assertEqual(joaat("player_zero"), 0x0D7114C9)     # GTA V model hash
        self.assertEqual(joaat(""), 0)


class TestMarshal(unittest.TestCase):
    def test_scalar_roundtrip(self):
        p = marshal.pack("iufbs>n", [-5, "adder", 1.5, True, "hi"])
        self.assertEqual(p.slots[0], (-5) & 0xFFFFFFFFFFFFFFFF)
        self.assertEqual(p.slots[1], 0xB779A091)
        self.assertAlmostEqual(marshal.slot_float(p.slots[2]), 1.5)
        self.assertEqual(p.slots[3], 1)
        self.assertEqual(ctypes.string_at(p.slots[4]), b"hi")

    def test_vector_and_outparams(self):
        p = marshal.pack("vIV>v", [Vector3(1, 2, 3)])
        self.assertEqual(len(p.slots), 5)
        ctypes.c_int64.from_address(p.slots[3]).value = -7
        self.assertEqual(p.slots[4] % 16, 0, "Vector3* out-param must be 16-byte aligned (vmovaps)")
        out_vec = marshal.Vec3Packed.from_address(p.slots[4])
        out_vec.x, out_vec.y, out_vec.z = 4.0, 5.0, 6.0
        ret = [marshal.float_slot(9), marshal.float_slot(8), marshal.float_slot(7), 0]
        r, i_out, v_out = marshal.unpack("vIV>v", ret, p)
        self.assertEqual((r, i_out, v_out), (Vector3(9, 8, 7), -7, Vector3(4, 5, 6)))

    def test_packed_vector_param_is_two_slots(self):
        """'w': x | y<<32 in the first slot, z in the low half of the second (the 16 bytes the game loads)."""
        p = marshal.pack("ewb>n", [0x100, Vector3(1.5, -2.0, 30.25), True])
        f = marshal.float_slot
        self.assertEqual(p.slots, [0x100, 0, f(1.5) | (f(-2.0) << 32), f(30.25), 1])
        self.assertEqual(marshal.slots_vec3(p.slots, 2), Vector3(1.5, -2.0, 30.25))
        self.assertEqual(marshal.pack("w>n", [(1.0, 2.0, 3.0)]).slots, marshal.vec3_packed_slots(Vector3(1, 2, 3)))

    def test_vec3_outparams_stay_aligned(self):
        for _ in range(64):  # different heap addresses; every buffer must be aligned
            p = marshal.pack("VV>n", [])
            self.assertEqual([a % 16 for a in p.slots], [0, 0])
        from gta6mod.runtime.mock import write_vec3_out
        with self.assertRaises(ValueError):
            write_vec3_out(p.slots[0] + 4, Vector3(1, 2, 3))

    def test_entity_handle_is_two_slots(self):
        h = (0xDEADBEEF << 64) | 0x1122334455667788
        p = marshal.pack("eb>e", [h, True])
        self.assertEqual(p.slots, [0x1122334455667788, 0xDEADBEEF, 1])
        self.assertEqual(marshal.unpack("eb>e", [p.slots[0], p.slots[1], 0, 0], p), h)

    def test_packed_vector_return(self):
        r = marshal.vec3_packed_slots(Vector3(1.5, -2.0, 30.25))
        self.assertEqual(len(r), 2)
        p = marshal.pack(">w", [])
        self.assertEqual(marshal.unpack(">w", r + [0, 0], p), Vector3(1.5, -2.0, 30.25))

    def test_entity_outparam(self):
        p = marshal.pack("E>n", [])
        buf = marshal.EntitySlot.from_address(p.slots[0])
        buf.lo, buf.hi = 7, 9
        self.assertEqual(marshal.unpack("E>n", [0, 0, 0, 0], p), ((9 << 64) | 7,))

    def test_arg_count_checked(self):
        with self.assertRaises(TypeError):
            marshal.pack("ii>n", [1])


class _StrictBackend(Backend):
    """Stands in for a real game backend: refuses unverified natives by policy."""
    accepts_unverified = False

    def invoke(self, native_hash, slots):
        return [0, 0, 0, 0]

    def build_info(self):
        return BuildInfo("1.0.2.1", "test", "0" * 64, 0, 0)

    def now_ms(self):
        return 0


class TestRuntimePolicy(unittest.TestCase):
    def test_unverified_refused_on_real_backends(self):
        runtime.use(_StrictBackend())
        with self.assertRaises(NativeError) as cm:
            N.PLAYER.PLAYER_PED_ID()
        self.assertEqual(cm.exception.status, 3)  # G6_E_UNVERIFIED_NATIVE

    def test_unverified_allowed_when_opted_in(self):
        runtime.use(_StrictBackend(), allow_unverified=True)
        self.assertEqual(N.PLAYER.PLAYER_PED_ID(), 0)


class TestMockWorld(unittest.TestCase):
    def setUp(self):
        self.rt = runtime.use_mock()
        self.sched = script.Scheduler()

    def test_natives_on_mock(self):
        ped = N.PLAYER.PLAYER_PED_ID()
        self.assertTrue(N.ENTITY.DOES_ENTITY_EXIST(ped))
        N.ENTITY.SET_ENTITY_COORDS(ped, Vector3(1.0, 2.0, 3.0), False, False, False, False)
        self.assertEqual(N.ENTITY.GET_ENTITY_COORDS(ped, True), Vector3(1.0, 2.0, 3.0))

    def test_set_entity_coords_round_trip_and_slots(self):
        """SET_ENTITY_COORDS 0x143c13760 reads e,e,V,V,b,b,b,b,f (findings/phase5/23, correction 2026-11-25)."""
        ped = N.PLAYER.PLAYER_PED_ID()
        target = Vector3(-1234.5, 678.25, 42.125)
        N.ENTITY.SET_ENTITY_COORDS(ped, target, True, False, True, False, 2.5)
        h, slots = self.rt.backend.calls[-1]
        self.assertEqual(h, 0x06843DA7060A026B)
        self.assertEqual(len(slots), 9)
        self.assertEqual(marshal.slots_entity(slots[0], slots[1]), ped)
        self.assertEqual(marshal.slots_vec3(slots, 2), target)
        self.assertEqual(slots[4:8], [1, 0, 1, 0])
        self.assertEqual(marshal.slot_float(slots[8]), 2.5)
        self.assertEqual(N.ENTITY.GET_ENTITY_COORDS(ped, True), target)
        self.assertEqual(N.ENTITY.GET_ENTITY_COORDS_F44D(ped, True, False), target)  # observed GTA VI hash

    def test_vec3_outparam_on_mock(self):
        N.STREAMING.REQUEST_COLLISION_AT_COORD(Vector3(3.0, 4.0, 0.0))
        self.rt.backend.step(100)
        found, gz, normal = N.MISC.GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD(Vector3(3.0, 4.0, 100.0))
        self.assertEqual((found, gz, normal), (True, 12.0, Vector3(0.0, 0.0, 1.0)))

    def test_scheduler_wait_and_errors(self):
        seen = []

        def waiter():
            while True:
                seen.append(self.rt.now_ms())
                yield script.wait(100)

        def broken():
            raise ValueError("boom")
            yield  # pragma: no cover

        self.sched.add("waiter", waiter)
        self.sched.add("broken", broken)
        script.run_for(500, frame_ms=10, sched=self.sched)
        self.assertEqual(seen, [0, 100, 200, 300, 400])
        broken_s = next(s for s in self.sched.scripts if s.name == "broken")
        self.assertTrue(broken_s.done and "boom" in broken_s.error)

    def test_example_mod_spawns_vehicle(self):
        import gta6mod.script as S
        S._default = script.Scheduler()  # isolate from other tests
        runpy.run_path(os.path.join(SDK, "examples", "python", "spawn_car.py"))
        backend = self.rt.backend
        for t in range(0, 3000, 16):
            if t == 1008:
                backend.press_control(0, 51)
            S._default.tick()
            backend.step(16)
        vehicles = [e for e in backend.world.entities.values() if e.kind == "vehicle"]
        self.assertEqual(len(vehicles), 1)
        self.assertEqual(vehicles[0].model, joaat("adder"))


@unittest.skipUnless(os.path.exists(MOCK_DLL), "build sdk/build/gta6mod_mock.dll first")
class TestCAbiBackend(unittest.TestCase):
    def test_python_bindings_over_c_abi(self):
        rt = runtime.use_cabi(MOCK_DLL)
        b = rt.backend

        def fake_coords(call):
            call.ret[0], call.ret[1] = marshal.vec3_packed_slots(Vector3(10.0, 20.0, 30.0))

        b.mock_register_native(0x3FEF770D40960D5A, fake_coords)
        self.assertEqual(N.ENTITY.GET_ENTITY_COORDS(0x100, True), Vector3(10.0, 20.0, 30.0))

        # Packed Vector3 parameter through the C ABI: SET writes what GET returns.
        seen = {}

        def fake_set(call):
            seen["argc"] = call.argc
            seen["pos"] = marshal.slots_vec3(list(call.args), 2)
            seen["tail"] = [call.args[i] for i in range(4, 8)] + [marshal.slot_float(call.args[8])]

        def fake_get(call):
            call.ret[0], call.ret[1] = marshal.vec3_packed_slots(seen["pos"])

        b.mock_register_native(0x06843DA7060A026B, fake_set)
        b.mock_register_native(0x3FEF770D40960D5A, fake_get)
        N.ENTITY.SET_ENTITY_COORDS(0x100, Vector3(-5.5, 6.25, 7.125), False, True, False, True, 1.5)
        self.assertEqual(seen["argc"], 9)
        self.assertEqual(seen["tail"], [0, 1, 0, 1, 1.5])
        self.assertEqual(N.ENTITY.GET_ENTITY_COORDS(0x100, True), Vector3(-5.5, 6.25, 7.125))
        ticks = []
        b.register_tick("py_tick", lambda: ticks.append(1))
        before = b.now_ms()
        b.step(16)
        b.step(16)
        self.assertEqual(len(ticks), 2)
        self.assertEqual(b.now_ms() - before, 32)
        self.assertEqual(N.MISC.GET_GAME_TIMER(), b.now_ms())  # built-in mock native in the DLL
        self.assertEqual(b.build_info().platform, "mock")
        with self.assertRaises(NativeError):
            N.PLAYER.PLAYER_ID()  # not registered in the C mock -> G6_E_UNKNOWN_NATIVE


class TestGenerator(unittest.TestCase):
    @unittest.skipUnless(HAVE_GENERATORS, "needs sdk/generators (private; not in the public repository)")
    def test_type_codes(self):
        gen = importlib.import_module("gen_bindings")
        self.assertEqual(gen.type_code("Entity"), "e")
        self.assertEqual(gen.type_code("Ped"), "e")
        self.assertEqual(gen.type_code("Player"), "i")
        self.assertEqual(gen.type_code("Vehicle*"), "E")
        self.assertEqual(gen.type_code("Vector3", is_return=True), "w")
        self.assertEqual(gen.type_code("Vector3"), "v")
        self.assertEqual(gen.type_code("Hash"), "u")
        self.assertEqual(gen.type_code("const char*"), "s")
        self.assertEqual(gen.type_code("Vector3*"), "V")
        self.assertEqual(gen.type_code("float*"), "F")
        self.assertEqual(gen.type_code("void", is_return=True), "n")

    def test_gta6_handle_abi_signatures(self):
        """findings/phase5/23_handle_abi.md: 16-byte entities, packed Vector3 returns, no pad slots."""
        sig_of = {h: sig for h, (_ns, _n, sig, _st) in N.BY_HASH.items()}
        self.assertEqual(sig_of[0x1BB501624FAF2BEA], "e>i")        # GET_ENTITY_ALPHA (GTA VI hash)
        self.assertTrue(sig_of[0x275F255ED201B937].endswith(">e"))  # GET_PLAYER_PED returns a handle
        coords = sig_of[0xA86D5F069399F44D]                          # GET_ENTITY_COORDS (GTA VI hash)
        self.assertTrue(coords.startswith("e") and coords.endswith(">w"), coords)

    @unittest.skipUnless(HAVE_GEN, "sdk/generators is not part of this checkout")
    def test_set_entity_coords_signature(self):
        sig_of = {h: sig for h, (_ns, _n, sig, _st) in N.BY_HASH.items()}
        self.assertEqual(sig_of[0x06843DA7060A026B], "ewbbbbf>n")
        gen = importlib.import_module("gen_bindings")
        self.assertEqual(gen.slot_layout("ewbbbbf>n"), list("eewwbbbbf"))  # e,e,V,V,b,b,b,b,f
        self.assertEqual(gen.slot_layout("eVfE>w"), list("eeVfE"))

    @unittest.skipUnless(HAVE_GEN and os.path.isfile(VECTOR_EVIDENCE), "needs sdk/generators and re-db evidence (private tree)")
    def test_every_packed_vector_gets_the_packed_code(self):
        """Every float triple the handler evidence marks packed is one 'w' param at that slot; the rest stay."""
        with open(VECTOR_EVIDENCE, encoding="utf-8") as fh:
            evidence = json.load(fh)["natives"]
        with open(os.path.join(SDK, "data", "generated", "natives.index.json"), encoding="utf-8") as fh:
            index = {int(n["hash"], 16): n for n in json.load(fh)["natives"]}
        gen = importlib.import_module("gen_bindings")
        packed = 0
        for h, ev in evidence.items():
            n = index[int(h, 16)]
            layout = gen.slot_layout(n["sig"])
            forms = {g["form"] for g in ev["groups"]}
            for g in ev["groups"]:
                if g["form"] == "packed":
                    packed += 1
                    fused = [p for p in n["params"] if p.get("fused") == g["names"]]
                    self.assertEqual(len(fused), 1, (n["name"], g))
                    self.assertEqual(fused[0]["code"], "w", n["name"])
                    self.assertEqual(layout[g["slot"]:g["slot"] + 2], ["w", "w"], (n["name"], g["slot"], n["sig"]))
                else:  # measured 3-float or unsettled: nativedb's float slots are kept
                    self.assertTrue(all(p.get("fused") != g["names"] for p in n["params"]), n["name"])
            want = "unconfirmed" if forms - {"packed", "3float"} else ("packed" if "packed" in forms else "3float")
            self.assertEqual(n["vector_layout"], want, n["name"])
        self.assertEqual(packed, 254)
        self.assertEqual(sum(1 for n in index.values() if "w" in n["sig"].split(">")[0]), 204)
        self.assertEqual(index[0xE39B4FF4FDEBDE27]["sig"], "eeifffi>n")  # TASK_GOTO_ENTITY_OFFSET: 3 float slots

    def test_vector_evidence_matches_handler(self):
        """The evidence file reproduces from the exe (SET_ENTITY_COORDS)."""
        if not (DUMP and os.path.exists(os.path.join(DUMP, "game_scarlett_master.exe"))):
            self.skipTest("set G6_DUMP")
        sys.path.insert(0, os.path.join(SDK, "..", "tools", "phase5"))
        try:
            na = importlib.import_module("natives_arity")
        finally:
            sys.path.pop(0)
        img = na.pe.Image(os.path.join(DUMP, "game_scarlett_master.exe"))
        md = na.Cs(na.CS_ARCH_X86, na.CS_MODE_64)
        md.detail = True
        b, e = img.va_func(0x143C13760)
        raw = []
        na.analyse(img, md, 0x143C13760, e - b, raw_reads=raw)
        self.assertEqual(sorted(set(raw)), [(0, 16, "xmm16"), (16, 16, "xmm16"), (32, 4, "gpr"), (40, 4, "gpr"),
                                            (48, 4, "gpr"), (56, 4, "gpr"), (64, 4, "float")])

    def test_bindings_agree_across_languages(self):
        with open(os.path.join(SDK, "luau", "lib", "natives.luau"), encoding="utf-8") as fh:
            luau = fh.read()
        with open(os.path.join(SDK, "include", "gta6mod", "natives.h"), encoding="utf-8") as fh:
            c_hdr = fh.read()
        for h, (_ns, name, sig, status) in N.BY_HASH.items():
            self.assertIn(f'native.resolve("{h:016X}", "{sig}", "{name}", "{status}")', luau)
            self.assertIn(f'0x{h:016X}ull, "{_ns}", "{name}", "{sig}"', c_hdr)


CHAIN_ANCHORS = ["camera.final.get_front", "camera.gameplay.director", "camera.gameplay.get_director", "camera.world_to_screen", "streaming.model.request", "ecs.comp.seats_sel77", "ecs.ped.comp_12", "ecs.ped.comp_30", "ecs.ped.comp_weapon_26", "ecs.vehicle.comp_01", "fs.packfile.cell_file_type", "pool.buildings",
                 "script.thread.current_id", "tls.alloc_current.read", "tls.thread_id_cached"]
RUNTIME_CHAIN_ANCHORS = ["pool.buildings", "script.thread.current_id", "tls.alloc_current.read",
                         "tls.thread_id_cached"]


def _synthetic_pe(text: bytes, data: bytes, image_base: int = 0x140000000) -> bytes:
    """Minimal PE32+: .text at RVA 0x1000 (raw 0x400), .data at RVA 0x2000 (raw 0x600), 0x200 each."""
    import struct
    hdr = bytearray(0x400)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x80)
    hdr[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", hdr, 0x84, 0x8664, 2, 0, 0, 0, 0xF0, 0x22)
    opt = 0x98
    struct.pack_into("<H", hdr, opt, 0x20B)
    struct.pack_into("<I", hdr, opt + 16, 0x1000)          # entry
    struct.pack_into("<Q", hdr, opt + 24, image_base)
    struct.pack_into("<II", hdr, opt + 32, 0x1000, 0x200)  # section / file alignment
    struct.pack_into("<I", hdr, opt + 56, 0x3000)          # SizeOfImage
    struct.pack_into("<I", hdr, opt + 108, 16)
    so = opt + 0xF0
    for i, (name, va, raw, ch) in enumerate(((b".text", 0x1000, 0x400, 0x60000020),
                                             (b".data", 0x2000, 0x600, 0xC0000040))):
        struct.pack_into("<8sIIIIIIHHI", hdr, so + 40 * i, name, 0x200, va, 0x200, raw, 0, 0, 0, 0, ch)
    return bytes(hdr) + text.ljust(0x200, b"\xCC") + data.ljust(0x200, b"\0")


class TestChainResolution(unittest.TestCase):
    """xref-chain anchors on a synthetic image (no game data)."""

    START = "55 48 89 E5 E8 ? ? ? ? 48 8D 05 ? ? ? ? C3"

    @classmethod
    def setUpClass(cls):
        import struct
        import tempfile
        from gta6mod.pe import PEImage
        ib = 0x140000000
        t = bytearray(b"\xCC" * 0x200)

        def put(rva, b):
            t[rva - 0x1000:rva - 0x1000 + len(b)] = b

        def rel(site, target):
            return struct.pack("<i", target - (site + 5))
        # 0x1000 start: push rbp; mov rbp,rsp; call 0x1100; lea rax,[rip -> 0x2010]; ret
        put(0x1000, b"\x55\x48\x89\xE5\xE8" + rel(0x1004, 0x1100)
            + b"\x48\x8D\x05" + struct.pack("<i", 0x2010 - (0x1009 + 7)) + b"\xC3")
        # 0x1040: mov rax,gs:[0x58] (HLE hosts rewrite it); mov rax,[rax]; ret
        put(0x1040, b"\x65\x48\x8B\x04\x25\x58\x00\x00\x00\x48\x8B\x00\xC3")
        # 0x1100: nop; nop; call 0x1180; call 0x1190; ret
        put(0x1100, b"\x90\x90\xE8" + rel(0x1102, 0x1180) + b"\xE8" + rel(0x1107, 0x1190) + b"\xC3")
        put(0x1180, b"\xB8\x11\x00\x00\x00\xC3")
        put(0x1190, b"\xB8\x22\x00\x00\x00\xC3")
        data = bytearray(0x20)
        struct.pack_into("<Q", data, 0x10, ib + 0x1190)
        cls.tmp = tempfile.NamedTemporaryFile(suffix=".exe", delete=False)
        cls.tmp.write(_synthetic_pe(bytes(t), bytes(data), ib))
        cls.tmp.close()
        cls.img = PEImage(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        os.unlink(cls.tmp.name)

    def test_steps(self):
        from gta6mod.pattern import apply_chain
        img = self.img
        self.assertEqual(apply_chain(img, 0x1000, [{"op": "call_target", "at": 4}]), (0x1100, "ok"))
        self.assertEqual(apply_chain(img, 0x1000, [{"op": "call_target", "at": 4},
                                                   {"op": "nth_call", "n": 1, "max_len": 16},
                                                   {"op": "expect", "at": 0, "signature": "B8 22 00 00 00 C3"}]),
                         (0x1190, "ok"))
        self.assertEqual(apply_chain(img, 0x1000, [{"op": "rip_target", "at": 9, "disp_at": 3, "insn_len": 7}]),
                         (0x2010, "ok"))
        self.assertEqual(apply_chain(img, 0x1000, [{"op": "rip_target", "at": 9, "disp_at": 3, "insn_len": 7},
                                                   {"op": "deref"}, {"op": "add", "value": 1},
                                                   {"op": "expect", "at": -1, "signature": "B8 ? 00"}]),
                         (0x1191, "ok"))

    def test_failures_are_reported_never_guessed(self):
        from gta6mod.pattern import apply_chain
        img = self.img
        self.assertEqual(apply_chain(img, 0x1000, [{"op": "call_target", "at": 0}]), (None, "chain-step0-call_target"))
        self.assertEqual(apply_chain(img, 0x1100, [{"op": "nth_call", "n": 2, "max_len": 16}]),
                         (None, "chain-step0-nth_call"))
        self.assertEqual(apply_chain(img, 0x1100, [{"op": "nth_call", "n": 1, "max_len": 8}]),
                         (None, "chain-step0-nth_call"))  # the second call lies outside the bounded window
        self.assertEqual(apply_chain(img, 0x1190, [{"op": "expect", "at": 0, "signature": "B8 11"}]),
                         (None, "chain-step0-expect"))
        self.assertEqual(apply_chain(img, 0x1000, [{"op": "jump_table"}]), (None, "chain-step0-bad-op"))

    def test_anchor_and_index_resolution(self):
        from gta6mod import anchors as A
        from gta6mod import patterns as P
        start = A.Anchor(name="t.start", kind="function", rva=0x1000, signature=self.START)
        via_anchor = A.Anchor(name="t.leaf", kind="function", rva=0x1190, chain={
            "from_anchor": "t.start", "steps": [{"op": "call_target", "at": 4},
                                                {"op": "nth_call", "n": 1, "max_len": 16}]})
        via_sig = A.Anchor(name="t.ptr", kind="data", rva=0x2010, chain={
            "from_signature": self.START, "steps": [{"op": "rip_target", "at": 9, "disp_at": 3, "insn_len": 7}]})
        rows = A.verify_all(self.img, [start, via_anchor, via_sig])
        self.assertEqual([r["status"] for r in rows], ["ok", "ok", "ok"])
        self.assertEqual(A.resolve(self.img, via_anchor), (None, "chain-from-unknown"))
        entry = {"name": "t.leaf", "kind": "function", "signature": "", "expected_rva": 0x1190, "chain": dict(
            via_anchor.chain, from_spec={"signature": self.START, "kind": "function", "offset": 0, "rip": None})}
        self.assertEqual(P.resolve(self.img, entry), (0x1190, "ok"))
        self.assertEqual(P.verify_all(self.img, [entry])[0]["status"], "ok")
        moved = dict(entry, expected_rva=0x1180)
        self.assertEqual(P.verify_all(self.img, [moved])[0]["status"], "moved")
        broken = dict(entry, chain=dict(entry["chain"], steps=[{"op": "call_target", "at": 0}]))
        self.assertEqual(P.verify_all(self.img, [broken])[0]["status"], "chain-step0-call_target")

    def test_tls_pinning_is_rejected(self):
        from gta6mod.pattern import Pattern, chain_problems
        img = self.img
        pinned = Pattern.parse("65 48 8B 04 25 58 00 00 00 48 8B 00 C3")
        partly = Pattern.parse("65 ? ? ? ? ? ? ? ? 48 8B 00 C3")
        self.assertTrue(chain_problems(img, 0x1040, [], pinned))
        self.assertTrue(chain_problems(img, 0x1040, [], partly))    # the 0x65 byte is still pinned
        self.assertEqual(chain_problems(img, 0x1040, [{"op": "expect", "at": 9, "signature": "48 8B 00 C3"}]), [])
        self.assertTrue(chain_problems(img, 0x1040, [{"op": "expect", "at": 0, "signature": "65 48"}]))
        self.assertEqual(chain_problems(img, 0x1000, [{"op": "call_target", "at": 4},
                                                      {"op": "nth_call", "n": 1, "max_len": 16}],
                                        Pattern.parse(self.START)), [])


@unittest.skipUnless(DUMP, "set G6_DUMP to your own dump folder")
class TestAgainstDump(unittest.TestCase):
    def test_rpf9_headers(self):
        from gta6mod.formats.rpf9 import read_rpf9_header
        h = read_rpf9_header(os.path.join(DUMP, "appdata0.rpf"))
        self.assertEqual(h.entry_count, 2)
        self.assertTrue(h.sentinel_ok)
        self.assertTrue(h.toc_transform_enabled)
        self.assertEqual(h.codec_descriptor_id, 4)

    def test_public_patterns_resolve(self):
        """Export-only: every pattern in the public index resolves uniquely at its recorded RVA."""
        import contextlib
        import io
        from gta6mod import patterns as P
        doc, _ = P.load_index()
        self.assertTrue(doc["patterns"])
        self.assertEqual([e["name"] for e in doc["patterns"] if not e.get("signature") and not e.get("chain")], [])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(P.check(os.path.join(DUMP, "game_scarlett_master.exe")), 0)

    def test_rds5_header(self):
        from gta6mod.formats.rds5 import read_rds5_header
        h = read_rds5_header(os.path.join(DUMP, "data", "ttdb", "ui", "platform.ttpf"))
        self.assertEqual(h.record_count, 3)
        self.assertEqual(h.header_region_size, 0x1000)
        self.assertEqual(h.size_flags, 0x16)

    @unittest.skipUnless(HAVE_REDB, "needs the private evidence database (re-db anchors.json)")
    def test_anchors_verify(self):
        from gta6mod import anchors as A
        from gta6mod.pe import PEImage
        img = PEImage(os.path.join(DUMP, "game_scarlett_master.exe"))
        items, _ = A.load_anchors()
        rows = A.verify_all(img, items)
        self.assertEqual([r["name"] for r in rows if r["status"] not in ("ok", "no-signature")], [])
        # Unsigned by design: offline-decoder data tables with no direct RIP-relative reference, so
        # neither a reference signature nor a cheap chain exists. The runtime never resolves them.
        unsigned = []  # the public anchor set has no unsigned anchors
        self.assertEqual(sorted(r["name"] for r in rows if r["status"] == "no-signature"), unsigned)
        by = {a.name: a for a in items}
        for name in unsigned:
            self.assertTrue(by[name].extra.get("unsigned_reason"), name)
        # Resolved through xref chains (no unique TLS-free signature of their own).
        self.assertEqual(sorted(a.name for a in items if a.chain), sorted(CHAIN_ANCHORS))

    @unittest.skipUnless(HAVE_REDB, "needs the private evidence database (re-db anchors.json)")
    def test_chain_anchors_static_and_hle(self):
        """Every chain reproduces its recorded RVA, passes the generation checks, and still resolves after
        every `mov r64, gs:[0x58]` is rewritten into `call <thunk outside the image>` + 4 NOPs, the way
        ztrx/VIctory rewrite them (findings/live/30_live_menu.md)."""
        import copy
        import re
        import struct
        from gta6mod import anchors as A
        from gta6mod import patterns as P
        from gta6mod.pattern import Pattern, chain_problems, scan
        from gta6mod.pe import PEImage
        img = PEImage(os.path.join(DUMP, "game_scarlett_master.exe"))
        items, _ = A.load_anchors()
        by = {a.name: a for a in items}
        for name in RUNTIME_CHAIN_ANCHORS:
            self.assertTrue(by[name].chain, name)
        index = {e["name"]: e for e in P.load_index()[0]["patterns"]}
        for name in CHAIN_ANCHORS:
            a = by[name]
            self.assertEqual(A.resolve(img, a, by), (a.rva, "ok"), name)
            self.assertEqual(P.resolve(img, index[name]), (a.rva, "ok"), name)
            ch = a.chain
            if ch.get("from_signature"):
                spat = Pattern.parse(ch["from_signature"])
                start = scan(img, spat, limit=2)[0]
            else:
                spat, (start, _) = None, A.resolve(img, by[ch["from_anchor"]])
            self.assertEqual(chain_problems(img, start, ch["steps"], spat), [], name)

        hle = copy.copy(img)
        buf = bytearray(img.data)
        tls = re.compile(rb"\x65[\x48\x4C]\x8B[\x04\x0C\x14\x1C\x24\x2C\x34\x3C]\x25\x58\x00\x00\x00")
        thunk = -0x10000 + 0x20  # 0x13fff0020 relative to the image base, as observed live
        rewritten = 0
        for lo, hi in img.code_ranges():
            for m in tls.finditer(img.data, lo, hi):
                site = img.off_to_rva(m.start())
                buf[m.start():m.start() + 9] = b"\xE8" + struct.pack("<i", thunk - (site + 5)) + b"\x90" * 4
                rewritten += 1
        hle.data = bytes(buf)
        hle._sha256 = None
        self.assertGreater(rewritten, 1000)
        for name in CHAIN_ANCHORS:
            self.assertEqual(A.resolve(hle, by[name], by), (by[name].rva, "ok"), f"{name} under HLE")
        # The TLS-pinned signatures these chains replace match only console-faithful images.
        for name in ("script.thread.current_id", "tls.alloc_current.read"):
            old = by[name].extra["signature_console_only"]
            self.assertEqual(scan(img, old, limit=2), [by[name].rva], name)
            self.assertEqual(scan(hle, old, limit=2), [], name)


if __name__ == "__main__":
    unittest.main()
