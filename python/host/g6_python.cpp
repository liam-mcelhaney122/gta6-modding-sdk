// g6_python.cpp — gta6mod runtime plugin that embeds CPython 3.12 and runs Python mods inside the game.
//
// Design and contracts: sdk/docs/python_host.md. In short:
//  * Off by default. g6_python.ini next to this DLL, [python] enable=1 turns it on. With enable=0 (or no ini)
//    g6_plugin_load returns G6_OK, registers nothing, and python312.dll is never loaded.
//  * python312.dll is DELAY-loaded (/DELAYLOAD) from [python] home with LoadLibraryExW before the first Python
//    call, so the plugin itself loads without Python on the search path. This file therefore uses no Python
//    DATA export (Py_None, PyExc_*, type objects): the linker refuses to delay-load data imports.
//  * Everything Python happens on the game script thread inside this plugin's tick (or the stop event the
//    runtime raises from a script fiber): the interpreter is initialised on the FIRST tick, the GIL is taken
//    with PyGILState_Ensure for each entry and released before returning to the game.
//  * The interpreter is never finalised and python312.dll and this DLL are pinned once Python runs:
//    Py_FinalizeEx would join every non-daemon thread a mod started (a hang inside the game), extension modules
//    do not survive finalise/re-initialise reliably, and the interpreter keeps pointers into this DLL (the _g6
//    method table). A later g6_plugin_load in the same process reuses the interpreter.
//  * Natives go through the runtime's g6_host_api (native_resolve once per hash, then native_invoke) from the
//    built-in module _g6 (PyImport_AppendInittab), not ctypes. A call from a thread that is not inside a plugin
//    entry (a thread a mod started) gets G6_E_NOT_ON_SCRIPT_THREAD without reaching the runtime.
//  * Single-player guard: before Python starts, the first tick calls GET_GAME_TIMER (observed hash
//    0x4F67E8ECA7D3F667); G6_E_ONLINE_SESSION means the runtime's guard is closed, and Python is not started
//    (re-checked every guard_retry_ms). Later refusals pause every mod (runtime/host.py).
//  * Watchdog: one mod operation (load, tick, stop) running longer than its budget of wall time gets
//    TickBudgetExceeded raised into it as an asynchronous exception (see "watchdog" below).
#include "gta6mod/gta6mod.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define G6_PLUGIN_EXPORT extern "C" __declspec(dllexport)

