// Minimal x64 inline detour: relocating trampoline builder (pure) and an installer.
#pragma once
#include <cstddef>
#include <cstdint>

namespace g6rt::hook {

inline constexpr std::size_t kRel32PatchLen = 5;    // E9 rel32 -> relay next to the trampoline
inline constexpr std::size_t kAbsPatchLen = 14;     // FF 25 00000000 abs64
inline constexpr std::size_t kMaxTrampoline = 160;
// A patch write is refused after kFreezeAttempts suspend/check rounds or kFreezeBudgetMs, whichever
// comes first. One round takes a Toolhelp32 snapshot of every thread on the system; walking it costs
// ~250 ms with ~6,000 threads (measured on the dev machine), so the time budget usually binds first.
inline constexpr int kFreezeAttempts = 200;
inline constexpr unsigned kFreezeBudgetMs = 10000;
inline constexpr unsigned kFreezeRetrySleepMs = 1;    // all threads run between rounds

// Bytes at the target that no other thread may be executing while the patch is written: the
// patch itself and, when the last relocated instruction runs past it, the rest of that instruction.
constexpr std::size_t guarded_len(std::size_t patch_len, std::size_t stolen) noexcept {
    return stolen > patch_len ? stolen : patch_len;
}
// True if `rip` lies in [lo, lo + n). Overflow-safe for any lo and n.
constexpr bool rip_in_range(std::uint64_t rip, std::uint64_t lo, std::size_t n) noexcept {
    return rip >= lo && rip - lo < n;
}

// Outcome of the last enable()/disable() thread freeze (for the log; refusal text is in `err`).
struct FreezeStats {
    std::uint32_t threads = 0;        // other threads suspended in the last attempt
    std::uint32_t attempts = 0;       // suspend/check rounds made (1 = no retry needed)
    std::uint32_t elapsed_ms = 0;     // wall time of the whole freeze loop
    std::uint32_t blocking_tid = 0;   // last thread found inside the range or not inspectable (0 = none)
    std::uint64_t blocking_rip = 0;   // its RIP, 0 when it could not be read
    std::uint32_t blocking_error = 0; // Win32 error when the thread could not be opened/suspended/read
};

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
    const FreezeStats& last_freeze() const noexcept { return freeze_; }

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
    FreezeStats freeze_{};
    bool created_ = false;
    bool enabled_ = false;
};

} // namespace g6rt::hook
