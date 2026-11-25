// x64 instruction length decoder: enough to relocate function prologues and to walk
// native-handler wrappers. It refuses (len == 0) anything it does not fully understand,
// so callers never copy a partially decoded instruction.
#pragma once
#include <cstddef>
#include <cstdint>

namespace g6rt::x64 {

enum class Branch : std::uint8_t {
    none = 0,
    jmp_rel8, jmp_rel32,
    call_rel32,
    jcc_rel8, jcc_rel32,
    loop_rel8,      // loop/loopz/loopnz/jrcxz: not relocatable to rel32
};

struct Insn {
    std::uint8_t len = 0;        // 0 = refused
    std::uint8_t op_off = 0;     // offset of the first opcode byte (after legacy prefixes/REX/VEX)
    std::uint8_t map = 0;        // 0 one-byte, 1 = 0F, 2 = 0F38, 3 = 0F3A (VEX/EVEX map number)
    std::uint8_t op = 0;         // opcode byte within the map
    std::uint8_t rex = 0;
    bool has_modrm = false;
    std::uint8_t modrm = 0;
    bool rip_relative = false;   // ModRM [rip+disp32]
    std::uint8_t disp_off = 0;   // offset of the rip disp32 (valid if rip_relative)
    std::uint8_t imm_off = 0;
    std::uint8_t imm_size = 0;
    Branch branch = Branch::none;
    std::uint8_t rel_off = 0;    // offset of rel8/rel32 (valid if branch != none)
    std::uint8_t rel_size = 0;
    std::uint8_t cond = 0;       // jcc condition nibble
    bool terminator = false;     // ret / jmp / int3 / ud2 / hlt: execution does not fall through
    bool vex = false;
};

Insn decode(const std::uint8_t* p, std::size_t avail) noexcept;

// Target of a relative branch or rip-relative operand decoded at runtime address `ip`.
std::uint64_t rel_target(const Insn& in, const std::uint8_t* p, std::uint64_t ip) noexcept;
std::uint64_t rip_target(const Insn& in, const std::uint8_t* p, std::uint64_t ip) noexcept;

} // namespace g6rt::x64