namespace g6py {
namespace {

constexpr const char*   kSource = "g6_python";
constexpr const char*   kVersion = "0.1.0";
constexpr std::uint64_t kGetGameTimer = 0x4F67E8ECA7D3F667ull;  // GET_GAME_TIMER_F667 (natives.h, observed)

// ------------------------------------------------------------------ config ([python] in g6_python.ini)
struct Config {
    bool                      enable = false;
    std::wstring              home;            // CPython 3.12: python312.dll + Lib/DLLs, or the embeddable zip
    std::wstring              sdk_path;        // folder holding the gta6mod package
    std::wstring              mods_dir;        // *.py mods
    std::vector<std::wstring> extra_paths;     // appended to sys.path
    bool                      allow_unverified = false;
    bool                      hot_reload = false;
    bool                      write_bytecode = true;
    unsigned                  reload_poll_ms = 1000;
    unsigned                  tick_budget_ms = 2000;   // 0 = watchdog off
    unsigned                  load_budget_ms = 10000;
    unsigned                  guard_retry_ms = 5000;
};

enum class State { off, waiting_guard, starting, running, failed };

const g6_host_api* g_api = nullptr;
Config             g_cfg;
std::wstring       g_dir;                 // folder of this DLL
std::wstring       g_ini;
HMODULE            g_runtime_module = nullptr;  // module holding the host table (g6_ui_* exports live there)
g6_script_id       g_tick_id = 0;
State              g_state = State::off;
std::mutex         g_mu;                  // one entry into Python at a time (tick, stop, unload)
std::uint64_t      g_next_guard_check = 0;
unsigned           g_guard_refusals = 0;
bool               g_stop_raised = false;
int                g_host_errors = 0;     // consecutive failures of the host's own tick
bool               g_thread_warned = false;
std::unordered_map<std::uint64_t, g6_native_handle> g_handles;

// Process lifetime: the interpreter outlives any single load/unload.
bool          g_py_inited = false;
bool          g_py_broken = false;        // initialisation failed once: never retried in this process
DWORD         g_main_tid = 0;             // thread that initialised Python
PyObject*     g_host = nullptr;           // gta6mod.runtime.host.ModHost
PyObject*     g_fn_tick = nullptr;
PyObject*     g_fn_stop = nullptr;
PyObject*     g_fn_shutdown = nullptr;
PyObject*     g_budget_exc = nullptr;     // TickBudgetExceeded (host.py)

thread_local int t_entry = 0;             // > 0 while this thread is inside a plugin entry

struct EntryScope {
    EntryScope() { ++t_entry; }
    ~EntryScope() { --t_entry; }
    EntryScope(const EntryScope&) = delete;
    EntryScope& operator=(const EntryScope&) = delete;
};

// ------------------------------------------------------------------ logging and strings
void logf(g6_log_level lvl, const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_api && g_api->log) g_api->log(lvl, kSource, buf);
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring trim(std::wstring s) {
    const wchar_t* ws = L" \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Relative paths are relative to this DLL's folder; the result is a normalised absolute path.
std::wstring resolve(const std::wstring& p) {
    if (p.empty()) return {};
    std::wstring joined = p;
    const bool absolute = (p.size() > 1 && p[1] == L':') || (p.size() > 1 && p[0] == L'\\' && p[1] == L'\\');
    if (!absolute) joined = g_dir + L"\\" + p;
    wchar_t full[2048];
    const DWORD n = GetFullPathNameW(joined.c_str(), 2048, full, nullptr);
    std::wstring out = (n > 0 && n < 2048) ? std::wstring(full, n) : joined;
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

std::wstring ini_str(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"python", key, def, buf, 2048, g_ini.c_str());
    std::wstring v = trim(buf);
    if (const auto semi = v.find(L';'); semi != std::wstring::npos && key != std::wstring(L"extra_paths"))
        v = trim(v.substr(0, semi));  // trailing comment
    return v;
}

bool ini_uint(const wchar_t* key, unsigned def, unsigned lo, unsigned hi, bool zero_ok, unsigned& out,
              std::vector<std::string>& errors) {
    const std::wstring v = ini_str(key, L"");
    if (v.empty()) { out = def; return true; }
    wchar_t* end = nullptr;
    const unsigned long n = std::wcstoul(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != L'\0' || ((n < lo || n > hi) && !(zero_ok && n == 0))) {
        char msg[256];
        std::snprintf(msg, sizeof msg, "[python] %s='%s': want %s%u..%u", utf8(key).c_str(), utf8(v).c_str(),
                      zero_ok ? "0 or " : "", lo, hi);
        errors.emplace_back(msg);
        return false;
    }
    out = static_cast<unsigned>(n);
    return true;
}

bool ini_bool(const wchar_t* key, bool def, bool& out, std::vector<std::string>& errors) {
    const std::wstring v = ini_str(key, L"");
    if (v.empty()) { out = def; return true; }
    if (v == L"1") { out = true; return true; }
    if (v == L"0") { out = false; return true; }
    errors.push_back("[python] " + utf8(key) + "='" + utf8(v) + "': want 0 or 1");
    return false;
}

bool read_config(std::vector<std::string>& errors) {
    g_cfg = Config{};
    if (!file_exists(g_ini)) return true;  // no ini = disabled
    ini_bool(L"enable", false, g_cfg.enable, errors);
    g_cfg.home = resolve(ini_str(L"home", L"python"));
    g_cfg.sdk_path = resolve(ini_str(L"sdk_path", L"python_sdk"));
    g_cfg.mods_dir = resolve(ini_str(L"mods_dir", L"mods\\python"));
    const std::wstring extra = ini_str(L"extra_paths", L"");
    for (std::size_t pos = 0; pos <= extra.size();) {
        const std::size_t semi = extra.find(L';', pos);
        const std::wstring item = trim(extra.substr(pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos));
        if (!item.empty()) g_cfg.extra_paths.push_back(resolve(item));
        if (semi == std::wstring::npos) break;
        pos = semi + 1;
    }
    ini_bool(L"allow_unverified", false, g_cfg.allow_unverified, errors);
    ini_bool(L"hot_reload", false, g_cfg.hot_reload, errors);
    ini_bool(L"write_bytecode", true, g_cfg.write_bytecode, errors);
    ini_uint(L"reload_poll_ms", 1000, 0, 60000, true, g_cfg.reload_poll_ms, errors);
    ini_uint(L"tick_budget_ms", 2000, 100, 600000, true, g_cfg.tick_budget_ms, errors);
    ini_uint(L"load_budget_ms", 10000, 100, 600000, true, g_cfg.load_budget_ms, errors);
    ini_uint(L"guard_retry_ms", 5000, 500, 60000, false, g_cfg.guard_retry_ms, errors);
    return errors.empty();
}

// ------------------------------------------------------------------ Python helpers (no data imports)
PyObject* py_none() { return Py_BuildValue(""); }  // new reference to None without touching _Py_NoneStruct

PyObject* raise_builtin(const char* exc_name, const char* msg) {
    PyObject* builtins = PyEval_GetBuiltins();  // borrowed
    PyObject* type = builtins ? PyDict_GetItemString(builtins, exc_name) : nullptr;  // borrowed
    if (type) PyErr_SetString(type, msg);
    return nullptr;
}

// Formats and clears the current Python exception.
std::string exception_text() {
    PyObject* exc = PyErr_GetRaisedException();
    if (!exc) return "(no Python exception set)";
    std::string out;
    if (PyObject* tb = PyImport_ImportModule("traceback")) {
        if (PyObject* lines = PyObject_CallMethod(tb, "format_exception", "O", exc)) {
            PyObject* sep = PyUnicode_FromString("");
            PyObject* joined = sep ? PyUnicode_Join(sep, lines) : nullptr;
            if (const char* s = joined ? PyUnicode_AsUTF8(joined) : nullptr) out = s;
            Py_XDECREF(joined);
            Py_XDECREF(sep);
            Py_DECREF(lines);
        }
        Py_DECREF(tb);
    }
    if (out.empty()) {
        PyErr_Clear();
        if (PyObject* r = PyObject_Repr(exc)) {
            if (const char* s = PyUnicode_AsUTF8(r)) out = s;
            Py_DECREF(r);
        }
    }
    PyErr_Clear();
    Py_DECREF(exc);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out.empty() ? "(unprintable exception)" : out;
}

struct Gil {
    PyGILState_STATE s;
    Gil() : s(PyGILState_Ensure()) {}
    ~Gil() { PyGILState_Release(s); }
    Gil(const Gil&) = delete;
    Gil& operator=(const Gil&) = delete;
};

// ------------------------------------------------------------------ watchdog
// A mod operation (load, tick or stop of one mod) is "armed" by host.py through _g6.watch. When it runs past its
// budget, this thread takes the GIL (the busy thread drops it within the switch interval, 5 ms, at its next
// bytecode boundary) and, if the same operation is still armed, sets TickBudgetExceeded as an asynchronous
// exception on the thread that armed it (PyThreadState_SetAsyncExc). Not Py_AddPendingCall: in 3.12 a pending
// call added from a non-main thread does not set the main thread's eval breaker, so it only runs after the
// next GIL hand-over (observed: a spinning mod was never interrupted). A mod blocked inside one C call that
// never releases the GIL cannot be interrupted; the exception lands when it returns to bytecode.
struct Watchdog {
    std::atomic<bool>          armed{ false };
    std::atomic<std::uint64_t> gen{ 0 };
    std::atomic<std::uint64_t> next_fire{ 0 };
    std::atomic<unsigned>      budget{ 0 };
    std::atomic<unsigned long> thread{ 0 };  // OS thread id of the armed operation (= PyThread_get_thread_ident)
    std::atomic<bool>          quit{ false };
    std::atomic<unsigned>      fired{ 0 };
    std::thread                th;
};
Watchdog g_wd;

void watchdog_loop() {
    while (!g_wd.quit.load()) {
        Sleep(20);
        if (!g_wd.armed.load()) continue;
        const unsigned budget = g_wd.budget.load();
        const std::uint64_t now = GetTickCount64();
        if (budget == 0 || now < g_wd.next_fire.load()) continue;
        const std::uint64_t gen = g_wd.gen.load();
        g_wd.next_fire.store(now + budget);  // fires again after another budget if the mod swallows it
        const PyGILState_STATE s = PyGILState_Ensure();
        // Under the GIL the armed state cannot change (watch() is called with the GIL held).
        if (g_wd.armed.load() && g_wd.gen.load() == gen && g_budget_exc) {
            if (PyObject* b = PyLong_FromUnsignedLong(budget)) {
                if (PyObject_SetAttrString(g_budget_exc, "budget_ms", b) != 0) PyErr_Clear();
                Py_DECREF(b);
            }
            if (PyThreadState_SetAsyncExc(g_wd.thread.load(), g_budget_exc) == 1) g_wd.fired.fetch_add(1);
        }
        PyGILState_Release(s);
    }
}

void watchdog_start() {
    if (g_wd.th.joinable() || g_cfg.tick_budget_ms == 0) return;
    g_wd.quit.store(false);
    g_wd.armed.store(false);
    g_wd.th = std::thread(watchdog_loop);
}

void watchdog_stop() {
    g_wd.armed.store(false);
    if (!g_wd.th.joinable()) return;
    g_wd.quit.store(true);
    g_wd.th.join();
}

void watchdog_arm(bool on, unsigned budget_ms) {
    if (!on) { g_wd.armed.store(false); return; }
    const unsigned b = budget_ms ? budget_ms : g_cfg.tick_budget_ms;
    g_wd.budget.store(b);
    g_wd.thread.store(GetCurrentThreadId());
    g_wd.next_fire.store(GetTickCount64() + b);
    g_wd.gen.fetch_add(1);
    g_wd.armed.store(g_cfg.tick_budget_ms != 0 && b != 0);
}

// ------------------------------------------------------------------ the _g6 module
g6_status invoke_cached(std::uint64_t hash, g6_native_call* call) {
    g6_native_handle h = nullptr;
    if (const auto it = g_handles.find(hash); it != g_handles.end()) {
        h = it->second;
    } else {
        const g6_status st = g_api->native_resolve(hash, &h);
        if (st != G6_OK) return st;
        g_handles.emplace(hash, h);
    }
    return g_api->native_invoke(h, call);
}

PyObject* m_invoke(PyObject*, PyObject* args) {
    unsigned long long hash = 0;
    PyObject* seq = nullptr;
    unsigned int string_slots = 0, text_slots = 0;  // API 0.4 masks (script objects, G-120)
    if (!PyArg_ParseTuple(args, "KO|II:invoke", &hash, &seq, &string_slots, &text_slots)) return nullptr;
    PyObject* fast = PySequence_Fast(seq, "invoke: slots must be a sequence of int");
    if (!fast) return nullptr;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    if (n > G6_MAX_NATIVE_ARGS) {
        Py_DECREF(fast);
        return raise_builtin("ValueError", "invoke: more than 32 argument slots");
    }
    g6_native_call call{};
    call.size = sizeof call;
    call.argc = static_cast<std::uint32_t>(n);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const unsigned long long v = PyLong_AsUnsignedLongLongMask(PySequence_Fast_GET_ITEM(fast, i));
        if (v == static_cast<unsigned long long>(-1) && PyErr_Occurred()) { Py_DECREF(fast); return nullptr; }
        call.args[i] = v;
    }
    Py_DECREF(fast);
    call.string_slots = string_slots;
    call.text_slots = text_slots;
    g6_status st = G6_E_NOT_ON_SCRIPT_THREAD;
    if (t_entry > 0 && g_api) st = invoke_cached(hash, &call);
    return Py_BuildValue("(i(KKKK))", static_cast<int>(st), call.ret[0], call.ret[1], call.ret[2], call.ret[3]);
}

PyObject* m_log(PyObject*, PyObject* args) {
    int level = 1;
    const char* source = nullptr;
    const char* message = nullptr;
    if (!PyArg_ParseTuple(args, "iss:log", &level, &source, &message)) return nullptr;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    if (g_api && g_api->log) g_api->log(static_cast<g6_log_level>(level), source, message);
    return py_none();
}

PyObject* m_now_ms(PyObject*, PyObject*) {
    return PyLong_FromUnsignedLongLong(g_api ? g_api->game_time_ms() : 0);
}

PyObject* m_build_info(PyObject*, PyObject*) {
    g6_build_info bi{};
    bi.size = sizeof bi;
    const g6_status st = g_api ? g_api->get_build_info(&bi) : G6_E_INTERNAL;
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", st == G6_OK ? bi.exe_sha256[i] : 0);
    return Py_BuildValue("(isssII)", static_cast<int>(st), (st == G6_OK && bi.game_version) ? bi.game_version : "",
                         (st == G6_OK && bi.platform) ? bi.platform : "", hex, bi.natives_known, bi.natives_verified);
}

PyObject* m_ui_proc(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s:ui_proc", &name)) return nullptr;
    FARPROC p = nullptr;
    if (g_runtime_module && std::strncmp(name, "g6_ui_", 6) == 0) p = GetProcAddress(g_runtime_module, name);
    return PyLong_FromUnsignedLongLong(reinterpret_cast<std::uintptr_t>(p));
}

PyObject* m_on_script_thread(PyObject*, PyObject*) { return PyBool_FromLong(t_entry > 0); }

PyObject* m_watch(PyObject*, PyObject* args) {
    int on = 0;
    unsigned int budget = 0;
    if (!PyArg_ParseTuple(args, "p|I:watch", &on, &budget)) return nullptr;
    watchdog_arm(on != 0, budget);
    return py_none();
}

PyObject* m_set_budget_exception(PyObject*, PyObject* args) {
    PyObject* cls = nullptr;
    if (!PyArg_ParseTuple(args, "O:set_budget_exception", &cls)) return nullptr;
    Py_INCREF(cls);
    Py_XDECREF(g_budget_exc);
    g_budget_exc = cls;
    return py_none();
}

PyMethodDef g_methods[] = {
    { "invoke", m_invoke, METH_VARARGS, "invoke(hash, slots[, string_slots, text_slots]) -> (status, (r0, r1, r2, r3))" },
    { "log", m_log, METH_VARARGS, "log(level, source, message)" },
    { "now_ms", m_now_ms, METH_NOARGS, "game time in ms (g6_host_api.game_time_ms)" },
    { "build_info", m_build_info, METH_NOARGS, "(status, version, platform, sha256_hex, natives_known, natives_verified)" },
    { "ui_proc", m_ui_proc, METH_VARARGS, "ui_proc(name) -> address of the runtime's g6_ui_* export, or 0" },
    { "on_script_thread", m_on_script_thread, METH_NOARGS, "True inside a plugin tick / stop / unload call" },
    { "watch", m_watch, METH_VARARGS, "watch(on[, budget_ms]): arm or disarm the tick-budget watchdog" },
    { "set_budget_exception", m_set_budget_exception, METH_VARARGS, "exception type the watchdog raises" },
    { nullptr, nullptr, 0, nullptr },
};

PyModuleDef g_module = {
    PyModuleDef_HEAD_INIT, "_g6", "gta6mod runtime host table, exposed by g6_python.dll (sdk/docs/python_host.md)",
    -1, g_methods, nullptr, nullptr, nullptr, nullptr,
};

PyObject* init_g6_module() { return PyModule_Create(&g_module); }

// ------------------------------------------------------------------ interpreter start
bool same_path(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring module_path(HMODULE m) {
    wchar_t buf[2048];
    const DWORD n = GetModuleFileNameW(m, buf, 2048);
    return (n > 0 && n < 2048) ? std::wstring(buf, n) : std::wstring();
}

bool status_failed(const PyStatus& s, const char* step, std::string& why) {
    if (!PyStatus_Exception(s)) return false;
    why = std::string(step) + ": " + (s.func ? std::string(s.func) + ": " : std::string()) + (s.err_msg ? s.err_msg : "failed");
    if (PyStatus_IsExit(s)) why += " (exit code " + std::to_string(s.exitcode) + ")";
    return true;
}

// Loads python312.dll from [python] home and initialises an isolated interpreter. On success the GIL is
// RELEASED again (the caller takes it with Gil).
bool init_python(std::string& why) {
    const std::wstring dll = g_cfg.home + L"\\python312.dll";
    if (HMODULE already = GetModuleHandleW(L"python312.dll")) {
        const std::wstring have = module_path(already);
        if (!same_path(have, dll)) {
            why = "another python312.dll is already loaded in this process (" + utf8(have) + "); [python] home must point at it";
            return false;
        }
    }
    HMODULE py = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!py) {
        why = "LoadLibrary(" + utf8(dll) + ") failed, error " + std::to_string(GetLastError());
        return false;
    }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, dll.c_str(), &pinned);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&init_python), &pinned);  // the interpreter will point into us

    const char* version = Py_GetVersion();  // first delay-loaded call
    if (!version || std::strncmp(version, "3.12.", 5) != 0) {
        why = std::string("python312.dll reports version ") + (version ? version : "?") + "; this plugin is built for 3.12";
        return false;
    }
    if (PyImport_AppendInittab("_g6", &init_g6_module) != 0) { why = "PyImport_AppendInittab(_g6) failed"; return false; }

    PyPreConfig pre;
    PyPreConfig_InitIsolatedConfig(&pre);
    pre.utf8_mode = 1;
    if (status_failed(Py_PreInitialize(&pre), "Py_PreInitialize", why)) return false;

    PyConfig c;
    PyConfig_InitIsolatedConfig(&c);
    c.site_import = 0;              // no site-packages of the install, no user site
    c.user_site_directory = 0;
    c.install_signal_handlers = 0;  // never touch the game's signal/console handlers
    c.parse_argv = 0;
    c.faulthandler = 0;
    c.write_bytecode = g_cfg.write_bytecode ? 1 : 0;
    std::vector<std::wstring> paths;
    if (file_exists(g_cfg.home + L"\\python312.zip")) paths.push_back(g_cfg.home + L"\\python312.zip");  // embeddable
    if (dir_exists(g_cfg.home + L"\\Lib")) paths.push_back(g_cfg.home + L"\\Lib");                       // full install
    if (dir_exists(g_cfg.home + L"\\DLLs")) paths.push_back(g_cfg.home + L"\\DLLs");
    paths.push_back(g_cfg.home);  // embeddable: the .pyd files sit next to python312.dll
    paths.push_back(g_cfg.sdk_path);
    for (const auto& p : g_cfg.extra_paths) paths.push_back(p);
    PyStatus s = PyConfig_SetString(&c, &c.home, g_cfg.home.c_str());
    if (!PyStatus_Exception(s)) s = PyConfig_SetString(&c, &c.program_name, L"g6_python");
    const std::wstring exe = g_cfg.home + L"\\python.exe";  // sys.executable: never the game exe
    if (!PyStatus_Exception(s) && file_exists(exe)) s = PyConfig_SetString(&c, &c.executable, exe.c_str());
    c.module_search_paths_set = 1;
    for (const auto& p : paths)
        if (!PyStatus_Exception(s)) s = PyWideStringList_Append(&c.module_search_paths, p.c_str());
    if (status_failed(s, "PyConfig", why)) { PyConfig_Clear(&c); return false; }
    s = Py_InitializeFromConfig(&c);
    PyConfig_Clear(&c);
    if (status_failed(s, "Py_InitializeFromConfig", why)) { g_py_broken = true; return false; }
    g_py_inited = true;
    g_main_tid = GetCurrentThreadId();
    PyEval_SaveThread();
    logf(G6_LOG_INFO, "Python %s initialised (isolated, no site) from %s on thread %lu", version, utf8(g_cfg.home).c_str(),
         static_cast<unsigned long>(g_main_tid));
    return true;
}

