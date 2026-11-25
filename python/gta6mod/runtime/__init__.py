"""Runtime facade: one active backend, verification policy, native dispatch.

    from gta6mod import runtime
    rt = runtime.use_mock()            # offline
    # rt = runtime.use_cabi("gta6mod_mock.dll")   # C ABI runtime
"""
from __future__ import annotations

from .backend import Backend, BuildInfo, NativeError, G6_E_UNVERIFIED_NATIVE
from .marshal import pack, unpack

__all__ = ["Runtime", "current", "use", "use_mock", "use_cabi", "NativeError", "BuildInfo"]


class Runtime:
    def __init__(self, backend: Backend, allow_unverified: bool = False):
        self.backend = backend
        self.allow_unverified = allow_unverified or backend.accepts_unverified
        self.call_count = 0

    def call(self, native_hash: int, sig: str, name: str, status: str, args):
        if status != "verified" and not self.allow_unverified:
            raise NativeError(G6_E_UNVERIFIED_NATIVE, name,
                              "native hash not verified for this game build; "
                              "start the runtime with allow_unverified=True to try anyway")
        packed = pack(sig, args)
        ret = self.backend.invoke(native_hash, packed.slots)
        self.call_count += 1
        return unpack(sig, ret, packed)

    def call_raw(self, native_hash: int, name: str, status: str, args):
        """Generic u64 path for untyped natives: raw slots in, first ret slot out."""
        if status != "verified" and not self.allow_unverified:
            raise NativeError(G6_E_UNVERIFIED_NATIVE, name,
                              "native hash not verified for this game build; "
                              "start the runtime with allow_unverified=True to try anyway")
        slots = [int(a) & 0xFFFFFFFFFFFFFFFF for a in args]
        if len(slots) > 32:
            raise NativeError(4, name, "too many arguments")
        ret = self.backend.invoke(native_hash, slots)
        self.call_count += 1
        return int(ret[0]) if ret else 0

    def build_info(self) -> BuildInfo:
        return self.backend.build_info()

    def now_ms(self) -> int:
        return self.backend.now_ms()

    def log(self, level: str, plugin: str, message: str) -> None:
        self.backend.log(level, plugin, message)


_current: Runtime | None = None


def use(backend: Backend, allow_unverified: bool = False) -> Runtime:
    global _current
    _current = Runtime(backend, allow_unverified)
    return _current


def use_mock(**kw) -> Runtime:
    from .mock import MockBackend
    return use(MockBackend(**kw))


def use_cabi(dll_path: str, allow_unverified: bool = False) -> Runtime:
    from .cabi import CAbiBackend
    return use(CAbiBackend(dll_path), allow_unverified)


def current() -> Runtime:
    if _current is None:
        raise RuntimeError("no gta6mod runtime active: call gta6mod.runtime.use_mock() or use_cabi()")
    return _current
