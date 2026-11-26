"""gta6mod.ui: the module, the mock UI backend, the trainer_menu example, and the ctypes backend over gta6mod_mock.dll.

    python -I -m unittest discover -s sdk/tests/python -v

The CAbi tests need a built C mock (sdk\\luau\\build.cmd builds sdk/build/luau/gta6mod_mock.dll); they compare the
Python mock's transcript with the C mock's for the same frame function, so both mocks keep one contract.
"""
import ctypes
import os
import runpy
import sys
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))

from gta6mod import runtime, scenario, script, ui  # noqa: E402
from gta6mod.game import Vector3  # noqa: E402
from gta6mod.runtime import cabi  # noqa: E402
from gta6mod.runtime.backend import NativeError  # noqa: E402
from gta6mod.runtime.mock_ui import MockUi  # noqa: E402

EXAMPLES = os.path.join(SDK, "examples", "python")
MOCK_DLLS = [os.path.join(SDK, "build", "gta6mod_mock.dll"), os.path.join(SDK, "build", "luau", "gta6mod_mock.dll")]
# Newest build wins: a stale sdk/build copy must not shadow a fresh sdk/build/luau one.
MOCK_DLL = max((p for p in MOCK_DLLS if os.path.exists(p)), key=os.path.getmtime, default=None)


def fresh():
    rt = runtime.use_mock(quiet=True)
    return rt, script.reset()


def every_widget(state):
    """One frame function using every widget; returns the frame function. Shared by the parity test."""
    def draw(frame):
        with ui.window("Widgets", x=10, y=20, auto_resize=True, closable=True) as w:
            if w:
                ui.text("frame", frame.frame >= 0)
                ui.text_colored((1, 0, 0), "red")
                if ui.button("Heal"):
                    state["heals"] += 1
                _, state["god"] = ui.checkbox("God mode", state["god"])
                ui.same_line()
                _, state["wanted"] = ui.slider_int("Wanted", state["wanted"], 0, 5)
                _, state["speed"] = ui.slider_float("Speed", state["speed"], 0.0, 10.0)
                _, state["name"] = ui.input_text("Name", state["name"], 32)
                _, state["weather"] = ui.combo("Weather", state["weather"], ["Clear", "Rain", "Fog"])
                ui.separator()
                with ui.tree("Advanced") as t:
                    if t:
                        ui.text("inside")
                with ui.tab_bar("tabs") as bar:
                    if bar:
                        with ui.tab("First") as a:
                            if a:
                                state["tab"] = "First"
                        with ui.tab("Second") as b:
                            if b:
                                state["tab"] = "Second"
                _, state["tint"] = ui.color_edit("Tint", state["tint"])
                ui.plot_lines("Plot", [1, 2, 3], overlay="fps")
            state["open"] = w.open
    return draw


def new_state():
    return {"heals": 0, "god": False, "wanted": 0, "speed": 1.0, "name": "Lucia", "weather": 0, "tab": "",
            "tint": (1.0, 1.0, 1.0, 1.0), "open": True}


def drive_widgets(backend, sched):
    """Interactions on every widget, then frames; returns (state, transcripts)."""
    state = new_state()
    ui.on_frame("widgets", every_widget(state))
    u = backend.ui
    u.frame()
    first = u.transcript
    for label in ("Heal", "God mode", "Advanced", "Second"):
        u.click(label)
    for label, value in (("Wanted", "4"), ("Speed", "2.5"), ("Name", "Jason"), ("Weather", "Fog"), ("Tint", "0.1,0.2,0.3,0.4")):
        u.set(label, value)
    u.frame()
    second = u.transcript
    u.click("Widgets")  # the close button of the closable window
    u.frame()
    return state, first, second