void drop_host_refs() {  // GIL held
    Py_CLEAR(g_fn_tick);
    Py_CLEAR(g_fn_stop);
    Py_CLEAR(g_fn_shutdown);
    Py_CLEAR(g_host);
}

bool boot_host(std::string& why) {  // GIL held
    PyObject* mod = PyImport_ImportModule("gta6mod.runtime.host");
    if (!mod) { why = "import gta6mod.runtime.host failed:\n" + exception_text(); return false; }
    PyObject* boot = PyObject_GetAttrString(mod, "boot");
    Py_DECREF(mod);
    if (!boot) { why = exception_text(); return false; }
    PyObject* kwargs = Py_BuildValue("{s:s,s:N,s:N,s:I,s:I}", "mods_dir", utf8(g_cfg.mods_dir).c_str(),
                                     "allow_unverified", PyBool_FromLong(g_cfg.allow_unverified),
                                     "hot_reload", PyBool_FromLong(g_cfg.hot_reload),
                                     "reload_poll_ms", g_cfg.reload_poll_ms, "load_budget_ms", g_cfg.load_budget_ms);
    PyObject* noargs = PyTuple_New(0);
    PyObject* host = (kwargs && noargs) ? PyObject_Call(boot, noargs, kwargs) : nullptr;
    Py_XDECREF(noargs);
    Py_XDECREF(kwargs);
    Py_DECREF(boot);
    if (!host) { why = "gta6mod.runtime.host.boot failed:\n" + exception_text(); return false; }
    g_host = host;
    g_fn_tick = PyObject_GetAttrString(host, "tick");
    g_fn_stop = PyObject_GetAttrString(host, "stop");
    g_fn_shutdown = PyObject_GetAttrString(host, "shutdown");
    if (!g_fn_tick || !g_fn_stop || !g_fn_shutdown) {
        why = "ModHost lacks tick/stop/shutdown: " + exception_text();
        drop_host_refs();
        return false;
    }
    return true;
}

