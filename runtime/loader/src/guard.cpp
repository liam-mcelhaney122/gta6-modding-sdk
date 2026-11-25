#include "guard.hpp"

#include "log.hpp"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace g6rt::guard {
namespace {

const char* state_str(std::uint32_t s) noexcept {
    switch (s) {
    case kStateSynSent: return "SYN_SENT";
    case kStateEstablished: return "ESTABLISHED";
    default: return "other";
    }
}

// Returns false on an API error (err set). Grows the buffer when the table grows between calls.
bool read_table(ULONG family, std::vector<std::uint8_t>& buf, unsigned long& err) noexcept {
    try {
        DWORD size = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            DWORD r = GetExtendedTcpTable(buf.empty() ? nullptr : buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
            if (r == NO_ERROR) return true;
            if (r != ERROR_INSUFFICIENT_BUFFER) { err = r; return false; }
            buf.assign(static_cast<std::size_t>(size) + 4096, 0);
            size = static_cast<DWORD>(buf.size());
        }
        err = ERROR_INSUFFICIENT_BUFFER;
        return false;
    } catch (...) {
        err = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

void note(ScanResult& r, bool v6, const std::uint8_t* a, DWORD port_net, std::uint32_t state, AddrClass c) noexcept {
    ++r.tripping;
    if (r.first_trip[0]) return;
    char rem[72];
    const std::uint16_t port = static_cast<std::uint16_t>(((port_net & 0xFF) << 8) | ((port_net >> 8) & 0xFF));   // network -> host order
    format_remote(v6, a, port, rem, sizeof rem);
    std::snprintf(r.first_trip, sizeof r.first_trip, "%s %s (%s)", rem, state_str(state), class_str(c));
}

struct Worker { Gate* g; std::uint32_t interval; };

DWORD WINAPI worker_main(void* p) {
    const Worker w = *static_cast<Worker*>(p);
    delete static_cast<Worker*>(p);
    const DWORD pid = GetCurrentProcessId();
    bool logged_fail = false, logged_first = false;
    for (;;) {
        const ScanResult r = scan_process(pid);
        const std::uint64_t now = GetTickCount64();
        if (!r.ok) {
            w.g->scan_failed();
            if (!logged_fail) {
                G6LOG_W("guard: GetExtendedTcpTable failed (error %lu); guard closed until a scan succeeds", r.error);
                logged_fail = true;
            }
        } else if (r.tripping) {
            if (w.g->trip())
                G6LOG_E("guard: CLOSED for this session: %u connection(s) to a non-loopback, non-link-local remote, first %s. "
                        "Plugin natives and the mods/ overlay are refused until the game is restarted.", r.tripping, r.first_trip);
        } else {
            w.g->scan_clean(now);
            logged_fail = false;
            if (!logged_first) {
                G6LOG_I("guard: first connection scan clean (%u own TCP row(s), none to a remote); interval %u ms", r.own_rows, w.interval);
                logged_first = true;
            }
        }
        Sleep(w.interval);
    }
}

std::atomic<bool> g_started{ false };

} // namespace

const char* class_str(AddrClass c) noexcept {
    switch (c) {
    case AddrClass::loopback: return "loopback";
    case AddrClass::link_local: return "link-local";
    case AddrClass::unspecified: return "unspecified";
    case AddrClass::private_net: return "private";
    default: return "public";
    }
}

AddrClass classify_v4(const std::uint8_t a[4]) noexcept {
    if (a[0] == 127) return AddrClass::loopback;
    if (a[0] == 169 && a[1] == 254) return AddrClass::link_local;
    if (a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0) return AddrClass::unspecified;
    if (a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
        (a[0] == 100 && (a[1] & 0xC0) == 64))
        return AddrClass::private_net;
    return AddrClass::public_net;
}

AddrClass classify_v6(const std::uint8_t a[16]) noexcept {
    static const std::uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    if (std::memcmp(a, mapped, 12) == 0) return classify_v4(a + 12);
    bool zero10 = true;
    for (int i = 0; i < 15; ++i) zero10 &= a[i] == 0;
    if (zero10 && a[15] == 1) return AddrClass::loopback;
    if (zero10 && a[15] == 0) return AddrClass::unspecified;
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return AddrClass::link_local;   // fe80::/10
    if ((a[0] & 0xFE) == 0xFC) return AddrClass::private_net;                  // fc00::/7 (ULA)
    return AddrClass::public_net;
}

bool trips(AddrClass c, std::uint32_t state) noexcept {
    if (state != kStateEstablished && state != kStateSynSent) return false;
    return c != AddrClass::loopback && c != AddrClass::link_local;
}

void format_remote(bool v6, const std::uint8_t* a, std::uint16_t port, char* out, std::size_t cap) noexcept {
    if (!v6) {
        std::snprintf(out, cap, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], port);
        return;
    }
    std::snprintf(out, cap, "[%x:%x:%x:%x:%x:%x:%x:%x]:%u", (a[0] << 8) | a[1], (a[2] << 8) | a[3], (a[4] << 8) | a[5],
                  (a[6] << 8) | a[7], (a[8] << 8) | a[9], (a[10] << 8) | a[11], (a[12] << 8) | a[13], (a[14] << 8) | a[15], port);
}

bool Gate::open(std::uint64_t now_ms) const noexcept {
    if (!attested_.load() || tripped_.load()) return false;
    const std::uint64_t t = last_clean_ms_.load();
    return t != 0 && now_ms >= t && now_ms - t <= kStaleMs;
}

const char* Gate::why(std::uint64_t now_ms) const noexcept {
    if (tripped_.load()) return "a connection to a remote host was seen (closed for this session)";
    if (!attested_.load()) return "[guard] offline_attested is not 1";
    const std::uint64_t t = last_clean_ms_.load();
    if (t == 0) return "no successful connection scan yet";
    if (now_ms < t || now_ms - t > kStaleMs) return "last clean connection scan is stale";
    return "open";
}

ScanResult scan_process(std::uint32_t pid) noexcept {
    ScanResult r;
    std::vector<std::uint8_t> b4, b6;
    if (!read_table(AF_INET, b4, r.error) || !read_table(AF_INET6, b6, r.error)) return r;
    const auto* t4 = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(b4.data());
    for (DWORD i = 0; t4 && i < t4->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = t4->table[i];
        if (row.dwOwningPid != pid) continue;
        ++r.own_rows;
        std::uint8_t a[4];
        std::memcpy(a, &row.dwRemoteAddr, 4);   // network order
        const AddrClass c = classify_v4(a);
        if (trips(c, row.dwState)) note(r, false, a, row.dwRemotePort, row.dwState, c);
    }
    const auto* t6 = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(b6.data());
    for (DWORD i = 0; t6 && i < t6->dwNumEntries; ++i) {
        const MIB_TCP6ROW_OWNER_PID& row = t6->table[i];
        if (row.dwOwningPid != pid) continue;
        ++r.own_rows;
        const AddrClass c = classify_v6(row.ucRemoteAddr);
        if (trips(c, row.dwState)) note(r, true, row.ucRemoteAddr, row.dwRemotePort, row.dwState, c);
    }
    r.ok = true;
    return r;
}

bool start_worker(Gate* g, std::uint32_t interval_ms) noexcept {
    if (g_started.exchange(true)) return true;
    auto* w = new (std::nothrow) Worker{ g, interval_ms };
    if (!w) { g_started.store(false); return false; }
    HANDLE t = CreateThread(nullptr, 0, worker_main, w, 0, nullptr);
    if (!t) { delete w; g_started.store(false); return false; }
    CloseHandle(t);
    return true;
}

} // namespace g6rt::guard
