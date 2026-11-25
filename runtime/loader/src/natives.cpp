#include "natives.hpp"

#include "log.hpp"
#include "manifest_1_0_2_1.inc"
#include "x64_decode.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace g6rt::natives {
namespace {
namespace m = g6rt::manifest;
constexpr std::size_t kCount = sizeof(m::kNatives) / sizeof(m::kNatives[0]);
std::atomic<std::uint8_t> g_state[kCount];
std::uint8_t* g_base = nullptr;
std::atomic<std::uint32_t> g_verified{ 0 };

int seh_filter(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

DWORD call_guarded(Handler h, CallContext* c) noexcept {
    __try {
        h(c);
    } __except (seh_filter(GetExceptionCode())) {
        return GetExceptionCode();
    }
    return 0;
}
} // namespace

const char* verify_str(Verify v) noexcept {
    switch (v) {
    case Verify::unchecked: return "unchecked";
    case Verify::ok: return "ok";
    case Verify::no_cmp: return "no cmp [rcx+0x10],0";
    case Verify::no_js: return "no js after cmp";
    case Verify::no_id: return "no imm64 id store";
    case Verify::id_mismatch: return "id mismatch";
    case Verify::out_of_range: return "out of .text";
    case Verify::faulted: return "faulted when called";
    }
    return "?";
}

Verify verify_selfid(const std::uint8_t* fn, const std::uint8_t* lo, const std::uint8_t* hi,
                     std::uint64_t hash, std::uint64_t* found_id) noexcept {
    if (found_id) *found_id = 0;
    // Head window: the wrapper cmp sits 0..0x40 bytes in on 1.0.2.1 (0x40 = TASK_FOLLOW_AND_CONVERSE_WITH_PED,
    // after a long register-save block). 128 bytes / 32 insns keeps a margin; the range guard covers it.
    constexpr std::size_t kHeadBytes = 128;
    constexpr int kHeadInsns = 32;
    if (!fn || fn < lo || fn + kHeadBytes + 32 > hi) return Verify::out_of_range;
    std::size_t off = 0;
    bool cmp = false;
    for (int k = 0; k < kHeadInsns && off < kHeadBytes; ++k) {
        const x64::Insn in = x64::decode(fn + off, static_cast<std::size_t>(hi - (fn + off)));
        if (in.len == 0) return Verify::no_cmp;
        if (in.len == 4 && fn[off] == 0x83 && fn[off + 1] == 0x79 && fn[off + 2] == 0x10 && fn[off + 3] == 0x00) {
            off += 4;
            cmp = true;
            break;
        }
        if (in.terminator) return Verify::no_cmp;
        off += in.len;
    }
    if (!cmp) return Verify::no_cmp;
    const std::uint8_t* tgt = nullptr;
    for (int k = 0; k < 3; ++k) {
        const x64::Insn in = x64::decode(fn + off, static_cast<std::size_t>(hi - (fn + off)));
        if (in.len == 0) return Verify::no_js;
        const bool js = in.cond == 8 && in.op_off == 0 &&
                        ((in.map == 0 && in.branch == x64::Branch::jcc_rel8) || (in.map == 1 && in.branch == x64::Branch::jcc_rel32));
        if (js) {
            tgt = reinterpret_cast<const std::uint8_t*>(
                x64::rel_target(in, fn + off, reinterpret_cast<std::uint64_t>(fn + off)));
            break;
        }
        off += in.len;
    }
    if (!tgt) return Verify::no_js;
    if (tgt < lo || tgt + 32 > hi) return Verify::out_of_range;
    std::size_t t = 0;
    for (int k = 0; k < 4; ++k) {
        const x64::Insn in = x64::decode(tgt + t, static_cast<std::size_t>(hi - (tgt + t)));
        if (in.len == 0) return Verify::no_id;
        if (in.map == 0 && in.op_off == 1 && (in.rex & 8) && in.op >= 0xB8 && in.op <= 0xBF && in.imm_size == 8) {
            std::uint64_t id;
            std::memcpy(&id, tgt + t + in.imm_off, 8);
            if (found_id) *found_id = id;
            return id == expected_id(hash) ? Verify::ok : Verify::id_mismatch;
        }
        t += in.len;
    }
    return Verify::no_id;
}

std::size_t table_size() noexcept { return kCount; }

std::ptrdiff_t find(std::uint64_t hash) noexcept {
    std::size_t lo = 0, hi = kCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::uint64_t h = m::kNatives[mid].hash;
        if (h == hash) return static_cast<std::ptrdiff_t>(mid);
        if (h < hash) lo = mid + 1; else hi = mid;
    }
    return -1;
}

std::uint64_t hash_at(std::size_t i) noexcept { return i < kCount ? m::kNatives[i].hash : 0; }
std::uint32_t rva_at(std::size_t i) noexcept { return i < kCount ? m::kNatives[i].handler_rva : 0; }
bool is_network(std::size_t i) noexcept { return i < kCount && (m::kNatives[i].flags & m::kNativeNetwork); }
const char* name_at(std::size_t i) noexcept { return i < kCount ? m::kNatives[i].name : "?"; }
Verify state_at(std::size_t i) noexcept { return i < kCount ? static_cast<Verify>(g_state[i].load()) : Verify::unchecked; }
std::uint32_t verified_count() noexcept { return g_verified.load(); }

std::uint32_t bind_and_verify(std::uint8_t* image_base, const std::uint8_t* text_lo, const std::uint8_t* text_hi,
                              std::uint32_t* bad_out) noexcept {
    g_base = image_base;
    std::uint32_t ok = 0, bad = 0, logged = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        std::uint64_t id = 0;
        const Verify v = verify_selfid(image_base + m::kNatives[i].handler_rva, text_lo, text_hi, m::kNatives[i].hash, &id);
        g_state[i].store(static_cast<std::uint8_t>(v));
        if (v == Verify::ok) {
            ++ok;
        } else {
            ++bad;
            if (logged++ < 20)
                G6LOG_W("native 0x%016llX %s rva 0x%x: %s (id read 0x%016llX)", static_cast<unsigned long long>(m::kNatives[i].hash),
                        m::kNatives[i].name, m::kNatives[i].handler_rva, verify_str(v), static_cast<unsigned long long>(id));
        }
    }
    g_verified.store(ok);
    if (bad_out) *bad_out = bad;
    return ok;
}

g6_status call_handler(std::size_t i, g6_native_call* call) noexcept {
    if (i >= kCount || !g_base) return G6_E_UNKNOWN_NATIVE;
    if (static_cast<Verify>(g_state[i].load()) != Verify::ok) return G6_E_UNVERIFIED_NATIVE;
    if (call->argc > G6_MAX_NATIVE_ARGS) return G6_E_TOO_MANY_ARGS;
    std::memset(call->ret, 0, sizeof call->ret);
    CallContext c{ call->args, call->ret, static_cast<std::int32_t>(call->argc), G6_MAX_NATIVE_RETS };
    const Handler h = reinterpret_cast<Handler>(g_base + m::kNatives[i].handler_rva);
    const DWORD code = call_guarded(h, &c);
    if (code != 0) {
        g_state[i].store(static_cast<std::uint8_t>(Verify::faulted));
        G6LOG_E("native 0x%016llX %s faulted (exception 0x%08lX); disabled for this session. The native lock may be "
                "held: the game can deadlock after this.",
                static_cast<unsigned long long>(m::kNatives[i].hash), m::kNatives[i].name, code);
        return G6_E_INTERNAL;
    }
    return G6_OK;
}

} // namespace g6rt::natives
