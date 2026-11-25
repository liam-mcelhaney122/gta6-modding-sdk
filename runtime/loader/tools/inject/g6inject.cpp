// g6inject: waits for the DexNom/VIctory title process (the `victory.exe run <exe> --execute` child,
// recognised by its `victory-image-<16 hex>.dll` module), then loads one DLL into it with
// VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW) and confirms the load with a
// module snapshot. x64 only. Exit codes: inject_core.hpp / README "Loading under DexNom".
#include "inject_core.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <clocale>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace g6inject {
namespace {

constexpr DWORD kPollMs = 250;
constexpr DWORD kRemoteThreadWaitMs = 60000;   // LoadLibraryW + the DLL's DllMain
constexpr int kSnapshotRetries = 50;           // ERROR_BAD_LENGTH / ERROR_PARTIAL_COPY while the target loads modules
constexpr std::size_t kMaxSnapshotEntries = 1u << 20;

ULONGLONG g_t0 = 0;

void logf(FILE* f, const char* tag, const char* fmt, va_list ap) {
    std::fprintf(f, "[g6inject +%6llums] %s", static_cast<unsigned long long>(GetTickCount64() - g_t0), tag);
    std::vfprintf(f, fmt, ap);
    std::fputc('\n', f);
    std::fflush(f);
}
void info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logf(stdout, "", fmt, ap);
    va_end(ap);
}
// Logs the error and returns `code`, so failure paths read `return fail(kX, "...")`.
int fail(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logf(stderr, "ERROR: ", fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[g6inject] exit %d (%s)\n", code, exit_name(code));
    std::fflush(stderr);
    return code;
}

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { reset(); h_ = std::exchange(o.h_, nullptr); }
        return *this;
    }
    ~Handle() { reset(); }
    void reset() noexcept {
        if (h_) CloseHandle(h_);
        h_ = nullptr;
    }
    HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }
private:
    HANDLE h_ = nullptr;
};

// Remote allocation, freed on scope exit unless leak() was called (a thread may still read it).
class RemoteBuffer {
public:
    RemoteBuffer(HANDLE proc, std::size_t size) noexcept
        : proc_(proc), p_(VirtualAllocEx(proc, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {}
    RemoteBuffer(const RemoteBuffer&) = delete;
    RemoteBuffer& operator=(const RemoteBuffer&) = delete;
    ~RemoteBuffer() { free(); }
    void* get() const noexcept { return p_; }
    bool free() noexcept {
        if (!p_) return true;
        const bool ok = VirtualFreeEx(proc_, p_, 0, MEM_RELEASE) != 0;
        p_ = nullptr;
        return ok;
    }
    void leak() noexcept { p_ = nullptr; }
private:
    HANDLE proc_;
    void* p_;
};

struct ModuleInfo {
    std::wstring name, path;
    std::uintptr_t base = 0;
    DWORD size = 0;
};

enum class SnapStatus { ok, access_denied, gone, failed };

// Every module of `pid` (64-bit view). Retries the transient errors a process that is still
// loading modules produces.
SnapStatus snapshot_modules(DWORD pid, std::vector<ModuleInfo>& out, DWORD& error) {
    out.clear();
    error = 0;
    for (int attempt = 0; attempt < kSnapshotRetries; ++attempt) {
        Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
        if (!snap) {
            error = GetLastError();
            if (error == ERROR_BAD_LENGTH || error == ERROR_PARTIAL_COPY) { Sleep(20); continue; }
            if (error == ERROR_ACCESS_DENIED) return SnapStatus::access_denied;
            if (error == ERROR_INVALID_PARAMETER) return SnapStatus::gone;
            return SnapStatus::failed;
        }
        MODULEENTRY32W me{};
        me.dwSize = sizeof me;
        if (Module32FirstW(snap.get(), &me)) {
            do {
                ModuleInfo m;
                m.name = me.szModule;
                m.path = me.szExePath;
                m.base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
                m.size = me.modBaseSize;
                out.push_back(std::move(m));
                me.dwSize = sizeof me;
            } while (out.size() < kMaxSnapshotEntries && Module32NextW(snap.get(), &me));
        }
        return SnapStatus::ok;
    }
    return SnapStatus::failed;
}

const ModuleInfo* find_image_module(const std::vector<ModuleInfo>& mods, const std::wstring& prefix) {
    for (const ModuleInfo& m : mods)
        if (is_image_module_name(m.name, prefix)) return &m;
    return nullptr;
}
const ModuleInfo* find_by_name(const std::vector<ModuleInfo>& mods, std::wstring_view name) {
    for (const ModuleInfo& m : mods)
        if (iequals(m.name, name) || iequals(base_name(m.path), name)) return &m;
    return nullptr;
}

struct ProcEntry { DWORD pid; std::wstring exe; };

bool list_processes(std::vector<ProcEntry>& out, DWORD& error) {
    out.clear();
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) { error = GetLastError(); return false; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap.get(), &pe)) {
        do {
            out.push_back({ pe.th32ProcessID, pe.szExeFile });
            pe.dwSize = sizeof pe;
        } while (out.size() < kMaxSnapshotEntries && Process32NextW(snap.get(), &pe));
    }
    return true;
}

