#include "game_clock.hpp"

#include "x64_decode.hpp"

#include <windows.h>

#include <cstring>

namespace g6rt::clock {
namespace {

bool in_range(const std::uint8_t* p, std::size_t n, const std::uint8_t* lo, const std::uint8_t* hi) noexcept {
    return p >= lo && p < hi && static_cast<std::size_t>(hi - p) >= n;
}

// Exact encodings accepted (the instruction bytes before disp32).
bool is_load(const std::uint8_t* p, const x64::Insn& in, LoadKind kind) noexcept {
    if (!in.rip_relative) return false;
    if (kind == LoadKind::mov_eax_rip32)
        return in.len == 6 && p[0] == 0x8B && p[1] == 0x05 && in.disp_off == 2;
    return in.len == 8 && p[0] == 0xC5 && p[1] == 0xFA && p[2] == 0x10 && p[3] == 0x05 && in.disp_off == 4;
}

const std::uint8_t* rip_of(const x64::Insn& in, const std::uint8_t* p) noexcept {
    return reinterpret_cast<const std::uint8_t*>(x64::rip_target(in, p, reinterpret_cast<std::uint64_t>(p)));
}

} // namespace

LoadSite find_handler_load(const std::uint8_t* fn, const std::uint8_t* lo, const std::uint8_t* hi, LoadKind kind) noexcept {
    LoadSite r;
    if (!fn || !in_range(fn, 16, lo, hi)) { r.why = "handler outside .text"; return r; }
    std::size_t off = 0;
    for (int k = 0; k < 32; ++k) {
        const std::uint8_t* p = fn + off;
        if (!in_range(p, 1, lo, hi)) { r.why = "handler runs past .text"; return r; }
        const x64::Insn in = x64::decode(p, static_cast<std::size_t>(hi - p));
        if (in.len == 0) { r.why = "undecodable instruction in the handler"; return r; }
        if (is_load(p, in, kind)) { r.insn = p; r.target = rip_of(in, p); return r; }
        if (in.branch == x64::Branch::call_rel32) {
            const std::uint8_t* core = reinterpret_cast<const std::uint8_t*>(x64::rel_target(in, p, reinterpret_cast<std::uint64_t>(p)));
            if (!in_range(core, 16, lo, hi)) { r.why = "call target outside .text"; return r; }
            const x64::Insn l = x64::decode(core, static_cast<std::size_t>(hi - core));
            if (l.len == 0 || !is_load(core, l, kind)) { r.why = "first call's target does not start with the expected rip load"; return r; }
            if (core[l.len] != 0xC3) { r.why = "first call's target is not `load; ret`"; return r; }
            r.insn = core;
            r.core = core;
            r.target = rip_of(l, core);
            return r;
        }
        if (in.terminator) { r.why = "handler ends before any load or call"; return r; }
        off += in.len;   // the js to the self-id path is not followed: it is not the value path
    }
    r.why = "no load or call in the first 32 instructions";
    return r;
}

const char* check_block(const std::uint8_t* base, std::size_t size, const std::uint8_t* block, std::size_t len, bool live,
                        char* section_name, std::size_t name_cap) noexcept {
    if (section_name && name_cap) section_name[0] = 0;
    if (!block || block < base || static_cast<std::size_t>(block - base) + len > size) return "outside the game module";
    if (reinterpret_cast<std::uintptr_t>(block) & 3) return "not 4-byte aligned";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sh = IMAGE_FIRST_SECTION(nt);
    const std::size_t rva = static_cast<std::size_t>(block - base);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        const std::size_t va = sh[s].VirtualAddress, vs = sh[s].Misc.VirtualSize;
        if (rva < va || rva >= va + vs) continue;
        if (section_name && name_cap) {
            std::size_t n = 0;
            for (; n < 8 && n + 1 < name_cap && sh[s].Name[n]; ++n) section_name[n] = static_cast<char>(sh[s].Name[n]);
            section_name[n] = 0;
        }
        if (rva + len > va + vs) return "crosses the end of its section";
        const DWORD ch = sh[s].Characteristics;
        if (!(ch & IMAGE_SCN_MEM_WRITE)) return "section is not writable";
        if (ch & IMAGE_SCN_MEM_EXECUTE) return "section is executable";
        if (live) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(block, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return "not committed";
            const DWORD pr = mbi.Protect & 0xFF;
            if (pr != PAGE_READWRITE && pr != PAGE_WRITECOPY) return "page protection is not RW/WC";
            if (static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < block + len) {
                MEMORY_BASIC_INFORMATION m2{};
                if (!VirtualQuery(block + len - 1, &m2, sizeof m2) || m2.State != MEM_COMMIT ||
                    ((m2.Protect & 0xFF) != PAGE_READWRITE && (m2.Protect & 0xFF) != PAGE_WRITECOPY))
                    return "second page not RW";
            }
        }
        return nullptr;
    }
    return "not inside any section";
}

Mixer::Event Mixer::feed(bool has_game, std::uint32_t game_ms, std::uint64_t wall_ms) noexcept {
    if (!primed_) {
        primed_ = true;
        last_game_ = game_ms;
        last_wall_ = wall_ms;
        first_wall_ = wall_ms;
        if (!has_game) state_ = State::rejected;
        return Event::none;
    }
    const std::uint64_t w = wall_ms >= last_wall_ ? wall_ms - last_wall_ : 0;
    std::uint64_t wall_step = w > 1000 ? 1000 : w;
    Event ev = Event::none;
    if (!has_game && state_ != State::rejected) state_ = State::rejected;
    if (state_ == State::rejected) {
        out_ += wall_step;
        last_wall_ = wall_ms;
        last_game_ = game_ms;
        return Event::none;
    }
    const std::uint32_t d = game_ms - last_game_;   // wrap-safe
    const bool backwards = d >= 0x80000000u;
    const bool too_fast = !backwards && static_cast<std::uint64_t>(d) > 4 * w + 1000;
    if (state_ == State::candidate) {
        out_ += wall_step;
        if (backwards || too_fast) {
            // Validation restarts (a timer reset during a load is possible); too many restarts reject it.
            ++anomalies_;
            good_ = moving_ = 0;
            first_wall_ = wall_ms;
            reason_ = backwards ? "game timer went backwards while being validated" : "game timer step implausible while being validated";
            ev = Event::anomaly;
            if (anomalies_ > kMaxAnomalies) {
                state_ = State::rejected;
                ev = Event::rejected;
            }
        } else {
            ++good_;
            if (d) ++moving_;
            if (good_ >= kNeedGood && moving_ >= kNeedMoving && wall_ms - first_wall_ >= kNeedWallSpanMs) {
                state_ = State::trusted;
                ev = Event::trusted;
            }
        }
    } else {   // trusted
        if (backwards || too_fast) {
            ++anomalies_;
            reason_ = backwards ? "game timer went backwards" : "game timer jumped";
            ev = Event::anomaly;          // no time added: output stays monotonic, the base is re-taken
            if (anomalies_ > kMaxAnomalies) {
                state_ = State::rejected;
                reason_ = "too many game timer anomalies";
                ev = Event::rejected;
            }
        } else {
            out_ += d;
        }
    }
    last_game_ = game_ms;
    last_wall_ = wall_ms;
    return ev;
}

} // namespace g6rt::clock
