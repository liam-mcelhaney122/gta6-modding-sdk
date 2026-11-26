"""Backend over the C ABI (include/gta6mod/gta6mod.h) via ctypes.

Works with any runtime DLL exporting ``g6_get_host_api``. Today that is the
offline ``gta6mod_mock.dll`` (sdk/build). Later it is the in-game runtime.
"""
from __future__ import annotations

import ctypes
import os

from .backend import Backend, BuildInfo, NativeError
from .ui_backend import UI_ABI_MAJOR, Color, Frame, FrameFn, UiBackend

G6_MAX_NATIVE_ARGS = 32
G6_MAX_NATIVE_RETS = 4


class G6NativeCall(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("argc", ctypes.c_uint32),
                ("args", ctypes.c_uint64 * G6_MAX_NATIVE_ARGS),
                ("ret", ctypes.c_uint64 * G6_MAX_NATIVE_RETS),
                ("string_slots", ctypes.c_uint32), ("text_slots", ctypes.c_uint32)]  # API 0.4


class G6BuildInfo(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("game_version", ctypes.c_char_p), ("platform", ctypes.c_char_p),
                ("exe_sha256", ctypes.c_uint8 * 32), ("natives_known", ctypes.c_uint32),
                ("natives_verified", ctypes.c_uint32)]


class G6ScriptDesc(ctypes.Structure):
    pass


SCRIPT_TICK = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
G6ScriptDesc._fields_ = [("size", ctypes.c_uint32), ("name", ctypes.c_char_p), ("tick", SCRIPT_TICK), ("user", ctypes.c_void_p)]

MOCK_NATIVE_FN = ctypes.CFUNCTYPE(None, ctypes.POINTER(G6NativeCall), ctypes.c_void_p)

# --- UI layer (gta6mod.h "UI layer", g6_ui_api) ---------------------------------------------------------
class G6UiFrame(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("menu_open", ctypes.c_uint32), ("display_w", ctypes.c_float),
                ("display_h", ctypes.c_float), ("delta_s", ctypes.c_float), ("_pad", ctypes.c_uint32),
                ("frame", ctypes.c_uint64)]


UI_FRAME_FN = ctypes.CFUNCTYPE(None, ctypes.POINTER(G6UiFrame), ctypes.c_void_p)


class G6UiFrameDesc(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("name", ctypes.c_char_p),
                ("draw", UI_FRAME_FN), ("user", ctypes.c_void_p)]


_i32, _u32, _f, _s = ctypes.c_int32, ctypes.c_uint32, ctypes.c_float, ctypes.c_char_p
_FN = ctypes.CFUNCTYPE
G6UiApi_fields = [
    ("size", ctypes.c_uint32), ("abi_major", ctypes.c_uint16), ("abi_minor", ctypes.c_uint16),
    ("frame_register", _FN(ctypes.c_int, ctypes.POINTER(G6UiFrameDesc), ctypes.POINTER(ctypes.c_uint32))),
    ("frame_unregister", _FN(ctypes.c_int, ctypes.c_uint32)),
    ("imgui_info", _FN(ctypes.c_int, ctypes.c_void_p)),
    ("begin", _FN(_i32, _s, ctypes.POINTER(_i32), _u32)),
    ("end", _FN(None)),
    ("set_next_window_pos", _FN(None, _f, _f, _u32)),
    ("set_next_window_size", _FN(None, _f, _f, _u32)),
    ("text", _FN(None, _s)),
    ("text_colored", _FN(None, _f, _f, _f, _f, _s)),
    ("button", _FN(_i32, _s)),
    ("checkbox", _FN(_i32, _s, ctypes.POINTER(_i32))),
    ("slider_int", _FN(_i32, _s, ctypes.POINTER(_i32), _i32, _i32)),
    ("slider_float", _FN(_i32, _s, ctypes.POINTER(_f), _f, _f, _s)),
    ("input_text", _FN(_i32, _s, ctypes.c_void_p, ctypes.c_size_t)),
    ("combo", _FN(_i32, _s, ctypes.POINTER(_i32), ctypes.POINTER(_s), _i32)),
    ("separator", _FN(None)),
    ("same_line", _FN(None)),
    ("tree_node", _FN(_i32, _s)),
    ("tree_pop", _FN(None)),
    ("begin_tab_bar", _FN(_i32, _s)),
    ("end_tab_bar", _FN(None)),
    ("begin_tab_item", _FN(_i32, _s)),
    ("end_tab_item", _FN(None)),
    ("color_edit", _FN(_i32, _s, ctypes.POINTER(_f), _u32)),
    ("plot_lines", _FN(None, _s, ctypes.POINTER(_f), _i32, _s, _f, _f, _f, _f)),
]