// 0 = x64 process on an x64 machine, otherwise a description of why it is not.
const char* arch_problem(HANDLE proc, USHORT& process_machine, USHORT& native_machine) {
    process_machine = native_machine = 0;
    if (!IsWow64Process2(proc, &process_machine, &native_machine)) return "IsWow64Process2 failed";
    if (process_machine != IMAGE_FILE_MACHINE_UNKNOWN) return "a 32-bit (WOW64) process";
    if (native_machine != IMAGE_FILE_MACHINE_AMD64) return "not running on an x64 machine";
    return nullptr;
}

std::wstring process_image_path(HANDLE proc) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = static_cast<DWORD>(sizeof buf / sizeof buf[0]);
    if (!QueryFullProcessImageNameW(proc, 0, buf, &n)) return L"?";
    return std::wstring(buf, n);
}

bool is_exited(HANDLE proc) { return WaitForSingleObject(proc, 0) == WAIT_OBJECT_0; }

// ---- DLL file checks ----------------------------------------------------------------------------
int check_dll_file(const std::wstring& in, std::wstring& full) {
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(sizeof buf / sizeof buf[0]), buf, nullptr);
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return fail(kDllFile, "cannot resolve --dll path (error %lu)", GetLastError());
    std::wstring p(buf, n);
    wchar_t lbuf[32768];
    n = GetLongPathNameW(p.c_str(), lbuf, static_cast<DWORD>(sizeof lbuf / sizeof lbuf[0]));
    if (n == 0) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return fail(kDllFile, "--dll '%ls' does not exist", p.c_str());
        return fail(kDllFile, "--dll '%ls' cannot be resolved (error %lu)", p.c_str(), e);
    }
    if (n < sizeof lbuf / sizeof lbuf[0]) p.assign(lbuf, n);
    Handle f(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) return fail(kDllFile, "cannot open --dll '%ls' (error %lu)", p.c_str(), GetLastError());
    unsigned char hdr[4096] = {};
    DWORD got = 0;
    if (!ReadFile(f.get(), hdr, sizeof hdr, &got, nullptr)) return fail(kDllFile, "cannot read --dll '%ls' (error %lu)", p.c_str(), GetLastError());
    if (got < sizeof(IMAGE_DOS_HEADER) || hdr[0] != 'M' || hdr[1] != 'Z') return fail(kDllFile, "--dll '%ls' is not a PE file (no MZ)", p.c_str());
    LONG lfanew = 0;
    std::memcpy(&lfanew, hdr + offsetof(IMAGE_DOS_HEADER, e_lfanew), sizeof lfanew);
    if (lfanew < 0 || static_cast<std::size_t>(lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) > got)
        return fail(kDllFile, "--dll '%ls': e_lfanew 0x%lx outside the first %lu bytes", p.c_str(), static_cast<unsigned long>(lfanew), got);
    if (std::memcmp(hdr + lfanew, "PE\0\0", 4) != 0) return fail(kDllFile, "--dll '%ls' is not a PE file (no PE signature)", p.c_str());
    IMAGE_FILE_HEADER fh;
    std::memcpy(&fh, hdr + lfanew + 4, sizeof fh);
    if (fh.Machine != IMAGE_FILE_MACHINE_AMD64)
        return fail(kArchMismatch, "--dll '%ls' is machine 0x%04x, not x64 (0x8664)", p.c_str(), fh.Machine);
    if (!(fh.Characteristics & IMAGE_FILE_DLL)) return fail(kDllFile, "--dll '%ls' is not a DLL (IMAGE_FILE_DLL not set)", p.c_str());
    full = std::move(p);
    return kOk;
}

