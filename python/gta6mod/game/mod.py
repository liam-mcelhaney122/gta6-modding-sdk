"""Mod structure: loops, timers, key bindings and toggles on top of ``gta6mod.script``.

    from gta6mod.game import Mod, player, GTA5Control

    m = Mod("god_mode")
    god = m.toggle(GTA5Control.DETONATE)

    @god.on_enable
    def _():
        player.ped().set_invincible(True)

    @m.every(250, when=god)
    def top_up():
        ped = player.ped()
        if ped.health < ped.max_health:
            ped.heal()

Every handler may be a plain function or a generator function (to ``yield wait(ms)`` or
``yield from`` a helper). A key handler that is a generator runs as its own script, so it never
delays other key bindings. Toggle and ``on_stop`` callbacks must be plain functions, because
``stop()`` runs them outside any script.
"""
from __future__ import annotations

import inspect
from typing import Callable

from .. import script as _script
from .core import log as _log
from .core import wait
from .world import controls


class Toggle:
    """An on/off switch bound to a key. Flip it with the key, or call ``set`` / ``flip``."""

    def __init__(self, mod: "Mod", key, label: str | None):
        self.mod = mod
        self.key = key
        self.label = label or mod.name
        self.on = False
        self._enable: list[Callable[[], None]] = []
        self._disable: list[Callable[[], None]] = []

    def on_enable(self, fn: Callable[[], None]) -> Callable[[], None]:
        self._enable.append(fn)
        return fn

    def on_disable(self, fn: Callable[[], None]) -> Callable[[], None]:
        self._disable.append(fn)
        return fn

    def set(self, on: bool) -> None:
        on = bool(on)
        if on == self.on:
            return
        self.on = on
        for fn in (self._enable if on else self._disable):
            fn()

    def flip(self) -> None:
        self.set(not self.on)

    def __bool__(self) -> bool:
        return self.on


def _run(fn, *args):
    """Call a handler; if it is a generator function, return its generator for ``yield from``."""
    r = fn(*args)
    return r if inspect.isgenerator(r) else None


class Mod:
    """A named mod. Registers its scripts on the default scheduler under ``<name>.<part>``."""

    def __init__(self, name: str, scheduler: _script.Scheduler | None = None):
        self.name = name
        self._sched = scheduler or _script.scheduler()
        self._scripts: list[str] = []
        self._keys: list[_Binding] = []
        self._key_seq = 0
        self._key_scripts: set[str] = set()
        self._toggles: list[Toggle] = []
        self._stop: list[Callable[[], None]] = []
        self._input_registered = False
        self.stopped = False

    # -- logging ----------------------------------------------------------------
    def log(self, message: str) -> None:
        _log(message, "INFO", self.name)

    def warn(self, message: str) -> None:
        _log(message, "WARN", self.name)

    def error(self, message: str) -> None:
        _log(message, "ERROR", self.name)

    # -- scripts ----------------------------------------------------------------
    def _add(self, part: str, fn: Callable) -> None:
        name = f"{self.name}.{part}"
        k = 2
        while name in self._scripts:
            name = f"{self.name}.{part}{k}"
            k += 1
        self._sched.add(name, fn)
        self._scripts.append(name)

    def loop(self, fn: Callable) -> Callable:
        """Decorator: run ``fn`` as its own script (generator: runs once, may loop; plain: every tick)."""
        self._add(fn.__name__, fn)
        return fn

    def every(self, ms: int, when: Toggle | None = None):
        """Decorator: call ``fn`` every ``ms`` of game time, only while ``when`` is on (if given)."""
        def deco(fn: Callable) -> Callable:
            def runner():
                while True:
                    if when is None or when.on:
                        g = _run(fn)
                        if g is not None:
                            yield from g
                    yield wait(ms)
            self._add(fn.__name__, runner)
            return fn
        return deco

    def on_key(self, key, pad: int = 0):
        """Decorator: call ``fn`` when ``key`` is just pressed on ``pad``."""
        def deco(fn: Callable) -> Callable:
            self._bind(key, pad, fn)
            return fn
        return deco

    def toggle(self, key, label: str | None = None, pad: int = 0) -> Toggle:
        t = Toggle(self, key, label)
        self._toggles.append(t)
        self._bind(key, pad, t.flip)
        return t

    def on_stop(self, fn: Callable[[], None]) -> Callable[[], None]:
        self._stop.append(fn)
        return fn

    def _bind(self, key, pad: int, fn: Callable) -> None:
        self._keys.append(_Binding(key, pad, fn))
        if not self._input_registered:
            self._input_registered = True
            self._add("input", self._input_loop)

    def _input_loop(self):
        """Poll every binding each frame. A handler that waits runs as its own script (``<mod>.keyN``),
        so it never delays other bindings; a press while that binding's handler still runs is ignored."""
        while True:
            for b in list(self._keys):
                if b.busy or not controls.just_pressed(b.key, b.pad):
                    continue
                g = _run(b.fn)
                if g is not None:
                    self._spawn_handler(b, g)
            self._purge_finished()
            yield wait(0)

    def _spawn_handler(self, b: "_Binding", gen) -> None:
        b.busy = True

        def runner():
            try:
                yield from gen
            finally:
                b.busy = False
        self._key_seq += 1
        self._add(f"key{self._key_seq}", runner)
        self._key_scripts.add(self._scripts[-1])

    def _purge_finished(self) -> None:
        done = {s.name for s in self._sched.scripts if s.done and s.name in self._key_scripts}
        for name in done:
            self._sched.remove(name)
            self._scripts.remove(name)
            self._key_scripts.discard(name)

    # -- shutdown ---------------------------------------------------------------
    def stop(self) -> list[BaseException]:
        """Stop the mod: cancel its scripts first (closing suspended handlers, which runs their ``finally``
        blocks), then turn every toggle off and run ``on_stop`` handlers. Each cleanup callback runs even
        if an earlier one fails; failures are logged and returned."""
        if self.stopped:
            return []
        self.stopped = True
        mine = set(self._scripts)
        for s in self._sched.scripts:
            if s.name in mine and not s.done:
                s.done = True
                if s.gen is not None:
                    try:
                        s.gen.close()
                    except ValueError:  # stop() was called from this script: it ends at its next yield
                        pass
        for name in self._scripts:
            self._sched.remove(name)
        self._scripts.clear()

        errors: list[BaseException] = []

        def guarded(fn: Callable[[], None]) -> None:
            try:
                fn()
            except Exception as e:  # noqa: BLE001 - every cleanup must get its turn
                errors.append(e)
                self.error(f"stop: cleanup failed: {e!r}")

        for t in self._toggles:
            if t.on:
                t.on = False
                for fn in t._disable:
                    guarded(fn)
        for fn in self._stop:
            guarded(fn)
        return errors


class _Binding:
    __slots__ = ("key", "pad", "fn", "busy")

    def __init__(self, key, pad: int, fn: Callable):
        self.key, self.pad, self.fn, self.busy = key, pad, fn, False
