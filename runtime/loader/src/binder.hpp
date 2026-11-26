// Which scrThread plugin scripts tick on (G-095). Pure policy behind one lock, so every Run call on every
// OS thread sees the same decision and a rebind has exactly one winner.
//
// scrThread state byte (+0x344, u8; 1.0.2.1 code, findings/live/2026-11-25_g093_g095.md):
//   1 set by reset (0x141c183ef); 1 -> 2 after the program's initialisers ran (0x141c18322);
//   2 -> 3 at the start of update (0x143de109c); 3 -> 2, or 3 -> 4, at its end (0x143de11c4);
//   update skips 4, 5 and 6 (0x143de1048..0x143de1066); kill paths 3 -> 4, -> 5, 5 -> 6, 6 -> 7 (0x143de1590);
//   the current-id wrapper rejects 7 (0x144f61eaf).
// So 2 and 3 are the states of a thread that the game updates; 1 is a thread that has not finished its
// initialisers, and 4..7 are finishing or dead. Only 2 and 3 count as alive here.
//
// Policy (bind=stable, alias first; bind=hash restricts candidates to one name hash):
//   * presence: a thread is continuously present while its alive Run calls are less than gap_ms apart;
//   * nothing binds before a candidate has been continuously present for observe_ms;
//   * the winner is the candidate with the longest continuous presence (ties: more alive Run calls, then
//     the lower id), chosen under the lock, so a rebind has exactly one winner and bumps the generation;
//   * the bound thread is dropped when it is seen in a finishing state (>= 4), or when it has had no alive Run
//     call for absent_ms while other threads had at least kAbsentRuns alive Run calls (a global stall,
//     a load screen for example, is not absence);
//   * a thread with no Run call for forget_ms (and kForgetRuns calls of others) is forgotten ("gone").
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace g6rt::bind {

enum class Mode : int { stable = 0, hash = 1, observe = 2 };

inline constexpr std::uint8_t kStateInit = 1;
inline constexpr std::uint8_t kStateIdle = 2;
inline constexpr std::uint8_t kStateUpdating = 3;
inline constexpr bool state_alive(std::uint8_t s) noexcept { return s == kStateIdle || s == kStateUpdating; }
inline constexpr bool state_finishing(std::uint8_t s) noexcept { return s >= 4; }

struct Config {
    Mode mode = Mode::stable;
    std::uint32_t hash = 0;            // bind=hash only
    std::uint32_t observe_ms = 3000;   // continuous presence a candidate needs before it can be bound
    std::uint32_t gap_ms = 2000;       // a longer gap between alive Run calls breaks continuous presence
    std::uint32_t absent_ms = 3000;    // bound thread missing this long (while others run): rebind
    std::uint32_t forget_ms = 15000;   // no Run call this long (while others run): forget the thread
};

inline constexpr std::uint32_t kAbsentRuns = 16;   // alive Run calls of other threads needed to call one absent
inline constexpr std::uint32_t kForgetRuns = 64;
inline constexpr std::size_t kMaxThreads = 256;

struct Event {
    enum Kind : std::uint8_t { none = 0, bound = 1, unbound = 2, gone = 3 } kind = none;
    std::uint32_t id = 0, name = 0, generation = 0;
    std::uint8_t state = 0;
    std::uint64_t presence_ms = 0;     // bound: continuous presence; gone: first..last Run call
    std::uint64_t runs = 0;            // Run calls seen for the thread
    const char* why = "";
};

struct Decision {
    bool tick = false;                 // this Run call is on the bound thread, in an alive state
    std::uint32_t generation = 0;      // binder generation when the decision was made
    std::uint32_t nev = 0;
    Event ev[4];                       // to log outside the lock
};

class Binder {
public:
    void configure(const Config& c) noexcept;
    Decision on_run(std::uint32_t id, std::uint32_t name, std::uint8_t state, std::uint64_t now_ms) noexcept;
    std::uint32_t bound_id() const noexcept;
    std::uint32_t generation() const noexcept;
    std::uint32_t tracked() const noexcept;
    struct Counts { std::uint32_t binds, unbinds, gone; };
    Counts counts() const noexcept;

private:
    struct Entry {
        std::uint32_t id, name;
        std::uint8_t state;
        bool alive_seen;
        std::uint64_t first_ms, last_ms;      // any Run call
        std::uint64_t streak_ms, alive_ms;    // start of continuous presence, last alive Run call
        std::uint64_t alive_seq;              // g_alive_seq at the last alive Run call
        std::uint64_t any_seq;                // g_any_seq at the last Run call
        std::uint64_t runs, alive_runs;
    };
    Entry* find(std::uint32_t id) noexcept;
    Entry* insert(std::uint32_t id, std::uint32_t name, std::uint64_t now) noexcept;
    void push(Decision& d, const Event& e) noexcept;
    void unbind(Decision& d, const Entry* e, const char* why) noexcept;
    void choose(Decision& d, std::uint64_t now) noexcept;
    void sweep(Decision& d, std::uint64_t now) noexcept;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    Config cfg_{};
    Entry t_[kMaxThreads]{};
    std::uint32_t n_ = 0;
    std::uint32_t bound_ = 0;
    std::uint32_t gen_ = 0;
    std::uint64_t alive_seq_ = 0, any_seq_ = 0;
    std::uint64_t last_sweep_ = 0;
    Counts counts_{ 0, 0, 0 };
};

} // namespace g6rt::bind
