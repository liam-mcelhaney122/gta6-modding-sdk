"""Backend over the C ABI (include/gta6mod/gta6mod.h) via ctypes.

Works with any runtime DLL exporting ``g6_get_host_api``. Today that is the
offline ``gta6mod_mock.dll`` (sdk/build). Later it is the in-game runtime.
"""
from __future__ import annotations

import ctypes
import os

from .backend import Backend, BuildInfo, NativeError

G6_MAX_NATIVE_ARGS = 32
G6_MAX_NATIVE_RETS = 4


class G6NativeCall(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("argc", ctypes.c_uint32),
                ("args", ctypes.c_uint64 * G6_MAX_NATIVE_ARGS),
                ("ret", ctypes.c_uint64 * G6_MAX_NATIVE_RETS)]


class G6BuildInfo(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("game_version", ctypes.c_char_p), ("platform", ctypes.c_char_p),
                ("exe_sha256", ctypes.c_uint8 * 32), ("natives_known", ctypes.c_uint32),
                ("natives_verified", ctypes.c_uint32)]


class G6ScriptDesc(ctypes.Structure):
    pass


SCRIPT_TICK = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
G6ScriptDesc._fields_ = [("size", ctypes.c_uint32), ("name", ctypes.c_char_p), ("tick", SCRIPT_TICK), ("user", ctypes.c_void_p)]

MOCK_NATIVE_FN = ctypes.CFUNCTYPE(None, ctypes.POINTER(G6NativeCall), ctypes.c_void_p)

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
        if self.api.size < ctypes.sizeof(G6HostApi):
            raise RuntimeError("runtime host api table smaller than expected")
        self._keep: list[object] = []  # callbacks must outlive registration
        self.is_mock = hasattr(self.dll, "g6_mock_step")
        self.accepts_unverified = self.is_mock

    def _status(self, s: int) -> str:
        return self.dll.g6_status_str(s).decode()

    def invoke(self, native_hash: int, slots: list[int]) -> list[int]:
        call = G6NativeCall()
        call.size = ctypes.sizeof(G6NativeCall)
        call.argc = len(slots)
        for i, v in enumerate(slots):
            call.args[i] = v
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
