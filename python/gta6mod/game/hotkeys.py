"""Hotkey state machine behind ``Mod``: press, release, hold and chord bindings, and per-tick blocked actions.

Pure logic with no native calls of its own. ``Hotkeys.poll(read, now_ms)`` reads input through
``read(query, action, pad) -> bool`` (query is ``"just_pressed"``, ``"pressed"`` or ``"just_released"``) and
returns the bindings that fire this tick. ``Mod`` passes the ``controls`` helpers; tests pass a fake.

Rules (the same in Luau, ``@gta6/hotkeys``):

- **press** fires on the tick the action is just pressed while every modifier is held.
- **release** fires on the tick it is just released while every modifier is held. With ``max_ms`` it fires
  only when the press that ends here began at most ``max_ms`` earlier (a tap), and only if that press was seen.
- **hold** fires once the action has been held ``ms`` since a press this object saw, while every modifier is
  held, then every ``repeat_ms`` while still held (once if ``repeat_ms`` is None). An action already held
  when the bindings start counts only from its next press.
- **Most specific wins.** When bindings of the same kind on the same action and pad fire on one tick, one
  whose modifiers are a strict subset of another's is dropped (``K`` loses to ``SPRINT+K``). A press that
  fired a chord also suppresses release and hold bindings of that action with fewer modifiers until the
  action is released, so ``SPRINT+K`` does not also count as a tap or hold of ``K``.
- A binding whose handler is still running (``busy``) does not fire; its state still updates.

Actions and modifiers are compared as 32-bit script keys (``Control`` member, int, or ``"INPUT_*"`` name).
"""
from __future__ import annotations

from typing import Callable, Iterable

from ..joaat import joaat

PRESS, RELEASE, HOLD = "press", "release", "hold"

Reader = Callable[[str, int, int], bool]


def action_key(action) -> int:
    """32-bit script key of a ``Control`` member, an int or an ``"INPUT_*"`` name."""
    if isinstance(action, str):
        return joaat(action)
    return int(action) & 0xFFFFFFFF


def modifier_keys(mods) -> frozenset[int]:
    """``None``, one action, or an iterable of actions -> a set of script keys."""
    if mods is None:
        return frozenset()
    if isinstance(mods, (str, int)):
        return frozenset((action_key(mods),))
    return frozenset(action_key(m) for m in mods)


class Hotkey:
    """One binding. ``fn`` is opaque here; ``busy`` is set by the owner while ``fn``'s script runs."""

    __slots__ = ("kind", "action", "pad", "mods", "ms", "repeat_ms", "fn", "busy", "_next_at")

    def __init__(self, kind: str, action, pad: int, mods, fn, ms: int | None = None, repeat_ms: int | None = None):
        if kind not in (PRESS, RELEASE, HOLD):
            raise ValueError(f"unknown hotkey kind {kind!r}")
        if kind == HOLD and (ms is None or ms < 0):
            raise ValueError("a hold binding needs ms >= 0")
        if ms is not None and ms < 0:
            raise ValueError("ms must be >= 0")
        if repeat_ms is not None and repeat_ms <= 0:
            raise ValueError("repeat_ms must be > 0")
        self.kind = kind
        self.action = action_key(action)
        self.pad = int(pad)
        self.mods = modifier_keys(mods)
        if self.action in self.mods:
            raise ValueError("an action cannot be its own modifier")
        self.ms = ms
        self.repeat_ms = repeat_ms
        self.fn = fn
        self.busy = False
        self._next_at: int | None = None  # hold: time of the next fire for the current press


class Hotkeys:
    def __init__(self) -> None:
        self.bindings: list[Hotkey] = []
        self._down_at: dict[tuple[int, int], int] = {}         # (action, pad) -> time of the press we saw
        self._chord: dict[tuple[int, int], frozenset[int]] = {}  # (action, pad) -> mods of the chord it fired

    def add(self, kind: str, action, fn, pad: int = 0, mods=None, ms: int | None = None,
            repeat_ms: int | None = None) -> Hotkey:
        b = Hotkey(kind, action, pad, mods, fn, ms, repeat_ms)
        self.bindings.append(b)
        return b

    def poll(self, read: Reader, now: int) -> list[Hotkey]:
        cache: dict[tuple[str, int, int], bool] = {}

        def q(query: str, action: int, pad: int) -> bool:
            k = (query, action, pad)
            if k not in cache:
                cache[k] = bool(read(query, action, pad))
            return cache[k]

        # actions whose press time or chord matters: release/hold bindings, and chord presses
        tracked = {(b.action, b.pad) for b in self.bindings if b.kind != PRESS or b.mods}
        for key in tracked:
            if q("just_pressed", *key):
                self._down_at[key] = now
                self._chord.pop(key, None)

        fired: list[Hotkey] = []
        for b in self.bindings:
            key = (b.action, b.pad)
            if b.kind == HOLD:
                if not q("pressed", b.action, b.pad) or key not in self._down_at:
                    b._next_at = None
                    continue
                if b._next_at is None:
                    b._next_at = self._down_at[key] + b.ms
                if now < b._next_at or not self._mods_held(q, b):
                    continue
                b._next_at = now + b.repeat_ms if b.repeat_ms else float("inf")
                fired.append(b)
            elif b.kind == PRESS:
                if q("just_pressed", b.action, b.pad) and self._mods_held(q, b):
                    fired.append(b)
            else:  # RELEASE
                if not q("just_released", b.action, b.pad) or not self._mods_held(q, b):
                    continue
                if b.ms is not None:
                    t0 = self._down_at.get(key)
                    if t0 is None or now - t0 > b.ms:
                        continue
                fired.append(b)

        fired = self._most_specific(fired)
        for b in fired:
            if b.kind == PRESS and b.mods:
                key = (b.action, b.pad)
                if len(b.mods) > len(self._chord.get(key, ())):
                    self._chord[key] = b.mods

        for key in tracked:  # after the bindings, so the release tick still sees the press time and chord
            if not q("pressed", *key) and not q("just_pressed", *key):
                self._down_at.pop(key, None)
                self._chord.pop(key, None)
        return [b for b in fired if not b.busy]

    def _mods_held(self, q, b: Hotkey) -> bool:
        return all(q("pressed", m, b.pad) for m in b.mods)

    def _most_specific(self, fired: list[Hotkey]) -> list[Hotkey]:
        out = []
        for b in fired:
            key = (b.action, b.pad)
            if b.kind != PRESS and b.mods < self._chord.get(key, b.mods):
                continue  # this press was consumed by a chord with more modifiers
            if any(o is not b and o.kind == b.kind and (o.action, o.pad) == key and b.mods < o.mods for o in fired):
                continue
            out.append(b)
        return out


class Block:
    """Actions disabled every tick while ``when`` is truthy (a ``Toggle``, a callable, or None = always)."""

    __slots__ = ("actions", "pad", "when")

    def __init__(self, actions: Iterable, pad: int = 0, when=None):
        self.actions = tuple(action_key(a) for a in actions)
        if not self.actions:
            raise ValueError("block needs at least one action")
        self.pad = int(pad)
        self.when = when

    def active(self) -> bool:
        if self.when is None:
            return True
        return bool(self.when()) if callable(self.when) else bool(self.when)
