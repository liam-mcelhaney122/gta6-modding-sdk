"""Timing and logging shared by the helper modules."""
from __future__ import annotations

from .. import runtime as _rt
from .. import script as _script

wait = _script.wait


def now() -> int:
    """Game clock in milliseconds (drives waits)."""
    return _rt.current().now_ms()


def log(message: str, level: str = "INFO", source: str = "mod") -> None:
    _rt.current().log(level.upper(), source, str(message))
