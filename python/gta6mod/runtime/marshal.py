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
# Script objects (G-120, include/gta6mod/script_objects.h): S = script String, T = script text object. The caller
# passes a str; the slot carries a pointer to its UTF-8 bytes and the slot's bit in string_slots / text_slots tells
# the backend to hand the native a script object built from them (None: a null object).
OBJECT_CODES = "ST"
# In/out: the caller passes a value, the native gets a pointer to scratch holding it, and the value read back
# after the call is returned like an out-param. X = 16-byte entity handle (docs/native-signatures.md).
INOUT_CODES = "X"


def entity_slots(handle: int) -> list[int]:
    """A GTA VI entity handle is a 128-bit opaque key: Python int -> [lo, hi] slots."""
    handle = int(handle)
    return [handle & MASK64, (handle >> 64) & MASK64]


def slots_entity(lo: int, hi: int) -> int:
    return (lo & MASK64) | ((hi & MASK64) << 64)


def handle16_slots(handle: int) -> list[int]:
    """A 16-byte non-entity handle ('h', ScenarioPoint): Python int -> [lo, hi] slots, as for entities. Only
    values a native returned are meaningful; the int must fit in 128 bits."""
    handle = int(handle)
    if handle < 0 or handle >> 128:
        raise ValueError(f"16-byte handle out of range: {handle:#x}")
    return [handle & MASK64, (handle >> 64) & MASK64]


def vec3_packed_slots(v) -> list[int]:
    """Packed 16-byte Vector3 {x,y,z,w} ('w' param or return): x,y in slot 0, z in the low half of slot 1."""
    x, y, z = (v.x, v.y, v.z) if isinstance(v, Vector3) else tuple(v)
    return [_f2u(x) | (_f2u(y) << 32), _f2u(z)]


def slots_vec3_packed(r0: int, r1: int) -> Vector3:
    return Vector3(_u2f(r0), _u2f(r0 >> 32), _u2f(r1))


class EntitySlot(ctypes.Structure):
    """Matches g6_entity (out-param buffer for Entity*/Ped*/... natives)."""
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class Vec3Slots(ctypes.Structure):
    """Matches g6_vec3_slots (GTA V scrVector, 8-byte stride). Not the GTA VI out-param layout: kept for
    callers that still name it. Vector3* out-params use Vec3Packed."""
    _fields_ = [("x", ctypes.c_float), ("_px", ctypes.c_uint32),
                ("y", ctypes.c_float), ("_py", ctypes.c_uint32),
                ("z", ctypes.c_float), ("_pz", ctypes.c_uint32)]


class Vec3Packed(ctypes.Structure):
    """Matches g6_vec3_packed: the 16-byte Vec3V a Vector3* out-param receives. The game writes it with
    vmovaps, so the address must be 16-byte aligned: allocate with aligned_vec3_packed()."""
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("z", ctypes.c_float), ("w", ctypes.c_float)]


def aligned_vec3_packed() -> tuple[Vec3Packed, object]:
    """A zeroed Vec3Packed at a 16-byte-aligned address, plus the raw buffer that must be kept alive."""
    raw = (ctypes.c_char * 32)()
    addr = (ctypes.addressof(raw) + 15) & ~15
    return Vec3Packed.from_address(addr), raw


@dataclass
class PackedCall:
    slots: list[int]
    out_params: list[tuple[str, object]] = field(default_factory=list)  # (code, ctypes buffer)
    keepalive: list[object] = field(default_factory=list)
    string_slots: int = 0  # bit i: slots[i] -> script String (g6_native_call.string_slots)
    text_slots: int = 0    # bit i: slots[i] -> script text object (g6_native_call.text_slots)


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
        elif code == "h":
            call.slots += handle16_slots(next(it))
        elif code == "E":
            buf = EntitySlot()
            call.out_params.append((code, buf))
            call.slots.append(ctypes.addressof(buf))
        elif code == "X":
            lo, hi = entity_slots(next(it))
            buf = EntitySlot(lo, hi)
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
        elif code in OBJECT_CODES:
            v = next(it)
            bit = 1 << len(call.slots)
            if code == "S":
                call.string_slots |= bit
            else:
                call.text_slots |= bit
            if v is None:
                call.slots.append(0)
            else:
                if not isinstance(v, str):
                    raise TypeError(f"script {'String' if code == 'S' else 'text'} argument must be str or None, "
                                    f"got {type(v).__name__}")
                buf = ctypes.create_string_buffer(v.encode("utf-8"))
                call.keepalive.append(buf)
                call.slots.append(ctypes.addressof(buf))
        elif code == "w":
            call.slots += vec3_packed_slots(next(it))
        elif code == "v":
            v = next(it)
            x, y, z = (v.x, v.y, v.z) if isinstance(v, Vector3) else tuple(v)
            call.slots += [_f2u(x), _f2u(y), _f2u(z)]
        elif code in "IFB":
            buf = ctypes.c_int64(0)
            call.out_params.append((code, buf))
            call.slots.append(ctypes.addressof(buf))
        elif code == "V":
            buf, raw = aligned_vec3_packed()
            call.keepalive.append(raw)
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
    elif ret_code in ("e", "h"):
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
        elif code in "EX":
            outs.append(slots_entity(buf.lo, buf.hi))
    return tuple(([] if ret_code == "n" else [result]) + outs)


# helpers for backends implementing natives (mock handlers) ---------------------
def slot_int(s: int) -> int:
    s &= 0xFFFFFFFF
    return s - 0x100000000 if s & 0x80000000 else s


def slot_float(s: int) -> float:
    return _u2f(s)


