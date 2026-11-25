"""The player, the world, the clock and controls."""
from __future__ import annotations

from enum import IntEnum
from typing import Generator

from ..types import Vector3
from . import names as n
from .core import now, wait
from .entities import Ped, Vehicle, _vec


class _Player:
    """The local player."""

    def ped(self) -> Ped:
        return Ped(n.player.ped_id())

    def id(self) -> int:
        """Player index. Uses PLAYER_ID, which is only a GTA V seed hash (unverified) in 1.0.2.1."""
        return n.player.id()

    def position(self) -> Vector3:
        return self.ped().position

    def vehicle(self) -> Vehicle | None:
        return self.ped().vehicle()

    def teleport(self, pos, heading: float | None = None, with_vehicle: bool = True) -> None:
        """Move the player, or the vehicle the player is in when ``with_vehicle`` is true."""
        ped = self.ped()
        target = (ped.vehicle() if with_vehicle else None) or ped
        target.position = _vec(pos)
        if heading is not None:
            target.heading = heading

    def wanted_level(self) -> int:
        return n.player.get_wanted_level(self.id(), 0)

    def clear_wanted(self) -> None:
        n.player.clear_wanted_level(self.id(), 0)

    def max_wanted(self) -> int:
        return n.player.get_max_wanted_level()

    def set_max_wanted(self, level: int) -> None:
        n.player.set_max_wanted_level(int(level))


class _World:
    def waypoint(self) -> Vector3 | None:
        """The map waypoint, or None when none is set. Its z may be 0; probe the ground before using it."""
        if not n.map.is_waypoint_active():
            return None
        return n.map.get_waypoint_coords()

    def request_collision(self, pos) -> None:
        p = _vec(pos)
        n.streaming.request_collision_at_coord(p)

    def ground_z(self, x: float, y: float, probe_z: float = 1000.0) -> float | None:
        """One ground probe straight down from ``probe_z``. None when there is no loaded ground there."""
        found, z, _normal = n.misc.get_ground_z_and_normal_for_3d_coord(Vector3(float(x), float(y), float(probe_z)))
        return z if found else None

    def find_ground(self, x: float, y: float, timeout_ms: int = 1500,
                    probe_z: float = 1000.0) -> Generator[object, None, float | None]:
        """Request collision at (x, y) and probe until ground is found. ``z = yield from world.find_ground(x, y)``."""
        deadline = now() + timeout_ms
        while True:
            self.request_collision(Vector3(x, y, probe_z))
            z = self.ground_z(x, y, probe_z)
            if z is not None or now() > deadline:
                return z
            yield wait(0)

    def set_time_scale(self, scale: float) -> None:
        n.misc.set_time_scale(float(scale))


class _Clock:
    def time(self) -> tuple[int, int, int]:
        return n.clock.get_hours(), n.clock.get_minutes(), n.clock.get_seconds()

    def set_time(self, hours: int, minutes: int = 0, seconds: int = 0) -> None:
        n.clock.set_time(int(hours), int(minutes), int(seconds), 0)

    def add(self, hours: int = 0, minutes: int = 0, seconds: int = 0) -> None:
        n.clock.add_to_clock_time(int(hours), int(minutes), int(seconds))

    def pause(self, on: bool = True) -> None:
        n.clock.pause(bool(on), 0)


class GTA5Control(IntEnum):
    """GTA V control indices (default keyboard key in the comment).

    GTA VI's IS_CONTROL_* natives carry RDR2's signature, where the action is a hash. These values are
    placeholders for config, not GTA VI facts; sdk/docs/script_layer.md says how to find the real ids.
    """
    MULTIPLAYER_INFO = 20      # Z
    SPECIAL_ABILITY_SECONDARY = 29  # B
    DETONATE = 47              # G
    CONTEXT = 51               # E
    CONTEXT_SECONDARY = 52     # Q
    VEH_DUCK = 73              # X
    VEH_HEADLIGHT = 74         # H


class _Controls:
    def just_pressed(self, action, pad: int = 0) -> bool:
        return n.pad.is_control_just_pressed(int(pad), action)

    def pressed(self, action, pad: int = 0) -> bool:
        return n.pad.is_control_pressed(int(pad), action)

    def just_released(self, action, pad: int = 0) -> bool:
        return n.pad.is_control_just_released(int(pad), action)

    def disable(self, action, pad: int = 0) -> None:
        """Disable an action for this frame (call every tick to keep it disabled)."""
        n.pad.disable_control_action(int(pad), action, False)


player = _Player()
world = _World()
clock = _Clock()
controls = _Controls()
