"""gta6mod.ui: in-game menus over the g6_ui_api C ABI (Dear ImGui drawn by the runtime's overlay).

    from gta6mod import ui

    state = {"god": False}

    @ui.on_frame("trainer")
    def menu(frame):
        with ui.window("Trainer", x=40, y=120, auto_resize=True) as w:
            if w:
                changed, state["god"] = ui.checkbox("God mode", state["god"])
                if changed:
                    ui.defer(lambda: apply_god(state["god"]))   # natives run on the next script tick
                if ui.button("Heal"):
                    ui.defer(heal)
                with ui.tree("Advanced") as open_:
                    if open_:
                        ui.text("details")

Rules (the same in every language, sdk/include/gta6mod/gta6mod.h "UI layer"):

* A frame function runs once per drawn frame while the menu is open (Insert in-game), or always with
  ``always=True`` (a HUD; it gets no input). Widget functions work only inside one (``RuntimeError`` elsewhere).
* Natives do not belong in a frame function: with the C ABI it runs on the render thread, where they fail.
  Keep state in variables your scripts read, and use ``ui.defer(fn)`` to run ``fn`` on a script tick (it may
  be a generator that waits). Offline, the mock backend refuses natives inside a frame too, so tests catch it.
* In game (``g6_python.dll``) Python never runs on the render thread: the frame function runs once per script
  tick and its widget calls are recorded into a program the runtime replays every frame (``EmbeddedUi``,
  runtime/embedded_ui.py; sdk/docs/python_host.md, "Python UI"). Interactions reach it on the next tick;
  ``ui.defer`` work it queues runs in that same tick, after it.
* Results follow pyimgui: ``checkbox``/``slider_*``/``input_text``/``combo``/``color_edit`` return
  ``(changed, value)``; ``button`` returns True on the click frame. Indices are 0-based.
* A frame function that raises is logged; after 10 failures in a row it is removed. The stop event (host
  unload, ``script.shutdown()``) removes every frame.

Backends: this module only calls ``runtime.current().backend.ui`` (``runtime/ui_backend.py``): ``MockUi``
offline (records calls; ``scenario.drive`` draws a frame after every tick), ``CAbiUi`` over a runtime DLL,
``EmbeddedUi`` in game. Frames and deferred work belong to the scheduler that was current when the frame was
registered (in game: the mod's own), so a mod's stop event removes only its frames.
"""
from __future__ import annotations

import inspect
import itertools
from collections import deque
from typing import Callable, Iterable, Sequence

from . import runtime as _rt
from . import script as _script
from .runtime.ui_backend import (FRAME_ALWAYS, WINDOW_AUTO_RESIZE, WINDOW_NO_BACKGROUND, WINDOW_NO_COLLAPSE,
                                 WINDOW_NO_INPUTS, WINDOW_NO_MOVE, WINDOW_NO_RESIZE, WINDOW_NO_TITLE_BAR, Frame)

__all__ = [
    "Frame", "on_frame", "remove", "available", "defer",
    "window", "begin_window", "end_window", "set_next_window_pos", "set_next_window_size",
    "text", "text_colored", "button", "checkbox", "slider_int", "slider_float", "input_text", "combo",
    "separator", "same_line", "tree", "tree_node", "tree_pop", "tab_bar", "tab", "begin_tab_bar", "end_tab_bar",
    "begin_tab_item", "end_tab_item", "color_edit", "plot_lines", "objective", "clear_objective",
]

MAX_ERRORS = 10  # a frame function that fails this often in a row is removed

_in_frame = False
_current: "_FrameEntry | None" = None   # the frame function running now (ui.defer inside it goes to its scheduler)
_frames: dict[tuple[int, str], "_FrameEntry"] = {}   # (id(scheduler), name) -> entry
_pending: dict[int, deque] = {}    # id(scheduler) -> ui.defer work, drained by that scheduler's tick
_defer_ids = itertools.count(1)
_DRAIN = "ui.defer"