def slots_vec3(slots: list[int], i: int) -> Vector3:
    """A packed Vector3 argument ('w') occupying slots[i] and slots[i + 1] (mock handlers)."""
    return slots_vec3_packed(slots[i], slots[i + 1])


def slots_handle16(slots: list[int], i: int) -> int:
    """A 16-byte non-entity handle argument ('h') occupying slots[i] and slots[i + 1] (mock handlers)."""
    return slots_entity(slots[i], slots[i + 1])


def read_entity_ptr(address: int) -> int:
    """The 16-byte handle an Entity* ('E'/'X') argument points at (mock handlers)."""
    e = EntitySlot.from_address(address)
    return slots_entity(e.lo, e.hi)


def write_entity_ptr(address: int, handle: int) -> None:
    e = EntitySlot.from_address(address)
    e.lo, e.hi = entity_slots(handle)


def slot_str(s: int) -> str | None:
    return ctypes.string_at(s).decode("utf-8", "replace") if s else None


# script objects (G-120): the layouts of include/gta6mod/script_objects.h -----------------------------------
SSTR_LENGTH, SSTR_CAPACITY, SSTR_STRIDE, SSTR_FLAGS = 0x08, 0x0C, 0x10, 0x14
SSTR_Q18, SSTR_ELEM, SSTR_TEXT = 0x18, 0x20, 0x28
SSTR_OUT_OF_LINE = 0x04
SSTR_LITERAL_Q18 = 9
STEXT_NATIVE, STEXT_SIZE = 0x18, 0x20
TEXT_GET_FN = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ubyte)  # virtual +0x18: (this, true)


def read_script_string(addr: int) -> str | None:
    """The text of a script String at ``addr`` the way the game's readers take it (+0x14 bit 2: out of line);
    None for a null object (mock handlers)."""
    if not addr:
        return None
    flags = ctypes.c_uint8.from_address(addr + SSTR_FLAGS).value
    text = ctypes.c_void_p.from_address(addr + SSTR_TEXT).value if flags & SSTR_OUT_OF_LINE else addr + SSTR_TEXT
    n = ctypes.c_int32.from_address(addr + SSTR_LENGTH).value
    return ctypes.string_at(text, n).decode("utf-8", "replace") if text else None


def read_script_text(addr: int) -> str | None:
    """The text of a script text object the way 0x142055010 reads it: null object -> "", otherwise the native
    object at +0x18 and its virtual +0x18 called with (this, true) (mock handlers)."""
    if not addr:
        return ""
    native = ctypes.c_void_p.from_address(addr + STEXT_NATIVE).value
    if not native:
        return None
    vtable = ctypes.c_void_p.from_address(native).value
    fn = TEXT_GET_FN(ctypes.c_void_p.from_address(vtable + 0x18).value)
    p = fn(native, 1)
    return ctypes.string_at(p).decode("utf-8", "replace") if p else None


def _text_get(native, _flag):
    return ctypes.c_void_p.from_address(native + 8).value


class ScriptObjects:
    """Builds the objects a backend passes for string_slots / text_slots, in ctypes memory with the C layouts
    (the pure-Python mock; the C runtimes build their own). Keep the instance alive for the call."""

    _get = TEXT_GET_FN(_text_get)
    _vtable = (ctypes.c_void_p * 4)(0, 0, 0, ctypes.cast(_get, ctypes.c_void_p).value)

    def __init__(self, type_record: int, elem_record: int):
        self.type_record, self.elem_record = type_record, elem_record
        self.keep: list[object] = []

    def string(self, text: bytes) -> int:
        n = len(text)
        buf = ctypes.create_string_buffer(SSTR_TEXT + n + 8)
        a = ctypes.addressof(buf)
        ctypes.c_void_p.from_address(a).value = self.type_record
        ctypes.c_int32.from_address(a + SSTR_LENGTH).value = n
        ctypes.c_uint32.from_address(a + SSTR_CAPACITY).value = n + 1
        ctypes.c_uint32.from_address(a + SSTR_STRIDE).value = 1
        ctypes.c_uint64.from_address(a + SSTR_Q18).value = SSTR_LITERAL_Q18
        ctypes.c_void_p.from_address(a + SSTR_ELEM).value = self.elem_record
        ctypes.memmove(a + SSTR_TEXT, text, n)
        self.keep.append(buf)
        return a

    def text(self, text: bytes) -> int:
        buf = ctypes.create_string_buffer(STEXT_SIZE + 16 + len(text) + 8)
        a = ctypes.addressof(buf)
        native = a + STEXT_SIZE
        ctypes.c_void_p.from_address(a + STEXT_NATIVE).value = native
        ctypes.c_void_p.from_address(native).value = ctypes.addressof(self._vtable)
        ctypes.c_void_p.from_address(native + 8).value = native + 16
        ctypes.memmove(native + 16, text, len(text))
        self.keep.append(buf)
        return a

    def convert(self, slots: list[int], string_slots: int, text_slots: int) -> list[int]:
        """slots with every masked non-null pointer replaced by an object (the pointers are NUL-terminated
        UTF-8, as marshal.pack makes them)."""
        if string_slots & text_slots or (string_slots | text_slots) >> len(slots):
            raise ValueError("bad script object masks")
        out = list(slots)
        for i, v in enumerate(slots):
            if v and (string_slots | text_slots) >> i & 1:
                raw = ctypes.string_at(v)
                out[i] = self.string(raw) if string_slots >> i & 1 else self.text(raw)
        return out


def float_slot(f: float) -> int:
    return _f2u(f)
