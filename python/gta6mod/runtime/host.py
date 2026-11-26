"""In-game Python mod host: loads ``mods/python/*.py``, ticks them on the game script thread, isolates failures.

Driven by ``g6_python.dll`` (sdk/python/host/g6_python.cpp; design in sdk/docs/python_host.md):

* ``boot(...)`` once, from the plugin's first tick: installs the ``EmbeddedBackend`` runtime, routes
  ``sys.stdout`` / ``sys.stderr`` to the runtime log and loads every mod;
* ``ModHost.tick()`` once per plugin tick;
* ``ModHost.stop()`` for the stop event (``g6_plugin_stop``), ``ModHost.shutdown()`` on unload.

All of it runs on the game script thread with the GIL held, and nothing here starts a thread.

Isolation (failures, not security: a mod is ordinary Python with the whole interpreter available):

* each mod is its own module ``g6mods.<file stem>`` with its own ``script.Scheduler``; its top-level code
  and every tick run inside ``script.using(<that scheduler>)``, so ``script.register`` and ``Mod(...)`` land
  in it and two mods may use the same script names;
* a mod whose load raises, whose script raises, or that overruns the watchdog budget is **disabled**: its
  stop handlers run (toggles off), its scripts are cancelled, the others keep running;
* a native refused with ``G6_E_ONLINE_SESSION`` (single-player guard closed) **pauses every mod** until
  the game restarts: the guard latches, so every later native call would be refused too.
"""
from __future__ import annotations

import gc
import importlib
import importlib.util
import os
import sys
import time
import traceback
import types
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import Iterator

from .. import script as _script
from . import use as _use
from .embedded import EmbeddedBackend, GuardClosed

SOURCE = "python"
PACKAGE = "g6mods"

LOADED, DISABLED, STOPPED = "loaded", "disabled", "stopped"


class TickBudgetExceeded(BaseException):
    """Raised into a mod by the plugin's watchdog when one mod operation (load, tick or stop) runs longer than
    its wall-clock budget ([python] tick_budget_ms / load_budget_ms). A BaseException, so a mod's
    ``except Exception`` does not swallow it; the host disables that mod. The plugin raises the class itself
    (an asynchronous exception has no arguments) after setting ``budget_ms``."""

    budget_ms = 0

    def __init__(self, *args):
        if not args:
            args = (f"ran longer than {type(self).budget_ms} ms of wall time; interrupted by the g6_python watchdog",)
        super().__init__(*args)


@dataclass
class ModEntry:
    name: str
    path: str
    module_name: str
    sched: _script.Scheduler
    stamp: tuple = ()
    module: types.ModuleType | None = None
    state: str = LOADED
    error: str | None = None
    loaded_at_tick: int = 0
    extra: dict = field(default_factory=dict)


def _stamp(path: str) -> tuple:
    st = os.stat(path)
    return (st.st_mtime_ns, st.st_size)


def module_name_for(stem: str) -> str:
    safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in stem)
    if not safe or safe[0].isdigit():
        safe = "_" + safe
    return f"{PACKAGE}.{safe}"


class LogStream:
    """``sys.stdout`` / ``sys.stderr`` inside the game: complete lines go to the runtime log."""

    encoding = "utf-8"
    errors = "replace"

    def __init__(self, g6, level: int, source: str):
        self._g6 = g6
        self._level = level
        self._source = source
        self._buf = ""

    def write(self, s) -> int:
        s = str(s)
        self._buf += s
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            self._g6.log(self._level, self._source, line.rstrip("\r"))
        return len(s)

    def flush(self) -> None:
        if self._buf:
            self._g6.log(self._level, self._source, self._buf)
            self._buf = ""

    def isatty(self) -> bool:
        return False

    def writable(self) -> bool:
        return True