class _FrameEntry:
    def __init__(self, name: str, fn: Callable[[Frame], None], backend, sched: _script.Scheduler):
        self.name = name
        self.fn = fn
        self.backend = backend
        self.sched = sched          # the scheduler of the mod that registered it (its ticks drain its ui.defer)
        self.id: int | None = None
        self.errors = 0

    def __call__(self, frame: Frame) -> None:
        global _in_frame, _current
        _in_frame = True
        _current = self
        try:
            self.fn(frame)
            self.errors = 0
        except Exception as e:  # noqa: BLE001 - a broken menu must not take the overlay (or the host) down
            self.errors += 1
            _log("ERROR", self.name, f"ui frame failed: {e!r}")
            if self.errors >= MAX_ERRORS:
                _log("ERROR", self.name, f"ui frame failed {MAX_ERRORS} times in a row; removed")
                _queue(self.sched).append(lambda: _remove_entry(self))
        finally:
            _in_frame = False
            _current = None


def _log(level: str, source: str, message: str) -> None:
    try:
        _rt.current().log(level, source, message)
    except RuntimeError:
        print(f"[{level}] {source}: {message}")


def _ui():
    backend = _rt.current().backend
    if backend.ui is None:
        raise RuntimeError(f"backend {type(backend).__name__} has no UI layer")
    return backend.ui


def _live(fn: str):
    if not _in_frame:
        raise RuntimeError(f"ui.{fn}: only inside a ui.on_frame function")
    return _current.backend.ui if _current is not None else _ui()


def _queue(sched: _script.Scheduler | None = None) -> deque:
    if sched is None:
        sched = _current.sched if _current is not None else _script.scheduler()
    return _pending.setdefault(id(sched), deque())


# -- registration ----------------------------------------------------------------------------------------
def available() -> bool:
    """True when the current backend has a UI layer (it may still be switched off: see ``on_frame``)."""
    try:
        return _rt.current().backend.ui is not None
    except RuntimeError:
        return False


def on_frame(name: str, fn: Callable[[Frame], None] | None = None, *, always: bool = False):
    """Register ``fn(frame)`` for every drawn frame. Use as ``@ui.on_frame("name")`` or ``ui.on_frame("name", fn)``.

    Returns ``fn`` (decorator) / ``True`` (direct call) when registered; ``False`` from a direct call when the
    host has no in-game UI (overlay off). Raises ``ValueError`` for a name this mod (scheduler) already registered."""
    def register(f: Callable[[Frame], None]) -> bool:
        backend = _rt.current().backend
        _forget_other_backends(backend)
        sched = _script.scheduler()
        if (id(sched), name) in _frames:
            raise ValueError(f"ui frame {name!r} already registered")
        if backend.ui is None:
            _log("INFO", name, "no in-game UI on this backend")
            return False
        entry = _FrameEntry(name, f, backend, sched)
        entry.id = backend.ui.frame_register(name, entry, FRAME_ALWAYS if always else 0)
        if entry.id is None:
            _log("INFO", name, "no in-game UI (overlay off: [ui] overlay=0)")
            return False
        _frames[(id(sched), name)] = entry
        _ensure_scripts(sched, backend)
        return True

    if fn is not None:
        return register(fn)

    def deco(f: Callable[[Frame], None]):
        register(f)
        return f
    return deco


def _forget_other_backends(backend) -> None:
    """A new runtime (``runtime.use_mock()`` in tests, a reload) starts with no frames and no deferred work."""
    stale = [k for k, e in _frames.items() if e.backend is not backend]
    for k in stale:
        del _frames[k]
    if stale:
        _pending.clear()


def _remove_entry(entry: "_FrameEntry") -> bool:
    if _frames.get((id(entry.sched), entry.name)) is not entry:
        return False
    del _frames[(id(entry.sched), entry.name)]
    if entry.id is not None:
        entry.backend.ui.frame_unregister(entry.id)
    return True


def remove(name: str) -> bool:
    """Unregister the frame ``name`` (of the calling mod). True if it was registered."""
    entry = _frames.get((id(_script.scheduler()), name))
    if entry is None:  # offline code may call it after switching schedulers: a unique name still matches
        matches = [e for e in _frames.values() if e.name == name]
        entry = matches[0] if len(matches) == 1 else None
    return _remove_entry(entry) if entry is not None else False


