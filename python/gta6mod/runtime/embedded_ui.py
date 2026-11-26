"""``EmbeddedUi``: ``gta6mod.ui`` inside the game (g6_python.dll), without Python on the render thread.

The overlay draws on the game's render thread; Python runs only on the script thread with the GIL, inside the
plugin's tick (sdk/docs/python_host.md). So in game a frame function does not run when the overlay draws.
Instead, once per script tick (``gta6mod.ui`` calls ``run_frame`` after the mod's scripts):

1. ``poll``: fetch what the user did since the last tick (clicks, edited values, which trees are open, which
   tab is selected, closed windows) and the latest frame info;
2. run the frame function; every widget call is *recorded* into a program (the command and the value the mod
   passed) and returns what the user produced: a click as ``True``, an edited value as ``(True, value)``, a
   tree as open when it was open in the last drawn frame;
3. ``submit`` the program. The runtime's C++ (``sdk/runtime/src/ui_retained.cpp``) replays the latest program
   on the render thread through the g6_ui_api, every frame, and stores the results for the next poll.

The render thread never enters Python and never waits for the GIL; the script thread never waits for a frame
(the runtime's per-frame lock is held only to swap a program or merge results).

Consequences for mods (the same frame function works with ``MockUi`` and ``CAbiUi``):

* An interaction reaches the frame function on the next script tick, not in the frame where it happened; the
  value the function then returns is drawn from the next program on, and until then the runtime keeps drawing
  the user's value (a slider being dragged does not jump back).
* A tree node or tab that the user opens shows its contents one frame later (the program recorded no contents
  for it); one that closes skips them at once.
* While no frame is drawn (menu hidden, overlay off, guard closed) the frame function does not run; the first
  frame after the menu opens shows the last program.

Exports used (``backend.ui_proc``; only ``g6_ui_*`` names resolve): ``g6_ui_retained_version/create/destroy/
submit/poll``. Wire format: ``sdk/docs/python_host.md``, "Python UI".
"""
from __future__ import annotations

import ctypes
import math
import struct
from dataclasses import dataclass, field
from typing import Callable, Sequence

from .backend import NativeError
from .ui_backend import FRAME_ALWAYS, Color, Frame, FrameFn, UiBackend

VERSION = 1   # G6_UI_RETAINED_VERSION (sdk/runtime/src/ui_retained.h)
EXPORTS = ("g6_ui_retained_version", "g6_ui_retained_create", "g6_ui_retained_destroy", "g6_ui_retained_submit",
           "g6_ui_retained_poll")
PROGRAM_MAGIC = 0x50553647  # "G6UP"
RESULT_MAGIC = 0x52553647   # "G6UR"
NO_STRING = 0xFFFFFFFF
MAX_STRING = 65536

(BEGIN, END, NEXT_POS, NEXT_SIZE, TEXT, TEXT_COLORED, BUTTON, CHECKBOX, SLIDER_INT, SLIDER_FLOAT, INPUT_TEXT, COMBO,
 SEPARATOR, SAME_LINE, TREE_NODE, TREE_POP, TAB_BAR, END_TAB_BAR, TAB_ITEM, END_TAB_ITEM, COLOR_EDIT,
 PLOT_LINES) = range(1, 23)
CLOSERS = {"W": END, "T": TREE_POP, "B": END_TAB_BAR, "I": END_TAB_ITEM}

G6_OK, G6_E_INVALID_ARG, G6_E_NOT_IMPLEMENTED = 0, 1, 8
COND_ALWAYS, COND_FIRST_USE = 0, 1
COLOR_NO_ALPHA = 0x1

Key = tuple  # (op, occurrence, label)


def _f32(v: float) -> float:
    """Round through a C float (what the runtime stores), refusing NaN/inf (ImGui cannot draw them)."""
    v = float(v)
    if not math.isfinite(v):
        raise ValueError(f"ui: {v} is not a finite number")
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _utf8(text: str) -> bytes:
    """UTF-8 up to the first NUL (a C string ends there, as with CAbiUi)."""
    b = str(text).encode("utf-8")
    nul = b.find(b"\0")
    if nul >= 0:
        b = b[:nul]
    if len(b) > MAX_STRING:
        raise ValueError(f"ui: a string of {len(b)} bytes (at most {MAX_STRING})")
    return b


