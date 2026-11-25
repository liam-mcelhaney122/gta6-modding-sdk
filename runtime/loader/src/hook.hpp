// Minimal x64 inline detour: relocating trampoline builder (pure) and an installer.
#pragma once
#include <cstddef>
#include <cstdint>

namespace g6rt::hook {

inline constexpr std::size_t kRel32PatchLen = 5;    // E9 rel32 -> relay next to the trampoline
inline constexpr std::size_t kAbsPatchLen = 14;     // FF 25 00000000 abs64
inline constexpr std::size_t kMaxTrampoline = 160;

struct BuildResult {
    std::size_t code_len = 0;    // bytes written to `out`
    std::size_t stolen = 0;      // bytes of the original function that were relocated
    char error[160] = {};
};

// Relocate whole instructions from `src` (live address `src_addr`) until at least
// `min_bytes` are covered, as code that will live at `tramp_addr`, followed by an
// absolute jump back to src_addr + stolen. Refuses: undecodable bytes, loop/jrcxz,
// branches into the stolen range, rip-relative operands that do not reach from
// tramp_addr, and functions that end inside the stolen range.
bool build_trampoline(const std::uint8_t* src, std::uint64_t src_addr, std::size_t src_avail,
                      std::size_t min_bytes, std::uint64_t tramp_addr,
                      std::uint8_t* out, std::size_t out_cap, BuildResult& res) noexcept;

enum class PatchKind : std::uint8_t { rel32_via_relay = 0, abs14 = 1 };

struct Options {
    bool allow_rel32 = true;   // try a near block (+-2 GiB) and a 5-byte patch first
};

class InlineHook {
public:
    InlineHook() = default;
    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;
    ~InlineHook();  // never frees executable memory that may still be reachable (see release())

    // Prepare everything (memory, trampoline, saved bytes) without touching the target.
    bool create(void* target, void* detour, const Options& opt, char* err, std::size_t errcap) noexcept;
    // Publish / unpublish the patch with all other threads suspended and outside the patch range.
    bool enable(char* err, std::size_t errcap) noexcept;
    bool disable(char* err, std::size_t errcap) noexcept;
    // Free the trampoline block. Only call when no thread can be inside the trampoline
    // or detour (the offline test proves this trivially; the runtime never calls it).
    void release_unsafe() noexcept;

    void* original() const noexcept { return trampoline_; }
    bool enabled() const noexcept { return enabled_; }
    PatchKind kind() const noexcept { return kind_; }
    std::size_t stolen() const noexcept { return stolen_; }
    std::size_t patch_len() const noexcept { return patch_len_; }
    const std::uint8_t* saved_bytes() const noexcept { return saved_; }

private:
    bool write_patch(const std::uint8_t* bytes, const std::uint8_t* expect, char* err, std::size_t errcap) noexcept;

    std::uint8_t* target_ = nullptr;
    void* detour_ = nullptr;
    std::uint8_t* block_ = nullptr;      // VirtualAlloc'd: [relay 16][trampoline]
    void* trampoline_ = nullptr;
    PatchKind kind_ = PatchKind::abs14;
    std::size_t stolen_ = 0;
    std::size_t patch_len_ = 0;
    std::uint8_t saved_[kAbsPatchLen] = {};
    std::uint8_t patch_[kAbsPatchLen] = {};
    bool created_ = false;
    bool enabled_ = false;
};

} // namespace g6rt::hook