def _remove_sched(sched: _script.Scheduler) -> None:
    """The stop event of one scheduler (mod): its frames go, its deferred work is dropped."""
    for entry in [e for e in _frames.values() if e.sched is sched]:
        _remove_entry(entry)
    _pending.pop(id(sched), None)


def _remove_all() -> None:
    for entry in list(_frames.values()):
        _remove_entry(entry)


def defer(fn: Callable[[], object]) -> None:
    """Run ``fn`` on the next script tick (natives work there). A generator function may ``yield wait(ms)``."""
    _queue().append(fn)


def _drain(sched: _script.Scheduler) -> None:
    q = _pending.get(id(sched))
    while q:
        fn = q.popleft()
        r = fn()
        if inspect.isgenerator(r):  # a waiting function runs as its own script
            gen = r

            def run(gen=gen):
                yield from gen
            sched.add(f"ui.defer.{next(_defer_ids)}", run)


def _pump(sched: _script.Scheduler) -> None:
    """Retained backends (EmbeddedUi, in game): after the scheduler's scripts, record each of its frames once and
    hand the program to the runtime; then run what those frames deferred, so a click seen this tick takes effect
    in this tick (as with a frame drawn between ticks offline)."""
    for entry in [e for e in _frames.values() if e.sched is sched and e.id is not None]:
        ui = entry.backend.ui
        if ui is None or not getattr(ui, "retained", False):
            continue
        try:
            ui.run_frame(entry.id)
        except Exception as e:  # noqa: BLE001 - the runtime refused a program or a poll: log, keep the mod
            _log("ERROR", entry.name, f"ui frame not updated: {e!r}")
    _drain(sched)


def _ensure_scripts(sched: _script.Scheduler, backend) -> None:
    """The tick drainer for defer(), the stop-event handler and (retained UI) the per-tick pump, once per
    scheduler."""
    if any(s.name == _DRAIN for s in sched.scripts):
        return
    sched.add(_DRAIN, lambda: _drain(sched))
    sched.on_stop(lambda: _remove_sched(sched))
    if getattr(backend.ui, "retained", False):
        sched.after_tick(lambda: _pump(sched))


# -- windows -------------------------------------------------------------------------------------------------
def _window_flags(no_title_bar, no_resize, no_move, no_collapse, auto_resize, no_background, no_inputs) -> int:
    return ((WINDOW_NO_TITLE_BAR if no_title_bar else 0) | (WINDOW_NO_RESIZE if no_resize else 0) |
            (WINDOW_NO_MOVE if no_move else 0) | (WINDOW_NO_COLLAPSE if no_collapse else 0) |
            (WINDOW_AUTO_RESIZE if auto_resize else 0) | (WINDOW_NO_BACKGROUND if no_background else 0) |
            (WINDOW_NO_INPUTS if no_inputs else 0))


class _Scope:
    """``with`` result of window/tree/tab_bar/tab: true when the contents are visible."""

    def __init__(self, visible: bool, end: Callable[[], None] | None, open_: bool = True):
        self.visible = visible
        self.open = open_          # windows with closable=True: False once the close button was clicked
        self._end = end

    def __bool__(self) -> bool:
        return self.visible

    def __enter__(self) -> "_Scope":
        return self

    def __exit__(self, *exc) -> bool:
        if self._end is not None:
            self._end()
            self._end = None
        return False


def window(title: str, *, x: float | None = None, y: float | None = None, width: float | None = None,
           height: float | None = None, closable: bool = False, no_title_bar: bool = False, no_resize: bool = False,
           no_move: bool = False, no_collapse: bool = False, auto_resize: bool = False, no_background: bool = False,
           no_inputs: bool = False) -> _Scope:
    """``with ui.window("Title") as w: if w: ...`` (end is called on exit, as ImGui requires after every begin).
    x/y and width/height place the window on first use; ``w.open`` turns False when a closable window's close
    button is clicked."""
    ui = _live("window")
    if x is not None and y is not None:
        ui.set_next_window_pos(x, y, False)
    if width is not None and height is not None:
        ui.set_next_window_size(width, height, False)
    drawn, open_ = ui.begin(title, closable, _window_flags(no_title_bar, no_resize, no_move, no_collapse, auto_resize,
                                                            no_background, no_inputs))
    return _Scope(drawn, ui.end, open_)


