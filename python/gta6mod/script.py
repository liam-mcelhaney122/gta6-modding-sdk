"""Script registration and cooperative scheduling (ScriptHookV-style).

A script is either
  * a generator function that ``yield wait(ms)`` to give up the frame, or
  * a plain function, called once per game tick.

    from gta6mod import script, natives as N

    @script.register("hello")
    def main():
        while True:
            if N.PAD.IS_CONTROL_JUST_PRESSED(0, 51):
                print(N.ENTITY.GET_ENTITY_COORDS(N.PLAYER.PLAYER_PED_ID(), True))
            yield script.wait(0)

The in-game runtime will drive ``Scheduler.tick()`` from the game's script
thread (one call per frame). Offline, ``run_for()`` drives it with the mock
backend's clock.
"""
from __future__ import annotations

import inspect
import traceback
from dataclasses import dataclass, field
from typing import Callable, Generator

from . import runtime as _rt


@dataclass(frozen=True)
class Wait:
    ms: int


def wait(ms: int = 0) -> Wait:
    return Wait(max(0, int(ms)))


@dataclass
class _Script:
    name: str
    fn: Callable
    gen: Generator | None = None
    wake_at: int = 0
    done: bool = False
    error: str | None = None
    ticks: int = 0


@dataclass
class Scheduler:
    scripts: list[_Script] = field(default_factory=list)

    def add(self, name: str, fn: Callable) -> None:
        if any(s.name == name for s in self.scripts):
            raise ValueError(f"script {name!r} already registered")
        self.scripts.append(_Script(name, fn))

    def remove(self, name: str) -> None:
        self.scripts = [s for s in self.scripts if s.name != name]

    def tick(self) -> None:
        rt = _rt.current()
        now = rt.now_ms()
        for s in self.scripts:
            if s.done or now < s.wake_at:
                continue
            try:
                s.ticks += 1
                if s.gen is None and inspect.isgeneratorfunction(s.fn):
                    s.gen = s.fn()
                if s.gen is None:
                    s.fn()  # plain per-tick callback
                    continue
                req = next(s.gen)
                ms = req.ms if isinstance(req, Wait) else 0
                s.wake_at = now + ms
            except StopIteration:
                s.done = True
            except Exception:  # one broken script must not kill the others
                s.done = True
                s.error = traceback.format_exc()
                rt.log("ERROR", s.name, s.error)


_default = Scheduler()


def register(name: str):
    """Decorator: register a script on the default scheduler."""
    def deco(fn):
        _default.add(name, fn)
        return fn
    return deco


def scheduler() -> Scheduler:
    return _default


def run_for(ms: int, frame_ms: int = 16, sched: Scheduler | None = None) -> None:
    """Offline driver: tick the scheduler for ``ms`` of mock game time."""
    sched = sched or _default
    backend = _rt.current().backend
    step = getattr(backend, "step", None)
    if step is None:
        raise RuntimeError("run_for() needs a backend with a controllable clock (mock)")
    elapsed = 0
    while elapsed < ms:
        sched.tick()
        step(frame_ms)
        elapsed += frame_ms
