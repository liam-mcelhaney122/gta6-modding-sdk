"""gta6mod.ui in game (EmbeddedUi): frame functions recorded on the script thread, replayed elsewhere.

    python -I -m unittest discover -s sdk/tests/python -v

``FakeRetained`` is ``sdk/runtime/src/ui_retained.cpp`` in Python (same wire format, same replay rules) drawing
into a ``MockUi``, so the recorder, the wire format and the one-tick interaction model are checked here without
a build. The C++ module itself runs the same mods in g6_python_test ("ui_trainer", "ui_widgets").

Transcript parity: sdk/tests/ui/*.ui.txt are written by g6_luau_runner (--ui-log) for the Luau twins of these
mods; the Python mock (immediate mode) must equal them, and the retained path too, except for the one frame in
which a tree node opens (its contents appear one frame later; python_host.md "Python UI").
"""
import os
import runpy
import struct
import sys
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))

from gta6mod import runtime, scenario, script, ui  # noqa: E402
from gta6mod.runtime import embedded_ui as E  # noqa: E402
from gta6mod.runtime.backend import NativeError  # noqa: E402

import _ui_log  # noqa: E402

EXAMPLES = os.path.join(SDK, "examples", "python")
HOST_MODS = os.path.join(SDK, "python", "host", "tests", "mods")
TRAINER_EVENTS = [("damage", "30@240"), ("ui_click", "God mode@496"), ("damage", "50@1008"),
                  ("ui_click", "Never wanted@1504"), ("wanted", "3@2000"), ("ui_click", "Heal@2496")]
WIDGET_EVENTS = [("ui_click", "Heal@96"), ("ui_click", "God mode@96"), ("ui_set", "Wanted=4@96"),
                 ("ui_set", "Speed=2.5@96"), ("ui_set", "Name=Jason@96"), ("ui_set", "Weather=Fog@96"),
                 ("ui_click", "Advanced@96"), ("ui_click", "Second@96"), ("ui_set", "Tint=0.1,0.2,0.3,0.4@96"),
                 ("ui_menu", "0@496"), ("ui_menu", "1@800")]


# ------------------------------------------------------------------ ui_retained.cpp, in Python
class _Reader:
    def __init__(self, data):
        self.d, self.at = data, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.at)
        self.at += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def str(self, optional=False):
        n = self.take("I")
        if n == E.NO_STRING:
            if not optional:
                raise ValueError("missing string")
            return None
        s = self.d[self.at:self.at + n]
        if len(s) != n or b"\0" in s:
            raise ValueError("bad string")
        self.at += n
        return s.decode("utf-8")


def decode(program):
    """Commands as dicts, with 'end' on openers and 'occ'; raises ValueError like the C parser refuses."""
    r = _Reader(program)
    magic, version, _, count = r.take("IHHI")
    if magic != E.PROGRAM_MAGIC or version != E.VERSION:
        raise ValueError("header")
    cmds, stack, seen = [], [], {}
    for idx in range(count):
        op = r.take("B")
        c = {"op": op, "s": ""}
        if op == E.BEGIN:
            c.update(s=r.str(), closable=r.take("B"), flags=r.take("I"))
            stack.append(("W", idx))
        elif op in (E.NEXT_POS, E.NEXT_SIZE):
            c.update(x=r.take("f"), y=r.take("f"), cond=r.take("I"))
        elif op in (E.TEXT, E.BUTTON):
            c.update(s=r.str())
        elif op == E.TEXT_COLORED:
            c.update(rgba=r.take("ffff"), s=r.str())
        elif op == E.CHECKBOX:
            c.update(s=r.str(), v=r.take("i"))
        elif op == E.SLIDER_INT:
            c.update(s=r.str(), v=r.take("i"), lo=r.take("i"), hi=r.take("i"))
        elif op == E.SLIDER_FLOAT:
            c.update(s=r.str(), v=r.take("f"), lo=r.take("f"), hi=r.take("f"), fmt=r.str(True))
        elif op == E.INPUT_TEXT:
            c.update(s=r.str(), v=r.str(), cap=r.take("I"))
        elif op == E.COMBO:
            c.update(s=r.str(), v=r.take("i"))
            c["items"] = [r.str() for _ in range(r.take("I"))]
        elif op in (E.TREE_NODE, E.TAB_BAR, E.TAB_ITEM):
            c.update(s=r.str(), body=r.take("B"))
            if op == E.TAB_ITEM and (not stack or stack[-1][0] != "B"):
                raise ValueError("tab item outside a tab bar")
            if c["body"]:
                stack.append(({E.TREE_NODE: "T", E.TAB_BAR: "B", E.TAB_ITEM: "I"}[op], idx))
        elif op == E.COLOR_EDIT:
            c.update(s=r.str(), rgba=r.take("ffff"), flags=r.take("I"))
        elif op == E.PLOT_LINES:
            c.update(s=r.str())
            n = r.take("I")
            c["values"] = list(r.take(f"{n}f")) if n > 1 else ([r.take("f")] if n == 1 else [])
            c.update(overlay=r.str(True), scale=r.take("ffff"))
        elif op in (E.END, E.TREE_POP, E.END_TAB_BAR, E.END_TAB_ITEM):
            kind = {E.END: "W", E.TREE_POP: "T", E.END_TAB_BAR: "B", E.END_TAB_ITEM: "I"}[op]
            if not stack or stack[-1][0] != kind:
                raise ValueError("unbalanced")
            cmds[stack.pop()[1]]["end"] = idx
        elif op not in (E.SEPARATOR, E.SAME_LINE):
            raise ValueError(f"op {op}")
        c["occ"] = seen.get((op, c["s"]), 0)
        seen[(op, c["s"])] = c["occ"] + 1
        cmds.append(c)
    if stack or r.at != len(program):
        raise ValueError("unbalanced or trailing bytes")
    return cmds