class G6UiApi(ctypes.Structure):
    _fields_ = G6UiApi_fields


G6_UI_COND_ALWAYS, G6_UI_COND_FIRST_USE = 0, 1
G6_UI_COLOR_NO_ALPHA = 0x1
G6_E_NOT_IMPLEMENTED = 8


def _b(text: str) -> bytes:
    return str(text).encode("utf-8")


class CAbiUi(UiBackend):
    """UiBackend over a runtime's g6_ui_api table (ctypes). With gta6mod_mock.dll the mock-only driver methods
    (``frame``, ``click``, ``set``, ``set_menu_open``, ``transcript``...) call its g6_mock_ui_* exports."""

    def __init__(self, backend: "CAbiBackend", api: "G6UiApi"):
        self.backend = backend
        self.api = api
        self._callbacks: dict[int, object] = {}  # frame id -> CFUNCTYPE object (must outlive the registration)
        dll = backend.dll
        self.is_mock = hasattr(dll, "g6_mock_ui_frame")
        if self.is_mock:
            dll.g6_mock_ui_frame.restype = ctypes.c_int32
            dll.g6_mock_ui_transcript.restype = ctypes.c_char_p
            dll.g6_mock_ui_click.argtypes = [ctypes.c_char_p]
            dll.g6_mock_ui_set.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            dll.g6_mock_ui_set_menu_open.argtypes = [ctypes.c_int32]
            for n in ("g6_mock_ui_pending", "g6_mock_ui_errors", "g6_mock_ui_registered"):
                getattr(dll, n).restype = ctypes.c_int32

    # -- mock driver --------------------------------------------------------------------------------------
    def _mock(self):
        if not self.is_mock:
            raise RuntimeError("only the mock runtime (gta6mod_mock.dll) draws UI frames on request")
        return self.backend.dll

    def frame(self) -> int:
        return int(self._mock().g6_mock_ui_frame())

    def click(self, label: str) -> None:
        self._mock().g6_mock_ui_click(_b(label))

    def set(self, label: str, value) -> None:
        self._mock().g6_mock_ui_set(_b(label), _b(value))

    def set_menu_open(self, open_: bool) -> None:
        self._mock().g6_mock_ui_set_menu_open(1 if open_ else 0)

    @property
    def transcript(self) -> str:
        return (self._mock().g6_mock_ui_transcript() or b"").decode("utf-8", "replace")

    @property
    def pending(self) -> int:
        return int(self._mock().g6_mock_ui_pending())

    @property
    def errors(self) -> int:
        return int(self._mock().g6_mock_ui_errors())

    @property
    def registered(self) -> int:
        return int(self._mock().g6_mock_ui_registered())

    # -- UiBackend ----------------------------------------------------------------------------------------
    def frame_register(self, name: str, fn: FrameFn, flags: int) -> int | None:
        def thunk(pframe, _user):
            f = pframe.contents
            fn(Frame(bool(f.menu_open), float(f.display_w), float(f.display_h), float(f.delta_s), int(f.frame)))

        cb = UI_FRAME_FN(thunk)
        name_b = _b(name)
        desc = G6UiFrameDesc(ctypes.sizeof(G6UiFrameDesc), flags, name_b, cb, None)
        fid = ctypes.c_uint32()
        st = self.api.frame_register(ctypes.byref(desc), ctypes.byref(fid))
        if st == G6_E_NOT_IMPLEMENTED:
            return None
        if st != 0:
            raise NativeError(st, "ui.frame_register", self.backend._status(st))
        self._callbacks[fid.value] = (cb, name_b)
        return fid.value

    def frame_unregister(self, frame_id: int) -> bool:
        ok = self.api.frame_unregister(frame_id) == 0  # waits for a frame in progress (in-game)
        self._callbacks.pop(frame_id, None)
        return ok

    def begin(self, title: str, closable: bool, flags: int) -> tuple[bool, bool]:
        o = ctypes.c_int32(1)
        drawn = self.api.begin(_b(title), ctypes.byref(o) if closable else None, flags)
        return bool(drawn), bool(o.value)

    def end(self) -> None:
        self.api.end()

    def set_next_window_pos(self, x: float, y: float, always: bool) -> None:
        self.api.set_next_window_pos(x, y, G6_UI_COND_ALWAYS if always else G6_UI_COND_FIRST_USE)

    def set_next_window_size(self, w: float, h: float, always: bool) -> None:
        self.api.set_next_window_size(w, h, G6_UI_COND_ALWAYS if always else G6_UI_COND_FIRST_USE)

    def text(self, text: str) -> None:
        self.api.text(_b(text))

    def text_colored(self, rgba: Color, text: str) -> None:
        self.api.text_colored(rgba[0], rgba[1], rgba[2], rgba[3], _b(text))

    def button(self, label: str) -> bool:
        return bool(self.api.button(_b(label)))

    def checkbox(self, label: str, value: bool) -> tuple[bool, bool]:
        v = ctypes.c_int32(1 if value else 0)
        changed = self.api.checkbox(_b(label), ctypes.byref(v))
        return bool(changed), bool(v.value)

    def slider_int(self, label: str, value: int, lo: int, hi: int) -> tuple[bool, int]:
        v = ctypes.c_int32(int(value))
        changed = self.api.slider_int(_b(label), ctypes.byref(v), int(lo), int(hi))
        return bool(changed), int(v.value)

    def slider_float(self, label: str, value: float, lo: float, hi: float, fmt: str | None) -> tuple[bool, float]:
        v = ctypes.c_float(value)
        changed = self.api.slider_float(_b(label), ctypes.byref(v), lo, hi, _b(fmt) if fmt else None)
        return bool(changed), float(v.value)

    def input_text(self, label: str, text: str, max_bytes: int) -> tuple[bool, str]:
        raw = _b(text)[:max_bytes]
        buf = ctypes.create_string_buffer(raw, max_bytes + 1)
        changed = self.api.input_text(_b(label), ctypes.cast(buf, ctypes.c_void_p), max_bytes + 1)
        return bool(changed), buf.value.decode("utf-8", "replace")

    def combo(self, label: str, index: int, items) -> tuple[bool, int]:
        arr = (ctypes.c_char_p * max(1, len(items)))(*[_b(i) for i in items])
        cur = ctypes.c_int32(int(index))
        changed = self.api.combo(_b(label), ctypes.byref(cur), arr, len(items))
        return bool(changed), int(cur.value)

    def separator(self) -> None:
        self.api.separator()

    def same_line(self) -> None:
        self.api.same_line()

    def tree_node(self, label: str) -> bool:
        return bool(self.api.tree_node(_b(label)))

    def tree_pop(self) -> None:
        self.api.tree_pop()

    def begin_tab_bar(self, tab_bar_id: str) -> bool:
        return bool(self.api.begin_tab_bar(_b(tab_bar_id)))

    def end_tab_bar(self) -> None:
        self.api.end_tab_bar()

    def begin_tab_item(self, label: str) -> bool:
        return bool(self.api.begin_tab_item(_b(label)))

    def end_tab_item(self) -> None:
        self.api.end_tab_item()

    def color_edit(self, label: str, rgba: Color, no_alpha: bool) -> tuple[bool, Color]:
        c = (ctypes.c_float * 4)(*rgba)
        changed = self.api.color_edit(_b(label), c, G6_UI_COLOR_NO_ALPHA if no_alpha else 0)
        return bool(changed), (float(c[0]), float(c[1]), float(c[2]), float(c[3]))

    def plot_lines(self, label: str, values, overlay: str | None, scale_min: float, scale_max: float,
                   width: float, height: float) -> None:
        arr = (ctypes.c_float * max(1, len(values)))(*values)
        self.api.plot_lines(_b(label), arr, len(values), _b(overlay) if overlay is not None else None,
                            scale_min, scale_max, width, height)


