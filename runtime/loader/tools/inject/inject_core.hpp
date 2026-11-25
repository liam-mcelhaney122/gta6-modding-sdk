// g6inject: argument parsing, name matching and exit codes. No process access here, so the
// offline test (tests/inject/inject_test.cpp) checks these directly.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace g6inject {

// Documented exit codes (README "Loading under DexNom"). Every failure path maps to exactly one.
enum Exit : int {
    kOk = 0,             // DLL loaded and confirmed by a module snapshot of the target
    kUsage = 1,          // bad or missing arguments; --pid names a process with another name
    kDllFile = 2,        // --dll missing, unreadable, not a PE file or not a DLL
    kTimeout = 3,        // no matching process with the image module within --timeout-ms
    kAmbiguous = 4,      // several matching processes and no --pid: refused, all listed
    kAccessDenied = 5,   // the target could not be opened/inspected with the rights needed
    kArchMismatch = 6,   // target process or --dll is not x64
    kAlreadyLoaded = 7,  // a module with the DLL's file name is already loaded in the target: refused
    kProcessExited = 8,  // the target exited (or --pid was not running) before injection finished
    kInjectFailed = 9,   // allocation/write/remote thread failed, or the DLL is absent afterwards
    kInternal = 10,      // unexpected local failure (snapshot of this machine's processes, out of memory)
};

inline const char* exit_name(int code) noexcept {
    switch (code) {
    case kOk: return "ok";
    case kUsage: return "usage";
    case kDllFile: return "dll-file";
    case kTimeout: return "timeout";
    case kAmbiguous: return "ambiguous";
    case kAccessDenied: return "access-denied";
    case kArchMismatch: return "arch-mismatch";
    case kAlreadyLoaded: return "already-loaded";
    case kProcessExited: return "process-exited";
    case kInjectFailed: return "inject-failed";
    case kInternal: return "internal";
    default: return "?";
    }
}

inline constexpr std::uint32_t kDefaultTimeoutMs = 300000;
inline constexpr std::uint32_t kMaxTimeoutMs = 86400000;  // 24 h

struct Options {
    std::wstring dll;
    std::wstring process = L"victory.exe";
    std::wstring image_prefix = L"victory-image-";
    std::uint32_t pid = 0;
    bool has_pid = false;
    std::uint32_t timeout_ms = kDefaultTimeoutMs;
    bool help = false;
};

// Ordinal, case-insensitive (the same folding the file system uses for names).
inline bool iequals(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    if (a.size() > 0x7FFFFFFF) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
inline bool istarts_with(std::wstring_view s, std::wstring_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}
inline bool iends_with(std::wstring_view s, std::wstring_view suffix) noexcept {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// File name after the last '\' or '/'.
inline std::wstring_view base_name(std::wstring_view path) noexcept {
    const std::size_t k = path.find_last_of(L"\\/");
    return k == std::wstring_view::npos ? path : path.substr(k + 1);
}

// "C:\..." or "\\server\share\..." (also "\\?\C:\..."). Drive-relative "C:x" and rooted "\x" are not absolute.
inline bool is_absolute_path(std::wstring_view p) noexcept {
    if (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' &&
        (p[2] == L'\\' || p[2] == L'/'))
        return true;
    return p.size() >= 3 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/') && p[2] != L'\\' && p[2] != L'/';
}

// The host maps the title as "<prefix><16 hex>.dll"; any "<prefix>*.dll" with a non-empty middle matches.
inline bool is_image_module_name(std::wstring_view name, std::wstring_view prefix) noexcept {
    return !prefix.empty() && name.size() > prefix.size() + 4 && istarts_with(name, prefix) && iends_with(name, L".dll");
}

// Decimal digits only, no sign, no overflow.
inline bool parse_u32(std::wstring_view s, std::uint32_t& out) noexcept {
    if (s.empty() || s.size() > 10) return false;
    std::uint64_t v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - L'0');
    }
    if (v > 0xFFFFFFFFull) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

// A bare file name: not empty, no path separators or drive colon.
inline bool is_bare_name(std::wstring_view s) noexcept {
    return !s.empty() && s.find_first_of(L"\\/:") == std::wstring_view::npos;
}

// For messages: printable ASCII kept, anything else becomes '?'.
inline std::string narrow_ascii(std::wstring_view s) {
    std::string r;
    r.reserve(s.size());
    for (wchar_t c : s) r.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    return r;
}

// Returns an empty string on success, otherwise the reason (ASCII).
inline std::string parse_args(int argc, const wchar_t* const* argv, Options& o) {
    o = Options{};
    bool seen_dll = false, seen_proc = false, seen_pid = false, seen_to = false, seen_pfx = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i] ? argv[i] : L"";
        if (a == L"--help" || a == L"-h" || a == L"/?") { o.help = true; return {}; }
        auto value = [&](bool& seen, const char* name, std::wstring_view& v) -> std::string {
            if (seen) return std::string("duplicate ") + name;
            seen = true;
            if (i + 1 >= argc || !argv[i + 1] || !argv[i + 1][0]) return std::string(name) + " needs a value";
            v = argv[++i];
            return {};
        };
        std::wstring_view v;
        std::string e;
        if (a == L"--dll") {
            if (!(e = value(seen_dll, "--dll", v)).empty()) return e;
            if (!is_absolute_path(v)) return "--dll must be an absolute path (C:\\... or \\\\server\\...)";
            o.dll.assign(v);
        } else if (a == L"--process") {
            if (!(e = value(seen_proc, "--process", v)).empty()) return e;
            if (!is_bare_name(v)) return "--process must be a file name such as victory.exe, not a path";
            o.process.assign(v);
        } else if (a == L"--pid") {
            if (!(e = value(seen_pid, "--pid", v)).empty()) return e;
            if (!parse_u32(v, o.pid) || o.pid == 0) return "--pid must be a decimal process id > 0";
            o.has_pid = true;
        } else if (a == L"--timeout-ms") {
            if (!(e = value(seen_to, "--timeout-ms", v)).empty()) return e;
            if (!parse_u32(v, o.timeout_ms) || o.timeout_ms > kMaxTimeoutMs) return "--timeout-ms must be a decimal number 0..86400000";
        } else if (a == L"--image-prefix") {
            if (!(e = value(seen_pfx, "--image-prefix", v)).empty()) return e;
            if (!is_bare_name(v)) return "--image-prefix must be a non-empty file name prefix";
            o.image_prefix.assign(v);
        } else {
            return "unknown argument '" + narrow_ascii(a) + "' (see --help)";
        }
    }
    if (!seen_dll) return "--dll is required";
    return {};
}

} // namespace g6inject
