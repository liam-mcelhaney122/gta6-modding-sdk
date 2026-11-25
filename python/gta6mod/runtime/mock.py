"""Offline mock backend: a tiny fake world so mods can run and be tested now.

It implements the seed natives from docs (GTA V semantics) on an in-memory
world: one player ped, spawnable vehicles, models that "load" after a few
frames, and a game clock driven by ``step()``. It is not a simulation of
GTA VI. It exists to exercise the binding/marshalling/scheduling stack and
to give mod authors something to unit-test against.
"""
from __future__ import annotations

import ctypes
from dataclasses import dataclass, field
from typing import Callable

from ..joaat import joaat
from ..types import Vector3
from .backend import G6_E_UNKNOWN_NATIVE, Backend, BuildInfo, NativeError
from .marshal import (Vec3Slots, float_slot, slot_float, slot_int, slot_str, slots_entity,
                      vec3_packed_slots)

Handler = Callable[["MockBackend", list[int], list[int]], None]


@dataclass
class MockEntity:
    handle: int
    kind: str
    model: int
    pos: Vector3
    heading: float = 0.0
    health: int = 200


@dataclass
class MockWorld:
    entities: dict[int, MockEntity] = field(default_factory=dict)
    next_handle: int = 0x100
    loaded_models: dict[int, int] = field(default_factory=dict)  # model -> ms when loaded
    controls_just_pressed: set[tuple[int, int]] = field(default_factory=set)

    def spawn(self, kind: str, model: int, pos: Vector3, heading: float = 0.0) -> int:
        h = self.next_handle
        self.next_handle += 1
        self.entities[h] = MockEntity(h, kind, model, pos, heading)
        return h


class MockBackend(Backend):
    accepts_unverified = True

    def __init__(self, model_load_ms: int = 50):
        self.world = MockWorld()
        self.clock_ms = 0
        self.model_load_ms = model_load_ms
        self.calls: list[tuple[int, list[int]]] = []
        self.player_ped = self.world.spawn("ped", joaat("player_zero"), Vector3(-1000.0, -2000.0, 15.0))
        self.handlers: dict[int, Handler] = {}
        self._install_defaults()

    # -- Backend ---------------------------------------------------------------
    def invoke(self, native_hash: int, slots: list[int]) -> list[int]:
        self.calls.append((native_hash, list(slots)))
        h = self.handlers.get(native_hash)
        if h is None:
            raise NativeError(G6_E_UNKNOWN_NATIVE, f"0x{native_hash:016X}", "no mock implementation")
        ret = [0, 0, 0, 0]
        h(self, slots, ret)
        return ret

    def build_info(self) -> BuildInfo:
        return BuildInfo("1.0.2.1", "mock", "0" * 64, len(self.handlers), 0)

    def now_ms(self) -> int:
        return self.clock_ms

    # -- test controls ---------------------------------------------------------
    def register(self, native_hash: int, handler: Handler) -> None:
        self.handlers[native_hash] = handler

    def step(self, elapsed_ms: int) -> None:
        self.clock_ms += elapsed_ms
        self.world.controls_just_pressed.clear()

    def press_control(self, pad: int, action: int) -> None:
        self.world.controls_just_pressed.add((pad, action))

    # -- default fake natives (GTA V semantics; hashes from the seed table) ----
    def _install_defaults(self) -> None:
        w = self.world

        # GTA VI ABI: entity args/returns are 16-byte handles (2 slots); Vector3 returns
        # are one packed 16-byte vector (findings/phase5/23_handle_abi.md).
        def ent(slots, i=0):
            return w.entities.get(slots_entity(slots[i], slots[i + 1]))

        def put_vec(ret, v: Vector3):
            ret[0], ret[1] = vec3_packed_slots(v)

        def player_ped_id(b, s, r): r[0] = b.player_ped
        def player_id(b, s, r): r[0] = 0
        def game_timer(b, s, r): r[0] = b.clock_ms & 0xFFFFFFFF
        def get_hash_key(b, s, r): r[0] = joaat(slot_str(s[0]) or "")
        def does_exist(b, s, r): r[0] = 1 if ent(s) else 0

        def get_coords(b, s, r):
            e = ent(s)
            put_vec(r, e.pos if e else Vector3())

        def set_coords(b, s, r):
            e = ent(s)
            if e:
                e.pos = Vector3(slot_float(s[2]), slot_float(s[3]), slot_float(s[4]))

        def get_heading(b, s, r):
            e = ent(s)
            r[0] = float_slot(e.heading if e else 0.0)

        def get_health(b, s, r):
            e = ent(s)
            r[0] = e.health if e else 0

        def request_model(b, s, r):
            m = s[0] & 0xFFFFFFFF
            w.loaded_models.setdefault(m, b.clock_ms + b.model_load_ms)

        def has_model_loaded(b, s, r):
            t = w.loaded_models.get(s[0] & 0xFFFFFFFF)
            r[0] = 1 if t is not None and b.clock_ms >= t else 0

        def release_model(b, s, r):
            pass

        def create_vehicle(b, s, r):
            m = s[0] & 0xFFFFFFFF
            t = w.loaded_models.get(m)
            if t is None or b.clock_ms < t:
                r[0] = 0  # game returns 0 when the model is not loaded
                return
            pos = Vector3(slot_float(s[1]), slot_float(s[2]), slot_float(s[3]))
            r[0] = w.spawn("vehicle", m, pos, slot_float(s[4]))

        def control_just_pressed(b, s, r):
            r[0] = 1 if (slot_int(s[0]), slot_int(s[1])) in w.controls_just_pressed else 0

        def wait(b, s, r):
            pass  # scripts use the scheduler's wait(); the native is a no-op here

        for h, fn in {
            0x4EDE34FBADD967A6: wait,
            0xD24D37CC275948CC: get_hash_key,
            0x9CD27B0045628463: game_timer,
            0xD80958FC74E988A6: player_ped_id,
            0x4F8644AF03D0E0D6: player_id,
            0x7239B21A38F536BA: does_exist,
            0x3FEF770D40960D5A: get_coords,
            0x06843DA7060A026B: set_coords,
            0xE83D4F9BA2A38914: get_heading,
            0xEEF059FAD016D209: get_health,
            0x963D27A58DF860AC: request_model,
            0x98A4EB5D89A0C952: has_model_loaded,
            0xE532F5D78798DAAB: release_model,
            0xAF35D0D2583051B0: create_vehicle,
            0x580417101DDB492F: control_just_pressed,
        }.items():
            self.register(h, fn)


def write_vec3_out(address: int, v: Vector3) -> None:
    """Helper for mock handlers that implement Vector3* out-params."""
    s = Vec3Slots.from_address(address)
    s.x, s.y, s.z = v.x, v.y, v.z


def write_int_out(address: int, value: int) -> None:
    ctypes.c_int64.from_address(address).value = value
