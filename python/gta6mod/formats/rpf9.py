"""RPF9 archive header (GTA VI).

Layout observed in all 22 archives of the 1.0.2.1 Xbox dump and in the
loader code (header reader @ 0x141fb2600, TOC transform select @ 0x140a348a0):

    +0x00 u32  magic 'RPF9' (on disk 39 46 50 52)
    +0x04 u32  header_region_size        read by the loader (mode 0)
    +0x08 u32  header_region_size_alt    read by the loader (mode != 0); extra
                                         (size - size_alt) bytes read at offset size_alt
    +0x0C u32  field_0c                  unknown (0x40 in appdata0)
    +0x10 u64  archive_id                unique per archive; salt/IV/GUID? unknown
    +0x18 u16  field_18 (0x7a00)         constant in this build
    +0x1A u16  span_param                passed to the TOC transform (u16)
    +0x1C u32  toc_span_end              transform span length = toc_span_end - toc_span_start
    +0x20 u16  field_20                  3..7 (varies per archive), unknown
    +0x22 u16  field_22                  1
    +0x24 u32  entry_count               (appdata0=2, levels=0x81e)
    +0x28 u32  key_id                    4 distinct values across the dump
    +0x2C u8   field_2c                  1 or 2
    +0x2D u8   field_2d                  0x0f (bit0 read by loader)
    +0x2E u8   transform_flags           bit0 = TOC transform enabled; >>1 = codec descriptor id
    +0x2F u8   field_2f
    +0x30 u32  toc_span_start            0x10
    +0x34..0x3F zero
    +0x40 [32] sentinel                  identical in every archive
    +0x60 ...  encrypted/encoded TOC

Every name except magic is provisional; see findings/00 §2.7.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0x52504639  # 'RPF9' little-endian
HEADER_SIZE = 0x40
SENTINEL_OFFSET = 0x40
SENTINEL = bytes.fromhex("5f08daabecbdd40258c3516807024c24fb9f756755ef4be458785bfed8aa6989")

KNOWN_KEY_IDS = {
    0xDA72573E: "key_a (appdata0, common, movies, shaders, story000_bvh4)",
    0xD98004D5: "key_b (anim, levels, navmeshes, packs, core_bvh4, …)",
    0xC0A92754: "key_c (audio, script, story000_script)",
    0xD5C19F6B: "key_d (data, textures, core_restofworld, story000, …)",
}


@dataclass(frozen=True)
class Rpf9Header:
    magic: int
    header_region_size: int
    header_region_size_alt: int
    field_0c: int
    archive_id: int
    field_18: int
    span_param: int
    toc_span_end: int
    field_20: int
    field_22: int
    entry_count: int
    key_id: int
    field_2c: int
    field_2d: int
    transform_flags: int
    field_2f: int
    toc_span_start: int
    sentinel_ok: bool

    @property
    def toc_transform_enabled(self) -> bool:
        return bool(self.transform_flags & 1)

    @property
    def codec_descriptor_id(self) -> int:
        return self.transform_flags >> 1

    @property
    def key_label(self) -> str:
        return KNOWN_KEY_IDS.get(self.key_id, "unknown")

    def to_dict(self) -> dict:
        d = {k: getattr(self, k) for k in self.__dataclass_fields__}
        d["toc_transform_enabled"] = self.toc_transform_enabled
        d["codec_descriptor_id"] = self.codec_descriptor_id
        d["key_label"] = self.key_label
        return d


def parse_rpf9_header(buf: bytes) -> Rpf9Header:
    if len(buf) < HEADER_SIZE:
        raise ValueError("buffer shorter than RPF9 header")
    (magic, hsz, hsz_alt, f0c, arc_id, f18, span_param, span_end,
     f20, f22, count, key_id, f2c, f2d, flags, f2f, span_start) = struct.unpack_from(
        "<IIIIQHHIHHIIBBBBI", buf, 0)
    if magic != MAGIC:
        raise ValueError(f"not an RPF9 archive (magic {magic:#010x})")
    sentinel_ok = buf[SENTINEL_OFFSET:SENTINEL_OFFSET + len(SENTINEL)] == SENTINEL if len(buf) >= 0x60 else False
    return Rpf9Header(magic, hsz, hsz_alt, f0c, arc_id, f18, span_param, span_end, f20, f22,
                      count, key_id, f2c, f2d, flags, f2f, span_start, sentinel_ok)


def read_rpf9_header(path: str) -> Rpf9Header:
    with open(path, "rb") as fh:
        return parse_rpf9_header(fh.read(0x60))


def read_toc_region(path: str, header: Rpf9Header | None = None) -> bytes:
    """Raw (still encoded) header region after the 64-byte header, as the
    loader reads it in mode 0. For analysis/emulation; never decrypted here."""
    hdr = header or read_rpf9_header(path)
    with open(path, "rb") as fh:
        fh.seek(HEADER_SIZE)
        return fh.read(max(0, hdr.header_region_size - HEADER_SIZE))