def begin_window(title: str, *, closable: bool = False, **flags) -> tuple[bool, bool]:
    """Unscoped begin: returns (drawn, open); call ``end_window()`` whatever it returned."""
    return _live("begin_window").begin(title, closable, _window_flags(**{k: flags.get(k, False) for k in (
        "no_title_bar", "no_resize", "no_move", "no_collapse", "auto_resize", "no_background", "no_inputs")}))


def end_window() -> None:
    _live("end_window").end()


def set_next_window_pos(x: float, y: float, always: bool = False) -> None:
    _live("set_next_window_pos").set_next_window_pos(x, y, always)


def set_next_window_size(width: float, height: float, always: bool = False) -> None:
    _live("set_next_window_size").set_next_window_size(width, height, always)


# -- widgets ---------------------------------------------------------------------------------------------------
def text(*values) -> None:
    """The values through ``str``, separated by spaces."""
    _live("text").text(" ".join(str(v) for v in values))


def _color(c: Iterable[float]) -> tuple[float, float, float, float]:
    t = tuple(float(v) for v in c)
    if len(t) == 3:
        t = t + (1.0,)
    if len(t) != 4:
        raise ValueError("a color is (r, g, b) or (r, g, b, a), each 0..1")
    return t  # type: ignore[return-value]


def text_colored(color: Iterable[float], *values) -> None:
    _live("text_colored").text_colored(_color(color), " ".join(str(v) for v in values))


def button(label: str) -> bool:
    return _live("button").button(label)


def checkbox(label: str, value: bool) -> tuple[bool, bool]:
    """(changed, value)"""
    return _live("checkbox").checkbox(label, bool(value))


def _i32(v) -> int:
    v = int(v)
    if not -2**31 <= v < 2**31:
        raise ValueError(f"{v} is outside the 32-bit integer range")
    return v


def slider_int(label: str, value: int, min_value: int, max_value: int) -> tuple[bool, int]:
    """(changed, value)"""
    return _live("slider_int").slider_int(label, _i32(value), _i32(min_value), _i32(max_value))


def slider_float(label: str, value: float, min_value: float, max_value: float, fmt: str | None = None) -> tuple[bool, float]:
    """(changed, value); ``fmt`` is printf-style, default "%.3f"."""
    return _live("slider_float").slider_float(label, float(value), float(min_value), float(max_value), fmt)


def input_text(label: str, value: str, max_bytes: int = 256) -> tuple[bool, str]:
    """(changed, text); ``max_bytes`` (1..65536) bounds the UTF-8 text."""
    if not 1 <= max_bytes <= 65536:
        raise ValueError("max_bytes must be 1..65536")
    return _live("input_text").input_text(label, str(value), int(max_bytes))


def combo(label: str, index: int, items: Sequence[str]) -> tuple[bool, int]:
    """(changed, index); ``index`` is 0-based into ``items``."""
    items = [str(i) for i in items]
    if len(items) > 4096:
        raise ValueError("at most 4096 items")
    return _live("combo").combo(label, _i32(index), items)


def separator() -> None:
    _live("separator").separator()


def same_line() -> None:
    """The next widget goes on the same line as the previous one."""
    _live("same_line").same_line()


def tree(label: str) -> _Scope:
    """``with ui.tree("Label") as open_: if open_: ...`` (tree_pop on exit only when it was open)."""
    ui = _live("tree")
    open_ = ui.tree_node(label)
    return _Scope(open_, ui.tree_pop if open_ else None)


def tree_node(label: str) -> bool:
    """Unscoped: call ``tree_pop()`` only when this returned True."""
    return _live("tree_node").tree_node(label)


def tree_pop() -> None:
    _live("tree_pop").tree_pop()


def tab_bar(tab_bar_id: str) -> _Scope:
    """``with ui.tab_bar("id") as bar: if bar: with ui.tab("A") as a: ...``"""
    ui = _live("tab_bar")
    visible = ui.begin_tab_bar(tab_bar_id)
    return _Scope(visible, ui.end_tab_bar if visible else None)