// First tick (script thread): interpreter (once per process), watchdog, host. False = this load stays failed.
bool start() {
    std::string why;
    if (g_py_broken) {
        logf(G6_LOG_ERROR, "Python failed to initialise earlier in this process; restart the game to retry");
        return false;
    }
    if (!g_py_inited) {
        if (!init_python(why)) {
            logf(G6_LOG_ERROR, "Python not started: %s", why.c_str());
            return false;
        }
    } else {
        logf(G6_LOG_INFO, "reusing the interpreter initialised earlier in this process (it is never finalised)");
    }
    watchdog_start();
    Gil gil;
    if (!boot_host(why)) {
        logf(G6_LOG_ERROR, "Python host not started: %s", why.c_str());
        return false;
    }
    return true;
}

// Shared cleanup of failed start and unload: drop the host (after its shutdown when it ran), stop the watchdog.
// Python itself stays initialised (see the top of this file).
void release_python_side(bool call_shutdown) {
    if (g_py_inited) {
        EntryScope e;
        Gil gil;
        if (call_shutdown && g_fn_shutdown) {
            if (PyObject* r = PyObject_CallNoArgs(g_fn_shutdown)) Py_DECREF(r);
            else logf(G6_LOG_ERROR, "host shutdown failed:\n%s", exception_text().c_str());
        }
        drop_host_refs();
        Py_CLEAR(g_budget_exc);
    }
    g_handles.clear();
    watchdog_stop();
}