# ------------------------------------------------------------------ poll results
@dataclass
class Results:
    frames: int = 0            # frames drawn since the previous poll
    have_frame: bool = False   # some frame has been drawn (the frame info below is real)
    menu_open: bool = False
    width: float = 0.0
    height: float = 0.0
    dt: float = 0.0
    frame_no: int = 0
    states: dict = field(default_factory=dict)   # Key -> 0/1: drawn / open / visible / selected
    values: dict = field(default_factory=dict)   # Key -> the user's value (not seen by Python before)
    clicks: dict = field(default_factory=dict)   # Key -> clicks
    closed: set = field(default_factory=set)     # Keys of closable windows the user closed


class _Cursor:
    def __init__(self, data: bytes):
        self.d = data
        self.at = 0

    def take(self, fmt: str):
        v = struct.unpack_from("<" + fmt, self.d, self.at)
        self.at += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def str(self) -> str:
        n = self.take("I")
        s = self.d[self.at:self.at + n]
        if len(s) != n:
            raise ValueError("truncated string")
        self.at += n
        return s.decode("utf-8", "replace")

    def key(self) -> Key:
        op, occ = self.take("BI")
        return (op, occ, self.str())


def parse_results(data: bytes) -> Results:
    c = _Cursor(data)
    magic, version, _ = c.take("IHH")
    if magic != RESULT_MAGIC or version != VERSION:
        raise ValueError(f"ui_retained results: magic 0x{magic:08x} version {version}")
    r = Results()
    r.frames, have, menu_open = c.take("III")
    r.width, r.height, r.dt = c.take("fff")
    r.frame_no = c.take("Q")
    r.have_frame, r.menu_open = bool(have), bool(menu_open)
    for _ in range(c.take("I")):
        k = c.key()
        r.states[k] = c.take("B")
    for _ in range(c.take("I")):
        k = c.key()
        op = k[0]
        if op == SLIDER_FLOAT:
            r.values[k] = c.take("f")
        elif op == INPUT_TEXT:
            r.values[k] = c.str()
        elif op == COLOR_EDIT:
            r.values[k] = tuple(c.take("ffff"))
        else:
            r.values[k] = c.take("i")
    for _ in range(c.take("I")):
        k = c.key()
        clicks, closed = c.take("IB")
        if clicks:
            r.clicks[k] = clicks
        if closed:
            r.closed.add(k)
    if c.at != len(data):
        raise ValueError(f"ui_retained results: {len(data) - c.at} trailing byte(s)")
    return r


# ------------------------------------------------------------------ recording
class Recorder:
    """One frame function run: the program being built and the results its widgets answer from."""

    def __init__(self, results: Results):
        self.res = results
        self.buf = bytearray()
        self.count = 0
        self.stack: list[list] = []          # [kind, tab items seen] per open scope
        self.seen: dict[tuple, int] = {}
        self.errors: list[str] = []

    # encoding
    def _op(self, op: int) -> None:
        self.buf.append(op)
        self.count += 1

    def _u8(self, v) -> None:
        self.buf += struct.pack("<B", 1 if v else 0)

    def _u32(self, v: int) -> None:
        self.buf += struct.pack("<I", int(v))

    def _i32(self, v: int) -> None:
        self.buf += struct.pack("<i", int(v))

    def _f(self, *vals: float) -> None:
        self.buf += struct.pack(f"<{len(vals)}f", *vals)

    def _s(self, text) -> None:
        b = _utf8(text)
        self.buf += struct.pack("<I", len(b)) + b

    def _opt(self, text) -> None:
        if text is None:
            self._u32(NO_STRING)
        else:
            self._s(text)

    def key(self, op: int, label: str) -> Key:
        label = _utf8(label).decode("utf-8", "replace")
        n = self.seen.get((op, label), 0)
        self.seen[(op, label)] = n + 1
        return (op, n, label)

    def push(self, kind: str) -> None:
        if len(self.stack) >= 64:
            raise RuntimeError("ui: more than 64 nested windows / trees / tab bars")
        self.stack.append([kind, 0])

    def pop(self, kind: str, what: str) -> bool:
        if self.stack and self.stack[-1][0] == kind:
            self.stack.pop()
            self._op(CLOSERS[kind])
            return True
        self.errors.append(f"{what} without its opener (ignored)")
        return False

    def finish(self) -> bytes:
        """Close what the frame function left open (as ImGui's error recovery does) and return the program."""
        if self.stack:
            n = {k: sum(1 for s in self.stack if s[0] == k) for k in "WTBI"}
            self.errors.append(f"left {n['W']} window(s), {n['T']} tree node(s), {n['B']} tab bar(s), "
                               f"{n['I']} tab item(s) open; closed them")
            while self.stack:
                self._op(CLOSERS[self.stack.pop()[0]])
        return struct.pack("<IHHI", PROGRAM_MAGIC, VERSION, 0, self.count) + bytes(self.buf)