// ---- target selection ---------------------------------------------------------------------------
struct Candidate {
    DWORD pid = 0;
    Handle proc;            // PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE
    bool ready = false;     // image module present
    std::wstring image;     // image module name when ready
    const char* problem = nullptr;
    DWORD error = 0;
    int code = kOk;         // non-ok: why this candidate cannot be used
};

// Opens and inspects one process. `c.code` is kOk for a usable x64 process; `c.ready` says
// whether the image module is mapped yet.
void inspect(DWORD pid, const Options& o, Candidate& c) {
    c = Candidate{};
    c.pid = pid;
    c.proc = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!c.proc) {
        c.error = GetLastError();
        c.code = c.error == ERROR_ACCESS_DENIED ? kAccessDenied : c.error == ERROR_INVALID_PARAMETER ? kProcessExited : kInternal;
        c.problem = c.code == kAccessDenied ? "OpenProcess: access denied" : c.code == kProcessExited ? "not running" : "OpenProcess failed";
        return;
    }
    if (is_exited(c.proc.get())) { c.code = kProcessExited; c.problem = "exited"; return; }
    USHORT pm = 0, nm = 0;
    if (const char* why = arch_problem(c.proc.get(), pm, nm)) {
        c.code = kArchMismatch;
        c.problem = why;
        return;
    }
    std::vector<ModuleInfo> mods;
    const SnapStatus st = snapshot_modules(pid, mods, c.error);
    if (st == SnapStatus::access_denied) { c.code = kAccessDenied; c.problem = "module snapshot: access denied"; return; }
    if (st == SnapStatus::gone) { c.code = kProcessExited; c.problem = "exited"; return; }
    if (st == SnapStatus::failed) {
        // Transient during process start-up; the caller keeps polling.
        c.problem = "module snapshot failed";
        return;
    }
    if (const ModuleInfo* im = find_image_module(mods, o.image_prefix)) {
        c.ready = true;
        c.image = im->name;
    }
}

