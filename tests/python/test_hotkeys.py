"""Hotkeys: the pure state machine (fake input) and its wiring into Mod on the mock world (no game needed)."""
import os
import sys
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))

from gta6mod import runtime, scenario, script  # noqa: E402
from gta6mod.controls import Control  # noqa: E402
from gta6mod.game import Mod, wait  # noqa: E402
from gta6mod.game.hotkeys import HOLD, PRESS, RELEASE, Block, Hotkeys, action_key  # noqa: E402

K, S, C = 0x1000, 0x2000, 0x3000  # a key and two modifiers (any u32 works in the pure engine)
FRAME = 16
DISABLE = 0xFE99B66D079CF6BC  # DISABLE_CONTROL_ACTION (observed)


class FakePad:
    """Frame-by-frame input: ``held`` is the set of (action, pad) down this tick; edges come from the last tick."""

    def __init__(self):
        self.prev, self.held = set(), set()

    def tick(self, *down, pad=0):
        self.prev, self.held = self.held, {(a, pad) for a in down}

    def read(self, query, action, pad):
        k = (action, pad)
        if query == "pressed":
            return k in self.held
        if query == "just_pressed":
            return k in self.held and k not in self.prev
        if query == "just_released":
            return k in self.prev and k not in self.held
        raise AssertionError(query)


def run(hk, frames, pad=0):
    """frames: list of tuples of actions held on each tick. Returns [(tick time, binding name)]."""
    p, out = FakePad(), []
    for i, down in enumerate(frames):
        p.tick(*down, pad=pad)
        out += [(i * FRAME, b.fn) for b in hk.poll(p.read, i * FRAME)]
    return out


class TestEngine(unittest.TestCase):
    def test_press_release_and_tap(self):
        hk = Hotkeys()
        hk.add(PRESS, K, "press")
        hk.add(RELEASE, K, "release")
        hk.add(RELEASE, K, "tap", ms=100)
        short = [(), (K,), (K,), ()]              # pressed at 16, released at 48: 32 ms
        self.assertEqual(run(hk, short), [(16, "press"), (48, "release"), (48, "tap")])
        long = [(), (K,)] + [(K,)] * 10 + [()]    # 176 ms: no tap
        self.assertEqual(run(hk, long), [(16, "press"), (192, "release")])

    def test_hold_once_and_repeat(self):
        hk = Hotkeys()
        hk.add(HOLD, K, "hold", ms=100)
        hk.add(HOLD, K, "repeat", ms=48, repeat_ms=32)
        frames = [(), (K,)] + [(K,)] * 9 + [(), (K,), ()]  # held 16..160, released, short press at 192
        self.assertEqual(run(hk, frames), [(64, "repeat"), (96, "repeat"), (128, "hold"), (128, "repeat"),
                                           (160, "repeat")])

    def test_held_before_start_counts_from_next_press(self):
        hk = Hotkeys()
        hk.add(HOLD, K, "hold", ms=32)
        hk.add(RELEASE, K, "tap", ms=1000)
        p = FakePad()
        p.prev = {(K, 0)}             # K was already down before the first poll
        p.held = {(K, 0)}
        self.assertEqual(hk.poll(p.read, 0), [])
        p.tick(K)
        self.assertEqual(hk.poll(p.read, 64), [])
        p.tick()
        self.assertEqual(hk.poll(p.read, 80), [])  # the press was never seen, so it is no tap

    def test_chord_wins_and_consumes_the_press(self):
        hk = Hotkeys()
        hk.add(PRESS, K, "plain")
        hk.add(PRESS, K, "sprint", mods=S)
        hk.add(PRESS, K, "sprint+crouch", mods=(S, C))
        hk.add(RELEASE, K, "tap", ms=500)
        hk.add(HOLD, K, "hold", ms=32)
        self.assertEqual(run(hk, [(), (K,), ()]), [(16, "plain"), (32, "tap")])
        self.assertEqual(run(hk, [(S,), (S, K), (S, K), (S, K), (S,)]), [(16, "sprint")])
        self.assertEqual(run(hk, [(S, C), (S, C, K), ()]), [(16, "sprint+crouch")])
        # the modifier pressed after the key: no chord, the plain press already fired
        self.assertEqual(run(hk, [(), (K,), (K, S), (K, S), (S,)]), [(16, "plain"), (48, "hold"), (64, "tap")])

    def test_busy_binding_does_not_fire_but_others_do(self):
        hk = Hotkeys()
        a = hk.add(PRESS, K, "a")
        hk.add(PRESS, K, "b", mods=S)
        a.busy = True
        self.assertEqual(run(hk, [(), (K,)]), [])
        b = hk.bindings[1]
        b.busy = True
        self.assertEqual(run(hk, [(S,), (S, K)]), [])  # the busy chord still outranks the plain binding

    def test_pads_are_separate(self):
        hk = Hotkeys()
        hk.add(PRESS, K, "pad2", pad=2)
        self.assertEqual(run(hk, [(), (K,)], pad=0), [])
        self.assertEqual(run(hk, [(), (K,)], pad=2), [(16, "pad2")])

    def test_keys_and_validation(self):
        self.assertEqual(action_key("INPUT_CONTEXT"), int(Control.CONTEXT))
        self.assertEqual(action_key(Control.JUMP), 0xD9D0E1C0)
        self.assertEqual(action_key(-1), 0xFFFFFFFF)
        hk = Hotkeys()
        self.assertEqual(hk.add(PRESS, "INPUT_JUMP", None, mods="INPUT_SPRINT").mods,
                         frozenset({int(Control.SPRINT)}))
        for bad in [dict(kind=HOLD, action=K, fn=None),
                    dict(kind=PRESS, action=K, fn=None, mods=K),
                    dict(kind=HOLD, action=K, fn=None, ms=10, repeat_ms=0),
                    dict(kind="tap", action=K, fn=None)]:
            with self.assertRaises(ValueError, msg=bad):
                hk.add(**bad)
        with self.assertRaises(ValueError):
            Block([])
        self.assertTrue(Block([K]).active())
        self.assertFalse(Block([K], when=lambda: False).active())


