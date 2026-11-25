"""RDS5 container (``.ttpf`` files under data/ttdb), header level.

From the loader rds5.open @ 0x140f435f0 (findings/00 §2.7):

    +0x00 'RDS5'
    +0x04 u32  record_count N
    +0x08 u64  low 40 bits: total (decompressed/virtual?) size; high 24 bits: flags (0x16 here)
    +0x10 N × u64 records             semantics unknown (offset/size/flags packing)
    +0x10+8N   16-byte trailer        hash/GUID? unknown
    header region = align_up(8N + 0x1f, 0x1000); payload follows

A file without the magic is treated by the game as raw/headerless.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"RDS5"
PAGE = 0x1000


@dataclass(frozen=True)
class Rds5Header:
    record_count: int
    total_size: int
    size_flags: int
    records: tuple[int, ...]
    trailer: bytes
    header_region_size: int

    def to_dict(self, max_records: int = 8) -> dict:
        return {
            "record_count": self.record_count,
            "total_size": self.total_size,
            "size_flags": hex(self.size_flags),
            "header_region_size": self.header_region_size,
            "trailer": self.trailer.hex(),
            "records_head": [hex(r) for r in self.records[:max_records]],
        }


def header_region_size(record_count: int) -> int:
    """align_up(8N + 0x1f, 0x1000), as computed by rds5.open."""
    return (record_count * 8 + 0x1F + PAGE - 1) & ~(PAGE - 1)


def read_rds5_header(path: str, max_records: int | None = None) -> Rds5Header | None:
    """Parse the header; returns None for headerless (raw) files."""
    with open(path, "rb") as fh:
        head = fh.read(16)
        if len(head) < 16 or head[:4] != MAGIC:
            return None
        n = struct.unpack_from("<I", head, 4)[0]
        size_word = struct.unpack_from("<Q", head, 8)[0]
        want = n if max_records is None else min(n, max_records)
        recs = struct.unpack(f"<{want}Q", fh.read(8 * want)) if want else ()
        fh.seek(0x10 + 8 * n)
        trailer = fh.read(16)
    return Rds5Header(
        record_count=n,
        total_size=size_word & ((1 << 40) - 1),
        size_flags=size_word >> 56,  # top byte (0x16 in this build); bits 40..55 were zero in every observed file
        records=tuple(recs),
        trailer=trailer,
        header_region_size=header_region_size(n),
    )
