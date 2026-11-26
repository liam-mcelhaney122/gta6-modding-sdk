"""Offline UI backend: records draw calls and plays back scripted interactions.

The rules and the transcript lines are those of the C mock (``runtime/src/mock_backend.c``, "mock UI"; contract
in sdk/docs/script_layer.md, "Mock UI"), so a Python UI mod and its Luau twin can be checked the same way:

* ``frame()`` runs every registered frame callback once (only ``FRAME_ALWAYS`` ones while the menu is hidden)
  and returns how many ran; ``transcript`` is that frame's calls, one per line, indented by nesting
  (``begin Trainer`` / ``  checkbox God mode = 1`` / ``end``).
* ``click(label)`` / ``set(label, value)`` queue one interaction for the next widget with exactly that label:
  a click makes a button return True, flips a checkbox, opens/closes a tree node, selects a tab, closes a
  closable window; a value sets a checkbox ("1"/"0"), slider, input text, combo (index or item text) or color
  ("r,g,b,a").
* Tree nodes start closed; the first tab of a bar is selected until another is clicked; windows are open.
* Unbalanced begin/end, tree, tab bar or tab item calls are errors (``errors``, logged at ERROR).
* While a frame runs, the owning backend refuses natives (in-game frames run on the render thread).
"""
from __future__ import annotations

import struct
from typing import Sequence

from .ui_backend import FRAME_ALWAYS, Color, Frame, FrameFn, UiBackend


def _f32(v: float) -> float:
    """Round through a C float, as the C ABI stores slider and color values."""
    return struct.unpack("<f", struct.pack("<f", float(v)))[0]


def _strtod(s: str) -> float:
    try:
        return float(s)
    except ValueError:
        return 0.0


def _strtol(s: str) -> int:
    try:
        return int(s, 0)
    except ValueError:
        try:
            return int(float(s))
        except ValueError:
            return 0