class TestMockUi(unittest.TestCase):
    def setUp(self):
        self.rt, self.sched = fresh()
        self.mock = self.rt.backend
        self.addCleanup(ui._remove_all)

    def test_every_widget_and_interaction(self):
        state, first, second = drive_widgets(self.mock, self.sched)
        self.assertIn("frame widgets\n  next_window_pos 10 20 (first use)\n  begin Widgets\n    text frame True\n", first)
        self.assertIn("    checkbox God mode = 0\n", first)
        self.assertIn("    tree_node Advanced\n", first)
        self.assertIn("      tab_item First (selected)\n", first)
        self.assertIn("    plot_lines Plot (3 values, fps)\n", first)
        self.assertIn("    button Heal (clicked)\n", second)
        self.assertIn("    tree_node Advanced (open)\n      text inside\n    tree_pop\n", second)
        self.assertIn("    combo Weather = 2 (Fog)\n", second)
        self.assertIn("    color_edit Tint = 0.100 0.200 0.300 0.400\n", second)
        self.assertEqual(state["heals"], 1)
        self.assertTrue(state["god"])
        self.assertEqual((state["wanted"], state["name"], state["weather"], state["tab"]), (4, "Jason", 2, "Second"))
        self.assertAlmostEqual(state["speed"], 2.5)
        self.assertAlmostEqual(state["tint"][3], 0.4, places=6)
        self.assertFalse(state["open"], "a click on the title closes a closable window")
        self.assertEqual((self.mock.ui.errors, self.mock.ui.pending), (0, 0))

    def test_hidden_menu_runs_only_always_frames(self):
        runs = {"menu": 0, "hud": 0}
        ui.on_frame("menu", lambda f: runs.__setitem__("menu", runs["menu"] + 1))
        ui.on_frame("hud", lambda f: runs.__setitem__("hud", runs["hud"] + 1), always=True)
        self.mock.ui.set_menu_open(False)
        self.assertEqual(self.mock.ui.frame(), 1)
        self.mock.ui.set_menu_open(True)
        self.assertEqual(self.mock.ui.frame(), 2)
        self.assertEqual(runs, {"menu": 1, "hud": 2})

    def test_unbalanced_calls_are_errors(self):
        ui.on_frame("bad", lambda f: ui.begin_window("left open"))
        self.mock.ui.frame()
        self.assertEqual(self.mock.ui.errors, 1)
        self.assertIn("error: ui frame 'bad' left 1 window(s)", self.mock.ui.transcript)
        self.assertIn(("ERROR", "mock-ui", "ui frame 'bad' left 1 window(s), 0 tree node(s), 0 tab bar(s), 0 tab item(s) open"),
                      self.mock.logs)

    def test_natives_are_refused_inside_a_frame(self):
        from gta6mod.game import player
        seen = {}

        def draw(frame):
            try:
                player.ped()
            except NativeError as e:
                seen["status"] = e.status
        ui.on_frame("natives", draw)
        self.mock.ui.frame()
        self.assertEqual(seen.get("status"), 5)  # G6_E_NOT_ON_SCRIPT_THREAD
        self.assertIsNotNone(player.ped())       # fine outside the frame


class TestUiModule(unittest.TestCase):
    def setUp(self):
        self.rt, self.sched = fresh()
        self.mock = self.rt.backend
        self.addCleanup(ui._remove_all)

    def test_widgets_only_inside_a_frame(self):
        with self.assertRaises(RuntimeError):
            ui.button("outside")
        with self.assertRaises(RuntimeError):
            ui.window("outside")

    def test_registration(self):
        self.assertTrue(ui.available())

        @ui.on_frame("deco")
        def deco(frame):
            pass
        self.assertTrue(ui.on_frame("direct", lambda f: None))
        with self.assertRaises(ValueError):
            ui.on_frame("direct", lambda f: None)
        self.assertEqual(self.mock.ui.registered, 2)
        self.assertTrue(ui.remove("deco"))
        self.assertFalse(ui.remove("deco"))
        self.assertEqual(self.mock.ui.registered, 1)

    def test_defer_runs_on_the_tick_and_may_wait(self):
        from gta6mod.game import player, wait
        log = []

        def later():
            log.append(("plain", player.ped() is not None))

        def waits():
            log.append(("gen start", self.mock.now_ms()))
            yield wait(100)
            log.append(("gen end", self.mock.now_ms()))

        def draw(frame):
            if frame.frame == 0:
                ui.defer(later)
                ui.defer(waits)
        ui.on_frame("d", draw)
        scenario.drive(self.sched, self.mock, 400)
        self.assertEqual(log[0], ("plain", True))
        self.assertEqual(log[1][0], "gen start")
        self.assertEqual(log[2][0], "gen end")
        self.assertGreaterEqual(log[2][1] - log[1][1], 100)

    def test_failing_frame_is_logged_then_removed(self):
        def boom(frame):
            raise ValueError("bad menu")
        ui.on_frame("boom", boom)
        scenario.drive(self.sched, self.mock, 16 * 12)
        errors = [m for lvl, src, m in self.mock.logs if lvl == "ERROR" and src == "boom"]
        self.assertEqual(len(errors), ui.MAX_ERRORS + 1)  # ten failures, then the removal line
        self.assertIn("removed", errors[-1])
        self.assertEqual(self.mock.ui.registered, 0)

    def test_stop_event_removes_frames(self):
        ui.on_frame("menu", lambda f: None)
        self.assertEqual(self.mock.ui.registered, 1)
        self.sched.stop()
        self.assertEqual(self.mock.ui.registered, 0)

    def test_backend_without_ui(self):
        class NoUi(runtime.Backend):
            def invoke(self, native_hash, slots):
                return [0, 0, 0, 0]

            def build_info(self):
                return runtime.BuildInfo("1.0.2.1", "none", "0" * 64, 0, 0)

            def now_ms(self):
                return 0
        runtime.use(NoUi())
        self.assertFalse(ui.available())
        self.assertFalse(ui.on_frame("menu", lambda f: None))

    def test_scenario_parses_ui_events(self):
        self.assertEqual(scenario.parse("ui_click", "God mode@496"), scenario.Event(496, "ui_click", "God mode"))
        self.assertEqual(scenario.parse("ui_set", "Tint=0.1,0.2@96"), scenario.Event(96, "ui_set", ("Tint", "0.1,0.2")))
        with self.assertRaises(ValueError):
            scenario.parse("ui_menu", "2@0")
        with self.assertRaises(ValueError):
            scenario.parse("ui_set", "NoValue@0")


