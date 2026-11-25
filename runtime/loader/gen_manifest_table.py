"""Generate src/manifest_<build>.inc for gta6mod_runtime from the address manifest.

Inputs (all read-only):
  --manifest  sdk/data/generated/address_manifest.<build>.json  (required)
              build, exe SHA-256, TimeDateStamp, entry RVA, section table, export list,
              imphash, anchor RVAs, native hash -> handler RVA.
  --exe       the original game_scarlett_master.exe            (optional)
              only for the values the manifest does not carry: SizeOfImage, SizeOfHeaders
              and the ORIGINAL import directory RVA/size (the host redirects the mapped
              image's import directory, so the runtime reads the original descriptors).
              The script refuses an exe whose SHA-256 differs from the manifest.
  pefile is used only to cross-check our imphash re-implementation and to take the
  ws2_32 ordinal->name table that pefile's imphash uses.

Usage (from the project root):
  python -I sdk/runtime/loader/gen_manifest_table.py \
      --manifest sdk/data/generated/address_manifest.1.0.2.1.json \
      --exe "%G6_DUMP%\\game_scarlett_master.exe"
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


class RawPe:
    """Minimal PE32+ reader over the file bytes (rva -> file offset through the section table)."""

    def __init__(self, data: bytes):
        self.d = data
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0"
        self.fh = e_lfanew + 4
        self.machine, self.nsec = struct.unpack_from("<HH", data, self.fh)
        self.timestamp = struct.unpack_from("<I", data, self.fh + 4)[0]
        opt_size = struct.unpack_from("<H", data, self.fh + 16)[0]
        self.opt = self.fh + 20
        assert struct.unpack_from("<H", data, self.opt)[0] == 0x20B
        self.size_of_image, self.size_of_headers = struct.unpack_from("<II", data, self.opt + 56)
        self.dirs = [struct.unpack_from("<II", data, self.opt + 112 + 8 * i) for i in range(16)]
        st = self.opt + opt_size
        self.sections = []
        for i in range(self.nsec):
            o = st + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode("ascii")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((name, va, vsize, raw, rsize))

    def off(self, rva: int) -> int:
        for _, va, vsize, raw, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return raw + (rva - va)
        if rva < self.size_of_headers:
            return rva
        raise ValueError(hex(rva))

    def u32(self, rva: int) -> int:
        return struct.unpack_from("<I", self.d, self.off(rva))[0]

    def u64(self, rva: int) -> int:
        return struct.unpack_from("<Q", self.d, self.off(rva))[0]

    def cstr(self, rva: int) -> bytes:
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)]


def imphash_like_pefile(pe: RawPe, import_rva: int, ordnames: dict) -> tuple[str, list]:
    """Same algorithm the runtime implements in C++ (identity.cpp). Returns (md5, ws2_32 ordinals used)."""
    parts, used = [], []
    rva = import_rva
    while True:
        oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", pe.d, pe.off(rva))
        if oft == 0 and name_rva == 0 and ft == 0:
            break
        dll = pe.cstr(name_rva).decode("ascii")
        lib = dll.lower()
        p = lib.rsplit(".", 1)
        if len(p) > 1 and p[1] in ("ocx", "sys", "dll"):
            lib = p[0]
        t = oft if oft else ft
        while True:
            v = pe.u64(t)
            if v == 0:
                break
            if v & (1 << 63):
                ordinal = v & 0xFFFF
                fn = ordnames.get((dll.lower(), ordinal))
                if fn is None:
                    fn = "ord%d" % ordinal
                else:
                    used.append((dll.lower(), ordinal, fn))
            else:
                fn = pe.cstr((v & 0x7FFFFFFF) + 2).decode("ascii")
            parts.append("%s.%s" % (lib, fn.lower()))
            t += 8
        rva += 20
    return hashlib.md5(",".join(parts).encode()).hexdigest(), used


def c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--exe")
    ap.add_argument("--out")
    a = ap.parse_args()

    with open(a.manifest, "r", encoding="utf-8") as f:
        m = json.load(f)
    build = m["game_build"]
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "src",
                                "manifest_%s.inc" % build.replace(".", "_"))
    manifest_sha = sha256_file(a.manifest)

    size_of_image = size_of_headers = import_rva = import_size = 0
    ordinal_rows: list = []
    exe_note = "not given: SizeOfImage/SizeOfHeaders/import directory left UNSET (0)"
    if a.exe:
        exe_sha = sha256_file(a.exe)
        if exe_sha != m["exe_sha256"]:
            print("exe SHA-256 %s != manifest %s; refusing" % (exe_sha, m["exe_sha256"]), file=sys.stderr)
            return 2
        with open(a.exe, "rb") as f:
            pe = RawPe(f.read())
        if pe.timestamp != m["image"]["timestamp"]:
            print("timestamp mismatch", file=sys.stderr)
            return 2
        size_of_image, size_of_headers = pe.size_of_image, pe.size_of_headers
        import_rva, import_size = pe.dirs[1]
        import pefile  # noqa: PLC0415  (only for the ordinal table and the cross-check)
        from pefile import ordlookup  # noqa: PLC0415
        ordnames = {}
        for libname, table in ordlookup.ords.items():
            ln = libname.decode() if isinstance(libname, bytes) else libname
            for k, v in table.items():
                ordnames[(ln.lower(), k)] = v.decode() if isinstance(v, bytes) else v
        ih, used = imphash_like_pefile(pe, import_rva, ordnames)
        ref = pefile.PE(a.exe, fast_load=False).get_imphash()
        if ih != ref or ih != m["rea"]["imports"]["imphash_md5"]:
            print("imphash re-implementation %s != pefile %s / manifest %s" % (ih, ref, m["rea"]["imports"]["imphash_md5"]),
                  file=sys.stderr)
            return 3
        ordinal_rows = sorted(set(used))
        exe_note = "exe %s (SHA-256 matches the manifest); imphash re-implementation == pefile == manifest" % os.path.basename(a.exe)

    lines = []
    w = lines.append
    w("/*")
    w(" * GENERATED by sdk/runtime/loader/gen_manifest_table.py. Do not edit.")
    w(" * Provenance:")
    w(" *   %s (generated_on %s), file SHA-256 %s" % (os.path.basename(a.manifest), m.get("generated_on", "?"), manifest_sha))
    w(" *   %s" % exe_note)
    w(" * All addresses are RVAs (image base 0x%x in the file; the live image is relocated)." % int(m["image"]["image_base"], 16))
    w(" */")
    w("#pragma once")
    w("#include <cstdint>")
    w("namespace g6rt::manifest {")
    w("inline constexpr char kGameBuild[] = %s;" % c_str(build))
    w("inline constexpr char kExeSha256Hex[] = %s;" % c_str(m["exe_sha256"]))
    w("inline constexpr char kImphashMd5Hex[] = %s;" % c_str(m["rea"]["imports"]["imphash_md5"]))
    w("inline constexpr std::uint16_t kMachine = 0x%x;" % int(m["image"]["machine"], 16))
    w("inline constexpr std::uint32_t kTimeDateStamp = %du;" % m["image"]["timestamp"])
    w("inline constexpr std::uint32_t kEntryRva = 0x%xu; /* the host zeroes the entry point: informational */" % int(m["image"]["entry_rva"], 16))
    w("inline constexpr std::uint32_t kSizeOfImage = 0x%xu;   /* 0 = unset (no --exe) */" % size_of_image)
    w("inline constexpr std::uint32_t kSizeOfHeaders = 0x%xu; /* 0 = unset */" % size_of_headers)
    w("inline constexpr std::uint32_t kOrigImportDirRva = 0x%xu;  /* 0 = unset; original, pre-host value */" % import_rva)
    w("inline constexpr std::uint32_t kOrigImportDirSize = 0x%xu;" % import_size)
    w("inline constexpr std::uint32_t kTlsIndexRva = 0x%xu;" % (int(m["rea"]["tls"]["index_va"], 16) - int(m["image"]["image_base"], 16)))
    w("struct SectionPin { const char* name; std::uint32_t rva, vsize, raw_size, characteristics; };")
    w("inline constexpr SectionPin kSections[] = {")
    for s in m["image"]["sections"]:
        w("    { %s, 0x%xu, 0x%xu, 0x%xu, 0x%xu }," % (c_str(s["name"]), int(s["rva"], 16), int(s["vsize"], 16),
                                                  int(s["raw_size"], 16), int(s["characteristics"], 16)))
    w("};")
    w("struct ExportPin { const char* name; std::uint32_t rva; };")
    w("inline constexpr ExportPin kExports[] = {")
    for e in m["rea"]["exports"]:
        w("    { %s, 0x%xu }," % (c_str(e["name"]), int(e["address"], 16) - int(m["image"]["image_base"], 16)))
    w("};")
    w("/* pefile ordlookup names for the ordinal imports of this exe (imphash input). */")
    w("struct OrdinalName { const char* dll_lower; std::uint16_t ordinal; const char* name; };")
    w("inline constexpr OrdinalName kOrdinalNames[] = {")
    for dll, o, fn in ordinal_rows:
        w("    { %s, %d, %s }," % (c_str(dll), o, c_str(fn)))
    if not ordinal_rows:
        w("    { \"\", 0, \"\" },")
    w("};")
    w("struct AnchorRva { const char* name; std::uint32_t rva; };")
    w("inline constexpr AnchorRva kAnchorRvas[] = {  /* sorted by name */")
    for name in sorted(m["patterns"]):
        w("    { %s, 0x%xu }," % (c_str(name), int(m["patterns"][name]["rva"], 16)))
    w("};")
    w("enum NativeFlags : std::uint32_t { kNativeNetwork = 1u };")
    w("struct NativeRow { std::uint64_t hash; std::uint32_t handler_rva; std::uint32_t flags; const char* ns; const char* name; };")
    w("inline constexpr NativeRow kNatives[] = {  /* sorted by hash */")
    rows = sorted(((int(h, 16), v) for h, v in m["natives"].items()), key=lambda t: t[0])
    for h, v in rows:
        ns, name = v.get("namespace", ""), v.get("name", "")
        net = ns.upper() == "NETWORK" or name.upper().lstrip("_").startswith("NETWORK_")
        w("    { 0x%016Xull, 0x%xu, %du, %s, %s }," % (h, int(v["handler_rva"], 16), 1 if net else 0, c_str(ns), c_str(name)))
    w("};")
    w("} /* namespace g6rt::manifest */")
    w("")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s: %d anchors, %d natives, %d ordinal names" % (out, len(m["patterns"]), len(rows), len(ordinal_rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