class MockUi(UiBackend):
    def __init__(self, backend=None):
        self.backend = backend            # MockBackend: logs errors, refuses natives during a frame
        self.menu_open = True
        self.errors = 0
        self.frame_no = 0
        self.in_frame = False
        self.transcript = ""
        self.transcripts: list[str] = []  # every frame's transcript (tests search them)
        self._lines: list[str] = []
        self._frames: dict[int, tuple[str, FrameFn, int]] = {}
        self._next_id = 1
        self._queue: list[tuple[bool, str, str]] = []   # (is_set, label, value)
        self._open_trees: set[str] = set()
        self._selected: dict[str, str] = {}
        self._bar = ""
        self._depth = 0
        self._windows = self._trees = self._bars = self._tabs = 0

    # -- driver -----------------------------------------------------------------------------------------
    def click(self, label: str) -> None:
        self._queue.append((False, label, ""))

    def set(self, label: str, value) -> None:
        self._queue.append((True, label, str(value)))

    def set_menu_open(self, open_: bool) -> None:
        self.menu_open = bool(open_)

    @property
    def pending(self) -> int:
        return len(self._queue)

    @property
    def registered(self) -> int:
        return len(self._frames)

    def frame(self) -> int:
        """Run every registered frame callback once (one overlay frame). Returns how many ran."""
        if self.in_frame:
            raise RuntimeError("frame() called from inside a frame")
        self._lines = []
        fr = Frame(self.menu_open, 1920.0, 1080.0, 1.0 / 60.0, self.frame_no)
        self.frame_no += 1
        ran = 0
        self.in_frame = True
        try:
            for fid in list(self._frames):
                entry = self._frames.get(fid)
                if entry is None:
                    continue
                name, fn, flags = entry
                if not self.menu_open and not (flags & FRAME_ALWAYS):
                    continue
                self._depth = self._windows = self._trees = self._bars = self._tabs = 0
                self._line(f"frame {name}")
                self._depth = 1
                fn(fr)
                ran += 1
                if self._windows or self._trees or self._bars or self._tabs:
                    msg = (f"ui frame '{name}' left {self._windows} window(s), {self._trees} tree node(s), "
                           f"{self._bars} tab bar(s), {self._tabs} tab item(s) open")
                    self._depth = 1
                    self._line(f"error: {msg}")
                    self.errors += 1
                    if self.backend is not None:
                        self.backend.log("ERROR", "mock-ui", msg)
        finally:
            self._depth = 0
            self.in_frame = False
        self.transcript = "".join(line + "\n" for line in self._lines)
        if ran:
            self.transcripts.append(self.transcript)
        return ran

    # -- internals --------------------------------------------------------------------------------------
    def _line(self, text: str) -> None:
        self._lines.append("  " * self._depth + text)

    def _take(self, label: str):
        for i, item in enumerate(self._queue):
            if item[1] == label:
                del self._queue[i]
                return item
        return None

    def _error(self, text: str) -> None:
        self._line(f"error: {text}")
        self.errors += 1

    # -- UiBackend ----------------------------------------------------------------------------------------
    def frame_register(self, name: str, fn: FrameFn, flags: int) -> int | None:
        fid = self._next_id
        self._next_id += 1
        self._frames[fid] = (name or "ui", fn, flags)
        return fid

    def frame_unregister(self, frame_id: int) -> bool:
        return self._frames.pop(frame_id, None) is not None

    def begin(self, title: str, closable: bool, flags: int) -> tuple[bool, bool]:
        if not self.in_frame:
            return False, True
        open_ = True
        if closable:
            a = self._take(title)
            if a is not None and not a[0]:
                open_ = False
        self._line(f"begin {title}")
        self._windows += 1
        self._depth += 1
        return True, open_

    def end(self) -> None:
        if not self.in_frame:
            return
        if self._windows <= 0:
            self._error("end without begin")
            return
        self._windows -= 1
        self._depth = max(0, self._depth - 1)
        self._line("end")

    def set_next_window_pos(self, x: float, y: float, always: bool) -> None:
        if self.in_frame:
            self._line(f"next_window_pos {x:.0f} {y:.0f}{'' if always else ' (first use)'}")

    def set_next_window_size(self, w: float, h: float, always: bool) -> None:
        if self.in_frame:
            self._line(f"next_window_size {w:.0f} {h:.0f}{'' if always else ' (first use)'}")

    def text(self, text: str) -> None:
        if self.in_frame:
            self._line(f"text {text}")

    def text_colored(self, rgba: Color, text: str) -> None:
        if self.in_frame:
            self._line(f"text {text}")

    def button(self, label: str) -> bool:
        if not self.in_frame:
            return False
        a = self._take(label)
        clicked = a is not None and not a[0]
        self._line(f"button {label}{' (clicked)' if clicked else ''}")
        return clicked

    def checkbox(self, label: str, value: bool) -> tuple[bool, bool]:
        if not self.in_frame:
            return False, bool(value)
        v = bool(value)
        changed = False
        a = self._take(label)
        if a is not None:
            nv = (_strtol(a[2]) != 0) if a[0] else not v
            changed = nv != v
            v = nv
        self._line(f"checkbox {label} = {1 if v else 0}")
        return changed, v

    def slider_int(self, label: str, value: int, lo: int, hi: int) -> tuple[bool, int]:
        if not self.in_frame:
            return False, int(value)
        v = int(value)
        changed = False
        a = self._take(label)
        if a is not None and a[0]:
            nv = min(max(_strtol(a[2]), lo), hi)
            changed = nv != v
            v = nv
        self._line(f"slider_int {label} = {v}")
        return changed, v

    def slider_float(self, label: str, value: float, lo: float, hi: float, fmt: str | None) -> tuple[bool, float]:
        if not self.in_frame:
            return False, float(value)
        v = _f32(value)
        changed = False
        a = self._take(label)
        if a is not None and a[0]:
            nv = _f32(min(max(_strtod(a[2]), lo), hi))
            changed = nv != v
            v = nv
        self._line(f"slider_float {label} = {v:.3f}")
        return changed, v

    def input_text(self, label: str, text: str, max_bytes: int) -> tuple[bool, str]:
        if not self.in_frame:
            return False, text
        t = text
        changed = False
        a = self._take(label)
        if a is not None and a[0]:
            nt = a[2].encode("utf-8")[: max(0, max_bytes)].decode("utf-8", "ignore")
            changed = nt != t
            t = nt
        self._line(f"input_text {label} = {t}")
        return changed, t

    def combo(self, label: str, index: int, items: Sequence[str]) -> tuple[bool, int]:
        if not self.in_frame:
            return False, int(index)
        cur = int(index)
        changed = False
        a = self._take(label)
        if a is not None and a[0]:
            try:
                nv = int(a[2], 0)
            except ValueError:
                nv = list(items).index(a[2]) if a[2] in items else -1
            if 0 <= nv < len(items):
                changed = nv != cur
                cur = nv
        shown = items[cur] if 0 <= cur < len(items) else ""
        self._line(f"combo {label} = {cur} ({shown})")
        return changed, cur

    def separator(self) -> None:
        if self.in_frame:
            self._line("separator")

    def same_line(self) -> None:
        if self.in_frame:
            self._line("same_line")

    def tree_node(self, label: str) -> bool:
        if not self.in_frame:
            return False
        a = self._take(label)
        if a is not None:
            want = (_strtol(a[2]) != 0) if a[0] else label not in self._open_trees
            if want:
                self._open_trees.add(label)
            else:
                self._open_trees.discard(label)
        open_ = label in self._open_trees
        self._line(f"tree_node {label}{' (open)' if open_ else ''}")
        if open_:
            self._trees += 1
            self._depth += 1
        return open_

    def tree_pop(self) -> None:
        if not self.in_frame:
            return
        if self._trees <= 0:
            self._error("tree_pop without an open tree_node")
            return
        self._trees -= 1
        self._depth = max(0, self._depth - 1)
        self._line("tree_pop")

    def begin_tab_bar(self, tab_bar_id: str) -> bool:
        if not self.in_frame:
            return False
        self._line(f"tab_bar {tab_bar_id}")
        self._bar = tab_bar_id
        self._bars += 1
        self._depth += 1
        return True

    def end_tab_bar(self) -> None:
        if not self.in_frame:
            return
        if self._bars <= 0:
            self._error("end_tab_bar without begin_tab_bar")
            return
        self._bars -= 1
        self._bar = ""
        self._depth = max(0, self._depth - 1)
        self._line("end_tab_bar")

    def begin_tab_item(self, label: str) -> bool:
        if not self.in_frame:
            return False
        if self._bars <= 0:
            self._error("tab_item outside a tab bar")
            return False
        a = self._take(label)
        if a is not None and not a[0]:
            self._selected[self._bar] = label
        self._selected.setdefault(self._bar, label)  # the first tab of the bar
        selected = self._selected[self._bar] == label
        self._line(f"tab_item {label}{' (selected)' if selected else ''}")
        if selected:
            self._tabs += 1
            self._depth += 1
        return selected

    def end_tab_item(self) -> None:
        if not self.in_frame:
            return
        if self._tabs <= 0:
            self._error("end_tab_item without a selected tab_item")
            return
        self._tabs -= 1
        self._depth = max(0, self._depth - 1)
        self._line("end_tab_item")

    def color_edit(self, label: str, rgba: Color, no_alpha: bool) -> tuple[bool, Color]:
        c = tuple(_f32(x) for x in rgba)
        if not self.in_frame:
            return False, c  # type: ignore[return-value]
        changed = False
        a = self._take(label)
        if a is not None and a[0]:
            parts = [p for p in a[2].split(",")]
            nc = list(c)
            for i, p in enumerate(parts[:4]):
                if p.strip():
                    nc[i] = _f32(_strtod(p))
            if no_alpha:
                nc[3] = c[3]
            changed = tuple(nc) != c
            c = tuple(nc)
        self._line(f"color_edit {label} = {c[0]:.3f} {c[1]:.3f} {c[2]:.3f} {c[3]:.3f}")
        return changed, c  # type: ignore[return-value]

    def plot_lines(self, label: str, values: Sequence[float], overlay: str | None, scale_min: float, scale_max: float,
                   width: float, height: float) -> None:
        if self.in_frame:
            self._line(f"plot_lines {label} ({len(values)} values{', ' + overlay if overlay else ''})")
