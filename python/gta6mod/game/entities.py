"""Entity, Ped and Vehicle wrappers, and model streaming.

A wrapper holds the 16-byte script handle exactly as a native returned it (a Python int). It never builds
or changes a handle. A handle whose low 8 bytes are 0 means "no entity", and such a wrapper is falsy.
"""
from __future__ import annotations

import math
from typing import Generator

from ..joaat import joaat
from ..types import Vector3
from . import names as n
from .core import now, wait

MASK64 = 0xFFFFFFFFFFFFFFFF

#: Driver seat for SET_PED_INTO_VEHICLE. Static evidence (findings/phase5/30_script_layer_evidence.md, G-083):
#: the handler maps seat s >= -1 to internal seat s + 1 (-2 = any free seat), and story scripts pass -1 at 197
#: of the 203 sites that seat the player ped. Not yet confirmed in game.
SEAT_DRIVER = -1


def _vec(v) -> Vector3:
    return v if isinstance(v, Vector3) else Vector3(*v)


class Entity:
    """Any script entity. Reads are properties; writes are property setters or ``set_*`` methods."""

    __slots__ = ("handle",)

    def __init__(self, handle: int):
        self.handle = int(handle)

    def __bool__(self) -> bool:
        return (self.handle & MASK64) != 0

    def __eq__(self, other) -> bool:
        return isinstance(other, Entity) and other.handle == self.handle

    def __hash__(self) -> int:
        return hash(self.handle)

    def __repr__(self) -> str:
        return f"{type(self).__name__}(0x{self.handle:x})"

    # -- state ------------------------------------------------------------------
    @property
    def exists(self) -> bool:
        return bool(self) and n.entity.does_exist(self.handle)

    @property
    def position(self) -> Vector3:
        return n.entity.get_coords(self.handle, True, False)

    @position.setter
    def position(self, pos) -> None:
        p = _vec(pos)
        n.entity.set_coords(self.handle, p, False, False, False, False)

    @property
    def heading(self) -> float:
        return n.entity.get_heading(self.handle)

    @heading.setter
    def heading(self, degrees: float) -> None:
        n.entity.set_heading(self.handle, float(degrees))

    @property
    def health(self) -> int:
        return n.entity.get_health(self.handle)

    @health.setter
    def health(self, value: int) -> None:
        n.entity.set_health(self.handle, int(value), 0, 0, 0)  # killer: no entity

    @property
    def max_health(self) -> int:
        return n.entity.get_max_health(self.handle, False)

    @property
    def is_dead(self) -> bool:
        return n.entity.is_dead(self.handle)

    @property
    def speed(self) -> float:
        return n.entity.get_speed(self.handle)

    @property
    def forward(self) -> Vector3:
        return n.entity.get_forward_vector(self.handle)

    @property
    def model(self) -> int:
        return n.entity.get_model(self.handle)

    @property
    def visible(self) -> bool:
        return n.entity.is_visible(self.handle)

    @visible.setter
    def visible(self, on: bool) -> None:
        n.entity.set_visible(self.handle, bool(on))

    # -- actions ----------------------------------------------------------------
    def set_invincible(self, on: bool = True) -> None:
        n.entity.set_invincible(self.handle, bool(on))

    def freeze(self, on: bool = True) -> None:
        n.entity.freeze_position(self.handle, bool(on))

    def set_velocity(self, v) -> None:
        v = _vec(v)
        n.entity.set_velocity(self.handle, v)

    def heal(self) -> None:
        """Set health to max health."""
        self.health = self.max_health

    def offset(self, x: float = 0.0, y: float = 0.0, z: float = 0.0) -> Vector3:
        """World position of a point given in this entity's local frame (x right, y forward, z up)."""
        return n.entity.get_offset_from_entity_in_world_coords(self.handle, Vector3(float(x), float(y), float(z)))

    def distance_to(self, other) -> float:
        a = self.position
        b = other.position if isinstance(other, Entity) else _vec(other)
        return math.dist((a.x, a.y, a.z), (b.x, b.y, b.z))

    # -- lifetime ---------------------------------------------------------------
    def delete(self) -> None:
        """Delete the entity (DELETE_ENTITY). The native takes the handle by pointer and clears it, so this
        wrapper's handle becomes what the game wrote back: 0 (falsy) once it is gone. The game only deletes
        entities this script created; it clears the handle either way."""
        (self.handle,) = n.entity.delete(self.handle)

    def release(self) -> None:
        """Tell the game this script no longer needs the entity (SET_ENTITY_AS_NO_LONGER_NEEDED); the world may
        clean it up later. The game clears the handle (the wrapper turns falsy) only when it accepted it."""
        (self.handle,) = n.entity.set_as_no_longer_needed(self.handle)


class Ped(Entity):
    __slots__ = ()

    def set_can_ragdoll(self, on: bool) -> None:
        n.ped.set_can_ragdoll(self.handle, bool(on))

    def in_vehicle(self) -> bool:
        return n.ped.is_in_any_vehicle(self.handle, False, 0, 0)

    def vehicle(self) -> "Vehicle | None":
        """The vehicle this ped is in, or None."""
        if not self.in_vehicle():
            return None
        v = Vehicle(n.ped.get_vehicle_ped_is_using(self.handle, 0))
        return v if v else None

    def put_into(self, vehicle: "Vehicle", seat: int = SEAT_DRIVER) -> None:
        n.ped.set_into_vehicle(self.handle, vehicle.handle, int(seat))


class Vehicle(Entity):
    __slots__ = ()

    def repair(self) -> None:
        n.vehicle.set_fixed(self.handle)

    @staticmethod
    def spawn(model, position, heading: float = 0.0,
              timeout_ms: int = 2000) -> Generator[object, None, "Vehicle | None"]:
        """Stream the model, create the vehicle, release the model. ``veh = yield from Vehicle.spawn(...)``.

        Returns None when the model does not load in time or the game refuses to create the vehicle.
        """
        h = models.hash_of(model)
        if not (yield from models.request(h, timeout_ms)):
            return None
        p = _vec(position)
        veh = Vehicle(n.vehicle.create(h, p.x, p.y, p.z, float(heading), False, False, False))
        models.release(h)
        return veh if veh else None


class _Models:
    """Model streaming. Models are names (hashed with joaat) or hashes."""

    @staticmethod
    def hash_of(model) -> int:
        return joaat(model) if isinstance(model, str) else int(model) & 0xFFFFFFFF

    def is_valid(self, model) -> bool:
        return n.streaming.is_model_valid(self.hash_of(model))

    def request(self, model, timeout_ms: int = 2000) -> Generator[object, None, bool]:
        """Request a model and wait until it is loaded. ``ok = yield from models.request("adder")``."""
        h = self.hash_of(model)
        n.streaming.request_model(h, False)
        loaded = False
        try:
            deadline = now() + timeout_ms
            while not n.streaming.has_model_loaded(h):
                if now() > deadline:
                    return False
                yield wait(0)
            loaded = True
            return True
        finally:
            if not loaded:  # timed out, or the script was cancelled (Mod.stop closes the generator)
                self.release(h)

    def release(self, model) -> None:
        n.streaming.set_model_as_no_longer_needed(self.hash_of(model))


models = _Models()