bool guard_open() {
    g6_native_call c{};
    c.size = sizeof c;
    const g6_status st = g_api->native_invoke_hash(kGetGameTimer, &c);
    if (st == G6_E_ONLINE_SESSION) return false;
    if (st != G6_OK) logf(G6_LOG_DEBUG, "guard probe GET_GAME_TIMER returned %d (not the guard); starting", static_cast<int>(st));
    return true;
}

// ------------------------------------------------------------------ tick
void G6_CALL py_tick(void*) {
    std::unique_lock<std::mutex> lk(g_mu, std::try_to_lock);
    if (!lk.owns_lock()) return;  // a stop/unload on another thread is in Python: skip this frame
    if (g_state == State::off || g_state == State::failed) return;
    EntryScope entry;
    if (g_state == State::waiting_guard) {
        const std::uint64_t now = GetTickCount64();
        if (now < g_next_guard_check) return;
        if (!guard_open()) {
            if (g_guard_refusals++ == 0)
                logf(G6_LOG_WARN, "single-player guard closed (GET_GAME_TIMER -> G6_E_ONLINE_SESSION): Python is not started; "
                                  "re-checked every %u ms", g_cfg.guard_retry_ms);
            g_next_guard_check = now + g_cfg.guard_retry_ms;
            return;
        }
        g_state = State::starting;
    }
    if (g_state == State::starting) {
        if (!start()) {
            g_state = State::failed;
            release_python_side(false);
            return;
        }
        g_state = State::running;
    }
    if (!g_thread_warned && GetCurrentThreadId() != g_main_tid) {
        g_thread_warned = true;
        logf(G6_LOG_INFO, "plugin tick on OS thread %lu, Python was initialised on %lu (the GIL state is per thread; "
                          "this costs a thread state per tick)", static_cast<unsigned long>(GetCurrentThreadId()),
             static_cast<unsigned long>(g_main_tid));
    }
    Gil gil;
    PyObject* r = PyObject_CallNoArgs(g_fn_tick);
    if (r) {
        Py_DECREF(r);
        g_host_errors = 0;
        return;
    }
    logf(G6_LOG_ERROR, "host tick failed:\n%s", exception_text().c_str());
    if (++g_host_errors >= 3) {
        logf(G6_LOG_ERROR, "host tick failed 3 times in a row; Python mods are stopped until the plugin is reloaded");
        g_state = State::failed;
    }
}