class FakeRetained:
    """The transport EmbeddedUi talks to; frames are drawn by ``target.frame()`` (the 'render thread')."""

    def __init__(self, target):
        self.target = target
        self.slots = {}
        self.next = 1
        self.refused = 0

    def version(self):
        return E.VERSION

    def create(self, name, flags):
        h = self.next
        self.next += 1
        slot = {"program": None, "states": {}, "values": {}, "events": {}, "frames": 0, "frame": None}
        slot["fid"] = self.target.frame_register(name, lambda fr, h=h: self._draw(h, fr), flags)
        self.slots[h] = slot
        return E.G6_OK, h

    def destroy(self, h):
        slot = self.slots.pop(h, None)
        if slot is None:
            return E.G6_E_INVALID_ARG
        self.target.frame_unregister(slot["fid"])
        return E.G6_OK

    def submit(self, h, program):
        try:
            cmds = decode(program)
        except (ValueError, struct.error):
            self.refused += 1
            return E.G6_E_INVALID_ARG
        slot = self.slots[h]
        slot["program"] = cmds
        slot["values"] = {k: v for k, v in slot["values"].items() if not v[1]}
        return E.G6_OK

    def poll(self, h):
        s = self.slots[h]
        out = bytearray(struct.pack("<IHH", E.RESULT_MAGIC, E.VERSION, 0))
        f = s["frame"]
        out += struct.pack("<III", s["frames"], 1 if f else 0, 1 if f and f.menu_open else 0)
        out += struct.pack("<fffQ", f.width if f else 0, f.height if f else 0, f.dt if f else 0, f.frame if f else 0)

        def key(k):
            b = k[2].encode("utf-8")
            return struct.pack("<BII", k[0], k[1], len(b)) + b
        out += struct.pack("<I", len(s["states"]))
        for k, v in s["states"].items():
            out += key(k) + struct.pack("<B", v)
        fresh = [(k, v[0]) for k, v in s["values"].items() if not v[1]]
        out += struct.pack("<I", len(fresh))
        for k, v in fresh:
            out += key(k)
            if k[0] == E.SLIDER_FLOAT:
                out += struct.pack("<f", v)
            elif k[0] == E.INPUT_TEXT:
                b = v.encode("utf-8")
                out += struct.pack("<I", len(b)) + b
            elif k[0] == E.COLOR_EDIT:
                out += struct.pack("<ffff", *v)
            else:
                out += struct.pack("<i", v)
        out += struct.pack("<I", len(s["events"]))
        for k, (clicks, closed) in s["events"].items():
            out += key(k) + struct.pack("<IB", clicks, closed)
        s["values"] = {k: (v[0], True) for k, v in s["values"].items()}
        s["events"] = {}
        s["frames"] = 0
        return E.G6_OK, bytes(out)

    def _draw(self, h, frame):
        s = self.slots.get(h)
        if s is None:
            return
        ui = self.target
        cmds = s["program"] or []
        ovr = {k: v[0] for k, v in s["values"].items()}

        def k_of(c):
            return (c["op"], c["occ"], c["s"])

        def run(i, stop):
            while i < stop:
                c = cmds[i]
                op, k = c["op"], k_of(c)
                if op == E.BEGIN:
                    drawn, open_ = ui.begin(c["s"], bool(c["closable"]), c["flags"])
                    s["states"][k] = 1 if drawn else 0
                    if c["closable"] and not open_:
                        s["events"][k] = (s["events"].get(k, (0, 0))[0], 1)
                    if drawn:
                        run(i + 1, c["end"])
                    ui.end()
                    i = c["end"] + 1
                    continue
                if op in (E.TREE_NODE, E.TAB_BAR, E.TAB_ITEM):
                    fn = {E.TREE_NODE: ui.tree_node, E.TAB_BAR: ui.begin_tab_bar, E.TAB_ITEM: ui.begin_tab_item}[op]
                    close = {E.TREE_NODE: ui.tree_pop, E.TAB_BAR: ui.end_tab_bar, E.TAB_ITEM: ui.end_tab_item}[op]
                    open_ = fn(c["s"])
                    s["states"][k] = 1 if open_ else 0
                    if open_ and c["body"]:
                        run(i + 1, c["end"])
                    if open_:
                        close()
                    i = c["end"] + 1 if c["body"] else i + 1
                    continue
                if op == E.NEXT_POS:
                    ui.set_next_window_pos(c["x"], c["y"], c["cond"] == E.COND_ALWAYS)
                elif op == E.NEXT_SIZE:
                    ui.set_next_window_size(c["x"], c["y"], c["cond"] == E.COND_ALWAYS)
                elif op == E.TEXT:
                    ui.text(c["s"])
                elif op == E.TEXT_COLORED:
                    ui.text_colored(c["rgba"], c["s"])
                elif op == E.BUTTON:
                    if ui.button(c["s"]):
                        s["events"][k] = (s["events"].get(k, (0, 0))[0] + 1, s["events"].get(k, (0, 0))[1])
                elif op in (E.CHECKBOX, E.SLIDER_INT, E.SLIDER_FLOAT, E.INPUT_TEXT, E.COMBO, E.COLOR_EDIT):
                    v = ovr.get(k, c.get("v", c.get("rgba")))
                    if op == E.CHECKBOX:
                        changed, nv = ui.checkbox(c["s"], bool(v))
                        nv = 1 if nv else 0
                    elif op == E.SLIDER_INT:
                        changed, nv = ui.slider_int(c["s"], v, c["lo"], c["hi"])
                    elif op == E.SLIDER_FLOAT:
                        changed, nv = ui.slider_float(c["s"], v, c["lo"], c["hi"], c["fmt"])
                    elif op == E.INPUT_TEXT:
                        changed, nv = ui.input_text(c["s"], v, c["cap"] - 1)
                    elif op == E.COMBO:
                        changed, nv = ui.combo(c["s"], v, c["items"])
                    else:
                        changed, nv = ui.color_edit(c["s"], tuple(v), bool(c["flags"] & E.COLOR_NO_ALPHA))
                    if changed:
                        s["values"][k] = (nv, False)
                elif op == E.SEPARATOR:
                    ui.separator()
                elif op == E.SAME_LINE:
                    ui.same_line()
                elif op == E.PLOT_LINES:
                    ui.plot_lines(c["s"], c["values"], c["overlay"], *c["scale"])
                i += 1

        run(0, len(cmds))
        s["frames"] += 1
        s["frame"] = frame


