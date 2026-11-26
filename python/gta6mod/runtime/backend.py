"""Backend interface: where native calls actually go.

Backends
--------
* ``MockBackend`` (``runtime/mock.py``): pure Python, in-process fake world.
  Lets mods be written and unit-tested today, with no game.
* ``CAbiBackend`` (``runtime/cabi.py``): loads a runtime DLL that implements
  ``include/gta6mod/gta6mod.h``. Today that is the C mock
  (``gta6mod_mock.dll``), or the in-game runtime from an external Python.
* ``EmbeddedBackend`` (``runtime/embedded.py``): Python embedded in the game by
  ``g6_python.dll``; calls the runtime's host table through the built-in ``_g6``
  module (sdk/docs/python_host.md).

Each backend may also carry ``ui``: a ``UiBackend`` (``MockUi`` / ``CAbiUi`` /
``EmbeddedUi``) behind ``gta6mod.ui``.
"""
from __future__ import annotations

import abc
from dataclasses import dataclass


class NativeError(RuntimeError):
    def __init__(self, status: int, name: str, message: str):
        super().__init__(f"{name}: {message} (status {status})")
        self.status = status


# g6_status values (gta6mod.h)
G6_OK = 0
G6_E_UNKNOWN_NATIVE = 2
G6_E_UNVERIFIED_NATIVE = 3
G6_E_NOT_ON_SCRIPT_THREAD = 5
G6_E_NOT_IMPLEMENTED = 8


@dataclass(frozen=True)
class BuildInfo:
    game_version: str
    platform: str
    exe_sha256: str
    natives_known: int
    natives_verified: int


class Backend(abc.ABC):
    """Executes packed native calls. Slots follow docs/native-signatures.md."""

    #: real game backends set this False; the runtime then enforces verification
    accepts_unverified: bool = False

    #: the UI layer (``runtime/ui_backend.py``), or None when the backend has none; ``gta6mod.ui`` uses only this
    ui = None

    @abc.abstractmethod
    def invoke(self, native_hash: int, slots: list[int], string_slots: int = 0, text_slots: int = 0) -> list[int]:
        """Run one native; return the 4 return slots. Raise NativeError on failure. string_slots / text_slots:
        bit i set = slots[i] points at NUL-terminated UTF-8 that the native must receive as a script String /
        text object (g6_native_call.string_slots / text_slots, G-120); the backend builds the objects."""

    @abc.abstractmethod
    def build_info(self) -> BuildInfo: ...

    @abc.abstractmethod
    def now_ms(self) -> int:
        """Game clock in milliseconds (drives script waits)."""

    def log(self, level: str, plugin: str, message: str) -> None:
        print(f"[{level}] {plugin}: {message}")
