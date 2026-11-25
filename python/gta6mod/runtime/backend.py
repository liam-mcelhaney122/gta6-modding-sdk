"""Backend interface: where native calls actually go.

Backends
--------
* ``MockBackend`` (``runtime/mock.py``): pure Python, in-process fake world.
  Lets mods be written and unit-tested today, with no game.
* ``CAbiBackend`` (``runtime/cabi.py``): loads a runtime DLL that implements
  ``include/gta6mod/gta6mod.h``. Today that is the C mock
  (``gta6mod_mock.dll``). Later it is the real in-game runtime: Python
  embedded in the host, or out-of-process over IPC.
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

    @abc.abstractmethod
    def invoke(self, native_hash: int, slots: list[int]) -> list[int]:
        """Run one native; return the 4 return slots. Raise NativeError on failure."""

    @abc.abstractmethod
    def build_info(self) -> BuildInfo: ...

    @abc.abstractmethod
    def now_ms(self) -> int:
        """Game clock in milliseconds (drives script waits)."""

    def log(self, level: str, plugin: str, message: str) -> None:
        print(f"[{level}] {plugin}: {message}")
