"""Control actions: generated tables, the 64-bit naming rule, and the script-layer plumbing (no game needed)."""
import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", ".."))
ROOT = os.path.normpath(os.path.join(SDK, ".."))
sys.path.insert(0, os.path.join(SDK, "python"))

from gta6mod import controls, scenario  # noqa: E402
from gta6mod.joaat import joaat, joaat_raw  # noqa: E402

SRC = os.path.join(ROOT, "re-db", "builds", "1.0.2.1", "controls", "input_actions.json")
# The public SDK export ships neither re-db nor sdk/generators; those tests skip there.
GEN = os.path.join(SDK, "generators", "gen_controls.py")


@unittest.skipUnless(os.path.exists(SRC), "needs the private evidence database (re-db)")
class TestRedb(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(SRC, encoding="utf-8") as fh:
            cls.doc = json.load(fh)

    def test_every_name_passes_both_keys(self):
        for a in self.doc["actions"]:
            if a["name"]:
                self.assertEqual(joaat(a["name"]), int(a["hash"], 16), a["name"])
                self.assertEqual(joaat_raw(a["name"].encode()), int(a["pso_hash"], 16), a["name"])

    def test_table_shape(self):
        acts = self.doc["actions"]
        self.assertEqual(len(acts), self.doc["_meta"]["enum"]["count"])
        self.assertEqual(len({a["hash"] for a in acts}), len(acts))
        self.assertEqual(acts[-1]["name"], "INPUT_COUNT")
        self.assertTrue(all(c["ok"] for c in self.doc["_meta"]["engine"]["checks"]))


class TestGenerated(unittest.TestCase):
    @unittest.skipUnless(os.path.exists(GEN) and os.path.exists(SRC), "needs sdk/generators and re-db (private)")
    def test_generated_files_are_current(self):
        r = subprocess.run([sys.executable, "-I", GEN, "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_control_members(self):
        self.assertEqual(controls.Control.JUMP, 0xD9D0E1C0)  # the same key RDR2 uses
        for m in controls.Control:
            self.assertEqual(int(m), joaat("INPUT_" + m.name), m.name)
            i = controls.action_index(m)
            self.assertEqual(controls.ACTION_NAMES[i], "INPUT_" + m.name)
        self.assertNotIn("COUNT", controls.Control.__members__)

    def test_lookups(self):
        self.assertEqual(len(controls.ACTION_HASHES), controls.ACTION_COUNT)
        self.assertEqual(controls.action_name("input_context"), "INPUT_CONTEXT")
        self.assertEqual(controls.action_name(controls.Control.CONTEXT), "INPUT_CONTEXT")
        self.assertIsNone(controls.action_index("INPUT_SPECIAL_ABILITY_SECONDARY"))  # GTA V only
        self.assertIsNone(controls.action_index(51))                                # a GTA V index is not a key
        self.assertTrue(controls.is_action(controls.ACTION_HASHES[0]))
        for gta5, key in controls.GTA5_INDEX_TO_CONTROL.items():
            self.assertTrue(controls.is_action(key), gta5)
        self.assertEqual(controls.GTA5_INDEX_TO_CONTROL[51], controls.Control.CONTEXT)

    def test_groups(self):
        self.assertEqual([int(g) for g in controls.ControlGroup], [0, 1, 2])


class TestScriptLayer(unittest.TestCase):
    def test_scenario_press_spec(self):
        self.assertEqual(scenario.parse("press", "INPUT_CONTEXT@16").value, controls.Control.CONTEXT)
        self.assertEqual(scenario.parse("press", "0xB73BCA77@16").value, 0xB73BCA77)
        self.assertEqual(scenario.parse("press", "51@16").value, 51)

    def test_mock_press_with_high_bit_key(self):
        from gta6mod import runtime
        from gta6mod.game import controls as ctl
        rt = runtime.use_mock()
        key = controls.Control.CONTEXT
        self.assertGreater(int(key), 0x7FFFFFFF)          # exercises the signed/unsigned slot path
        rt.backend.press_control(0, key)
        self.assertTrue(ctl.just_pressed(key))
        self.assertTrue(ctl.just_pressed("INPUT_CONTEXT"))
        self.assertFalse(ctl.just_pressed(controls.Control.JUMP))
        self.assertFalse(ctl.just_pressed(key, controls.ControlGroup.FRONTEND_CONTROL))


if __name__ == "__main__":
    unittest.main()
