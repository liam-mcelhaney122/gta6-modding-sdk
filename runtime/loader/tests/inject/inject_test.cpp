// Offline test for g6inject.exe. Runs from build\inject_test\ next to:
//   g6inject_target.exe, victory-image-0000000000000000.dll, g6inject_marker.dll   (x64)
//   g6inject_target32.exe, g6inject_marker32.dll                                    (x86, arch tests)
// Usage: inject_test.exe <path to g6inject.exe>
// Each case starts stand-in targets, runs the injector as a child process, and checks its exit code,
// its log and (for successful loads) the marker file the payload writes inside the target.
#include "inject_core.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0, g_skip = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { ++g_pass; }                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
               std::printf(__VA_ARGS__); std::printf("\n"); }                   \
    } while (0)

static std::wstring g_dir;       // folder of this exe, with trailing backslash
static std::wstring g_injector;  // g6inject.exe
static const wchar_t* kImage = L"victory-image-0000000000000000.dll";

static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ------------------------------------------------------------------ pure parts (inject_core.hpp)
static int parse(std::vector<const wchar_t*> args, g6inject::Options& o, std::string* err = nullptr) {
    args.insert(args.begin(), L"g6inject.exe");
    const std::string e = g6inject::parse_args(static_cast<int>(args.size()), args.data(), o);
    if (err) *err = e;
    return e.empty() ? 1 : 0;
}

static void test_core() {
    using namespace g6inject;
    Options o;
    std::string e;
    CHECK(parse({ L"--dll", L"C:\\x\\gta6mod_runtime.dll" }, o) == 1 && o.dll == L"C:\\x\\gta6mod_runtime.dll" &&
              o.process == L"victory.exe" && !o.has_pid && o.timeout_ms == 300000 && o.image_prefix == L"victory-image-",
          "defaults");
    CHECK(parse({ L"--dll", L"\\\\srv\\share\\a.dll", L"--pid", L"1234", L"--timeout-ms", L"0", L"--process", L"x.exe",
                  L"--image-prefix", L"img-" }, o) == 1 &&
              o.has_pid && o.pid == 1234 && o.timeout_ms == 0 && o.process == L"x.exe" && o.image_prefix == L"img-",
          "all options");
    CHECK(parse({}, o, &e) == 0 && e.find("--dll is required") != std::string::npos, "no --dll: %s", e.c_str());
    CHECK(parse({ L"--dll", L"a.dll" }, o, &e) == 0 && e.find("absolute") != std::string::npos, "relative dll: %s", e.c_str());
    CHECK(parse({ L"--dll", L"C:a.dll" }, o) == 0, "drive-relative dll refused");
    CHECK(parse({ L"--dll", L"\\a.dll" }, o) == 0, "rooted-not-absolute dll refused");
    CHECK(parse({ L"--dll" }, o, &e) == 0 && e.find("needs a value") != std::string::npos, "missing value: %s", e.c_str());
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--dll", L"C:\\b.dll" }, o, &e) == 0 && e.find("duplicate") != std::string::npos, "dup");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--pid", L"0" }, o) == 0, "pid 0 refused");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--pid", L"-5" }, o) == 0, "negative pid refused");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--pid", L"4294967296" }, o) == 0, "pid overflow refused");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--pid", L"4294967295" }, o) == 1 && o.pid == 0xFFFFFFFFu, "pid max");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--timeout-ms", L"86400001" }, o) == 0, "timeout cap");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--timeout-ms", L"1e3" }, o) == 0, "timeout must be decimal");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--process", L"C:\\victory.exe" }, o) == 0, "process must be a bare name");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--image-prefix", L"a/b" }, o) == 0, "prefix must be a bare name");
    CHECK(parse({ L"--dll", L"C:\\a.dll", L"--bogus" }, o, &e) == 0 && e.find("'--bogus'") != std::string::npos, "unknown arg: %s", e.c_str());
    CHECK(parse({ L"--help" }, o) == 1 && o.help, "help");

    CHECK(is_image_module_name(L"victory-image-0123456789abcdef.dll", L"victory-image-"), "image name");
    CHECK(is_image_module_name(L"VICTORY-IMAGE-0123456789ABCDEF.DLL", L"victory-image-"), "image name, other case");
    CHECK(!is_image_module_name(L"victory-image-.dll", L"victory-image-"), "empty middle refused");
    CHECK(!is_image_module_name(L"victory-image-0123.exe", L"victory-image-"), "not .dll");
    CHECK(!is_image_module_name(L"victory.exe", L"victory-image-"), "host exe");
    CHECK(!is_image_module_name(L"xvictory-image-0123.dll", L"victory-image-"), "prefix must be at the start");
    CHECK(!is_image_module_name(L"victory-image-0123.dll", L""), "empty prefix never matches");
    CHECK(base_name(L"C:\\a\\b\\c.dll") == L"c.dll" && base_name(L"C:/a/c.dll") == L"c.dll" && base_name(L"c.dll") == L"c.dll", "base_name");
    CHECK(iequals(L"Victory.EXE", L"victory.exe") && !iequals(L"victory.exe", L"victory.ex"), "iequals");
    CHECK(is_absolute_path(L"C:\\a") && is_absolute_path(L"z:/a") && is_absolute_path(L"\\\\?\\C:\\a") && !is_absolute_path(L"\\\\") &&
              !is_absolute_path(L"a\\b"),
          "is_absolute_path");
    CHECK(std::strcmp(exit_name(kAlreadyLoaded), "already-loaded") == 0 && std::strcmp(exit_name(99), "?") == 0, "exit_name");
    std::printf("core: done\n");
}

