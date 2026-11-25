"""Offline scenarios: timed mock-world events for ``python -m gta6mod run`` and tests.

Event specs match ``g6_luau_runner`` (sdk/docs/script_layer.md, "Mock world"):

    press   CTL@MS        control CTL "just pressed" on pad 0 for one tick
    damage  N@MS          hurt the player ped (and its vehicle) by N unless invincible
    wanted  N@MS          set the wanted level to N (a crime; the max is not applied)
    waypoint X,Y,Z@MS     set the map waypoint

An event fires on the tick whose time equals MS, so use multiples of the frame time (16 ms).
"""
from __future__ import annotations

from dataclasses import dataclass

from .types import Vector3

KINDS = ("press", "damage", "wanted", "waypoint")


@dataclass(frozen=True)
class Event:
    at: int
    kind: str
    value: object


def parse(kind: str, spec: str) -> Event:
    if kind not in KINDS:
        raise ValueError(f"unknown event kind {kind!r}")
    value, _, at = spec.partition("@")
    if kind == "waypoint":
        parts = [float(p) for p in value.split(",")]
        if len(parts) != 3:
            raise ValueError(f"waypoint needs X,Y,Z: {spec!r}")
        v: object = Vector3(*parts)
    else:
        v = int(value)
        if kind in ("damage", "wanted") and not 0 <= v <= 0x7FFFFFFF:
            raise ValueError(f"{kind} needs 0 <= N <= 2147483647: {spec!r}")  # same rule as g6_luau_runner
    return Event(int(at or 0), kind, v)


def apply(backend, ev: Event) -> None:
    if ev.kind == "press":
        backend.press_control(0, ev.value)
    elif ev.kind == "damage":
        backend.damage_player(ev.value)
    elif ev.kind == "wanted":
        backend.set_wanted(ev.value)
    elif ev.kind == "waypoint":
        backend.set_waypoint(ev.value)


def drive(sched, backend, ms: int, events: list[Event] = (), frame_ms: int = 16) -> int:
    """Tick ``sched`` for ``ms`` of mock time, firing ``events``. Returns the elapsed time."""
    t = 0
    while t < ms:
        for ev in events:
            if ev.at == t:
                apply(backend, ev)
        sched.tick()
        backend.step(frame_ms)
        t += frame_ms
    return t
