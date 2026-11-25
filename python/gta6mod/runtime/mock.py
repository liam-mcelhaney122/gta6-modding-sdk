"""Offline mock backend: a tiny fake world so mods can run and be tested now.

It implements the seed natives from docs (GTA V semantics) and the observed
GTA VI hashes the script layer uses, on an in-memory world: one player ped,
spawnable vehicles, models that "load" after a few frames, wanted level,
waypoint, ground probe, clock, and a game clock driven by ``step()``. The exact
semantics are in sdk/docs/script_layer.md ("Mock world") and are shared with the
Luau runner (luau/host/g6_luau_runner.cpp). It is not a simulation of GTA VI. It
exists to exercise the binding/marshalling/scheduling stack and to give mod
authors something to unit-test against.
"""
from __future__ import annotations

import ctypes
import math
from dataclasses import dataclass, field
from typing import Callable

from ..joaat import joaat
from ..types import Vector3
from .backend import G6_E_UNKNOWN_NATIVE, Backend, BuildInfo, NativeError
from .marshal import (Vec3Packed, float_slot, slot_float, slot_int, slot_str, slots_entity, slots_vec3,
                      vec3_packed_slots)

Handler = Callable[["MockBackend", list[int], list[int]], None]

GROUND_Z = 12.0
COLLISION_LOAD_MS = 100


@dataclass
class MockEntity:
    handle: int
    kind: str
    model: int
    pos: Vector3
    heading: float = 0.0
    health: int = 200
    max_health: int = 200
    invincible: bool = False
    can_ragdoll: bool = True
    frozen: bool = False
    visible: bool = True
    velocity: Vector3 = field(default_factory=Vector3)
    vehicle: int = 0  # peds: handle of the vehicle they are in

    def forward(self) -> Vector3:
        h = math.radians(self.heading)
        return Vector3(-math.sin(h), math.cos(h), 0.0)

    def right(self) -> Vector3:
        h = math.radians(self.heading)
        return Vector3(math.cos(h), math.sin(h), 0.0)


@dataclass
class MockWorld:
    entities: dict[int, MockEntity] = field(default_factory=dict)
    next_handle: int = 0x100
    loaded_models: dict[int, int] = field(default_factory=dict)  # model -> ms when loaded
    controls_just_pressed: set[tuple[int, int]] = field(default_factory=set)
    wanted_level: int = 0
    max_wanted_level: int = 5
    waypoint: Vector3 | None = None
    collision_cells: dict[tuple[int, int], int] = field(default_factory=dict)  # (x, y) -> ms when loaded
    clock_seconds: int = 12 * 3600
    clock_paused: bool = False
    time_scale: float = 1.0

    def spawn(self, kind: str, model: int, pos: Vector3, heading: float = 0.0) -> int:
        h = self.next_handle
        self.next_handle += 1
        hp = 1000 if kind == "vehicle" else 200
        self.entities[h] = MockEntity(h, kind, model, pos, heading, hp, hp)
        return h