# ------------------------------------------------------------------ harness
def embedded():
    """A mock runtime whose gta6mod.ui is EmbeddedUi over FakeRetained; returns (rt, sched, target MockUi, eui)."""
    rt = runtime.use_mock(quiet=True)
    sched = script.reset()
    target = rt.backend.ui
    eui = E.EmbeddedUi(FakeRetained(target), log=rt.backend.log)
    rt.backend.ui = eui
    return rt, sched, target, eui


def drive(rt, sched, target, ms, events, ui_log=None):
    """scenario.drive with the UI events going to the drawing side (the mock 'render thread')."""
    evs = [scenario.parse(k, v) for k, v in events]
    t = 0
    while t < ms:
        for ev in evs:
            if ev.at != t:
                continue
            if ev.kind == "ui_click":
                target.click(ev.value)
            elif ev.kind == "ui_set":
                target.set(*ev.value)
            elif ev.kind == "ui_menu":
                target.set_menu_open(ev.value == "1")
            else:
                scenario.apply(rt.backend, ev)
        sched.tick()
        rt.backend.step(16)
        ran = target.frame()
        if ui_log is not None:
            if ran:
                ui_log.add(t, target.transcript)
            else:
                ui_log.gap()
        t += 16


def immediate(mod_path, ms, events):
    """The same run with the Python mock UI in immediate mode (frames call the mod directly)."""
    rt = runtime.use_mock(quiet=True)
    sched = script.reset()
    runpy.run_path(mod_path, run_name="__gta6mod__")
    log = _ui_log.UiLog()
    drive(rt, sched, rt.backend.ui, ms, events, log)
    return rt, sched, log


