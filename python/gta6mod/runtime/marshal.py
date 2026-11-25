"""Pack Python values into native-call slots and back (docs/native-signatures.md)."""
from __future__ import annotations

import ctypes
import struct
from dataclasses import dataclass, field

from ..joaat import joaat
from ..types import Vector3

MAX_ARGS = 32
MAX_RETS = 4


def _f2u(f: float) -> int:
    return struct.unpack("<I", struct.pack("<f", float(f)))[0]


def _u2f(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


MASK64 = 0xFFFFFFFFFFFFFFFF
OUT_CODES = "IFBVE"


def entity_slots(handle: int) -> list[int]:
    """A GTA VI entity handle is a 128-bit opaque key: Python int -> [lo, hi] slots."""
    handle = int(handle)
    return [handle & MASK64, (handle >> 64) & MASK64]


def slots_entity(lo: int, hi: int) -> int:
    return (lo & MASK64) | ((hi & MASK64) << 64)


def vec3_packed_slots(v) -> list[int]:
    """Packed 16-byte Vector3 return {x,y,z,w}: x,y in slot 0, z in the low half of slot 1."""
    x, y, z = (v.x, v.y, v.z) if isinstance(v, Vector3) else tuple(v)
    return [_f2u(x) | (_f2u(y) << 32), _f2u(z)]


def slots_vec3_packed(r0: int, r1: int) -> Vector3:
    return Vector3(_u2f(r0), _u2f(r0 >> 32), _u2f(r1))


class EntitySlot(ctypes.Structure):
    """Matches g6_entity (out-param buffer for Entity*/Ped*/... natives)."""
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class Vec3Slots(ctypes.Structure):
    """Matches g6_vec3_slots: x, y, z each padded to 8 bytes."""
    _fields_ = [("x", ctypes.c_float), ("_px", ctypes.c_uint32),
                ("y", ctypes.c_float), ("_py", ctypes.c_uint32),
                ("z", ctypes.c_float), ("_pz", ctypes.c_uint32)]


@dataclass
class PackedCall:
    slots: list[int]
    out_params: list[tuple[str, object]] = field(default_factory=list)  # (code, ctypes buffer)
    keepalive: list[object] = field(default_factory=list)


def parse_signature(sig: str) -> tuple[str, str]:
    if ">" not in sig:
        raise ValueError(f"bad signature {sig!r}")
    args, ret = sig.split(">", 1)
    return args, ret


def pack(sig: str, values) -> PackedCall:
    arg_codes, _ = parse_signature(sig)
    values = list(values)
    expected = sum(1 for c in arg_codes if c not in OUT_CODES)  # out-params are not passed by the caller
    if len(values) != expected:
        raise TypeError(f"expected {expected} arguments, got {len(values)}")
    call = PackedCall(slots=[])
    it = iter(values)
    for code in arg_codes:
        if code == "i":
            call.slots.append(int(next(it)) & 0xFFFFFFFFFFFFFFFF)
        elif code == "u":
            v = next(it)
            call.slots.append((joaat(v) if isinstance(v, str) else int(v)) & 0xFFFFFFFF)
        elif code == "q":
            call.slots.append(int(next(it)) & 0xFFFFFFFFFFFFFFFF)
        elif code == "e":
            call.slots += entity_slots(next(it))
        elif code == "E":
            buf = EntitySlot()
            call.out_params.append((code, buf))
            call.slots.append(ctypes.addressof(buf))
        elif code == "f":
            call.slots.append(_f2u(next(it)))
        elif code == "b":
            call.slots.append(1 if next(it) else 0)
        elif code == "s":
            v = next(it)
            if v is None:
                call.slots.append(0)
            else:
                buf = ctypes.create_string_buffer(str(v).encode("utf-8"))
                call.keepalive.append(buf)
                call.slots.append(ctypes.addressof(buf))
        elif code == "v":
            v = next(it)
            x, y, z = (v.x, v.y, v.z) if isinstance(v, Vector3) else tuple(v)
            call.slots += [_f2u(x), _f2u(y), _f2u(z)]
        elif code in "IFB":
            buf = ctypes.c_int64(0)
            call.out_params.append((code, buf))
            call.slots.append(ctypes.addressof(buf))
        elif code == "V":
            buf = Vec3Slots()
            call.out_params.append((code, buf))
            call.slots.append(ctypes.addressof(buf))
        else:
            raise ValueError(f"unknown signature code {code!r} in {sig!r}")
    if len(call.slots) > MAX_ARGS:
        raise ValueError("too many argument slots")
    return call


def unpack(sig: str, ret_slots: list[int], packed: PackedCall):
    _, ret_code = parse_signature(sig)
    if ret_code == "n":
        result = None
    elif ret_code == "i":
        r = ret_slots[0] & 0xFFFFFFFF
        result = r - 0x100000000 if r & 0x80000000 else r
    elif ret_code == "u":
        result = ret_slots[0] & 0xFFFFFFFF
    elif ret_code == "q":
        result = ret_slots[0] & 0xFFFFFFFFFFFFFFFF
    elif ret_code == "f":
        result = _u2f(ret_slots[0])
    elif ret_code == "b":
        result = bool(ret_slots[0] & 0xFF)
    elif ret_code == "s":
        result = ctypes.string_at(ret_slots[0]).decode("utf-8", "replace") if ret_slots[0] else None
    elif ret_code == "v":
        result = Vector3(_u2f(ret_slots[0]), _u2f(ret_slots[1]), _u2f(ret_slots[2]))
    elif ret_code == "w":
        result = slots_vec3_packed(ret_slots[0], ret_slots[1])
    elif ret_code == "e":
        result = slots_entity(ret_slots[0], ret_slots[1])
    else:
        raise ValueError(f"unknown return code {ret_code!r}")
    if not packed.out_params:
        return result
    outs = []
    for code, buf in packed.out_params:
        if code == "I":
            v = buf.value & 0xFFFFFFFF
            outs.append(v - 0x100000000 if v & 0x80000000 else v)
        elif code == "F":
            outs.append(_u2f(buf.value))
        elif code == "B":
            outs.append(bool(buf.value & 0xFF))
        elif code == "V":
            outs.append(Vector3(buf.x, buf.y, buf.z))
        elif code == "E":
            outs.append(slots_entity(buf.lo, buf.hi))
    return tuple(([] if ret_code == "n" else [result]) + outs)


# helpers for backends implementing natives (mock handlers) ---------------------
def slot_int(s: int) -> int:
    s &= 0xFFFFFFFF
    return s - 0x100000000 if s & 0x80000000 else s


def slot_float(s: int) -> float:
    return _u2f(s)


def slot_str(s: int) -> str | None:
    return ctypes.string_at(s).decode("utf-8", "replace") if s else None


def float_slot(f: float) -> int:
    return _f2u(f)