class MockBackend(Backend):
    accepts_unverified = True

    def __init__(self, model_load_ms: int = 50, quiet: bool = False):
        self.world = MockWorld()
        self.clock_ms = 0
        self.model_load_ms = model_load_ms
        self.quiet = quiet
        self.logs: list[tuple[str, str, str]] = []  # (level, plugin, message)
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

    def log(self, level: str, plugin: str, message: str) -> None:
        self.logs.append((level, plugin, message))
        if not self.quiet:
            super().log(level, plugin, message)

    # -- test controls ---------------------------------------------------------
    def register(self, native_hash: int, handler: Handler) -> None:
        self.handlers[native_hash] = handler

    def step(self, elapsed_ms: int) -> None:
        self.clock_ms += elapsed_ms
        self.world.controls_just_pressed.clear()

    def press_control(self, pad: int, action: int) -> None:
        self.world.controls_just_pressed.add((pad, action))

    def damage_player(self, amount: int) -> None:
        """Scenario event: hurt the player ped and the vehicle it is in, unless invincible."""
        ped = self.world.entities[self.player_ped]
        for e in (ped, self.world.entities.get(ped.vehicle)):
            if e is not None and not e.invincible:
                e.health = max(0, e.health - int(amount))

    def set_wanted(self, level: int) -> None:
        """Scenario event: a crime. Sets the wanted level without applying the max."""
        self.world.wanted_level = int(level)

    def set_waypoint(self, pos: Vector3 | None) -> None:
        self.world.waypoint = pos

    @property
    def player(self) -> MockEntity:
        return self.world.entities[self.player_ped]

    # -- default fake natives ---------------------------------------------------
    def _install_defaults(self) -> None:
        w = self.world

        # GTA VI ABI: entity args/returns are 16-byte handles (2 slots); Vector3 params ('w') and
        # returns are one packed 16-byte vector in 2 slots; Vector3* out-params are 16-byte aligned
        # Vec3V buffers (findings/phase5/23_handle_abi.md, correction 2026-11-25).
        def ent(slots, i=0):
            return w.entities.get(slots_entity(slots[i], slots[i + 1]))

        def put_vec(ret, v: Vector3):
            ret[0], ret[1] = vec3_packed_slots(v)

        def b_(slot: int) -> bool:
            return bool(slot & 0xFF)

        # -- seed natives (GTA V semantics; hashes from the seed table) -------
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
            if not e:
                return
            e.pos = slots_vec3(s, 2)  # 'ewbbbbf': entity 0-1, packed pos 2-3, four BOOLs 4-7, float 8
            if e.kind == "vehicle":  # peds in the vehicle move with it
                for p in w.entities.values():
                    if p.vehicle == e.handle:
                        p.pos = e.pos

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

        # -- natives used by the script layer (observed GTA VI hashes) --------
        def set_flag(attr: str):
            def fn(b, s, r):
                e = ent(s)
                if e:
                    setattr(e, attr, b_(s[2]))
            return fn

        def set_heading(b, s, r):
            e = ent(s)
            if e:
                e.heading = slot_float(s[2])

        def get_max_health(b, s, r):
            e = ent(s)
            r[0] = e.max_health if e else 0

        def set_health(b, s, r):
            e = ent(s)
            if e:
                e.health = min(max(slot_int(s[2]), 0), e.max_health)

        def is_visible(b, s, r):
            e = ent(s)
            r[0] = 1 if e and e.visible else 0

        def set_velocity(b, s, r):
            e = ent(s)
            if e:
                e.velocity = slots_vec3(s, 2)

        def get_speed(b, s, r):
            e = ent(s)
            r[0] = float_slot(e.velocity.length() if e else 0.0)

        def get_forward(b, s, r):
            e = ent(s)
            put_vec(r, e.forward() if e else Vector3())

        def get_model(b, s, r):
            e = ent(s)
            r[0] = e.model if e else 0

        def is_dead(b, s, r):
            e = ent(s)
            r[0] = 1 if e and e.health <= 0 else 0

        def get_offset_world(b, s, r):
            e = ent(s)
            if not e:
                put_vec(r, Vector3())
                return
            o = slots_vec3(s, 2)
            put_vec(r, e.pos + e.right() * o.x + e.forward() * o.y + Vector3(0.0, 0.0, o.z))

        def is_in_any_vehicle(b, s, r):
            e = ent(s)
            r[0] = 1 if e and e.vehicle in w.entities else 0

        def get_vehicle_using(b, s, r):
            e = ent(s)
            r[0] = e.vehicle if e and e.vehicle in w.entities else 0

        def set_into_vehicle(b, s, r):
            p, v = ent(s, 0), ent(s, 2)
            if p and v:
                p.vehicle = v.handle
                p.pos = v.pos

        def set_fixed(b, s, r):
            e = ent(s)
            if e:
                e.health = e.max_health

        def is_model_valid(b, s, r):
            r[0] = 1 if s[0] & 0xFFFFFFFF else 0

        def cell(x: float, y: float) -> tuple[int, int]:
            # half away from zero, like std::lround in the Luau runner (Python's round() is half-even)
            def r(v: float) -> int:
                return int(math.copysign(math.floor(abs(v) + 0.5), v))
            return r(x), r(y)

        def request_collision(b, s, r):
            p = slots_vec3(s, 0)
            w.collision_cells.setdefault(cell(p.x, p.y), b.clock_ms + COLLISION_LOAD_MS)

        def waypoint_active(b, s, r):
            r[0] = 1 if w.waypoint is not None else 0

        def waypoint_coords(b, s, r):
            put_vec(r, w.waypoint or Vector3())

        def ground_z(b, s, r):  # 'wFV>b': packed coord 0-1, float* 2, Vector3* 3
            p = slots_vec3(s, 0)
            t = w.collision_cells.get(cell(p.x, p.y))
            found = t is not None and b.clock_ms >= t and p.z > GROUND_Z
            write_int_out(s[2], float_slot(GROUND_Z if found else 0.0))
            write_vec3_out(s[3], Vector3(0.0, 0.0, 1.0) if found else Vector3())
            r[0] = 1 if found else 0

        def set_time_scale(b, s, r):
            w.time_scale = slot_float(s[0])

        def clock_part(div: int, mod: int):
            def fn(b, s, r):
                r[0] = (w.clock_seconds // div) % mod
            return fn

        def set_clock_time(b, s, r):
            w.clock_seconds = (slot_int(s[0]) * 3600 + slot_int(s[1]) * 60 + slot_int(s[2])) % 86400

        def add_clock_time(b, s, r):
            delta = slot_int(s[0]) * 3600 + slot_int(s[1]) * 60 + slot_int(s[2])
            w.clock_seconds = (w.clock_seconds + delta) % 86400

        def pause_clock(b, s, r):
            w.clock_paused = b_(s[0])

        def control_false(b, s, r):
            r[0] = 0

        def noop(b, s, r):
            pass

        def wanted_level(b, s, r):
            r[0] = w.wanted_level

        def clear_wanted(b, s, r):
            w.wanted_level = 0

        def max_wanted(b, s, r):
            r[0] = w.max_wanted_level

        def set_max_wanted(b, s, r):
            w.max_wanted_level = slot_int(s[0])
            w.wanted_level = min(w.wanted_level, max(w.max_wanted_level, 0))

        for h, fn in {
            # seed hashes
            0x4EDE34FBADD967A6: wait,
            0xD24D37CC275948CC: get_hash_key,
            0x9CD27B0045628463: game_timer,
            0xD80958FC74E988A6: player_ped_id,
            0x4F8644AF03D0E0D6: player_id,
            0x7239B21A38F536BA: does_exist,
            0x3FEF770D40960D5A: get_coords,
            0x06843DA7060A026B: set_coords,  # also the observed GTA VI hash
            0xE83D4F9BA2A38914: get_heading,
            0xEEF059FAD016D209: get_health,
            0x963D27A58DF860AC: request_model,
            0x98A4EB5D89A0C952: has_model_loaded,
            0xE532F5D78798DAAB: release_model,
            0xAF35D0D2583051B0: create_vehicle,
            0x580417101DDB492F: control_just_pressed,  # also the observed GTA VI hash
            # observed GTA VI hashes (sdk/docs/script_layer.md, "Mock world")
            0x096275889B8E0EE0: player_ped_id,
            0xD42BD6EB2E0F1677: does_exist,
            0xA86D5F069399F44D: get_coords,
            0xC230DD956E2F5507: get_heading,
            0xCF2B9C0645C4651B: set_heading,
            0x82368787EA73C0F7: get_health,
            0x15D757606D170C3C: get_max_health,
            0xAC2767ED8BDFAB15: set_health,
            0xA5C38736C426FCB8: set_flag("invincible"),
            0x7D9EFB7AD6B19754: set_flag("frozen"),
            0x1794B4FCC84D812F: set_flag("visible"),
            0xFFC96ECB7FA404CA: is_visible,
            0x1C99BB7B6E96D16F: set_velocity,
            0xFB6BA510A533DF81: get_speed,
            0x2412D9C05BB09B97: get_forward,
            0xDA76A9F39210D365: get_model,
            0x7D5B1F88E7504BBA: is_dead,
            0x1899F328B0E12848: get_offset_world,
            0xB128377056A54E2A: set_flag("can_ragdoll"),
            0x997ABD671D25CA0B: is_in_any_vehicle,
            0x6094AD011A2EA87D: get_vehicle_using,
            0xF75B0D629E1C063D: set_into_vehicle,
            0x79811282A9D1AE56: set_fixed,
            0xFA28FE3A6246FC30: request_model,
            0x1283B8B89DD5D1B6: has_model_loaded,
            0x4AD96EF928BD4F9A: release_model,
            0x392C8D8E07B70EFC: is_model_valid,
            0x0A3720F162A033C9: request_collision,
            0x202B1BBFC6AB5EE4: waypoint_active,
            0x29B30D07C3F7873B: waypoint_coords,
            0x2A29CA9A6319E6AB: ground_z,
            0x9682AF6050854856: set_time_scale,
            0x4F67E8ECA7D3F667: game_timer,
            0xC82CF208C2B19199: clock_part(3600, 24),
            0x4E162231B823DBBF: clock_part(60, 60),
            0xB6101ABE62B5F080: clock_part(1, 60),
            0x3A52C59FFB2DEED8: set_clock_time,
            0xAB7C251C7701D336: add_clock_time,
            0x4D1A590C92BF377E: pause_clock,
            0xF3A21BCD95725A4A: control_just_pressed,  # IS_CONTROL_PRESSED: a mock press lasts one tick
            0x50F940259D3841E6: control_false,
            0xFE99B66D079CF6BC: noop,
            0xABC532F9098BFD9D: wanted_level,
            0x4E4B996C928C7AA6: clear_wanted,
            0xD04CFAD1E2B7984A: max_wanted,
            0x28A4BD2CEE236E19: set_max_wanted,
        }.items():
            self.register(h, fn)


def write_vec3_out(address: int, v: Vector3) -> None:
    """Helper for mock handlers that implement Vector3* out-params: one 16-byte Vec3V store. The game
    uses vmovaps here, which faults on a misaligned address, so the mock refuses one too."""
    if address % 16:
        raise ValueError(f"Vector3* out-param at 0x{address:X} is not 16-byte aligned")
    s = Vec3Packed.from_address(address)
    s.x, s.y, s.z, s.w = v.x, v.y, v.z, 0.0


def write_int_out(address: int, value: int) -> None:
    ctypes.c_int64.from_address(address).value = value
