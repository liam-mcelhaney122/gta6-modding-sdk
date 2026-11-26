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

When the host unloads the scripts it raises the **stop event** with ``Scheduler.stop()``
(``script.shutdown()`` for the default scheduler): every ``on_stop`` handler runs once, on the
calling thread, then every script still alive is cancelled. ``gta6mod.game.Mod`` registers its own
``stop()`` there, so toggles are switched off and the mod's scripts end. ``python -m gta6mod run``
raises it when the run ends.
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
    stop_handlers: list[Callable[[], object]] = field(default_factory=list)
    stopped: bool = False

    def on_stop(self, fn: Callable[[], object]) -> Callable[[], object]:
        """Register a stop-event handler (a plain function: it runs outside any script, so it cannot wait)."""
        self.stop_handlers.append(fn)
        return fn

    def stop(self) -> list[BaseException]:
        """Raise the stop event (host unload): run every ``on_stop`` handler once, in registration order, each
        even if an earlier one failed, then cancel every script still alive (closing suspended generators runs
        their ``finally`` blocks). Failures are logged as ``stop: handler failed`` and returned. A second call
        does nothing."""
        if self.stopped:
            return []
        self.stopped = True
        errors: list[BaseException] = []
        for fn in list(self.stop_handlers):
            try:
                r = fn()
                if isinstance(r, list):  # Mod.stop() returns the cleanup failures it already logged
                    errors += [e for e in r if isinstance(e, BaseException)]
            except Exception as e:  # noqa: BLE001 - every handler must get its turn
                errors.append(e)
                _rt.current().log("ERROR", "script", f"stop: handler failed: {e!r}")
        for s in self.scripts:
            if s.done:
                continue
            s.done = True
            if s.gen is not None:
                try:
                    s.gen.close()
                except Exception as e:  # noqa: BLE001
                    errors.append(e)
                    _rt.current().log("ERROR", s.name, f"stop: close failed: {e!r}")
        return errors

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


def shutdown() -> list[BaseException]:
    """Raise the stop event on the default scheduler (what the host does when it unloads the scripts)."""
    return _default.stop()


def reset() -> Scheduler:
    """Replace the default scheduler with an empty one (tests, reloading mods offline)."""
    global _default
    _default = Scheduler()
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
