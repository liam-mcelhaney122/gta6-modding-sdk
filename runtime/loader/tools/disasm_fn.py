"""Read-only disassembly of game_scarlett_master.exe at given VAs (capstone + pefile).

Used to establish the clock-block and mods/ overlay ABI facts for sdk/runtime/loader.
It only reads the exe FILE; it never starts, attaches to or writes anything.

    python -I disasm_fn.py --exe <dump>\\game_scarlett_master.exe 0x143e85810 [0x1412c5670:200] ...

Each argument is VA[:max_instructions]. Disassembly stops at the first ret/int3 after
max_instructions/4 instructions, or at max_instructions. `--follow` also disassembles the
targets of direct calls/jmps found in the listed functions (one level).
"""
import argparse
import sys

import capstone
import pefile


def load(exe):
    pe = pefile.PE(exe, fast_load=True)
    return pe, pe.OPTIONAL_HEADER.ImageBase


def read_va(pe, base, va, n):
    rva = va - base
    try:
        return pe.get_data(rva, n)
    except pefile.PEFormatError:
        return b""


def section_of(pe, base, va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.Name.rstrip(b"\0").decode(errors="replace"), s.Characteristics
    return None, 0


def disasm(pe, base, md, va, limit, out_targets):
    code = read_va(pe, base, va, limit * 15)
    name, ch = section_of(pe, base, va)
    print(f"== 0x{va:x} ({name}, characteristics 0x{ch:08x})")
    count = 0
    for ins in md.disasm(code, va):
        extra = ""
        if ins.op_str and "rip" in ins.op_str:
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
                    sn, sc = section_of(pe, base, tgt)
                    extra = f"    ; rip -> 0x{tgt:x} ({sn}, 0x{sc:08x})"
        if ins.mnemonic in ("call", "jmp") and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
            out_targets.append(ins.operands[0].imm)
        print(f"  0x{ins.address:x}: {ins.bytes.hex(' '):<45} {ins.mnemonic} {ins.op_str}{extra}")
        count += 1
        if count >= limit:
            break
        if ins.mnemonic in ("ret", "int3") and count >= limit // 4:
            break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--follow", action="store_true")
    ap.add_argument("vas", nargs="+")
    a = ap.parse_args()
    pe, base = load(a.exe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    seen = set()
    for spec in a.vas:
        va_s, _, lim_s = spec.partition(":")
        va = int(va_s, 16)
        lim = int(lim_s) if lim_s else 60
        targets = []
        disasm(pe, base, md, va, lim, targets)
        seen.add(va)
        if a.follow:
            for t in targets:
                if t not in seen:
                    seen.add(t)
                    disasm(pe, base, md, t, lim, [])
    return 0


if __name__ == "__main__":
    sys.exit(main())
