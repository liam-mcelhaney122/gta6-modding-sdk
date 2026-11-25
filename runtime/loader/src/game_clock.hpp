// game_time_ms source: the game's own timer block (GET_GAME_TIMER), resolved from the native
// handler bodies, with the system timer as fallback. Pure parts (decode, validator) are exposed
// for the offline self-test.
#pragma once
#include <cstddef>
#include <cstdint>

namespace g6rt::clock {

enum class LoadKind : int {
    mov_eax_rip32 = 0,     // 8B 05 disp32           mov eax, dword [rip+disp]
    vmovss_xmm0_rip = 1,   // C5 FA 10 05 disp32     vmovss xmm0, dword [rip+disp]
};

struct LoadSite {
    const std::uint8_t* target = nullptr;   // address the load reads
    const std::uint8_t* insn = nullptr;     // the load instruction
    const std::uint8_t* core = nullptr;     // callee that holds the load (null if inline in the handler)
    const char* why = "";                   // reason when target == nullptr
};

// Walks a native handler from its entry, stops at the first direct call or terminator, and
// finds the `kind` load either inline (before the call) or as the callee's whole body
// (`load; ret`). Every address followed must stay inside [lo, hi).
LoadSite find_handler_load(const std::uint8_t* fn, const std::uint8_t* lo, const std::uint8_t* hi, LoadKind kind) noexcept;

// The block must lie in one section of the image that is writable and not executable, inside
// that section's VirtualSize. `live` adds a VirtualQuery check (committed, RW/WC, not exec).
const char* check_block(const std::uint8_t* base, std::size_t size, const std::uint8_t* block, std::size_t len,
                        bool live, char* section_name, std::size_t name_cap) noexcept;

// Mixes the game timer with the wall clock into one monotonic millisecond output.
//   candidate: the game timer is sampled but not used; output follows the wall clock.
//   trusted:   >= kNeedGood consecutive plausible samples (>= kNeedMoving advanced, >= 1 s wall):
//              output follows the game timer (pauses when the game pauses).
//   rejected:  more than kMaxAnomalies backward/implausible steps (each one restarts the
//              validation while a candidate, and adds no time while trusted): wall clock for
//              the rest of the session.
// Plausible: the u32 delta (wrap-safe) is < 2^31 and <= 4 * wall delta + 1000 ms.
// Wall steps are capped at 1000 ms per update so a stall does not fast-forward waits.
class Mixer {
public:
    enum class State : int { candidate = 0, trusted = 1, rejected = 2 };
    enum class Event : int { none = 0, trusted, rejected, anomaly };
    static constexpr std::uint32_t kNeedGood = 30;
    static constexpr std::uint32_t kNeedMoving = 5;
    static constexpr std::uint64_t kNeedWallSpanMs = 1000;
    static constexpr std::uint32_t kMaxAnomalies = 8;

    // has_game=false: no game timer this build/session (system timer only).
    Event feed(bool has_game, std::uint32_t game_ms, std::uint64_t wall_ms) noexcept;
    void reject() noexcept { state_ = State::rejected; }
    State state() const noexcept { return state_; }
    std::uint64_t now() const noexcept { return out_; }
    std::uint32_t anomalies() const noexcept { return anomalies_; }
    std::uint32_t last_game() const noexcept { return last_game_; }
    const char* last_reason() const noexcept { return reason_; }

private:
    State state_ = State::candidate;
    bool primed_ = false;
    std::uint32_t last_game_ = 0;
    std::uint64_t last_wall_ = 0;
    std::uint64_t first_wall_ = 0;
    std::uint64_t out_ = 0;
    std::uint32_t good_ = 0, moving_ = 0, anomalies_ = 0;
    const char* reason_ = "";
};

} // namespace g6rt::clock