class TestTrainerMenu(unittest.TestCase):
    """examples/python/trainer_menu.py with the scenario from its docstring (the Luau twin runs the same one)."""

    def test_trainer_menu(self):
        rt, sched = fresh()
        self.addCleanup(ui._remove_all)
        runpy.run_path(os.path.join(EXAMPLES, "trainer_menu.py"), run_name="__gta6mod__")
        events = [scenario.parse("ui_click", "God mode@496"), scenario.parse("damage", "50@1008"),
                  scenario.parse("ui_click", "Never wanted@1504"), scenario.parse("wanted", "3@2000"),
                  scenario.parse("ui_click", "Heal@2496")]
        mock = rt.backend
        scenario.drive(sched, mock, 3000, events)
        self.assertEqual([s.name for s in sched.scripts if s.error], [])
        msgs = [m for _, src, m in mock.logs if src == "trainer_menu"]
        self.assertEqual(msgs, ["god mode ON", "never wanted ON (max wanted level was 5)", "cleared wanted level 3", "healed"])
        self.assertTrue(mock.player.invincible)
        self.assertEqual(mock.player.health, 200)  # the damage at 1008 was blocked
        self.assertEqual(mock.world.max_wanted_level, 0)
        self.assertEqual((mock.ui.errors, mock.ui.pending), (0, 0))
        last = mock.ui.transcript
        self.assertIn("    checkbox God mode = 1\n    checkbox Never wanted = 1\n    button Heal\n", last)
        self.assertIn("    text Player: -1000.0 -2000.0 15.0\n", last)
        self.assertEqual(sched.stop(), [])
        self.assertEqual([m for _, src, m in mock.logs if src == "trainer_menu"][-2:],
                         ["god mode OFF", "never wanted OFF (max wanted level restored to 5)"])
        self.assertEqual(mock.ui.registered, 0)


@unittest.skipUnless(MOCK_DLL, "build the C mock first (sdk\\luau\\build.cmd builds sdk/build/luau/gta6mod_mock.dll)")
class TestCAbiUi(unittest.TestCase):
    def setUp(self):
        self.rt = runtime.use_cabi(MOCK_DLL)
        self.sched = script.reset()
        self.b = self.rt.backend
        self.addCleanup(ui._remove_all)

    def test_struct_layout_matches_the_header(self):
        # gta6mod.h, x64: checked against the C compiler in sdk/tests/c/headers_check.cpp
        self.assertEqual(ctypes.sizeof(cabi.G6UiFrame), 32)
        self.assertEqual(ctypes.sizeof(cabi.G6UiFrameDesc), 32)
        self.assertEqual(ctypes.sizeof(cabi.G6UiApi), 8 + 25 * 8)
        self.assertEqual(ctypes.sizeof(cabi.G6HostApi), 8 + 12 * 8)
        self.assertIsNotNone(self.b.ui)
        self.assertEqual(self.b.ui.api.abi_major, 1)

    def test_same_transcript_as_the_python_mock(self):
        c_state, c_first, c_second = drive_widgets(self.b, self.sched)
        ui._remove_all()
        rt, sched = fresh()
        p_state, p_first, p_second = drive_widgets(rt.backend, sched)
        self.assertEqual(c_first, p_first)
        self.assertEqual(c_second, p_second)
        self.assertEqual(c_state["heals"], p_state["heals"])
        self.assertEqual(c_state["god"], p_state["god"])
        self.assertEqual(c_state["name"], p_state["name"])
        self.assertEqual(c_state["tab"], p_state["tab"])
        self.assertAlmostEqual(c_state["speed"], p_state["speed"], places=6)
        self.assertFalse(c_state["open"])

    def test_frames_and_unregister_through_the_dll(self):
        runs = []
        self.assertTrue(ui.on_frame("hud", lambda f: runs.append(f.frame), always=True))
        self.b.ui.set_menu_open(False)
        self.assertEqual(self.b.ui.frame(), 1)
        self.b.ui.set_menu_open(True)
        self.assertEqual(self.b.ui.registered, 1)
        self.assertTrue(ui.remove("hud"))
        self.assertEqual(self.b.ui.registered, 0)
        self.assertEqual(self.b.ui.frame(), 0)
        self.assertEqual(len(runs), 1)
        self.assertEqual(self.b.ui.errors, 0)


if __name__ == "__main__":
    unittest.main()