g6_status load(const g6_host_api* api) {
    g_api = api;
    g_state = State::off;
    g_stop_raised = false;
    g_host_errors = 0;
    g_guard_refusals = 0;
    g_next_guard_check = 0;
    g_thread_warned = false;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&load), &self)) {
        logf(G6_LOG_ERROR, "cannot find this DLL's folder; not loaded");
        return G6_E_INTERNAL;
    }
    g_dir = module_path(self);
    if (const auto slash = g_dir.find_last_of(L"\\/"); slash != std::wstring::npos) g_dir.resize(slash);
    g_ini = g_dir + L"\\g6_python.ini";
    g_runtime_module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(api), &g_runtime_module);

    std::vector<std::string> errors;
    const bool ok = read_config(errors);
    if (!g_cfg.enable) {
        logf(G6_LOG_INFO, "disabled ([python] enable=1 not set in %s); Python is not loaded", utf8(g_ini).c_str());
        return G6_OK;
    }
    for (const auto& e : errors) logf(G6_LOG_ERROR, "config: %s", e.c_str());
    if (!ok) { logf(G6_LOG_ERROR, "not loaded: fix %s", utf8(g_ini).c_str()); return G6_E_INVALID_ARG; }
    if (!file_exists(g_cfg.home + L"\\python312.dll")) {
        logf(G6_LOG_ERROR, "not loaded: [python] home=%s has no python312.dll (a CPython 3.12 install or the 3.12 "
                           "embeddable distribution)", utf8(g_cfg.home).c_str());
        return G6_E_INVALID_ARG;
    }
    if (!file_exists(g_cfg.sdk_path + L"\\gta6mod\\__init__.py")) {
        logf(G6_LOG_ERROR, "not loaded: [python] sdk_path=%s has no gta6mod\\__init__.py", utf8(g_cfg.sdk_path).c_str());
        return G6_E_INVALID_ARG;
    }
    if (!dir_exists(g_cfg.mods_dir))
        logf(G6_LOG_WARN, "[python] mods_dir=%s does not exist: no mods will load", utf8(g_cfg.mods_dir).c_str());

    const g6_script_desc d{ sizeof(g6_script_desc), "python-host", &py_tick, nullptr };
    const g6_status st = api->script_register(&d, &g_tick_id);
    if (st != G6_OK) {
        logf(G6_LOG_ERROR, "script_register failed (%d); not loaded", static_cast<int>(st));
        g_tick_id = 0;
        return st;
    }
    g_state = State::waiting_guard;
    logf(G6_LOG_INFO, "g6_python %s: home=%s sdk=%s mods=%s allow_unverified=%d hot_reload=%d tick_budget_ms=%u; "
                      "Python starts on the first script tick", kVersion, utf8(g_cfg.home).c_str(), utf8(g_cfg.sdk_path).c_str(),
         utf8(g_cfg.mods_dir).c_str(), g_cfg.allow_unverified ? 1 : 0, g_cfg.hot_reload ? 1 : 0, g_cfg.tick_budget_ms);
    return G6_OK;
}