class TestTranscriptParity(unittest.TestCase):
    def tearDown(self):
        ui._remove_all()

    def test_trainer_menu_immediate_equals_luau(self):
        rt, sched, log = immediate(os.path.join(EXAMPLES, "trainer_menu.py"), 3000, TRAINER_EVENTS)
        self.assertEqual(log.text(), _ui_log.read("trainer_menu.ui.txt"))

    def test_trainer_menu_retained_equals_luau(self):
        rt, sched, target, eui = embedded()
        runpy.run_path(os.path.join(EXAMPLES, "trainer_menu.py"), run_name="__gta6mod__")
        log = _ui_log.UiLog()
        drive(rt, sched, target, 3000, TRAINER_EVENTS, log)
        self.assertEqual(log.text(), _ui_log.read("trainer_menu.ui.txt"))
        msgs = [m for _, src, m in rt.backend.logs if src == "trainer_menu"]
        self.assertEqual(msgs, ["god mode ON", "never wanted ON (max wanted level was 5)", "cleared wanted level 3",
                                "healed"])
        self.assertTrue(rt.backend.player.invincible)
        self.assertEqual((target.errors, target.pending, eui.t.refused), (0, 0, 0))
        self.assertEqual(sched.stop(), [])
        self.assertEqual((target.registered, eui.registered), (0, 0))

    def test_widgets_immediate_equals_luau(self):
        rt, sched, log = immediate(os.path.join(HOST_MODS, "ui_widgets.py"), 1500, WIDGET_EVENTS)
        self.assertEqual(log.text(), _ui_log.read("ui_widgets.ui.txt"))
        self.assertIn("PASS ui_widgets", [m for _, src, m in rt.backend.logs if src == "ui_widgets"])

    def test_widgets_retained_differs_only_where_a_tree_opens(self):
        rt, sched, target, eui = embedded()
        runpy.run_path(os.path.join(HOST_MODS, "ui_widgets.py"), run_name="__gta6mod__")
        log = _ui_log.UiLog()
        drive(rt, sched, target, 1500, WIDGET_EVENTS, log)
        self.assertIn("PASS ui_widgets", [m for _, src, m in rt.backend.logs if src == "ui_widgets"],
                      [m for _, src, m in rt.backend.logs])
        got, want = _ui_log.frames(log.text()), _ui_log.frames(_ui_log.read("ui_widgets.ui.txt"))
        self.assertEqual(sorted(got), sorted(want))
        diff = [ms for ms in want if got[ms] != want[ms]]
        self.assertEqual(diff, [96])
        self.assertEqual(got[96], want[96].replace("      text inside\n", ""))
        self.assertEqual((target.errors, target.pending, eui.t.refused), (0, 0, 0))


