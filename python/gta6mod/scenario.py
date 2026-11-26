"""Offline scenarios: timed mock-world events for ``python -m gta6mod run`` and tests.

Event specs match ``g6_luau_runner`` (sdk/docs/script_layer.md, "Mock world"):

    press   CTL@MS[+HOLD] action CTL pressed on control group 0 at MS and held HOLD ms (default: that tick
                          only), then just released on the first tick at or after MS+HOLD; CTL is an INPUT_*
                          name (hashed with joaat) or a number (decimal or 0x hex script key)
    damage  N@MS          hurt the player ped (and its vehicle) by N unless invincible
    wanted  N@MS          set the wanted level to N (a crime; the max is not applied)
    waypoint X,Y,Z@MS     set the map waypoint
    ui_click LABEL@MS     the next widget labelled LABEL in the UI frame at MS is clicked (mock UI)
    ui_set   LABEL=V@MS   the next widget labelled LABEL takes value V (checkbox 1/0, slider, text, combo, r,g,b,a)
    ui_menu  0|1@MS       hide/show the menu (hidden: only frames registered with always=True run)

``drive`` draws one UI frame after every tick when the backend has a UI that draws on request (the mock UIs).

An event fires on the tick whose time equals MS, so use multiples of the frame time (16 ms).
"""
from __future__ import annotations

from dataclasses import dataclass

from .joaat import joaat
from .types import Vector3

KINDS = ("press", "damage", "wanted", "waypoint", "ui_click", "ui_set", "ui_menu")


@dataclass(frozen=True)
class Event:
    at: int
    kind: str
    value: object
    hold: int = 0  # press: ms the action stays held


def parse(kind: str, spec: str) -> Event:
    if kind not in KINDS:
        raise ValueError(f"unknown event kind {kind!r}")
    if kind.startswith("ui_"):
        value, sep, at = spec.rpartition("@")
        if not sep or not value:
            raise ValueError(f"{kind} needs LABEL@MS: {spec!r}")
        if kind == "ui_set":
            label, eq, v = value.partition("=")
            if not eq or not label:
                raise ValueError(f"ui_set needs LABEL=VALUE@MS: {spec!r}")
            return Event(int(at), kind, (label, v))
        if kind == "ui_menu" and value not in ("0", "1"):
            raise ValueError(f"ui_menu needs 0@MS or 1@MS: {spec!r}")
        return Event(int(at), kind, value)
    value, _, at = spec.partition("@")
    at, plus, hold = at.partition("+")
    if plus and kind != "press":
        raise ValueError(f"only press takes +HOLD: {spec!r}")
    if kind == "waypoint":
        parts = [float(p) for p in value.split(",")]
        if len(parts) != 3:
            raise ValueError(f"waypoint needs X,Y,Z: {spec!r}")
        v: object = Vector3(*parts)
    elif kind == "press":
        v = joaat(value) if value[:1].isalpha() else int(value, 0)
    else:
        v = int(value)
        if kind in ("damage", "wanted") and not 0 <= v <= 0x7FFFFFFF:
            raise ValueError(f"{kind} needs 0 <= N <= 2147483647: {spec!r}")  # same rule as g6_luau_runner
    hold_ms = int(hold) if plus else 0
    if hold_ms < 0:
        raise ValueError(f"press needs HOLD >= 0: {spec!r}")
    return Event(int(at or 0), kind, v, hold_ms)


def apply(backend, ev: Event) -> None:
    if ev.kind == "press":
        backend.press_control(0, ev.value, ev.hold)
    elif ev.kind == "damage":
        backend.damage_player(ev.value)
    elif ev.kind == "wanted":
        backend.set_wanted(ev.value)
    elif ev.kind == "waypoint":
        backend.set_waypoint(ev.value)
    elif ev.kind == "ui_click":
        backend.ui.click(ev.value)
    elif ev.kind == "ui_set":
        backend.ui.set(*ev.value)
    elif ev.kind == "ui_menu":
        backend.ui.set_menu_open(ev.value == "1")


def drive(sched, backend, ms: int, events: list[Event] = (), frame_ms: int = 16) -> int:
    """Tick ``sched`` for ``ms`` of mock time, firing ``events``. Returns the elapsed time."""
    draw = getattr(getattr(backend, "ui", None), "frame", None)  # MockUi / CAbiUi over gta6mod_mock.dll
    if draw is not None and not getattr(backend.ui, "is_mock", True):
        draw = None
    t = 0
    while t < ms:
        for ev in events:
            if ev.at == t:
                apply(backend, ev)
        sched.tick()
        backend.step(frame_ms)
        if draw is not None:
            draw()
        t += frame_ms
    return t