void stop_event() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_stop_raised || g_state != State::running || !g_fn_stop) return;
    g_stop_raised = true;
    EntryScope entry;
    Gil gil;
    if (PyObject* r = PyObject_CallNoArgs(g_fn_stop)) Py_DECREF(r);
    else logf(G6_LOG_ERROR, "stop event failed:\n%s", exception_text().c_str());
}

void unload() {
    if (g_api && g_tick_id) g_api->script_unregister(g_tick_id);  // no tick after this point
    g_tick_id = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const bool ran = g_state == State::running;
        release_python_side(ran);  // shutdown raises the stop event first when the runtime did not
        g_state = State::off;
    }
    if (g_api) logf(G6_LOG_INFO, "unloaded");
    g_api = nullptr;
}

} // namespace
} // namespace g6py

G6_PLUGIN_EXPORT const g6_plugin_info* G6_CALL g6_plugin_query(void) {
    static const g6_plugin_info info = { sizeof(g6_plugin_info), "python-host", g6py::kVersion, G6_API_VERSION_MAJOR,
                                         G6_API_VERSION_MINOR };
    return &info;
}

G6_PLUGIN_EXPORT g6_status G6_CALL g6_plugin_load(const g6_host_api* api) {
    if (!api || api->size < sizeof(g6_host_api) || api->version_major != G6_API_VERSION_MAJOR) return G6_E_UNSUPPORTED_BUILD;
    try {
        return g6py::load(api);
    } catch (...) {
        return G6_E_INTERNAL;
    }
}

G6_PLUGIN_EXPORT void G6_CALL g6_plugin_stop(void) {
    try {
        g6py::stop_event();
    } catch (...) {
    }
}

G6_PLUGIN_EXPORT void G6_CALL g6_plugin_unload(void) {
    try {
        g6py::unload();
    } catch (...) {
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
