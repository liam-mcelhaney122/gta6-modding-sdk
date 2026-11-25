"""IDA-style byte signatures: parse, scan, and generate unique signatures.

Signature text: space-separated hex bytes, ``?`` or ``??`` for wildcards,
e.g. ``"48 8B 05 ? ? ? ? 48 85 C0 74 ?"``.

Signatures (not raw addresses) are how the runtime finds engine anchors on a
new game build (FiveM ``hook::pattern`` / YimMenu model, research/01).
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from .pe import PEImage


@dataclass(frozen=True)
class Pattern:
    data: bytes
    mask: bytes  # 1 = must match, 0 = wildcard

    @classmethod
    def parse(cls, text: str) -> "Pattern":
        data, mask = bytearray(), bytearray()
        for tok in text.split():
            if tok in ("?", "??"):
                data.append(0)
                mask.append(0)
            else:
                data.append(int(tok, 16))
                mask.append(1)
        if not data or not mask[0] or not mask[-1]:
            raise ValueError("pattern must start and end with a concrete byte")
        return cls(bytes(data), bytes(mask))

    def __str__(self) -> str:
        return " ".join(f"{b:02X}" if m else "?" for b, m in zip(self.data, self.mask))

    def regex(self) -> re.Pattern:
        parts = [re.escape(bytes([b])) if m else b"." for b, m in zip(self.data, self.mask)]
        return re.compile(b"".join(parts), re.DOTALL)


def scan(image: PEImage, pattern: Pattern | str, code_only: bool = True, limit: int | None = None) -> list[int]:
    """Return RVAs of every match (in executable sections by default)."""
    if isinstance(pattern, str):
        pattern = Pattern.parse(pattern)
    rx = pattern.regex()
    ranges = list(image.code_ranges()) if code_only else [(0, len(image.data))]
    hits: list[int] = []
    for lo, hi in ranges:
        for m in rx.finditer(image.data, lo, hi):
            rva = image.off_to_rva(m.start())
            if rva is not None:
                hits.append(rva)
                if limit and len(hits) >= limit:
                    return hits
    return hits


def _instruction_mask(code: bytes, address: int) -> bytes:
    """Mask that wildcards bytes likely to change between builds:
    rel32 branch/call targets, RIP-relative displacements, and large
    immediates/displacements that look like addresses. Uses capstone when
    available; falls back to an opcode heuristic."""
    try:
        from capstone import CS_ARCH_X86, CS_MODE_64, Cs
        from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
    except ImportError:  # pragma: no cover - heuristic path
        return _heuristic_mask(code)

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    mask = bytearray(b"\x01" * len(code))
    pos = 0
    for ins in md.disasm(code, address):
        start = ins.address - address
        disp_off = ins.disp_offset if hasattr(ins, "disp_offset") else 0
        imm_off = ins.imm_offset if hasattr(ins, "imm_offset") else 0
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and disp_off:
                for i in range(start + disp_off, start + disp_off + 4):
                    mask[i] = 0
            elif op.type == X86_OP_IMM and imm_off:
                is_branch = ins.group(1) or ins.group(2) or ins.group(7)  # jump/call/branch_relative
                size = ins.size - imm_off
                if is_branch and size == 4:
                    for i in range(start + imm_off, start + ins.size):
                        mask[i] = 0
        pos = start + ins.size
    if pos < len(code):  # undecodable tail
        for i in range(pos, len(code)):
            mask[i] = 0
    return bytes(mask)


def _heuristic_mask(code: bytes) -> bytes:  # pragma: no cover
    mask = bytearray(b"\x01" * len(code))
    for i, b in enumerate(code):
        if b in (0xE8, 0xE9) and i + 5 <= len(code):
            for j in range(i + 1, i + 5):
                mask[j] = 0
    return bytes(mask)


def _tls_mask(code: bytes, mask: bytes) -> bytes:
    """Wildcard `mov r64, qword gs:[0x58]` (65 48|4C 8B /r 25 58 00 00 00). HLE hosts such as
    ztrx/VIctory rewrite these 9 bytes into a call (findings/live/30_live_menu.md), so signatures
    that pin them only match on console-faithful images."""
    m = bytearray(mask)
    for i in range(len(code) - 8):
        if (code[i] == 0x65 and code[i + 1] in (0x48, 0x4C) and code[i + 2] == 0x8B
                and (code[i + 3] & 0xC7) == 0x04 and code[i + 4:i + 9] == b"\x25\x58\x00\x00\x00"):
            m[i:i + 9] = b"\x00" * 9
    return bytes(m)


def make_signature(image: PEImage, rva: int, min_len: int = 12, max_len: int = 96) -> Pattern | None:
    """Grow a signature from ``rva`` until it matches exactly once in code.
    TLS-base loads are wildcarded so the signature also matches under HLE hosts."""
    code = image.read_rva(rva, max_len)
    if not code:
        return None
    mask = _tls_mask(code, _instruction_mask(code, image.image_base + rva))
    if not mask[0]:
        return None  # a signature cannot start on a rewritten TLS load

    def pat_at(n: int) -> Pattern:
        m = bytearray(mask[:n])
        m[0] = 1
        while n > 1 and not m[n - 1]:  # never end on a wildcard
            n -= 1
        return Pattern(bytes(code[:n]), bytes(m[:n]))

    def unique(n: int) -> bool:
        pat = pat_at(n)
        return sum(pat.mask) >= 8 and scan(image, pat, limit=2) == [rva]

    # Matches only shrink as the pattern grows, so binary-search the shortest unique length.
    lo, hi = min_len, len(code)
    if not unique(hi):
        return None
    while lo < hi:
        mid = (lo + hi) // 2
        if unique(mid):
            hi = mid
        else:
            lo = mid + 1
    return pat_at(lo)


# -- xref-chain anchors --------------------------------------------------------------------------
# An anchor that has no unique signature of its own is resolved from a uniquely signed start point
# through a short list of steps. The cursor is an RVA. The same semantics are compiled into
# patterns.hpp (``resolve_chain``); keep both in step.
#
#   call_target {at}                    cursor = target of the `E8 rel32` at cursor+at (must land in code)
#   rip_target  {at, disp_at, insn_len} cursor = cursor+at + insn_len + int32 [cursor+at+disp_at]
#   add         {value}                 cursor += value
#   deref       {}                      cursor = u64 [cursor] (rebased to an RVA offline)
#   nth_call    {n, max_len}            cursor = target of the n-th (0-based) `E8 rel32` in
#                                       [cursor, cursor+max_len) whose target lands in code
#   expect      {at, signature}         masked compare at cursor+at; does not move the cursor
CHAIN_OPS = ("call_target", "rip_target", "add", "deref", "nth_call", "expect")


def _exec_section(image: PEImage, rva: int):
    for s in image.sections:
        if s.executable and s.va <= rva < s.va + s.vsize:
            return s
    return None


def _in_image(image: PEImage, rva: int) -> bool:
    return any(s.va <= rva < s.va + max(s.vsize, s.rsize) for s in image.sections)


def _code_bytes(image: PEImage, rva: int, n: int) -> bytes:
    """Up to ``n`` bytes at ``rva``, clamped to the end of the containing executable section."""
    s = _exec_section(image, rva)
    if s is None:
        return b""
    return image.read_rva(rva, max(0, min(n, s.va + min(s.vsize, s.rsize) - rva)))


def _naive_calls(image: PEImage, start: int, max_len: int) -> list[tuple[int, int]]:
    """(site, target) for every E8 byte in the window whose rel32 lands in code. Byte-level on
    purpose: the C++ resolver has no length decoder, so both sides count exactly the same way."""
    code = _code_bytes(image, start, max_len)
    out = []
    for k in range(len(code) - 4):
        if code[k] == 0xE8:
            t = start + k + 5 + int.from_bytes(code[k + 1:k + 5], "little", signed=True)
            if _exec_section(image, t) is not None:
                out.append((start + k, t))
    return out


def apply_chain(image: PEImage, start_rva: int, steps: list[dict]) -> tuple[int | None, str]:
    """Walk ``steps`` from ``start_rva``. Returns (rva, "ok") or (None, "chain-step<i>-<op>")."""
    cur = start_rva
    for i, st in enumerate(steps):
        op = st.get("op")
        fail = (None, f"chain-step{i}-{op}")
        at = int(st.get("at", 0))
        if op == "call_target":
            p = cur + at
            raw = _code_bytes(image, p, 5)
            if len(raw) != 5 or raw[0] != 0xE8:
                return fail
            cur = p + 5 + int.from_bytes(raw[1:], "little", signed=True)
            if _exec_section(image, cur) is None:
                return fail
        elif op == "rip_target":
            p = cur + at
            raw = _code_bytes(image, p + int(st["disp_at"]), 4)
            if len(raw) != 4:
                return fail
            cur = p + int(st["insn_len"]) + int.from_bytes(raw, "little", signed=True)
            if not _in_image(image, cur):
                return fail
        elif op == "add":
            cur += int(st["value"])
        elif op == "deref":
            raw = image.read_rva(cur, 8) if _in_image(image, cur) else b""
            if len(raw) != 8:
                return fail
            cur = int.from_bytes(raw, "little") - image.image_base
            if not _in_image(image, cur):
                return fail
        elif op == "nth_call":
            calls = _naive_calls(image, cur, int(st["max_len"]))
            n = int(st["n"])
            if n < 0 or n >= len(calls):
                return fail
            cur = calls[n][1]
        elif op == "expect":
            pat = Pattern.parse(st["signature"])
            raw = image.read_rva(cur + at, len(pat.data)) if _in_image(image, cur + at) else b""
            if len(raw) != len(pat.data) or any(m and raw[j] != pat.data[j] for j, m in enumerate(pat.mask)):
                return fail
        else:
            return None, f"chain-step{i}-bad-op"
    return cur, "ok"


def tls_spans(code: bytes, base_rva: int) -> list[tuple[int, int]]:
    """[start, end) RVAs of every `mov r64, gs:[0x58]` (the 9 bytes HLE hosts rewrite)."""
    keep = _tls_mask(code, b"\x01" * len(code))
    out, i = [], 0
    while i < len(code):
        if not keep[i]:
            out.append((base_rva + i, base_rva + i + 9))
            i += 9
        else:
            i += 1
    return out


def pins_tls(image: PEImage, rva: int, pat: Pattern) -> list[int]:
    """RVAs of TLS loads under ``pat`` matched at ``rva`` that the pattern does not fully wildcard."""
    lo, hi = rva, rva + len(pat.data)
    code = image.read_rva(lo - 8, hi - lo + 16)
    out = []
    for a, b in tls_spans(code, lo - 8):
        if a < hi and b > lo and any(pat.mask[j - lo] for j in range(max(a, lo), min(b, hi))):
            out.append(a)
    return out


def chain_problems(image: PEImage, start_rva: int, steps: list[dict],
                   start_pattern: Pattern | None = None) -> list[str]:
    """Generation-time checks that make a chain safe under HLE hosts and on the C++ side.

    * no step reads bytes of a `gs:[0x58]` load, and a start signature does not pin one;
    * every nth_call window is TLS-load-free and its byte-level call candidates are exactly the
      decoded `call rel32` instructions (capstone, linear from the window start), so the naive
      count is the real call order;
    * call_target / rip_target sit on an instruction of the expected form.
    """
    probs: list[str] = []
    if start_pattern is not None:
        probs += [f"start signature pins the TLS load at {a:#x}" for a in pins_tls(image, start_rva, start_pattern)]
    try:
        from capstone import CS_ARCH_X86, CS_MODE_64, Cs
    except ImportError:  # pragma: no cover
        return probs + ["capstone missing: cannot validate chain"]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def tls_in(lo: int, hi: int) -> bool:
        code = image.read_rva(lo - 8, hi - lo + 16)
        return any(a < hi and b > lo for a, b in tls_spans(code, lo - 8))

    cur = start_rva
    for i, st in enumerate(steps):
        op = st.get("op")
        at = int(st.get("at", 0))
        if op not in CHAIN_OPS:
            probs.append(f"step{i}: unknown op {op!r}")
            break
        if op in ("call_target", "rip_target"):
            p = cur + at
            ins = next(md.disasm(image.read_rva(p, 16), p), None)
            if ins is None:
                probs.append(f"step{i} {op}: undecodable at {p:#x}")
            elif op == "call_target" and not (ins.bytes[0] == 0xE8 and ins.size == 5):
                probs.append(f"step{i} call_target: {p:#x} is `{ins.mnemonic} {ins.op_str}`, not call rel32")
            elif op == "rip_target" and (ins.size != int(st["insn_len"]) or "rip" not in ins.op_str
                                         or getattr(ins, "disp_offset", -1) != int(st["disp_at"])):
                probs.append(f"step{i} rip_target: {p:#x} is `{ins.mnemonic} {ins.op_str}`")
            if tls_in(p, p + (16 if ins is None else ins.size)):
                probs.append(f"step{i} {op}: instruction at {p:#x} overlaps a TLS load")
        elif op == "nth_call":
            n, max_len = int(st["n"]), int(st["max_len"])
            naive = _naive_calls(image, cur, max_len)
            if n < len(naive):
                end = naive[n][0] + 5
                decoded = [ins.address for ins in md.disasm(image.read_rva(cur, end - cur), cur)
                           if ins.bytes[0] == 0xE8 and ins.size == 5]
                if [s for s, _ in naive[:n + 1]] != decoded:
                    probs.append(f"step{i} nth_call: byte-level candidates {[hex(s) for s, _ in naive[:n + 1]]} "
                                 f"!= decoded calls {[hex(s) for s in decoded]}")
                if tls_in(cur, end):
                    probs.append(f"step{i} nth_call: window {cur:#x}..{end:#x} contains a TLS load")
        elif op == "expect":
            pat = Pattern.parse(st["signature"])
            probs += [f"step{i} expect: pins the TLS load at {a:#x}" for a in pins_tls(image, cur + at, pat)]
        nxt, status = apply_chain(image, cur, [st])
        if status != "ok":
            probs.append(f"step{i} {op}: fails ({status})")
            break
        cur = nxt
    return probs
