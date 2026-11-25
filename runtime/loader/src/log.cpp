#include "log.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace g6rt::log {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::atomic<int> g_level{ static_cast<int>(Level::info) };
ULONGLONG g_t0 = 0;
const char* const kNames[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
} // namespace

bool open(const wchar_t* path) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) { ReleaseSRWLockExclusive(&g_lock); return true; }
    wchar_t prev[1200];
    if (std::swprintf(prev, 1200, L"%ls.prev", path) > 0) MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_t0 = GetTickCount64();
    const bool ok = g_file != INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void set_level(Level l) noexcept { g_level.store(static_cast<int>(l)); }

void vwrite(Level l, const char* who, const char* fmt, va_list ap) noexcept {
    if (static_cast<int>(l) < g_level.load(std::memory_order_relaxed)) return;
    char line[2048];
    const ULONGLONG t = GetTickCount64() - g_t0;
    int n = std::snprintf(line, sizeof line, "%8llu.%03llu %s [%5lu] %s: ", t / 1000, t % 1000,
                          kNames[static_cast<int>(l) & 3], GetCurrentThreadId(), who ? who : "?");
    if (n < 0) return;
    int m = std::vsnprintf(line + n, sizeof line - static_cast<size_t>(n) - 2, fmt, ap);
    if (m < 0) m = 0;
    size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
    if (len > sizeof line - 3) len = sizeof line - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(g_file, line, static_cast<DWORD>(len), &wr, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void write(Level l, const char* who, const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    vwrite(l, who, fmt, ap);
    va_end(ap);
}

} // namespace g6rt::log