// ------------------------------------------------------------------ processes
struct Child {
    HANDLE proc = nullptr;
    DWORD pid = 0;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { kill(); }
    void kill() {
        if (!proc) return;
        TerminateProcess(proc, 1);
        WaitForSingleObject(proc, 5000);
        CloseHandle(proc);
        proc = nullptr;
    }
};

static bool start(Child& c, const std::wstring& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + g_dir + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    c.proc = pi.hProcess;
    c.pid = pi.dwProcessId;
    return true;
}

// Polls the 64-bit module list of `pid` until the image module appears (bounded).
static bool wait_image(DWORD pid, DWORD timeout_ms = 10000) {
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < timeout_ms) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (s != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof me;
            bool found = false;
            if (Module32FirstW(s, &me)) {
                do {
                    if (g6inject::is_image_module_name(me.szModule, L"victory-image-")) found = true;
                    me.dwSize = sizeof me;
                } while (!found && Module32NextW(s, &me));
            }
            CloseHandle(s);
            if (found) return true;
        }
        Sleep(20);
    }
    return false;
}

struct Run { int code = -1; std::string out; DWORD ms = 0; };

static Run run_injector(const std::wstring& args, DWORD limit_ms = 60000) {
    Run r;
    const std::wstring outp = g_dir + L"inject_out.txt";
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE out = CreateFileW(outp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { r.out = "cannot create inject_out.txt"; return r; }
    std::wstring cmd = L"\"" + g_injector + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const ULONGLONG t0 = GetTickCount64();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g_dir.c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) { r.out = "cannot start g6inject.exe"; return r; }
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, limit_ms) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 255);
        WaitForSingleObject(pi.hProcess, 5000);
        r.out = "g6inject did not finish in time";
    }
    r.ms = static_cast<DWORD>(GetTickCount64() - t0);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    r.code = static_cast<int>(code);
    HANDLE in = CreateFileW(outp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        char buf[8192];
        DWORD got = 0;
        while (r.out.size() < (1u << 20) && ReadFile(in, buf, sizeof buf, &got, nullptr) && got) r.out.append(buf, got);
        CloseHandle(in);
    }
    return r;
}