# ------------------------------------------------------------------ transport (ctypes over the runtime's exports)
class CTransport:
    """The g6_ui_retained_* exports, bound by address (``backend.ui_proc``)."""

    def __init__(self, proc: Callable[[str], int]):
        addr = {n: int(proc(n) or 0) for n in EXPORTS}
        missing = [n for n, a in addr.items() if not a]
        if missing:
            raise LookupError(", ".join(missing))
        u32, i32, sz = ctypes.c_uint32, ctypes.c_int32, ctypes.c_size_t
        self._version = ctypes.CFUNCTYPE(u32)(addr["g6_ui_retained_version"])
        self._create = ctypes.CFUNCTYPE(i32, ctypes.c_char_p, u32, ctypes.POINTER(u32))(addr["g6_ui_retained_create"])
        self._destroy = ctypes.CFUNCTYPE(i32, u32)(addr["g6_ui_retained_destroy"])
        self._submit = ctypes.CFUNCTYPE(i32, u32, ctypes.c_char_p, sz)(addr["g6_ui_retained_submit"])
        self._poll = ctypes.CFUNCTYPE(i32, u32, ctypes.c_void_p, sz, ctypes.POINTER(sz))(addr["g6_ui_retained_poll"])
        self._buf = ctypes.create_string_buffer(1 << 16)

    def version(self) -> int:
        return int(self._version())

    def create(self, name: str, flags: int) -> tuple[int, int]:
        h = ctypes.c_uint32(0)
        st = self._create(_utf8(name), flags, ctypes.byref(h))
        return int(st), int(h.value)

    def destroy(self, handle: int) -> int:
        return int(self._destroy(handle))

    def submit(self, handle: int, program: bytes) -> int:
        return int(self._submit(handle, program, len(program)))

    def poll(self, handle: int) -> tuple[int, bytes]:
        size = ctypes.c_size_t(0)
        for _ in range(2):
            st = self._poll(handle, ctypes.cast(self._buf, ctypes.c_void_p), len(self._buf), ctypes.byref(size))
            if st == G6_E_INVALID_ARG and size.value > len(self._buf):
                self._buf = ctypes.create_string_buffer(size.value * 2)
                continue
            break
        return int(st), (self._buf.raw[:size.value] if st == G6_OK else b"")


# ------------------------------------------------------------------ the UiBackend
@dataclass
class _Frame:
    name: str
    fn: FrameFn
    flags: int
    submitted: bool = False


