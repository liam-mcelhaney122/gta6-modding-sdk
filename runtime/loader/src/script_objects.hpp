// Script String / text objects for natives that take them (g6_native_call.string_slots / text_slots, G-120).
// Layouts: include/gta6mod/script_objects.h; evidence: findings/phase5/37_script_strings.md.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gta6mod/gta6mod.h"

namespace g6rt::objects {

// Byte offsets inside the opcode 52 handler (anchor script.string.literal_op) for build 1.0.2.1.
inline constexpr std::size_t kLeaTypeAt = 0x09;   // 48 8D 15 disp32   lea rdx,[String record]
inline constexpr std::size_t kLeaElemAt = 0x10;   // 4C 8D 05 disp32   lea r8,[element record]
inline constexpr std::size_t kStoreTypeAt = 0x30; // 48 89 11          mov [rcx],rdx
inline constexpr std::size_t kStoreElemAt = 0x33; // 4C 89 41 20       mov [rcx+0x20],r8
inline constexpr std::size_t kLiteralOpSpan = 0x37;
// The text accessor (anchor script.text.get): mov rcx,[rcx+0x18]; call native_get.
inline constexpr std::size_t kTextReadAt = 0x2f;  // 48 8B 49 18 E8 rel32
inline constexpr std::size_t kTextGetSpan = 0x38;
// The native text dispatcher (anchor script.text.native_get): 17 bytes, ends in jmp rax.
inline constexpr std::uint8_t kNativeGetBytes[] = { 0x48, 0x85, 0xC9, 0x74, 0x0C, 0x48, 0x8B, 0x01, 0xB2,
                                                    0x01, 0x48, 0x8B, 0x40, 0x18, 0x48, 0xFF, 0xE0 };

// Pure: the two records opcode 52 writes, read from its code. Every byte checked lies in [lo, hi).
bool literal_op_records(const std::uint8_t* op, const std::uint8_t* lo, const std::uint8_t* hi, const void** type,
                        const void** elem) noexcept;
// Pure: the accessor reads +0x18 and calls native_get, whose body is the expected virtual +0x18 tail call.
bool text_path_ok(const std::uint8_t* get, const std::uint8_t* native_get, const std::uint8_t* lo,
                  const std::uint8_t* hi) noexcept;

// Session configuration (runtime init). type/elem null: String arguments are refused; text_ok false: text
// arguments are refused. Clears nothing that was handed out before.
void configure(const void* string_type, const void* string_elem, bool text_ok) noexcept;
bool string_ready() noexcept;
bool text_ready() noexcept;

struct Stats { std::uint32_t strings, texts, temporaries; std::size_t bytes; };
Stats stats() noexcept;

// Converts the masked slots of `call` for one native call and restores them in the destructor.
class CallObjects {
public:
    CallObjects() noexcept = default;
    CallObjects(const CallObjects&) = delete;
    CallObjects& operator=(const CallObjects&) = delete;
    ~CallObjects();
    // G6_OK, G6_E_INVALID_ARG (masks beyond argc, overlapping, text too long, out of memory) or
    // G6_E_NO_SUCH_ANCHOR (the object kind is not available on this build). On failure nothing is changed.
    g6_status prepare(g6_native_call* call) noexcept;

private:
    g6_native_call* call_ = nullptr;
    std::uint32_t mask_ = 0;
    std::uint64_t saved_[G6_MAX_NATIVE_ARGS] = {};
    void* temps_[G6_MAX_NATIVE_ARGS] = {};
};

} // namespace g6rt::objects
