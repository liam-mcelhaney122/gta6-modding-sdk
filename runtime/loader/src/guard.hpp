// Single-player guard (docs/runtime_design.md §7, revised 2026-11-25). 1.0.2.1 has no
// session-state natives (findings/phase5/26), so the guard has two inputs, both required:
//   1. operator attestation  [guard] offline_attested=1 (the host blocks the game's network);
//   2. a read-only check of this process's TCP connections (GetExtendedTcpTable, own PID),
//      repeated by a worker thread. Any ESTABLISHED / SYN_SENT connection to a remote that is
//      not loopback or link-local trips the guard for the rest of the session.
// Nothing here touches game code.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace g6rt::guard {

enum class AddrClass : int { loopback = 0, link_local, unspecified, private_net, public_net };
const char* class_str(AddrClass c) noexcept;

// a: 4 bytes (v4, network order) or 16 bytes (v6). IPv4-mapped IPv6 (::ffff:a.b.c.d) is classified as IPv4.
AddrClass classify_v4(const std::uint8_t a[4]) noexcept;
AddrClass classify_v6(const std::uint8_t a[16]) noexcept;

// MIB_TCP_STATE values used by the decision (iphlpapi).
inline constexpr std::uint32_t kStateSynSent = 3;
inline constexpr std::uint32_t kStateEstablished = 5;

// True when a connection in `state` to a remote of class `c` must close the guard.
// Only loopback and link-local remotes are ignored; LAN (private) remotes close it too.
bool trips(AddrClass c, std::uint32_t state) noexcept;

// Formats "a.b.c.d:port" / "[v6]:port". `port` in host order.
void format_remote(bool v6, const std::uint8_t* a, std::uint16_t port, char* out, std::size_t cap) noexcept;

// The gate. open() == attested && never tripped && a clean scan no older than kStaleMs.
class Gate {
public:
    static constexpr std::uint64_t kStaleMs = 20000;   // 4 missed scans at the default 5 s
    void set_attested(bool v) noexcept { attested_.store(v); }
    // A completed scan with no tripping connection.
    void scan_clean(std::uint64_t now_ms) noexcept { last_clean_ms_.store(now_ms ? now_ms : 1); }
    // A scan that could not complete (API error): the gate closes until the next clean scan.
    void scan_failed() noexcept { last_clean_ms_.store(0); }
    // Permanent for this session. Returns true for the call that tripped it.
    bool trip() noexcept { return !tripped_.exchange(true); }
    bool tripped() const noexcept { return tripped_.load(); }
    bool attested() const noexcept { return attested_.load(); }
    bool open(std::uint64_t now_ms) const noexcept;
    // Reason the gate is closed at `now_ms` (or "open").
    const char* why(std::uint64_t now_ms) const noexcept;

private:
    std::atomic<bool> attested_{ false };
    std::atomic<bool> tripped_{ false };
    std::atomic<std::uint64_t> last_clean_ms_{ 0 };
};

struct ScanResult {
    bool ok = false;                 // both tables (v4 and v6) were read
    std::uint32_t own_rows = 0;      // rows owned by this PID, any state
    std::uint32_t tripping = 0;      // rows that trip the guard
    char first_trip[96] = {};        // "remote state" of the first tripping row
    unsigned long error = 0;         // GetExtendedTcpTable error when !ok
};
// One read-only pass over the v4 and v6 TCP tables, filtered to `pid`.
ScanResult scan_process(std::uint32_t pid) noexcept;

// Worker thread: scans every `interval_ms`, feeds `g`, logs trips. Idempotent start; never stops.
bool start_worker(Gate* g, std::uint32_t interval_ms) noexcept;

} // namespace g6rt::guard
