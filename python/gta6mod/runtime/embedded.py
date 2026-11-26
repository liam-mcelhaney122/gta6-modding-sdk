"""Backend for Python running INSIDE the game process (g6_python.dll, sdk/docs/python_host.md).

The plugin embeds CPython and registers a built-in module ``_g6`` (``PyImport_AppendInittab``) whose
functions call the runtime's ``g6_host_api`` table directly: no ctypes on the native path. Slot layouts are
``marshal.py``'s, exactly as for the other backends (``Runtime.call`` packs, this backend only moves slots).

``_g6`` surface (implemented in sdk/python/host/g6_python.cpp):

    invoke(hash, slots[, string_slots, text_slots]) -> (status, (r0, r1, r2, r3))
                                                         native_resolve (cached) + native_invoke; the masks
                                                         are g6_native_call.string_slots / text_slots
    log(level, source, message)                          g6_host_api.log (0 DEBUG .. 3 ERROR)
    now_ms() -> int                                      g6_host_api.game_time_ms
    build_info() -> (status, version, platform, sha256_hex, known, verified)
    ui_proc(name) -> int                                 address of a g6_ui_* export of the runtime module, or 0
                                                         (EmbeddedUi binds g6_ui_retained_* through it)
    on_script_thread() -> bool                           True inside a plugin tick / stop / unload call
    watch(on[, budget_ms])                               arm/disarm the tick-budget watchdog (host.py uses it)
    set_budget_exception(cls)                            exception type the watchdog raises

Natives only run inside a plugin tick on the game script thread. A call from any other thread (a thread a
mod started) gets ``G6_E_NOT_ON_SCRIPT_THREAD`` from ``_g6`` itself, without reaching the runtime.
"""
from __future__ import annotations

from .backend import Backend, BuildInfo, NativeError
from .embedded_ui import EmbeddedUi

# g6_status (gta6mod.h)
G6_E_NOT_ON_SCRIPT_THREAD = 5
G6_E_ONLINE_SESSION = 7
STATUS_TEXT = {
    0: "ok", 1: "invalid argument", 2: "unknown native", 3: "unverified native", 4: "too many arguments",
    5: "not on script thread", 6: "unsupported game build", 7: "online session (refused)",
    8: "not implemented", 9: "no such anchor", 100: "internal error",
}
LEVELS = {"DEBUG": 0, "INFO": 1, "WARN": 2, "WARNING": 2, "ERROR": 3}
UI_PREFIX = "g6_ui_"


class GuardClosed(NativeError):
    """The runtime refused a native because the single-player guard is closed (G6_E_ONLINE_SESSION). The host
    pauses every mod when it sees this: the guard latches until the game restarts."""


class EmbeddedBackend(Backend):
    def __init__(self, g6=None):
        if g6 is None:
            import _g6 as g6  # only exists inside g6_python.dll
        self._g6 = g6
        info = self.build_info()
        # Same rule as CAbiBackend: only the mock runtime accepts unverified natives by itself. In game the
        # Runtime's allow_unverified ([python] allow_unverified in g6_python.ini) decides.
        self.accepts_unverified = info.platform == "mock"
        # gta6mod.ui: the runtime's retained UI (g6_ui_retained_* exports), or None when it has none. Frame
        # functions then run on this (script) thread once per tick; the overlay replays what they recorded.
        self.ui = EmbeddedUi.bind(self.ui_proc, self.log)

    def invoke(self, native_hash: int, slots: list[int], string_slots: int = 0, text_slots: int = 0) -> list[int]:
        if self.ui is not None and self.ui.in_frame:  # same rule as the render-thread hosts (gta6mod.ui)
            raise NativeError(G6_E_NOT_ON_SCRIPT_THREAD, f"0x{native_hash:016X}",
                              "natives are refused inside a ui frame function; hand the work to ui.defer")
        if string_slots or text_slots:
            status, ret = self._g6.invoke(native_hash, slots, string_slots, text_slots)
        else:
            status, ret = self._g6.invoke(native_hash, slots)
        if status != 0:
            cls = GuardClosed if status == G6_E_ONLINE_SESSION else NativeError
            raise cls(status, f"0x{native_hash:016X}", STATUS_TEXT.get(status, "unknown status"))
        return list(ret)

    def build_info(self) -> BuildInfo:
        status, version, platform, sha, known, verified = self._g6.build_info()
        if status != 0:
            raise NativeError(status, "get_build_info", STATUS_TEXT.get(status, "unknown status"))
        return BuildInfo(version, platform, sha, known, verified)

    def now_ms(self) -> int:
        return int(self._g6.now_ms())

    def log(self, level: str, plugin: str, message: str) -> None:
        self._g6.log(LEVELS.get(level.upper(), 1), str(plugin), str(message))

    # -- UI hook (sdk/docs/python_host.md, "Python UI") -----------------------------------------------------
    def ui_proc(self, name: str) -> int:
        """Address of the runtime's ``g6_ui_*`` export ``name``, or 0 (no UI in this runtime, or another
        prefix). EmbeddedUi binds the g6_ui_retained_* exports with ctypes and calls them on the script thread,
        in a tick."""
        if not name.startswith(UI_PREFIX):
            return 0
        return int(self._g6.ui_proc(name))

    def ui_available(self, export: str) -> bool:
        """True when the runtime module exports ``export`` (the UI layer passes its own version export)."""
        return self.ui_proc(export) != 0