class TestRecorder(unittest.TestCase):
    def setUp(self):
        self.rt, self.sched, self.target, self.eui = embedded()
        self.addCleanup(ui._remove_all)

    def tick(self, n=1):
        for _ in range(n):
            self.sched.tick()
            self.rt.backend.step(16)
            self.target.frame()

    def test_click_reaches_the_next_tick_once(self):
        seen = []

        @ui.on_frame("b")
        def draw(frame):
            with ui.window("W") as w:
                if w:
                    seen.append(ui.button("Go"))
        self.tick()
        self.target.click("Go")
        self.tick(3)
        self.assertEqual(seen, [False, False, True, False])  # drawn in frame 1, seen by the tick after it

    def test_user_value_is_kept_until_the_program_carries_it(self):
        values = []

        @ui.on_frame("s")
        def draw(frame):
            _, v = ui.slider_int("N", 1, 0, 10)   # the mod ignores the change: always passes 1
            values.append(v)
        self.tick()
        self.target.set("N", "7")
        self.tick()          # frame drawn with the user's 7
        self.assertIn("slider_int N = 7", self.target.transcript)
        self.tick()          # the tick saw 7 and recorded 7 (the returned value); afterwards the mod passes 1 again
        self.assertEqual(values[:3], [1, 1, 7])
        self.tick()
        self.assertIn("slider_int N = 1", self.target.transcript)

    def test_closable_window_reports_closed_once(self):
        opens = []

        @ui.on_frame("c")
        def draw(frame):
            with ui.window("Box", closable=True) as w:
                opens.append(w.open)
        self.tick()
        self.target.click("Box")
        self.tick(3)
        self.assertEqual(opens, [True, True, False, True])

    def test_unbalanced_calls_are_repaired_and_logged(self):
        @ui.on_frame("u")
        def draw(frame):
            ui.begin_window("Left open")
            ui.tree_pop()   # without a tree node
            ui.begin_tab_item("Orphan")
        self.tick(2)
        self.assertEqual((self.target.errors, self.eui.t.refused), (0, 0))  # the program reaching the overlay is balanced
        errs = [m for lvl, src, m in self.rt.backend.logs if lvl == "ERROR" and src == "u"]
        self.assertTrue(any("tree_pop() without its opener" in m for m in errs), errs)
        self.assertTrue(any("outside a tab bar" in m for m in errs), errs)
        self.assertTrue(any("left 1 window(s)" in m for m in errs), errs)

    def test_natives_are_refused_while_recording(self):
        from gta6mod.game import player
        caught = []

        @ui.on_frame("n")
        def draw(frame):
            try:
                player.ped()
            except NativeError as e:
                caught.append(e.status)
        self.tick()
        self.assertEqual(caught, [5])

    def test_no_drawn_frame_no_recording(self):
        runs = []
        ui.on_frame("h", lambda f: runs.append(f.frame))
        self.tick()
        self.target.set_menu_open(False)
        self.tick(5)
        self.assertEqual(len(runs), 2)   # the first program, then the one after the last drawn frame
        self.target.set_menu_open(True)
        self.tick(2)
        self.assertEqual(len(runs), 3)

    def test_non_finite_values_fail_the_frame_not_the_program(self):
        ui.on_frame("f", lambda f: ui.slider_float("X", float("nan"), 0, 1))
        self.tick(2)
        errs = [m for lvl, src, m in self.rt.backend.logs if lvl == "ERROR" and src == "f"]
        self.assertTrue(errs and "not a finite number" in errs[0], errs)
        self.assertEqual(self.eui.t.refused, 0)

    def test_stop_event_removes_only_that_schedulers_frames(self):
        other = script.Scheduler()
        ui.on_frame("mine", lambda f: None)
        with script.using(other):
            ui.on_frame("theirs", lambda f: None)
            ui.on_frame("mine", lambda f: None)   # same name, other mod: allowed
        self.assertEqual(self.eui.registered, 3)
        other.stop()
        self.assertEqual(self.eui.registered, 1)
        self.assertEqual(self.target.registered, 1)


class TestWire(unittest.TestCase):
    def test_results_round_trip(self):
        t = FakeRetained(runtime.use_mock(quiet=True).backend.ui)
        st, h = t.create("x", 0)
        slot = t.slots[h]
        slot["states"][(E.TREE_NODE, 0, "Tree")] = 1
        slot["values"][(E.INPUT_TEXT, 1, "Name")] = ("Jäson", False)
        slot["values"][(E.COLOR_EDIT, 0, "Tint")] = ((0.5, 0.25, 0.125, 1.0), False)
        slot["values"][(E.CHECKBOX, 0, "Old")] = (1, True)   # delivered before: not sent again
        slot["events"][(E.BUTTON, 0, "Go")] = (2, 0)
        st, data = t.poll(h)
        r = E.parse_results(data)
        self.assertEqual(r.states, {(E.TREE_NODE, 0, "Tree"): 1})
        self.assertEqual(r.values, {(E.INPUT_TEXT, 1, "Name"): "Jäson", (E.COLOR_EDIT, 0, "Tint"): (0.5, 0.25, 0.125, 1.0)})
        self.assertEqual(r.clicks, {(E.BUTTON, 0, "Go"): 2})
        self.assertFalse(r.have_frame)
        with self.assertRaises(ValueError):
            E.parse_results(data + b"\0")

    def test_strings_stop_at_nul_like_c(self):
        rec = E.Recorder(E.Results())
        self.assertEqual(rec.key(E.BUTTON, "a\0b"), (E.BUTTON, 0, "a"))
        with self.assertRaises(ValueError):
            E._utf8("x" * (E.MAX_STRING + 1))


if __name__ == "__main__":
    unittest.main()