def tab(label: str) -> _Scope:
    ui = _live("tab")
    selected = ui.begin_tab_item(label)
    return _Scope(selected, ui.end_tab_item if selected else None)


def begin_tab_bar(tab_bar_id: str) -> bool:
    return _live("begin_tab_bar").begin_tab_bar(tab_bar_id)


def end_tab_bar() -> None:
    _live("end_tab_bar").end_tab_bar()


def begin_tab_item(label: str) -> bool:
    return _live("begin_tab_item").begin_tab_item(label)


def end_tab_item() -> None:
    _live("end_tab_item").end_tab_item()


def color_edit(label: str, color: Iterable[float], no_alpha: bool = False) -> tuple[bool, tuple[float, float, float, float]]:
    """(changed, (r, g, b, a))"""
    return _live("color_edit").color_edit(label, _color(color), bool(no_alpha))


def plot_lines(label: str, values: Sequence[float], *, overlay: str | None = None, scale_min: float | None = None,
               scale_max: float | None = None, width: float = 0.0, height: float = 0.0) -> None:
    """Scale: automatic unless both ``scale_min`` < ``scale_max`` are given."""
    vals = [float(v) for v in values]
    if len(vals) > 65536:
        raise ValueError("at most 65536 values")
    lo = float(scale_min) if scale_min is not None else 0.0
    hi = float(scale_max) if scale_max is not None else 0.0
    _live("plot_lines").plot_lines(label, vals, overlay, lo, hi, float(width), float(height))


# -- game HUD text (natives, not the overlay) ---------------------------------------------------------------
_objective_seq = itertools.count(1)
_objective_last = [0]

OBJECTIVE_MS = 7500  # what 152 of the story scripts' _UILOG_PRINT_CACHED_OBJECTIVE calls pass


def objective(text: str, ms: int = OBJECTIVE_MS) -> None:
    """Show ``text`` as the game's mission objective line for ``ms`` game milliseconds.

    ``_UILOG_SET_CACHED_OBJECTIVE(text)`` then ``_UILOG_PRINT_CACHED_OBJECTIVE(ms)`` now (694 of the 800 story-script
    SET calls are followed by PRINT), and ``_UILOG_CLEAR_CACHED_OBJECTIVE`` after ``ms`` from a script of the
    current scheduler, unless a newer ``objective`` came first. ``ms < 0`` (scripts pass -1) shows it without a
    timeout and clears nothing. Call it from a script tick (or ``ui.defer``), not from a frame function: these are
    natives. The text reaches the game as an SDK-built script text object (``T``, include/gta6mod/script_objects.h);
    the game copies at most 511 bytes of it.

    UNVERIFIED LIVE (G-087, G-120): the native path is static evidence only (findings/phase5/35_ui_system.md §5,
    37_script_strings.md); whether and where the game draws the text, and the unit of ``ms``, are not confirmed in
    game. The natives are ``observed``: real backends need ``allow_unverified``.
    """
    from . import natives as N  # noqa: N812 - imported here: the bindings module is large
    if not isinstance(text, str):
        raise TypeError(f"objective text must be str, got {type(text).__name__}")
    ms = int(ms)
    N.UILOG._UILOG_SET_CACHED_OBJECTIVE(text)
    N.UILOG._UILOG_PRINT_CACHED_OBJECTIVE(ms & 0xFFFFFFFF)  # the handler reads a 32-bit int (-1: no timeout)
    seq = _objective_last[0] = next(_objective_seq)
    if ms < 0:
        return

    def clear_later():
        yield _script.wait(ms)
        if _objective_last[0] == seq:  # a newer objective owns the line (and its own clear)
            N.UILOG._UILOG_CLEAR_CACHED_OBJECTIVE()
    _script.scheduler().add(f"ui.objective.{seq}", clear_later)


def clear_objective() -> None:
    """``_UILOG_CLEAR_CACHED_OBJECTIVE`` now; a pending clear of an earlier ``objective`` is dropped. Unverified
    live, like ``objective``."""
    from . import natives as N  # noqa: N812
    _objective_last[0] = next(_objective_seq)
    N.UILOG._UILOG_CLEAR_CACHED_OBJECTIVE()