int select_target(const Options& o, Candidate& chosen) {
    const ULONGLONG start = GetTickCount64();
    const std::uint64_t max_polls = static_cast<std::uint64_t>(o.timeout_ms) / kPollMs + 2;
    bool logged_wait = false, any_named = false;
    std::vector<DWORD> logged_pids;
    for (std::uint64_t poll = 0;; ++poll) {
        if (o.has_pid) {
            if (chosen.proc && is_exited(chosen.proc.get()))
                return fail(kProcessExited, "pid %lu exited while waiting for its image module", static_cast<unsigned long>(o.pid));
            std::vector<ProcEntry> procs;
            DWORD err = 0;
            if (!list_processes(procs, err)) return fail(kInternal, "process snapshot failed (error %lu)", err);
            const ProcEntry* pe = nullptr;
            for (const ProcEntry& p : procs)
                if (p.pid == o.pid) { pe = &p; break; }
            if (!pe) return fail(kProcessExited, chosen.proc ? "pid %lu exited while waiting for its image module" : "pid %lu is not running",
                                 static_cast<unsigned long>(o.pid));
            if (!iequals(pe->exe, o.process))
                return fail(kUsage, "pid %lu is '%ls', not '%ls' (pass --process %ls to target it)", static_cast<unsigned long>(o.pid),
                            pe->exe.c_str(), o.process.c_str(), pe->exe.c_str());
            if (!chosen.proc) {
                inspect(o.pid, o, chosen);
                if (chosen.code != kOk)
                    return fail(chosen.code, "pid %lu (%ls): %s%s%s", static_cast<unsigned long>(o.pid), o.process.c_str(), chosen.problem,
                                chosen.error ? (" (error " + std::to_string(chosen.error) + ")").c_str() : "",
                                chosen.code == kAccessDenied ? "; if the target runs elevated, run g6inject elevated too" : "");
            } else {
                if (is_exited(chosen.proc.get())) return fail(kProcessExited, "pid %lu exited while waiting for its image module", static_cast<unsigned long>(o.pid));
                Candidate again;
                inspect(o.pid, o, again);
                if (again.code != kOk)
                    return fail(again.code, "pid %lu: %s%s", static_cast<unsigned long>(o.pid), again.problem,
                                again.error ? (" (error " + std::to_string(again.error) + ")").c_str() : "");
                chosen.ready = again.ready;
                chosen.image = again.image;
            }
            // The handle pins the process object, so the pid cannot be reused while we hold it.
            if (is_exited(chosen.proc.get())) return fail(kProcessExited, "pid %lu exited while waiting for its image module", static_cast<unsigned long>(o.pid));
            if (chosen.ready) return kOk;
            if (!logged_wait) {
                info("pid %lu (%ls) is running; waiting for a module '%ls*.dll'", static_cast<unsigned long>(o.pid), o.process.c_str(),
                     o.image_prefix.c_str());
                logged_wait = true;
            }
        } else {
            std::vector<ProcEntry> procs;
            DWORD err = 0;
            if (!list_processes(procs, err)) return fail(kInternal, "process snapshot failed (error %lu)", err);
            std::vector<Candidate> cands;
            for (const ProcEntry& p : procs) {
                if (!iequals(p.exe, o.process)) continue;
                cands.emplace_back();
                inspect(p.pid, o, cands.back());
                Candidate& c = cands.back();
                bool seen = false;
                for (DWORD d : logged_pids) seen = seen || d == c.pid;
                if (!seen) {
                    logged_pids.push_back(c.pid);
                    info("found %ls pid %lu: %s%s%ls", o.process.c_str(), static_cast<unsigned long>(c.pid),
                         c.code != kOk ? "unusable: " : c.ready ? "image module " : "image module not mapped yet",
                         c.code != kOk && c.problem ? c.problem : "", c.ready ? c.image.c_str() : L"");
                }
            }
            any_named = any_named || !cands.empty();
            std::size_t ready = 0, denied = 0, arch = 0, usable = 0;
            for (const Candidate& c : cands) {
                if (c.code == kOk) { ++usable; if (c.ready) ++ready; }
                else if (c.code == kAccessDenied) ++denied;
                else if (c.code == kArchMismatch) ++arch;
            }
            if (ready > 1) {
                std::wstring list;
                for (const Candidate& c : cands)
                    if (c.code == kOk && c.ready)
                        list += L"\n  pid " + std::to_wstring(c.pid) + L"  image " + c.image + L"  path " + process_image_path(c.proc.get());
                return fail(kAmbiguous, "%zu %ls processes have a '%ls*.dll' module; refusing without --pid. Candidates:%ls", ready,
                            o.process.c_str(), o.image_prefix.c_str(), list.c_str());
            }
            if (ready == 1) {
                for (Candidate& c : cands)
                    if (c.code == kOk && c.ready) { chosen = std::move(c); break; }
                if (cands.size() > 1) info("%zu %ls processes exist; only pid %lu has the image module", cands.size(), o.process.c_str(),
                                           static_cast<unsigned long>(chosen.pid));
                return kOk;
            }
            // Nothing ready. A refusal that waiting cannot change ends the wait now.
            if (usable == 0 && denied > 0)
                return fail(kAccessDenied, "every %ls process refused inspection (access denied); if the target runs elevated, run g6inject elevated too",
                            o.process.c_str());
            if (usable == 0 && arch > 0)
                return fail(kArchMismatch, "every %ls process is not an x64 process; g6inject and gta6mod_runtime are x64 only", o.process.c_str());
            if (!logged_wait) {
                info("waiting up to %lu ms for %ls with a module '%ls*.dll'", static_cast<unsigned long>(o.timeout_ms), o.process.c_str(),
                     o.image_prefix.c_str());
                logged_wait = true;
            }
        }
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= o.timeout_ms || poll >= max_polls) {
            if (o.has_pid)
                return fail(kTimeout, "pid %lu has no module '%ls*.dll' after %llu ms", static_cast<unsigned long>(o.pid), o.image_prefix.c_str(),
                            static_cast<unsigned long long>(elapsed));
            if (!any_named) return fail(kTimeout, "no process named %ls after %llu ms", o.process.c_str(), static_cast<unsigned long long>(elapsed));
            return fail(kTimeout, "%ls is running but no instance has a module '%ls*.dll' after %llu ms", o.process.c_str(), o.image_prefix.c_str(),
                        static_cast<unsigned long long>(elapsed));
        }
        const ULONGLONG left = o.timeout_ms - elapsed;
        Sleep(static_cast<DWORD>(left < kPollMs ? left : kPollMs));
    }
}