_st = ctypes.c_int
G6HostApi_fields = [
    ("size", ctypes.c_uint32), ("version_major", ctypes.c_uint16), ("version_minor", ctypes.c_uint16),
    ("get_build_info", ctypes.CFUNCTYPE(_st, ctypes.POINTER(G6BuildInfo))),
    ("log", ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p)),
    ("native_resolve", ctypes.CFUNCTYPE(_st, ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p))),
    ("native_invoke", ctypes.CFUNCTYPE(_st, ctypes.c_void_p, ctypes.POINTER(G6NativeCall))),
    ("native_invoke_hash", ctypes.CFUNCTYPE(_st, ctypes.c_uint64, ctypes.POINTER(G6NativeCall))),
    ("script_register", ctypes.CFUNCTYPE(_st, ctypes.POINTER(G6ScriptDesc), ctypes.POINTER(ctypes.c_uint32))),
    ("script_unregister", ctypes.CFUNCTYPE(_st, ctypes.c_uint32)),
    ("script_wait", ctypes.CFUNCTYPE(_st, ctypes.c_uint32)),
    ("anchor_address", ctypes.CFUNCTYPE(_st, ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t))),
    ("vfs_add_overlay", ctypes.CFUNCTYPE(_st, ctypes.c_char_p, ctypes.c_int32)),
    ("game_time_ms", ctypes.CFUNCTYPE(ctypes.c_uint64)),
    ("ui", ctypes.CFUNCTYPE(ctypes.POINTER(G6UiApi))),  # API 0.3
]


class G6HostApi(ctypes.Structure):
    _fields_ = G6HostApi_fields


