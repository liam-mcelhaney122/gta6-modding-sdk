"""Minimal read-only PE32+ reader for the SDK (no third-party deps).

Self-contained on purpose: the SDK must not depend on the analysis tooling in
``tools/`` (owned by the RE workstream), only on its *outputs* in re-db.
"""
from __future__ import annotations

import bisect
import hashlib
import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class Section:
    name: str
    va: int        # RVA
    vsize: int
    raw: int       # file offset
    rsize: int
    characteristics: int

    @property
    def executable(self) -> bool:
        return bool(self.characteristics & 0x20000000)


class PEImage:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE (no MZ)")
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e:e + 4] != b"PE\0\0":
            raise ValueError(f"{path}: bad PE signature")
        self.machine, nsec = struct.unpack_from("<HH", d, e + 4)
        self.timestamp = struct.unpack_from("<I", d, e + 8)[0]
        optsz = struct.unpack_from("<H", d, e + 20)[0]
        opt = e + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise ValueError(f"{path}: not PE32+")
        self.entry_rva = struct.unpack_from("<I", d, opt + 16)[0]
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
        so = opt + optsz
        self.sections: list[Section] = []
        for i in range(nsec):
            name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", d, so + 40 * i)
            ch = struct.unpack_from("<I", d, so + 40 * i + 36)[0]
            self.sections.append(Section(name.rstrip(b"\0").decode("latin-1"), va, vsz, rptr, rsz, ch))
        self._pdata = None
        self._sha256 = None

    # -- identity -------------------------------------------------------------
    @property
    def sha256(self) -> str:
        if self._sha256 is None:
            self._sha256 = hashlib.sha256(self.data).hexdigest()
        return self._sha256

    # -- address translation --------------------------------------------------
    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def rva_to_off(self, rva: int) -> int | None:
        for s in self.sections:
            if s.va <= rva < s.va + s.rsize:
                return s.raw + rva - s.va
        return None

    def off_to_rva(self, off: int) -> int | None:
        for s in self.sections:
            if s.raw <= off < s.raw + s.rsize:
                return s.va + off - s.raw
        return None

    def va_to_off(self, va: int) -> int | None:
        return self.rva_to_off(va - self.image_base)

    def read_rva(self, rva: int, n: int) -> bytes:
        off = self.rva_to_off(rva)
        return b"" if off is None else self.data[off:off + n]

    def code_ranges(self):
        """(file_offset_start, file_offset_end) of executable sections."""
        for s in self.sections:
            if s.executable:
                yield s.raw, s.raw + s.rsize

    # -- functions (.pdata) -----------------------------------------------------
    def functions(self) -> list[tuple[int, int]]:
        """Sorted (begin_rva, end_rva) from the exception directory."""
        if self._pdata is None:
            out = []
            if len(self.dirs) > 3 and self.dirs[3][0]:
                off = self.rva_to_off(self.dirs[3][0])
                for i in range(self.dirs[3][1] // 12):
                    b, en, _ = struct.unpack_from("<III", self.data, off + 12 * i)
                    if en > b:
                        out.append((b, en))
            out.sort()
            self._pdata = out
        return self._pdata

    def function_containing(self, rva: int) -> tuple[int, int] | None:
        funcs = self.functions()
        k = bisect.bisect_right(funcs, (rva, 0xFFFFFFFF)) - 1
        if k >= 0 and funcs[k][0] <= rva < funcs[k][1]:
            return funcs[k]
        return None