static void show(const char* name, const Run& r) {
    std::printf("--- %s: exit %d (%s) in %lu ms\n", name, r.code, g6inject::exit_name(r.code), static_cast<unsigned long>(r.ms));
    std::size_t a = 0;
    while (a < r.out.size()) {
        std::size_t b = r.out.find('\n', a);
        if (b == std::string::npos) b = r.out.size();
        std::size_t e = b;
        if (e > a && r.out[e - 1] == '\r') --e;
        std::printf("    %.*s\n", static_cast<int>(e - a), r.out.c_str() + a);
        a = b + 1;
    }
}
static bool has(const Run& r, const char* s) { return r.out.find(s) != std::string::npos; }

static std::wstring marker_path(DWORD pid) { return g_dir + L"marker_" + std::to_wstring(pid) + L".txt"; }
static std::string read_small(const std::wstring& p) {
    std::string s;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    char buf[64];
    DWORD got = 0;
    if (ReadFile(f, buf, sizeof buf, &got, nullptr)) s.assign(buf, got);
    CloseHandle(f);
    return s;
}
static void clear_markers() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"marker_*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do { DeleteFileW((g_dir + fd.cFileName).c_str()); } while (++n < 10000 && FindNextFileW(h, &fd));
    FindClose(h);
}

// ------------------------------------------------------------------ end-to-end
static void test_e2e() {
    using namespace g6inject;
    const std::wstring marker = g_dir + L"g6inject_marker.dll";
    const std::wstring dll = L" --dll \"" + marker + L"\"";
    const std::wstring proc = L" --process g6inject_target.exe";
    clear_markers();

    {  // usage and DLL-file refusals
        Run r = run_injector(L"");
        show("no arguments", r);
        CHECK(r.code == kUsage && has(r, "--dll is required"), "no args -> usage (%d)", r.code);
        r = run_injector(L"--dll g6inject_marker.dll");
        show("relative --dll", r);
        CHECK(r.code == kUsage && has(r, "absolute"), "relative dll -> usage (%d)", r.code);
        r = run_injector(L"--dll \"" + g_dir + L"no_such.dll\"" + proc);
        show("missing --dll", r);
        CHECK(r.code == kDllFile && has(r, "does not exist"), "missing dll -> dll-file (%d)", r.code);
        r = run_injector(L"--dll \"" + g_dir + L"g6inject_target.exe\"" + proc);
        show("--dll is an exe", r);
        CHECK(r.code == kDllFile && has(r, "not a DLL"), "exe as dll -> dll-file (%d)", r.code);
        if (exists(g_dir + L"g6inject_marker32.dll")) {
            r = run_injector(L"--dll \"" + g_dir + L"g6inject_marker32.dll\"" + proc);
            show("x86 --dll", r);
            CHECK(r.code == kArchMismatch && has(r, "not x64"), "x86 dll -> arch-mismatch (%d)", r.code);
        } else {
            ++g_skip;
            std::printf("SKIP x86 --dll (g6inject_marker32.dll not built)\n");
        }
    }
    {  // timeout: no such process
        Run r = run_injector(L"--process g6inject_nosuch.exe --timeout-ms 1000" + dll);
        show("timeout, no process", r);
        CHECK(r.code == kTimeout && has(r, "no process named g6inject_nosuch.exe"), "no process -> timeout (%d)", r.code);
        CHECK(r.ms >= 900 && r.ms < 15000, "timeout honoured: %lu ms", static_cast<unsigned long>(r.ms));
    }
    {  // timeout: process without the image module
        Child t;
        CHECK(start(t, L"g6inject_target.exe", L""), "start target without image");
        Run r = run_injector(proc + L" --timeout-ms 1500" + dll);
        show("timeout, no image module", r);
        CHECK(r.code == kTimeout && has(r, "no instance has a module 'victory-image-*.dll'"), "no image -> timeout (%d)", r.code);
        CHECK(!exists(marker_path(t.pid)), "nothing injected without the image module");
    }
    {  // process exits while waited for (--pid)
        Child t;
        CHECK(start(t, L"g6inject_target.exe", L"--exit-after-ms 1500"), "start short-lived target");
        Run r = run_injector(proc + L" --pid " + std::to_wstring(t.pid) + L" --timeout-ms 20000" + dll);
        show("process exits mid-wait", r);
        CHECK(r.code == kProcessExited && has(r, "exited while waiting"), "exit mid-wait -> process-exited (%d)", r.code);
        CHECK(r.ms < 10000, "noticed the exit promptly: %lu ms", static_cast<unsigned long>(r.ms));
        r = run_injector(proc + L" --pid " + std::to_wstring(t.pid) + L" --timeout-ms 1000" + dll);
        show("--pid not running", r);
        CHECK(r.code == kProcessExited && has(r, "is not running"), "dead pid -> process-exited (%d)", r.code);
    }
    {  // success, then already-loaded, then several matches
        Child a, b;
        CHECK(start(a, L"g6inject_target.exe", std::wstring(L"--image ") + kImage), "start target A with image");
        // started before A maps its image: the injector must wait for it
        Run r = run_injector(proc + L" --timeout-ms 20000" + dll);
        show("inject into A", r);
        CHECK(r.code == kOk && has(r, "confirmed: "), "inject A -> ok (%d)", r.code);
        const std::string m = read_small(marker_path(a.pid));
        CHECK(m == "pid=" + std::to_string(a.pid), "marker written inside A: '%s'", m.c_str());

        r = run_injector(proc + L" --timeout-ms 5000" + dll);
        show("inject into A again", r);
        CHECK(r.code == kAlreadyLoaded && has(r, "already loaded"), "second load -> already-loaded (%d)", r.code);

        CHECK(start(b, L"g6inject_target.exe", std::wstring(L"--image ") + kImage), "start target B with image");
        CHECK(wait_image(b.pid), "B mapped its image");
        r = run_injector(proc + L" --timeout-ms 5000" + dll);
        show("two matching processes, no --pid", r);
        const std::string pa = "pid " + std::to_string(a.pid) + " ", pb = "pid " + std::to_string(b.pid) + " ";
        CHECK(r.code == kAmbiguous && has(r, pa.c_str()) && has(r, pb.c_str()), "two matches -> ambiguous, both listed (%d)", r.code);
        CHECK(!exists(marker_path(b.pid)), "nothing injected into B on refusal");

        r = run_injector(proc + L" --pid " + std::to_wstring(b.pid) + L" --timeout-ms 5000" + dll);
        show("two matching processes, --pid B", r);
        CHECK(r.code == kOk, "--pid B -> ok (%d)", r.code);
        const std::string mb = read_small(marker_path(b.pid));
        CHECK(mb == "pid=" + std::to_string(b.pid), "marker written inside B: '%s'", mb.c_str());

        r = run_injector(L"--process victory.exe --pid " + std::to_wstring(b.pid) + L" --timeout-ms 1000" + dll);
        show("--pid with another process name", r);
        CHECK(r.code == kUsage && has(r, "not 'victory.exe'"), "pid/name mismatch -> usage (%d)", r.code);
    }
    {  // 32-bit target
        if (exists(g_dir + L"g6inject_target32.exe")) {
            Child t;
            CHECK(start(t, L"g6inject_target32.exe", L""), "start x86 target");
            Sleep(300);
            Run r = run_injector(L"--process g6inject_target32.exe --timeout-ms 5000" + dll);
            show("x86 target", r);
            CHECK(r.code == kArchMismatch && has(r, "not an x64 process"), "x86 target -> arch-mismatch (%d)", r.code);
            r = run_injector(L"--process g6inject_target32.exe --pid " + std::to_wstring(t.pid) + L" --timeout-ms 5000" + dll);
            show("x86 target, --pid", r);
            CHECK(r.code == kArchMismatch && has(r, "32-bit (WOW64)"), "x86 target --pid -> arch-mismatch (%d)", r.code);
        } else {
            ++g_skip;
            std::printf("SKIP x86 target (g6inject_target32.exe not built)\n");
        }
    }
    {  // access denied: the System process (pid 4) never allows a module snapshot or injection rights
        Run r = run_injector(L"--process System --pid 4 --timeout-ms 1000" + dll);
        show("System process", r);
        CHECK(r.code == kAccessDenied && has(r, "access denied"), "pid 4 -> access-denied (%d)", r.code);
    }
    clear_markers();
    std::printf("end-to-end: done\n");
}