class EmbeddedUi(UiBackend):
    """UiBackend that records frame functions on the script thread into programs the runtime replays."""

    retained = True   # gta6mod.ui runs the frame functions itself, once per tick (run_frame)

    def __init__(self, transport, log: Callable[[str, str, str], None] | None = None):
        self.t = transport
        self._log = log or (lambda level, source, message: print(f"[{level}] {source}: {message}"))
        self._frames: dict[int, _Frame] = {}
        self._rec: Recorder | None = None

    @classmethod
    def bind(cls, proc: Callable[[str], int], log=None) -> "EmbeddedUi | None":
        """The UI over the runtime's g6_ui_retained_* exports, or None when the runtime has none or speaks
        another version."""
        try:
            t = CTransport(proc)
        except LookupError:
            return None
        if t.version() != VERSION:
            if log:
                log("WARN", "ui", f"runtime UI format {t.version()}, gta6mod speaks {VERSION}: no in-game UI")
            return None
        return cls(t, log)

    @property
    def registered(self) -> int:
        return len(self._frames)

    @property
    def in_frame(self) -> bool:
        """A frame function is being recorded. EmbeddedBackend refuses natives then: the frame function runs on
        the script thread here, but the rule is the same for every host (in C and Luau it runs on the render
        thread, where natives fail), so a mod behaves the same everywhere. Use ui.defer."""
        return self._rec is not None

    # -- registration -------------------------------------------------------------------------------------
    def frame_register(self, name: str, fn: FrameFn, flags: int) -> int | None:
        st, handle = self.t.create(name, flags & FRAME_ALWAYS)
        if st == G6_E_NOT_IMPLEMENTED:
            return None
        if st != G6_OK:
            raise NativeError(st, "ui.frame_register", "g6_ui_retained_create failed")
        self._frames[handle] = _Frame(name, fn, flags)
        return handle

    def frame_unregister(self, frame_id: int) -> bool:
        self._frames.pop(frame_id, None)
        return self.t.destroy(frame_id) == G6_OK   # waits for a frame in progress

    # -- one script tick ----------------------------------------------------------------------------------
    def run_frame(self, frame_id: int) -> bool:
        """Poll, run the frame function (recording) and submit. False when skipped: the last program has not
        been drawn yet (menu hidden, overlay off, guard closed), so there is nothing new to answer."""
        fr = self._frames.get(frame_id)
        if fr is None:
            return False
        st, data = self.t.poll(frame_id)
        if st != G6_OK:
            raise NativeError(st, "ui.poll", "g6_ui_retained_poll failed")
        res = parse_results(data)
        if fr.submitted and res.frames == 0:
            return False
        menu_open = res.menu_open if res.have_frame else not (fr.flags & FRAME_ALWAYS)
        frame = Frame(menu_open, res.width, res.height, res.dt, res.frame_no)
        rec = self._rec = Recorder(res)
        try:
            fr.fn(frame)
        finally:
            self._rec = None
            program = rec.finish()
            for e in rec.errors:
                self._log("ERROR", fr.name, f"ui frame: {e}")
            st = self.t.submit(frame_id, program)
            if st == G6_OK:
                fr.submitted = True
        if st != G6_OK:
            raise NativeError(st, "ui.submit", "g6_ui_retained_submit refused the program")
        return True

    def _r(self) -> Recorder:
        if self._rec is None:
            raise RuntimeError("ui widgets work only inside a ui.on_frame function")
        return self._rec

    # -- widgets: record, answer from the last results ----------------------------------------------------------
    def begin(self, title: str, closable: bool, flags: int) -> tuple[bool, bool]:
        r = self._r()
        k = r.key(BEGIN, title)
        r._op(BEGIN)
        r._s(title)
        r._u8(closable)
        r._u32(flags)
        r.push("W")
        drawn = r.res.states.get(k, 1) != 0
        return drawn, not (closable and k in r.res.closed)

    def end(self) -> None:
        self._r().pop("W", "end()")

    def set_next_window_pos(self, x: float, y: float, always: bool) -> None:
        r = self._r()
        r._op(NEXT_POS)
        r._f(_f32(x), _f32(y))
        r._u32(COND_ALWAYS if always else COND_FIRST_USE)

    def set_next_window_size(self, w: float, h: float, always: bool) -> None:
        r = self._r()
        r._op(NEXT_SIZE)
        r._f(_f32(w), _f32(h))
        r._u32(COND_ALWAYS if always else COND_FIRST_USE)

    def text(self, text: str) -> None:
        r = self._r()
        r._op(TEXT)
        r._s(text)

    def text_colored(self, rgba: Color, text: str) -> None:
        r = self._r()
        r._op(TEXT_COLORED)
        r._f(*(_f32(c) for c in rgba))
        r._s(text)

    def button(self, label: str) -> bool:
        r = self._r()
        k = r.key(BUTTON, label)
        r._op(BUTTON)
        r._s(label)
        return r.res.clicks.pop(k, 0) > 0

    def checkbox(self, label: str, value: bool) -> tuple[bool, bool]:
        r = self._r()
        k = r.key(CHECKBOX, label)
        v = bool(r.res.values.pop(k)) if k in r.res.values else bool(value)
        r._op(CHECKBOX)
        r._s(label)
        r._i32(1 if v else 0)
        return v != bool(value), v

    def slider_int(self, label: str, value: int, lo: int, hi: int) -> tuple[bool, int]:
        r = self._r()
        k = r.key(SLIDER_INT, label)
        v = int(r.res.values.pop(k)) if k in r.res.values else int(value)
        r._op(SLIDER_INT)
        r._s(label)
        r._i32(v)
        r._i32(lo)
        r._i32(hi)
        return v != int(value), v

    def slider_float(self, label: str, value: float, lo: float, hi: float, fmt: str | None) -> tuple[bool, float]:
        r = self._r()
        k = r.key(SLIDER_FLOAT, label)
        given = _f32(value)
        v = _f32(r.res.values.pop(k)) if k in r.res.values else given
        r._op(SLIDER_FLOAT)
        r._s(label)
        r._f(v, _f32(lo), _f32(hi))
        r._opt(fmt)
        return v != given, v

    def input_text(self, label: str, text: str, max_bytes: int) -> tuple[bool, str]:
        r = self._r()
        k = r.key(INPUT_TEXT, label)
        given = _utf8(text)[:max_bytes].decode("utf-8", "replace")
        v = str(r.res.values.pop(k)) if k in r.res.values else given
        r._op(INPUT_TEXT)
        r._s(label)
        r._s(v)
        r._u32(max_bytes + 1)
        return v != given, v

    def combo(self, label: str, index: int, items: Sequence[str]) -> tuple[bool, int]:
        r = self._r()
        k = r.key(COMBO, label)
        v = int(r.res.values.pop(k)) if k in r.res.values else int(index)
        r._op(COMBO)
        r._s(label)
        r._i32(v)
        r._u32(len(items))
        for it in items:
            r._s(it)
        return v != int(index), v

    def separator(self) -> None:
        self._r()._op(SEPARATOR)

    def same_line(self) -> None:
        self._r()._op(SAME_LINE)

    def tree_node(self, label: str) -> bool:
        r = self._r()
        k = r.key(TREE_NODE, label)
        open_ = r.res.states.get(k, 0) != 0
        r._op(TREE_NODE)
        r._s(label)
        r._u8(open_)
        if open_:
            r.push("T")
        return open_

    def tree_pop(self) -> None:
        self._r().pop("T", "tree_pop()")

    def begin_tab_bar(self, tab_bar_id: str) -> bool:
        r = self._r()
        k = r.key(TAB_BAR, tab_bar_id)
        visible = r.res.states.get(k, 1) != 0
        r._op(TAB_BAR)
        r._s(tab_bar_id)
        r._u8(visible)
        if visible:
            r.push("B")
        return visible

    def end_tab_bar(self) -> None:
        self._r().pop("B", "end_tab_bar()")

    def begin_tab_item(self, label: str) -> bool:
        r = self._r()
        if not r.stack or r.stack[-1][0] != "B":
            r.errors.append(f"tab item {label!r} outside a tab bar (ignored)")
            return False
        nth = r.stack[-1][1]
        r.stack[-1][1] += 1
        k = r.key(TAB_ITEM, label)
        selected = (r.res.states[k] != 0) if k in r.res.states else nth == 0   # ImGui selects the first tab
        r._op(TAB_ITEM)
        r._s(label)
        r._u8(selected)
        if selected:
            r.push("I")
        return selected

    def end_tab_item(self) -> None:
        self._r().pop("I", "end_tab_item()")

    def color_edit(self, label: str, rgba: Color, no_alpha: bool) -> tuple[bool, Color]:
        r = self._r()
        k = r.key(COLOR_EDIT, label)
        given = tuple(_f32(c) for c in rgba)
        v = tuple(_f32(c) for c in r.res.values.pop(k)) if k in r.res.values else given
        r._op(COLOR_EDIT)
        r._s(label)
        r._f(*v)
        r._u32(COLOR_NO_ALPHA if no_alpha else 0)
        return v != given, v  # type: ignore[return-value]

    def plot_lines(self, label: str, values: Sequence[float], overlay: str | None, scale_min: float, scale_max: float,
                   width: float, height: float) -> None:
        r = self._r()
        vals = [_f32(v) for v in values]
        r._op(PLOT_LINES)
        r._s(label)
        r._u32(len(vals))
        if vals:
            r._f(*vals)
        r._opt(overlay)
        r._f(_f32(scale_min), _f32(scale_max), _f32(width), _f32(height))