// ---- injection ----------------------------------------------------------------------------------
int inject(const Options& o, const std::wstring& dll, Candidate& target) {
    const DWORD pid = target.pid;
    const DWORD rights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                         PROCESS_VM_READ | SYNCHRONIZE;
    Handle proc(OpenProcess(rights, FALSE, pid));
    if (!proc) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
            return fail(kAccessDenied, "OpenProcess(pid %lu) for injection: access denied; if the target runs elevated, run g6inject elevated too",
                        static_cast<unsigned long>(pid));
        if (e == ERROR_INVALID_PARAMETER) return fail(kProcessExited, "pid %lu exited before injection", static_cast<unsigned long>(pid));
        return fail(kInjectFailed, "OpenProcess(pid %lu) failed (error %lu)", static_cast<unsigned long>(pid), e);
    }
    if (is_exited(proc.get())) return fail(kProcessExited, "pid %lu exited before injection", static_cast<unsigned long>(pid));
    info("target pid %lu: %ls", static_cast<unsigned long>(pid), process_image_path(proc.get()).c_str());

    std::vector<ModuleInfo> mods;
    DWORD err = 0;
    switch (snapshot_modules(pid, mods, err)) {
    case SnapStatus::ok: break;
    case SnapStatus::access_denied: return fail(kAccessDenied, "module snapshot of pid %lu: access denied", static_cast<unsigned long>(pid));
    case SnapStatus::gone: return fail(kProcessExited, "pid %lu exited before injection", static_cast<unsigned long>(pid));
    default: return fail(kInjectFailed, "module snapshot of pid %lu failed (error %lu)", static_cast<unsigned long>(pid), err);
    }
    const ModuleInfo* image = find_image_module(mods, o.image_prefix);
    if (!image) return fail(kInjectFailed, "image module '%ls*.dll' disappeared from pid %lu", o.image_prefix.c_str(), static_cast<unsigned long>(pid));
    info("image module %ls at 0x%llx size 0x%lx (%ls)", image->name.c_str(), static_cast<unsigned long long>(image->base),
         static_cast<unsigned long>(image->size), image->path.c_str());
    const std::wstring_view dll_name = base_name(dll);
    if (const ModuleInfo* dup = find_by_name(mods, dll_name))
        return fail(kAlreadyLoaded, "a module named %ls is already loaded in pid %lu from '%ls' at 0x%llx; refusing to load a second copy",
                    std::wstring(dll_name).c_str(), static_cast<unsigned long>(pid), dup->path.c_str(), static_cast<unsigned long long>(dup->base));

    // LoadLibraryW in the target = target kernel32 base + the same RVA as here.
    const ModuleInfo* rk32 = find_by_name(mods, L"kernel32.dll");
    if (!rk32) return fail(kInjectFailed, "kernel32.dll not found in pid %lu (process still initialising?)", static_cast<unsigned long>(pid));
    HMODULE lk32 = GetModuleHandleW(L"kernel32.dll");
    const auto load_library = reinterpret_cast<std::uintptr_t>(lk32 ? GetProcAddress(lk32, "LoadLibraryW") : nullptr);
    if (!lk32 || !load_library) return fail(kInternal, "LoadLibraryW not found in this process");
    const std::uintptr_t rva = load_library - reinterpret_cast<std::uintptr_t>(lk32);
    if (rva >= rk32->size) return fail(kInternal, "LoadLibraryW RVA 0x%llx outside the target's kernel32 (size 0x%lx)",
                                       static_cast<unsigned long long>(rva), static_cast<unsigned long>(rk32->size));
    const std::uintptr_t remote_ll = rk32->base + rva;
    info("kernel32 in target at 0x%llx (here 0x%llx); LoadLibraryW at 0x%llx", static_cast<unsigned long long>(rk32->base),
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(lk32)), static_cast<unsigned long long>(remote_ll));

    const std::size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    RemoteBuffer buf(proc.get(), bytes);
    if (!buf.get()) {
        const DWORD e = GetLastError();
        return fail(e == ERROR_ACCESS_DENIED ? kAccessDenied : kInjectFailed, "VirtualAllocEx(%zu bytes) failed (error %lu)", bytes, e);
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(proc.get(), buf.get(), dll.c_str(), bytes, &written) || written != bytes) {
        const DWORD e = GetLastError();
        return fail(e == ERROR_ACCESS_DENIED ? kAccessDenied : kInjectFailed, "WriteProcessMemory wrote %zu of %zu bytes (error %lu)",
                    static_cast<std::size_t>(written), bytes, e);
    }
    info("path written to 0x%llx (%zu bytes)", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(buf.get())), bytes);

    DWORD tid = 0;
    Handle thread(CreateRemoteThread(proc.get(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_ll), buf.get(), 0, &tid));
    if (!thread) {
        const DWORD e = GetLastError();
        return fail(e == ERROR_ACCESS_DENIED ? kAccessDenied : kInjectFailed, "CreateRemoteThread failed (error %lu)", e);
    }
    info("remote thread %lu started: LoadLibraryW(\"%ls\")", static_cast<unsigned long>(tid), dll.c_str());
    const HANDLE waits[2] = { thread.get(), proc.get() };
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, kRemoteThreadWaitMs);
    if (w == WAIT_OBJECT_0 + 1) {
        buf.leak();
        return fail(kProcessExited, "pid %lu exited while LoadLibraryW was running", static_cast<unsigned long>(pid));
    }
    if (w != WAIT_OBJECT_0) {
        buf.leak();  // the thread may still read the path
        return fail(kInjectFailed, "remote LoadLibraryW thread did not finish within %lu ms (wait %lu, error %lu); the path buffer is left allocated",
                    static_cast<unsigned long>(kRemoteThreadWaitMs), w, GetLastError());
    }
    DWORD code = 0;
    GetExitCodeThread(thread.get(), &code);
    info("remote thread finished; exit code 0x%08lx (low 32 bits of the HMODULE; a hint only)", static_cast<unsigned long>(code));
    if (!buf.free()) info("warning: VirtualFreeEx of the path buffer failed (error %lu)", GetLastError());

    switch (snapshot_modules(pid, mods, err)) {
    case SnapStatus::ok: break;
    case SnapStatus::gone: return fail(kProcessExited, "pid %lu exited right after LoadLibraryW", static_cast<unsigned long>(pid));
    case SnapStatus::access_denied: return fail(kAccessDenied, "confirmation snapshot of pid %lu: access denied", static_cast<unsigned long>(pid));
    default: return fail(kInjectFailed, "confirmation snapshot of pid %lu failed (error %lu)", static_cast<unsigned long>(pid), err);
    }
    const ModuleInfo* loaded = find_by_name(mods, dll_name);
    if (!loaded)
        return fail(kInjectFailed, "%ls is not in pid %lu's module list after LoadLibraryW (thread exit code 0x%08lx): the load failed in the target. "
                    "Check that its dependencies resolve there and that the target can read '%ls'",
                    std::wstring(dll_name).c_str(), static_cast<unsigned long>(pid), static_cast<unsigned long>(code), dll.c_str());
    if (!iequals(loaded->path, dll))
        info("warning: the module list reports '%ls', the request was '%ls'", loaded->path.c_str(), dll.c_str());
    if (code != 0 && static_cast<DWORD>(loaded->base) != code)
        info("warning: thread exit code 0x%08lx does not match the module base 0x%llx", static_cast<unsigned long>(code),
             static_cast<unsigned long long>(loaded->base));
    info("confirmed: %ls loaded in pid %lu at 0x%llx size 0x%lx", loaded->path.c_str(), static_cast<unsigned long>(pid),
         static_cast<unsigned long long>(loaded->base), static_cast<unsigned long>(loaded->size));
    return kOk;
}