// The real runtime, injected late into a stand-in that has the image-named module but is not the
// game: LoadLibraryW must return (DllMain only starts the init thread), the init thread must run
// after the load, and identity must refuse (no module exports __XMemAllocationHooks__).
static void test_runtime_smoke() {
    using namespace g6inject;
    const std::wstring src = g_dir + L"..\\gta6mod_runtime.dll";
    if (!exists(src)) {
        ++g_skip;
        std::printf("SKIP runtime smoke test (%ls not built)\n", src.c_str());
        return;
    }
    const std::wstring rt = g_dir + L"gta6mod_runtime.dll", log = g_dir + L"gta6mod_runtime.log";
    CHECK(CopyFileW(src.c_str(), rt.c_str(), FALSE) != 0, "copy runtime (%lu)", GetLastError());
    const char ini[] = "[runtime]\r\nmode=readonly\r\nwait_for_game_ms=1000\r\n";
    HANDLE f = CreateFileW((g_dir + L"gta6mod_runtime.ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    CHECK(f != INVALID_HANDLE_VALUE && WriteFile(f, ini, sizeof ini - 1, &w, nullptr), "write test ini");
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileW(log.c_str());

    Child t;
    CHECK(start(t, L"g6inject_target.exe", std::wstring(L"--image ") + kImage), "start target for the runtime");
    CHECK(wait_image(t.pid), "target mapped its image");
    Run r = run_injector(L"--process g6inject_target.exe --pid " + std::to_wstring(t.pid) + L" --timeout-ms 5000 --dll \"" + rt + L"\"");
    show("inject gta6mod_runtime.dll", r);
    CHECK(r.code == kOk && has(r, "confirmed: "), "runtime injected (%d)", r.code);
    std::string text;
    for (int i = 0; i < 200 && text.find("init finished") == std::string::npos; ++i) {
        Sleep(50);
        HANDLE in = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (in == INVALID_HANDLE_VALUE) continue;
        text.clear();
        char buf[4096];
        DWORD got = 0;
        while (text.size() < (1u << 20) && ReadFile(in, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
        CloseHandle(in);
    }
    std::printf("--- runtime log after the late load:\n%s", text.c_str());
    CHECK(text.find("mode=readonly") != std::string::npos, "runtime init thread ran after the injection");
    CHECK(text.find("no module exporting __XMemAllocationHooks__") != std::string::npos && text.find("init finished") != std::string::npos,
          "runtime refused the non-game process and finished init");
    CHECK(WaitForSingleObject(t.proc, 0) == WAIT_TIMEOUT, "target still alive after the runtime refused");
    std::printf("runtime smoke: done\n");
}

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    while (n > 0 && self[n - 1] != L'\\') --n;
    g_dir.assign(self, n);
    g_injector = argc > 1 ? argv[1] : g_dir + L"..\\g6inject.exe";
    if (!exists(g_injector)) { std::printf("g6inject.exe not found: %ls\n", g_injector.c_str()); return 2; }
    for (const wchar_t* f : { L"g6inject_target.exe", L"g6inject_marker.dll", kImage })
        if (!exists(g_dir + f)) { std::printf("missing test binary: %ls\n", f); return 2; }
    test_core();
    test_e2e();
    test_runtime_smoke();
    std::printf("\ninjector: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
