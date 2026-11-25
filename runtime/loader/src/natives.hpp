// Native table (manifest handler RVAs), static handler identity check and the invoker.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gta6mod/gta6mod.h"

namespace g6rt::natives {

// GTA VI scrNativeCallContext (findings/phase2/13_script_vm.md §3, types/parts/script-vm.json).
struct CallContext {
    std::uint64_t* args;     // +0x00
    std::uint64_t* ret;      // +0x08
    std::int32_t arg_count;  // +0x10
    std::uint32_t ret_count; // +0x14
};
static_assert(sizeof(CallContext) == 0x18, "scrNativeCallContext is 0x18 bytes");
static_assert(offsetof(CallContext, arg_count) == 0x10 && offsetof(CallContext, ret_count) == 0x14, "ctx layout");

using Handler = void (*)(CallContext*);

enum class Verify : std::uint8_t {
    unchecked = 0,
    ok,                 // wrapper shape found and its embedded id == rotl64(~hash, 32)
    no_cmp,             // no `cmp dword [rcx+0x10], 0` in the prologue window
    no_js,              // no js after the cmp
    no_id,              // js target does not store an imm64
    id_mismatch,        // imm64 present but belongs to another hash
    out_of_range,       // handler or js target outside .text
    faulted,            // the handler raised an exception when called; disabled
};
const char* verify_str(Verify v) noexcept;

// Reads code only. `text_lo/hi` bound every address followed.
Verify verify_selfid(const std::uint8_t* fn, const std::uint8_t* text_lo, const std::uint8_t* text_hi,
                     std::uint64_t hash, std::uint64_t* found_id) noexcept;

inline constexpr std::uint64_t expected_id(std::uint64_t hash) noexcept {
    const std::uint64_t x = ~hash;
    return (x << 32) | (x >> 32);
}

std::size_t table_size() noexcept;
// Index into the manifest table, or -1.
std::ptrdiff_t find(std::uint64_t hash) noexcept;
std::uint64_t hash_at(std::size_t i) noexcept;
std::uint32_t rva_at(std::size_t i) noexcept;
bool is_network(std::size_t i) noexcept;
const char* name_at(std::size_t i) noexcept;

// Binds the table to a live image and verifies every handler. Returns the number verified.
std::uint32_t bind_and_verify(std::uint8_t* image_base, const std::uint8_t* text_lo, const std::uint8_t* text_hi,
                              std::uint32_t* bad_out) noexcept;
std::uint32_t verified_count() noexcept;
Verify state_at(std::size_t i) noexcept;

// Raw call of a verified handler with SEH protection. Caller guarantees script-thread context.
g6_status call_handler(std::size_t i, g6_native_call* call) noexcept;

} // namespace g6rt::natives