void usage() {
    std::printf(
        "g6inject --dll <absolute path> [--process victory.exe] [--pid <n>] [--timeout-ms 300000]\n"
        "         [--image-prefix victory-image-]\n"
        "Waits for a process named --process that has a module '<image-prefix>*.dll', then loads --dll into it\n"
        "with CreateRemoteThread(LoadLibraryW) and confirms the load with a module snapshot. x64 only.\n"
        "With several matching processes, --pid is required.\n"
        "Exit codes: 0 ok, 1 usage, 2 dll-file, 3 timeout, 4 ambiguous, 5 access-denied, 6 arch-mismatch,\n"
        "            7 already-loaded, 8 process-exited, 9 inject-failed, 10 internal\n");
}

} // namespace
} // namespace g6inject

int wmain(int argc, wchar_t** argv) {
    using namespace g6inject;
    std::setlocale(LC_ALL, ".UTF8");
    g_t0 = GetTickCount64();
    Options o;
    const std::string err = parse_args(argc, argv, o);
    if (o.help) { usage(); return kOk; }
    if (!err.empty()) {
        usage();
        return fail(kUsage, "%s", err.c_str());
    }
    info("dll=%ls process=%ls pid=%s timeout_ms=%lu image_prefix=%ls", o.dll.c_str(), o.process.c_str(),
         o.has_pid ? std::to_string(o.pid).c_str() : "(any)", static_cast<unsigned long>(o.timeout_ms), o.image_prefix.c_str());
    std::wstring dll;
    if (const int rc = check_dll_file(o.dll, dll); rc != kOk) return rc;
    info("dll file ok: x64 DLL '%ls'", dll.c_str());
    try {
        Candidate target;
        if (const int rc = select_target(o, target); rc != kOk) return rc;
        info("selected pid %lu (%ls)", static_cast<unsigned long>(target.pid), target.image.c_str());
        const int rc = inject(o, dll, target);
        if (rc == kOk) info("exit 0 (ok)");
        return rc;
    } catch (const std::exception& e) {
        return fail(kInternal, "exception: %s", e.what());
    }
}
