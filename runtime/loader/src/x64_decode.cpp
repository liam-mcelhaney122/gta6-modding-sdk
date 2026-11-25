#include "x64_decode.hpp"

#include <cstring>

namespace g6rt::x64 {
namespace {

enum Imm : std::uint8_t { kI0 = 0, kI8, kI16, kIZ, kIV, kEnter, kMoffs, kInvalid };

struct OneByte { bool modrm; Imm imm; };

OneByte one_byte(std::uint8_t b) noexcept {
    if (b < 0x40) {
        const std::uint8_t lo = b & 7;
        if (lo <= 3) return { true, kI0 };
        if (lo == 4) return { false, kI8 };
        if (lo == 5) return { false, kIZ };
        return { false, kInvalid };  // push/pop seg, daa/das/aaa/aas (prefixes were consumed earlier)
    }
    if (b >= 0x50 && b <= 0x5F) return { false, kI0 };
    switch (b) {
    case 0x63: return { true, kI0 };
    case 0x68: return { false, kIZ };
    case 0x69: return { true, kIZ };
    case 0x6A: return { false, kI8 };
    case 0x6B: return { true, kI8 };
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: return { false, kI0 };
    case 0x80: return { true, kI8 };
    case 0x81: return { true, kIZ };
    case 0x83: return { true, kI8 };
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: return { false, kMoffs };
    case 0xA8: return { false, kI8 };
    case 0xA9: return { false, kIZ };
    case 0xC0: case 0xC1: return { true, kI8 };
    case 0xC2: case 0xCA: return { false, kI16 };
    case 0xC3: case 0xC9: case 0xCB: case 0xCC: case 0xCF: return { false, kI0 };
    case 0xC6: return { true, kI8 };
    case 0xC7: return { true, kIZ };
    case 0xC8: return { false, kEnter };
    case 0xCD: return { false, kI8 };
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: return { true, kI0 };
    case 0xD7: return { false, kI0 };
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: return { false, kI8 };
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return { false, kI0 };
    case 0xF1: case 0xF4: case 0xF5: return { false, kI0 };
    case 0xF6: case 0xF7: case 0xFE: case 0xFF: return { true, kI0 };  // F6/F7 imm handled by reg
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: return { false, kI0 };
    default: break;
    }
    if (b >= 0x84 && b <= 0x8F) return { true, kI0 };
    if (b >= 0x90 && b <= 0x99) return { false, kI0 };
    if (b >= 0x9B && b <= 0x9F) return { false, kI0 };
    if (b >= 0xA4 && b <= 0xA7) return { false, kI0 };
    if (b >= 0xAA && b <= 0xAF) return { false, kI0 };
    if (b >= 0xB0 && b <= 0xB7) return { false, kI8 };
    if (b >= 0xB8 && b <= 0xBF) return { false, kIV };
    if (b >= 0xD8 && b <= 0xDF) return { true, kI0 };
    return { false, kInvalid };  // 60 61 82 9A CE D4 D5 D6 EA, and the ones handled by the caller
}

// 0F xx. Returns false for opcodes we refuse.
bool two_byte(std::uint8_t b, bool opsize, bool repne, bool rep, bool& modrm, std::uint8_t& imm) noexcept {
    modrm = true; imm = 0;
    if (b <= 0x03) return true;
    switch (b) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: modrm = false; return true;
    case 0x0D: return true;
    case 0x30: case 0x31: case 0x32: case 0x33: modrm = false; return true;
    case 0x77: modrm = false; return true;
    case 0x78: imm = (opsize || repne) ? 2 : 0; return true;  // EXTRQ/INSERTQ ib,ib vs VMREAD
    case 0x79: return true;
    case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA: modrm = false; return true;
    case 0xA3: case 0xA5: case 0xAB: case 0xAD: case 0xAE: case 0xAF: return true;
    case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4: case 0xC5: case 0xC6: imm = 1; return true;
    case 0xB8: return rep;  // POPCNT only (bare 0F B8 is JMPE)
    default: break;
    }
    if (b >= 0x10 && b <= 0x1F) return true;
    if (b >= 0x28 && b <= 0x2F) return true;
    if (b >= 0x40 && b <= 0x6F) return true;
    if (b >= 0x70 && b <= 0x73) { imm = 1; return true; }
    if (b >= 0x74 && b <= 0x76) return true;
    if (b >= 0x7C && b <= 0x7F) return true;
    if (b >= 0x90 && b <= 0x9F) return true;
    if (b >= 0xB0 && b <= 0xB7) return true;
    if (b >= 0xB9 && b <= 0xBF) return true;
    if (b >= 0xC0 && b <= 0xC7) return true;
    if (b >= 0xC8 && b <= 0xCF) { modrm = false; return true; }
    if (b >= 0xD0) return true;
    return false;  // 04 0A 0C 0E 0F(3DNow) 20-27(cr/dr) 34-37 39 3B-3F 7A 7B A6 A7, 38/3A handled by caller
}

bool vex_map1_has_imm(std::uint8_t op) noexcept {
    return (op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
}

} // namespace

Insn decode(const std::uint8_t* p, std::size_t avail) noexcept {
    Insn r{};
    if (!p || avail == 0) return r;
    if (avail > 15) avail = 15;
    std::size_t i = 0;
    bool opsize = false, adsize = false, rep = false, repne = false, lock = false;
    for (;; ++i) {
        if (i >= avail) return Insn{};
        const std::uint8_t b = p[i];
        if (b == 0x66) opsize = true;
        else if (b == 0x67) adsize = true;
        else if (b == 0xF2) repne = true;
        else if (b == 0xF3) rep = true;
        else if (b == 0xF0) lock = true;
        else if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {}
        else break;
    }
    const bool any_legacy = i != 0;
    if ((p[i] & 0xF0) == 0x40) {
        r.rex = p[i++];
        if (i >= avail) return Insn{};
    }
    const bool rexw = (r.rex & 8) != 0;
    r.op_off = static_cast<std::uint8_t>(i);
    std::uint8_t b = p[i++];

    bool modrm = false;
    std::uint8_t imm = 0;
    bool imm_by_reg_f6 = false, imm_by_reg_f7 = false;

    if (b == 0xC4 || b == 0xC5 || b == 0x62) {
        // VEX / EVEX (always in 64-bit mode). Legacy 66/F2/F3/F0 or REX before them is #UD.
        if (r.rex || opsize || rep || repne || lock) return Insn{};
        r.vex = true;
        std::uint8_t map;
        if (b == 0xC5) {
            if (i + 1 > avail) return Insn{};
            map = 1; i += 1;
        } else if (b == 0xC4) {
            if (i + 2 > avail) return Insn{};
            map = p[i] & 0x1F; i += 2;
            if (map < 1 || map > 3) return Insn{};
        } else {
            if (i + 3 > avail) return Insn{};
            map = p[i] & 0x07; i += 3;
            if (!(map == 1 || map == 2 || map == 3 || map == 5 || map == 6)) return Insn{};
        }
        if (i >= avail) return Insn{};
        r.op_off = static_cast<std::uint8_t>(i);
        r.map = map;
        r.op = p[i++];
        modrm = !(map == 1 && r.op == 0x77 && b != 0x62);  // vzeroupper / vzeroall
        imm = (map == 3 || (map == 1 && vex_map1_has_imm(r.op))) ? 1 : 0;
    } else if (b == 0x0F) {
        if (i >= avail) return Insn{};
        const std::uint8_t b2 = p[i++];
        if (b2 == 0x38 || b2 == 0x3A) {
            if (i >= avail) return Insn{};
            r.map = b2 == 0x38 ? 2 : 3;
            r.op = p[i++];
            modrm = true;
            imm = b2 == 0x3A ? 1 : 0;
        } else {
            r.map = 1;
            r.op = b2;
            if (b2 >= 0x80 && b2 <= 0x8F) {
                if (opsize) return Insn{};
                r.branch = Branch::jcc_rel32;
                r.cond = b2 & 0x0F;
                r.rel_off = static_cast<std::uint8_t>(i);
                r.rel_size = 4;
                imm = 4;
            } else if (!two_byte(b2, opsize, repne, rep, modrm, imm)) {
                return Insn{};
            }
            if (b2 == 0x0B) r.terminator = true;  // ud2
        }
    } else {
        r.map = 0;
        r.op = b;
        if (b >= 0x70 && b <= 0x7F) {
            r.branch = Branch::jcc_rel8; r.cond = b & 0x0F;
            r.rel_off = static_cast<std::uint8_t>(i); r.rel_size = 1; imm = 1;
        } else if (b >= 0xE0 && b <= 0xE3) {
            r.branch = Branch::loop_rel8; r.rel_off = static_cast<std::uint8_t>(i); r.rel_size = 1; imm = 1;
        } else if (b == 0xE8 || b == 0xE9) {
            if (opsize) return Insn{};
            r.branch = b == 0xE8 ? Branch::call_rel32 : Branch::jmp_rel32;
            r.rel_off = static_cast<std::uint8_t>(i); r.rel_size = 4; imm = 4;
            r.terminator = b == 0xE9;
        } else if (b == 0xEB) {
            r.branch = Branch::jmp_rel8; r.rel_off = static_cast<std::uint8_t>(i); r.rel_size = 1; imm = 1;
            r.terminator = true;
        } else {
            const OneByte ob = one_byte(b);
            if (ob.imm == kInvalid) return Insn{};
            modrm = ob.modrm;
            switch (ob.imm) {
            case kI0: imm = 0; break;
            case kI8: imm = 1; break;
            case kI16: imm = 2; break;
            case kIZ: imm = (opsize && !rexw) ? 2 : 4; break;
            case kIV: imm = rexw ? 8 : (opsize ? 2 : 4); break;
            case kEnter: imm = 3; break;
            case kMoffs: imm = adsize ? 4 : 8; break;
            default: return Insn{};
            }
            imm_by_reg_f6 = b == 0xF6;
            imm_by_reg_f7 = b == 0xF7;
            if (b == 0xC3 || b == 0xC2 || b == 0xCA || b == 0xCB || b == 0xCC || b == 0xCF || b == 0xF4)
                r.terminator = true;
        }
    }
    (void)any_legacy;

    if (modrm) {
        if (i >= avail) return Insn{};
        const std::uint8_t m = p[i++];
        r.has_modrm = true;
        r.modrm = m;
        const std::uint8_t mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
        if (r.map == 0) {
            if (r.op == 0x8F && reg != 0) return Insn{};          // XOP space (not on Zen 2)
            if (r.op == 0xC7 && m == 0xF8) return Insn{};         // XBEGIN rel32
            if (r.op == 0xC6 && reg != 0 && m != 0xF8) return Insn{};
            if (r.op == 0xC7 && reg != 0) return Insn{};
            if (imm_by_reg_f6 && reg < 2) imm = 1;
            if (imm_by_reg_f7 && reg < 2) imm = (opsize && !rexw) ? 2 : 4;
            if (r.op == 0xFF) {
                if (reg == 7) return Insn{};
                if (reg == 4 || reg == 5) r.terminator = true;   // jmp r/m
            }
            if (r.op == 0xFE && reg > 1) return Insn{};
        }
        std::size_t disp = 0;
        if (mod != 3) {
            if (rm == 4) {
                if (i >= avail) return Insn{};
                const std::uint8_t sib = p[i++];
                if ((sib & 7) == 5 && mod == 0) disp = 4;
            }
            if (mod == 0 && rm == 5) {
                r.rip_relative = true;
                r.disp_off = static_cast<std::uint8_t>(i);
                disp = 4;
            } else if (mod == 1) {
                disp = 1;
            } else if (mod == 2) {
                disp = 4;
            }
        }
        i += disp;
    }
    if (r.branch == Branch::none) {
        r.imm_off = static_cast<std::uint8_t>(i);
        r.imm_size = imm;
    }
    i += imm;
    if (i > avail || i > 15) return Insn{};
    r.len = static_cast<std::uint8_t>(i);
    return r;
}

std::uint64_t rel_target(const Insn& in, const std::uint8_t* p, std::uint64_t ip) noexcept {
    std::int64_t rel = 0;
    if (in.rel_size == 1) {
        rel = static_cast<std::int8_t>(p[in.rel_off]);
    } else if (in.rel_size == 4) {
        std::int32_t v;
        std::memcpy(&v, p + in.rel_off, 4);
        rel = v;
    }
    return ip + in.len + static_cast<std::uint64_t>(rel);
}

std::uint64_t rip_target(const Insn& in, const std::uint8_t* p, std::uint64_t ip) noexcept {
    std::int32_t v;
    std::memcpy(&v, p + in.disp_off, 4);
    return ip + in.len + static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
}

} // namespace g6rt::x64
