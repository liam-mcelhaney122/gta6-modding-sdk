"""In-game Python host logic (gta6mod.runtime.host / embedded) with a fake ``_g6`` over the Python mock world.

The same host code runs inside g6_python.dll against the C mock world in g6_python_test.exe (build.ps1 -Test);
this file checks the Python half without the plugin:

    python -I -m unittest discover -s sdk/tests/python -v
"""
import os
import shutil
import sys
import tempfile
import textwrap
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))

from gta6mod import scenario, script  # noqa: E402
from gta6mod.runtime import host as hostmod  # noqa: E402
from gta6mod.runtime.backend import NativeError  # noqa: E402
from gta6mod.runtime.embedded import EmbeddedBackend, GuardClosed  # noqa: E402
from gta6mod.runtime.mock import MockBackend  # noqa: E402

EXAMPLES = os.path.join(SDK, "examples", "python")


class FakeG6:
    """The ``_g6`` surface of g6_python.dll, over a Python MockBackend."""

    def __init__(self, mock: MockBackend):
        self.mock = mock
        self.logs: list[tuple[int, str, str]] = []
        self.watch_calls: list[bool] = []
        self.budget_exc = None
        self.guard_closed = False
        self.exports: dict[str, int] = {}

    def invoke(self, native_hash, slots):
        if self.guard_closed:
            return 7, (0, 0, 0, 0)
        try:
            return 0, tuple(self.mock.invoke(native_hash, list(slots)))
        except NativeError as e:
            return e.status, (0, 0, 0, 0)

    def log(self, level, source, message):
        self.logs.append((level, source, message))

    def now_ms(self):
        return self.mock.now_ms()

    def build_info(self):
        return 0, "1.0.2.1", "mock", "0" * 64, len(self.mock.handlers), 0

    def ui_proc(self, name):
        return self.exports.get(name, 0)

    def watch(self, on, budget_ms=0):
        self.watch_calls.append(bool(on))

    def set_budget_exception(self, cls):
        self.budget_exc = cls

    def on_script_thread(self):
        return True

    def lines(self, source=None, level=None):
        names = {0: "DEBUG", 1: "INFO", 2: "WARN", 3: "ERROR"}
        return [f"[{names[lv]}] {src}: {msg}" for lv, src, msg in self.logs
                if (source is None or src == source) and (level is None or lv == level)]


def drive(host, mock, ms, events=(), start=0):
    t = start
    while t < start + ms:
        for ev in events:
            if ev.at == t:
                scenario.apply(mock, ev)
        host.tick()
        mock.step(16)
        t += 16
    return t


class HostCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="g6pyhost_")
        self.mock = MockBackend(quiet=True)
        self.g6 = FakeG6(self.mock)
        self.default_sched = script.scheduler()

    def tearDown(self):
        self.assertIs(script.scheduler(), self.default_sched, "the host must restore the default scheduler")
        for name in [n for n in sys.modules if n.startswith("g6mods.")]:
            del sys.modules[name]
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, name, body):
        path = os.path.join(self.dir, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(textwrap.dedent(body))
        return path

    def host(self, **kw):
        h = hostmod.ModHost(self.dir, self.g6, **kw)
        h.load_all()
        return h


class TestExamplesThroughHost(HostCase):
    """The five examples through ModHost + EmbeddedBackend: the log lines of test_script_layer.TestExampleMods."""

    def run_example(self, name, ms, **events):
        shutil.copy(os.path.join(EXAMPLES, f"{name}.py"), self.dir)
        h = self.host()
        self.assertEqual(h.status(), {name: hostmod.LOADED})
        evs = [scenario.parse(kind, spec) for kind, specs in events.items() for spec in specs]
        drive(h, self.mock, ms, evs)
        self.assertEqual(self.g6.lines(level=3), [])
        return h, self.g6.lines(source=name)

    def test_god_mode(self):
        h, logs = self.run_example("god_mode", 4000, damage=["50@496", "50@2000", "30@3504"],
                                   press=["INPUT_DETONATE@1008", "INPUT_DETONATE@3008"])
        self.assertEqual(logs, ["[INFO] god_mode: god mode ON", "[INFO] god_mode: god mode OFF"])
        self.assertEqual(self.mock.player.health, 170)

    def test_never_wanted(self):
        _, logs = self.run_example("never_wanted", 4000, press=["INPUT_VEH_HEADLIGHT@496", "INPUT_VEH_HEADLIGHT@3008"],
                                   wanted=["3@1504"])
        self.assertEqual(logs, ["[INFO] never_wanted: never wanted ON (max wanted level was 5)",
                                "[INFO] never_wanted: cleared wanted level 3",
                                "[INFO] never_wanted: never wanted OFF (max wanted level restored to 5)"])

    def test_waypoint_teleport(self):
        _, logs = self.run_example("waypoint_teleport", 3000, press=["INPUT_VEH_DUCK@496", "INPUT_VEH_DUCK@1504"],
                                   waypoint=["250,-400,0@1008"])
        self.assertEqual(logs, ["[WARN] waypoint_teleport: no waypoint set",
                                "[INFO] waypoint_teleport: teleported to 250.0 -400.0 13.0"])

    def test_vehicle_spawner(self):
        _, logs = self.run_example("vehicle_spawner", 3000, press=["INPUT_CONTEXT_SECONDARY@496"], damage=["150@2000"])
        self.assertEqual(logs, ["[INFO] vehicle_spawner: spawned adder and seated the player",
                                "[INFO] vehicle_spawner: repaired vehicle (850 -> 1000)"])

    def test_time_control(self):
        _, logs = self.run_example("time_control", 3000, press=[
            "INPUT_MULTIPLAYER_INFO@496", "INPUT_MULTIPLAYER_INFO@1008", "INPUT_SPECIAL_ABILITY@1504",
            "INPUT_MULTIPLAYER_INFO@1600+1200"])
        self.assertEqual(logs, ["[INFO] time_control: time scale 0.50", "[INFO] time_control: time scale 0.20",
                                "[INFO] time_control: clock 13:00", "[INFO] time_control: time scale reset to 1.00"])

    def test_stop_event_switches_god_mode_off(self):
        h, _ = self.run_example("god_mode", 1008, press=["INPUT_DETONATE@496"])
        self.assertTrue(self.mock.player.invincible)
        self.assertEqual(h.stop(), 0)
        self.assertEqual(h.stop(), 0)  # once only
        self.assertFalse(self.mock.player.invincible)
        self.assertEqual(self.g6.lines(source="god_mode")[-1], "[INFO] god_mode: god mode OFF")
        self.assertEqual(h.status(), {"god_mode": hostmod.STOPPED})
        drive(h, self.mock, 160, start=1008)  # ticks after the stop event do nothing
        self.assertEqual(len(self.g6.lines(source="god_mode")), 2)


class TestIsolation(HostCase):
    def test_failures_disable_only_their_mod(self):
        self.write("a_good.py", """
            from gta6mod.game import Mod, wait
            m = Mod("good")
            n = {"ticks": 0}
            @m.loop
            def run():
                while True:
                    n["ticks"] += 1
                    if n["ticks"] % 10 == 0:
                        m.log(f"good tick {n['ticks']}")
                    yield wait(0)
            @m.on_stop
            def bye():
                m.log("good stopped")
            """)
        self.write("bad_load.py", "raise RuntimeError('broken at import')\n")
        self.write("bad_tick.py", """
            from gta6mod.game import Mod, wait
            m = Mod("bad_tick")
            @m.loop
            def run():
                yield wait(100)
                raise ValueError("broken in a tick")
            @m.on_stop
            def cleanup():
                m.log("bad_tick cleanup ran")
            """)
        self.write("bad_exit.py", """
            import sys
            from gta6mod import script
            @script.register("exit")
            def run():
                yield script.wait(200)
                sys.exit(3)
            """)
        self.write("_helper.py", "raise AssertionError('underscore files are not mods')\n")
        h = self.host()
        self.assertEqual(h.status(), {"a_good": "loaded", "bad_exit": "loaded", "bad_load": "disabled",
                                      "bad_tick": "loaded"})
        drive(h, self.mock, 480)
        self.assertEqual(h.status(), {"a_good": "loaded", "bad_exit": "disabled", "bad_load": "disabled",
                                      "bad_tick": "disabled"})
        self.assertIn("[INFO] bad_tick: bad_tick cleanup ran", self.g6.lines(source="bad_tick"))
        self.assertIn("[INFO] good: good tick 30", self.g6.lines(source="good"))
        errors = "\n".join(self.g6.lines(level=3))
        self.assertIn("broken at import", errors)
        self.assertIn("broken in a tick", errors)
        self.assertIn("SystemExit: 3", errors)
        self.assertIn("mod bad_tick disabled", errors)
        self.assertEqual(h.stop(), 0)
        self.assertEqual(self.g6.lines(source="good")[-1], "[INFO] good: good stopped")
        h.shutdown()
        self.assertFalse([n for n in sys.modules if n.startswith("g6mods.")])

    def test_mods_have_separate_namespaces_and_schedulers(self):
        body = """
            from gta6mod import script
            from gta6mod.runtime import current
            VALUE = {v!r}
            @script.register("main")
            def main():
                current().log("INFO", "ns", f"{{__name__}} {{VALUE}}")
                yield script.wait(10000)
            """
        self.write("one.py", body.format(v=1))
        self.write("two.py", body.format(v=2))
        h = self.host()
        drive(h, self.mock, 16)
        self.assertEqual(self.g6.lines(source="ns"), ["[INFO] ns: g6mods.one 1", "[INFO] ns: g6mods.two 2"])
        self.assertIsNot(h.mods["one"].sched, h.mods["two"].sched)
        self.assertEqual(sys.modules["g6mods.two"].VALUE, 2)
        self.assertEqual(self.g6.watch_calls.count(True), self.g6.watch_calls.count(False))

    def test_guard_closed_pauses_every_mod(self):
        self.write("ped.py", """
            from gta6mod.game import Mod, player
            m = Mod("ped")
            @m.every(100)
            def look():
                player.ped().health
            @m.on_stop
            def bye():
                m.log("stop handler ran")
            """)
        h = self.host()
        drive(h, self.mock, 160)
        self.g6.guard_closed = True
        drive(h, self.mock, 160, start=160)
        self.assertEqual(h.paused, "guard")
        self.assertEqual(h.status(), {"ped": "loaded"})  # paused, not disabled
        self.assertIn("G6_E_ONLINE_SESSION", "\n".join(self.g6.lines(source="python", level=3)))
        h.stop()
        self.assertNotIn("[INFO] ped: stop handler ran", self.g6.lines(source="ped"))

    def test_hot_reload(self):
        path = self.write("live.py", """
            from gta6mod.game import Mod, wait
            m = Mod("live")
            @m.loop
            def run():
                m.log("v1")
                yield wait(100000)
            @m.on_stop
            def bye():
                m.log("v1 stopped")
            """)
        h = self.host(hot_reload=True, reload_poll_ms=0)
        drive(h, self.mock, 32)
        with open(path, "w", encoding="utf-8") as f:
            f.write("from gta6mod.game import Mod, wait\nm = Mod('live')\n@m.loop\ndef run():\n"
                    "    m.log('version two')\n    yield wait(100000)\n")
        drive(h, self.mock, 32, start=32)
        self.assertEqual(self.g6.lines(source="live"), ["[INFO] live: v1", "[INFO] live: v1 stopped",
                                                        "[INFO] live: version two"])
        os.remove(path)
        drive(h, self.mock, 16, start=64)
        self.assertEqual(h.status(), {})


class TestEmbeddedBackend(unittest.TestCase):
    def test_status_mapping_and_ui_prefix(self):
        g6 = FakeG6(MockBackend(quiet=True))
        g6.exports = {"g6_ui_test": 0x1234, "g6_get_host_api": 0x5678}
        b = EmbeddedBackend(g6)
        self.assertTrue(b.accepts_unverified)  # platform "mock"
        self.assertEqual(b.ui_proc("g6_ui_test"), 0x1234)
        self.assertEqual(b.ui_proc("g6_get_host_api"), 0)  # only g6_ui_* is resolvable
        self.assertTrue(b.ui_available("g6_ui_test"))
        with self.assertRaises(NativeError) as cm:
            b.invoke(0x1, [])
        self.assertEqual(cm.exception.status, 2)
        g6.guard_closed = True
        with self.assertRaises(GuardClosed):
            b.invoke(0x096275889B8E0EE0, [])

    def test_log_stream_splits_lines(self):
        g6 = FakeG6(MockBackend(quiet=True))
        s = hostmod.LogStream(g6, 1, "stdout")
        s.write("a\nb")
        s.write("c\n")
        s.write("tail")
        s.flush()
        self.assertEqual([m for _, _, m in g6.logs], ["a", "bc", "tail"])


if __name__ == "__main__":
    unittest.main()