class TestModHotkeys(unittest.TestCase):
    def setUp(self):
        self.rt = runtime.use_mock(quiet=True)
        self.sched = script.reset()
        self.mock = self.rt.backend

    def drive(self, ms, *presses):
        scenario.drive(self.sched, self.mock, ms, [scenario.parse("press", p) for p in presses])

    def test_scenario_hold_spec(self):
        ev = scenario.parse("press", "INPUT_CONTEXT@96+600")
        self.assertEqual((ev.at, ev.value, ev.hold), (96, Control.CONTEXT, 600))
        self.assertEqual(scenario.parse("press", "51@16").hold, 0)
        for bad in ("51@16+-5", "51@16+x"):
            with self.assertRaises(ValueError):
                scenario.parse("press", bad)
        with self.assertRaises(ValueError):
            scenario.parse("damage", "5@16+10")

    def test_mock_hold_and_release(self):
        from gta6mod.game import controls
        seen = []
        self.mock.press_control(0, Control.CONTEXT, 40)
        for _ in range(5):
            seen.append((controls.pressed(Control.CONTEXT), controls.just_released(Control.CONTEXT),
                         controls.released(Control.CONTEXT)))
            self.mock.step(FRAME)
        self.assertEqual(seen, [(True, False, False), (True, False, False), (True, False, False),
                                (False, True, True), (False, False, True)])

    def test_release_hold_and_chord_through_mod(self):
        m = Mod("hk")
        log = []
        m.on_key(Control.CONTEXT)(lambda: log.append(("press", self.mock.now_ms())))
        m.on_key(Control.CONTEXT, mods=Control.SPRINT)(lambda: log.append(("chord", self.mock.now_ms())))
        m.on_release(Control.CONTEXT, max_ms=200)(lambda: log.append(("tap", self.mock.now_ms())))
        m.on_hold(Control.CONTEXT, 500)(lambda: log.append(("hold", self.mock.now_ms())))
        self.drive(2000, "INPUT_CONTEXT@96",              # tap
                   "INPUT_CONTEXT@400+800",               # hold
                   "INPUT_SPRINT@1504+300", "INPUT_CONTEXT@1520+100")  # chord
        self.assertEqual(log, [("press", 96), ("tap", 112),
                               ("press", 400), ("hold", 912),
                               ("chord", 1520)])
        m.stop()

    def test_waiting_hold_handler_runs_as_its_own_script(self):
        m = Mod("hk")
        runs = []

        @m.on_hold(Control.CONTEXT, 100, repeat_ms=100)
        def slow():
            runs.append(self.mock.now_ms())
            yield wait(250)

        self.drive(1000, "INPUT_CONTEXT@0+600")
        # fires at 112, 224 (busy until ~368: skipped), 336 (busy: skipped), 448, 560 (busy: skipped)
        self.assertEqual(runs, [112, 448])
        m.stop()

    def test_block_disables_while_toggle_is_on(self):
        m = Mod("hk")
        menu = m.toggle(Control.CONTEXT)
        m.block(Control.ATTACK, "INPUT_AIM", when=menu)
        m.block(Control.JUMP)  # always

        def disabled():
            got = [(s[0], s[1] & 0xFFFFFFFF) for h, s in self.mock.calls if h == DISABLE]
            self.mock.calls.clear()
            return {a: got.count((0, int(a))) for a in (Control.ATTACK, Control.AIM, Control.JUMP)}

        self.drive(160)
        self.assertEqual(disabled(), {Control.ATTACK: 0, Control.AIM: 0, Control.JUMP: 10})
        self.drive(160, "INPUT_CONTEXT@0")  # the toggle turns on in the first tick, before the blocks run
        self.assertEqual(disabled(), {Control.ATTACK: 10, Control.AIM: 10, Control.JUMP: 10})
        menu.set(False)
        self.drive(160)
        self.assertEqual(disabled(), {Control.ATTACK: 0, Control.AIM: 0, Control.JUMP: 10})
        m.stop()
        self.drive(160)
        self.assertEqual(disabled(), {Control.ATTACK: 0, Control.AIM: 0, Control.JUMP: 0})

if __name__ == "__main__":
    unittest.main()