class ModHost:
    def __init__(self, mods_dir: str, g6=None, allow_unverified: bool = False, hot_reload: bool = False,
                 reload_poll_ms: int = 1000, load_budget_ms: int = 0, preload: tuple[str, ...] = ("gta6mod.game",)):
        self.g6 = g6 if g6 is not None else importlib.import_module("_g6")
        self.backend = EmbeddedBackend(self.g6)
        self.rt = _use(self.backend, allow_unverified)
        self.mods_dir = os.path.abspath(mods_dir)
        self.mods: dict[str, ModEntry] = {}
        self.hot_reload = bool(hot_reload)
        self.reload_poll_ms = max(0, int(reload_poll_ms))
        self.load_budget_ms = max(0, int(load_budget_ms))
        self.paused: str | None = None
        self.stopped = False
        self.ticks = 0
        self._next_poll = 0.0
        self.g6.set_budget_exception(TickBudgetExceeded)
        if PACKAGE not in sys.modules:
            pkg = types.ModuleType(PACKAGE, "Namespace of the loaded Python mods (one module per mod file).")
            pkg.__path__ = []
            sys.modules[PACKAGE] = pkg
        # Import the SDK before any mod runs: the first import compiles natives (2.5 MB) and must not count
        # against a mod's watchdog budget.
        for name in preload:
            importlib.import_module(name)

    # -- logging -------------------------------------------------------------------------------------------
    def log(self, level: str, message: str) -> None:
        self.backend.log(level, SOURCE, message)

    # -- discovery and loading ------------------------------------------------------------------------------
    def discover(self) -> dict[str, str]:
        """``<mods_dir>/*.py`` by file stem, sorted; names starting with ``_`` or ``.`` are skipped."""
        try:
            names = sorted(os.listdir(self.mods_dir))
        except OSError:
            return {}
        out: dict[str, str] = {}
        for fn in names:
            stem, ext = os.path.splitext(fn)
            if ext.lower() != ".py" or stem.startswith(("_", ".")):
                continue
            path = os.path.join(self.mods_dir, fn)
            if os.path.isfile(path):
                out[stem] = path
        return out

    def load_all(self) -> int:
        found = self.discover()
        if not found:
            self.log("WARN", f"no mods in {self.mods_dir}")
        for name, path in found.items():
            self._load(name, path)
        ok = sum(1 for e in self.mods.values() if e.state == LOADED)
        self.log("INFO", f"{ok} of {len(found)} mod(s) loaded from {self.mods_dir}")
        return ok

    @contextmanager
    def _running(self, entry: ModEntry, budget_ms: int = 0) -> Iterator[None]:
        with _script.using(entry.sched):
            self.g6.watch(True, budget_ms)
            try:
                yield
            finally:
                self.g6.watch(False, 0)

    def _load(self, name: str, path: str) -> ModEntry:
        try:
            stamp = _stamp(path)
        except OSError:
            stamp = ()
        entry = ModEntry(name, path, module_name_for(name), _script.Scheduler(), stamp, loaded_at_tick=self.ticks)
        self.mods[name] = entry
        try:
            # Compiled from source on every load, never from __pycache__: a .pyc is keyed by the source's mtime in
            # whole seconds and its size, so two edits within a second (hot reload) could run stale code.
            with open(path, "rb") as f:
                source = f.read()
            spec = importlib.util.spec_from_file_location(entry.module_name, path)
            module = importlib.util.module_from_spec(spec) if spec else types.ModuleType(entry.module_name)
            module.__file__ = path
            entry.module = module
            sys.modules[entry.module_name] = module
            with self._running(entry, self.load_budget_ms):
                code = compile(source, path, "exec", dont_inherit=True)
                exec(code, module.__dict__)  # noqa: S102 - running the mod is the point
        except BaseException as e:  # noqa: BLE001 - a broken mod must not stop the host
            self._disable(entry, "load failed", e)
            return entry
        self.log("INFO", f"mod {name} loaded ({len(entry.sched.scripts)} script(s))")
        return entry

    # -- ticking ------------------------------------------------------------------------------------------
    def tick(self) -> None:
        if self.stopped or self.paused:
            return
        self.ticks += 1
        if self.hot_reload:
            self._poll_reload()
        for entry in list(self.mods.values()):
            if entry.state != LOADED:
                continue
            try:
                with self._running(entry):
                    entry.sched.tick()
            except BaseException as e:  # noqa: BLE001 - watchdog, SystemExit, ... from this mod only
                self._disable(entry, "tick failed", e)
                continue
            failed = [s for s in entry.sched.scripts if s.error is not None]
            if not failed:
                continue
            if any(isinstance(s.exc, GuardClosed) for s in failed):
                self._pause_for_guard(entry)
                return
            names = ", ".join(s.name for s in failed)
            self._disable(entry, f"script {names} failed (traceback above)", None)

    def _disable(self, entry: ModEntry, why: str, exc: BaseException | None) -> None:
        entry.state = DISABLED
        if exc is not None:
            entry.error = f"{why}: {exc!r}"
            tb = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__)).rstrip()
            self.log("ERROR", f"mod {entry.name}: {why}:\n{tb}")
        else:
            entry.error = why
        self.log("ERROR", f"mod {entry.name} disabled: {why}; the other mods keep running")
        self._run_stop(entry)

    def _run_stop(self, entry: ModEntry) -> int:
        """The mod's stop event: its ``on_stop`` handlers (``Mod.stop``: toggles off) and script cancellation.
        Returns the number of failures (each already logged)."""
        try:
            with self._running(entry):
                return len(entry.sched.stop())
        except BaseException as e:  # noqa: BLE001 - e.g. the watchdog interrupting a stop handler
            self.log("ERROR", f"mod {entry.name}: stop failed: {e!r}")
            self._cancel(entry)
            return 1

    @staticmethod
    def _cancel(entry: ModEntry) -> None:
        """End every script of ``entry`` without running its stop handlers."""
        entry.sched.stopped = True
        for s in entry.sched.scripts:
            s.done = True

    def _pause_for_guard(self, entry: ModEntry) -> None:
        self.paused = "guard"
        self.log("ERROR", f"mod {entry.name}: the runtime refused a native with G6_E_ONLINE_SESSION (single-player "
                          "guard closed); every Python mod is paused until the game restarts")

    # -- hot reload -----------------------------------------------------------------------------------------
    def _poll_reload(self) -> None:
        now = time.monotonic()
        if now < self._next_poll:
            return
        self._next_poll = now + self.reload_poll_ms / 1000.0
        found = self.discover()
        for name, entry in list(self.mods.items()):
            if name not in found:
                self.log("INFO", f"mod {name}: file removed, unloading")
                self._unload(entry)
                del self.mods[name]
        for name, path in found.items():
            entry = self.mods.get(name)
            try:
                stamp = _stamp(path)
            except OSError:
                continue
            if entry is None:
                self.log("INFO", f"mod {name}: new file, loading")
                self._load(name, path)
            elif stamp != entry.stamp:
                self.log("INFO", f"mod {name}: file changed, reloading")
                self._unload(entry)
                self._load(name, path)

    def _unload(self, entry: ModEntry) -> None:
        if entry.state == LOADED:
            entry.state = STOPPED
            self._run_stop(entry)
        sys.modules.pop(entry.module_name, None)
        entry.module = None

    # -- stop event and unload ------------------------------------------------------------------------------
    def stop(self) -> int:
        """The plugin's stop event: every loaded mod's stop handlers, once. Returns the failure count."""
        if self.stopped:
            return 0
        self.stopped = True
        failures = 0
        n = 0
        for entry in self.mods.values():
            if entry.state != LOADED:
                continue
            entry.state = STOPPED
            n += 1
            if self.paused:
                self._cancel(entry)  # natives are refused: the handlers could only fail
            else:
                failures += self._run_stop(entry)
        how = "scripts cancelled without stop handlers (guard closed)" if self.paused else f"{failures} cleanup failure(s)"
        self.log("INFO", f"stop event: {n} mod(s) stopped; {how}")
        return failures

    def shutdown(self) -> None:
        """Unload: raise the stop event if it was not raised, then drop every mod module."""
        self.stop()
        for entry in self.mods.values():
            sys.modules.pop(entry.module_name, None)
            entry.module = None
        self.mods.clear()
        gc.collect()
        self.log("INFO", "Python host shut down (interpreter stays initialised; see python_host.md)")

    def status(self) -> dict[str, str]:
        return {name: e.state for name, e in self.mods.items()}


def install_stdio(g6, source: str = "stdout") -> None:
    sys.stdout = LogStream(g6, 1, source)
    sys.stderr = LogStream(g6, 2, "stderr")


def boot(mods_dir: str, allow_unverified: bool = False, hot_reload: bool = False, reload_poll_ms: int = 1000,
         load_budget_ms: int = 0, g6=None) -> ModHost:
    """Called once by g6_python.dll (first plugin tick, script thread, GIL held). Returns the host whose
    ``tick`` / ``stop`` / ``shutdown`` the plugin calls."""
    if g6 is None:
        g6 = importlib.import_module("_g6")
    install_stdio(g6)
    host = ModHost(mods_dir, g6, allow_unverified, hot_reload, reload_poll_ms, load_budget_ms)
    host.log("INFO", f"Python {sys.version.split()[0]} (isolated={sys.flags.isolated}, no_site={sys.flags.no_site}); "
                     f"mods {host.mods_dir}; allow_unverified={int(host.rt.allow_unverified)}; "
                     f"hot_reload={int(host.hot_reload)}")
    host.load_all()
    return host