EXPECTED_API_MAJOR = 0
_LEVELS = {"DEBUG": 0, "INFO": 1, "WARN": 2, "ERROR": 3}


class CAbiBackend(Backend):
    def __init__(self, dll_path: str):
        self.dll = ctypes.CDLL(os.path.abspath(dll_path))
        self.dll.g6_get_host_api.restype = ctypes.POINTER(G6HostApi)
        self.dll.g6_status_str.restype = ctypes.c_char_p
        self.dll.g6_status_str.argtypes = [ctypes.c_int]
        self.api = self.dll.g6_get_host_api().contents
        if self.api.version_major != EXPECTED_API_MAJOR:
            raise RuntimeError(f"runtime API {self.api.version_major}.{self.api.version_minor}, "
                               f"bindings expect {EXPECTED_API_MAJOR}.x")
        if self.api.size < G6HostApi.ui.offset:
            raise RuntimeError("runtime host api table smaller than expected")
        self._keep: list[object] = []  # callbacks must outlive registration
        self.is_mock = hasattr(self.dll, "g6_mock_step")
        self.accepts_unverified = self.is_mock
        self.ui = None  # API 0.3 runtimes: the g6_ui_api table (gta6mod.ui uses it)
        if self.api.size >= ctypes.sizeof(G6HostApi) and self.api.ui:
            table = self.api.ui()
            if table and table.contents.abi_major == UI_ABI_MAJOR and table.contents.size >= ctypes.sizeof(G6UiApi):
                self.ui = CAbiUi(self, table.contents)

    def _status(self, s: int) -> str:
        return self.dll.g6_status_str(s).decode()

    def invoke(self, native_hash: int, slots: list[int], string_slots: int = 0, text_slots: int = 0) -> list[int]:
        call = G6NativeCall()
        call.size = ctypes.sizeof(G6NativeCall)
        call.argc = len(slots)
        for i, v in enumerate(slots):
            call.args[i] = v
        call.string_slots = string_slots
        call.text_slots = text_slots
        s = self.api.native_invoke_hash(native_hash, ctypes.byref(call))
        if s != 0:
            raise NativeError(s, f"0x{native_hash:016X}", self._status(s))
        return list(call.ret)

    def build_info(self) -> BuildInfo:
        bi = G6BuildInfo()
        bi.size = ctypes.sizeof(G6BuildInfo)
        s = self.api.get_build_info(ctypes.byref(bi))
        if s != 0:
            raise NativeError(s, "get_build_info", self._status(s))
        return BuildInfo(bi.game_version.decode(), bi.platform.decode(), bytes(bi.exe_sha256).hex(),
                         bi.natives_known, bi.natives_verified)

    def now_ms(self) -> int:
        return int(self.api.game_time_ms())

    def log(self, level: str, plugin: str, message: str) -> None:
        self.api.log(_LEVELS.get(level, 1), plugin.encode(), message.encode())

    def ui_proc(self, name: str) -> int:
        """Address of the runtime DLL's ``g6_ui_*`` export ``name``, or 0 (same contract as
        ``EmbeddedBackend.ui_proc``; sdk/docs/python_host.md, "Python UI")."""
        if not name.startswith("g6_ui_"):
            return 0
        try:
            fn = getattr(self.dll, name)
        except AttributeError:
            return 0
        return int(ctypes.cast(fn, ctypes.c_void_p).value or 0)

    # -- mock-only helpers -------------------------------------------------------
    def mock_register_native(self, native_hash: int, fn) -> None:
        """fn(call: G6NativeCall) — fill call.ret. Mock runtime only."""
        cb = MOCK_NATIVE_FN(lambda pcall, _user: fn(pcall.contents))
        self._keep.append(cb)
        self.dll.g6_mock_register_native.argtypes = [ctypes.c_uint64, MOCK_NATIVE_FN, ctypes.c_void_p]
        s = self.dll.g6_mock_register_native(native_hash, cb, None)
        if s != 0:
            raise NativeError(s, "g6_mock_register_native", self._status(s))

    def register_tick(self, name: str, fn) -> int:
        cb = SCRIPT_TICK(lambda _u: fn())
        desc = G6ScriptDesc(ctypes.sizeof(G6ScriptDesc), name.encode(), cb, None)
        self._keep += [cb, desc]
        sid = ctypes.c_uint32()
        s = self.api.script_register(ctypes.byref(desc), ctypes.byref(sid))
        if s != 0:
            raise NativeError(s, "script_register", self._status(s))
        return sid.value

    def step(self, elapsed_ms: int) -> None:
        if not self.is_mock:
            raise RuntimeError("step() is only available on the mock runtime")
        self.dll.g6_mock_step(ctypes.c_uint32(elapsed_ms))
