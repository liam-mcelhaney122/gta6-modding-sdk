// g6_python_test — loads g6_python.dll the way the runtime does (g6_plugin_query / load / stop / unload) against
// the offline mock runtime (gta6mod_mock.dll) and the shared C mock world (runtime/src/mock_world.cpp), and runs
// Python mods inside the embedded interpreter.
//
//   g6_python_test.exe <g6_python.dll> <sdk dir> <python home> <work dir>
//       runs every scenario, each in its own child process (one interpreter per process: it is never finalised)
//   g6_python_test.exe --scenario NAME <g6_python.dll> <sdk dir> <python home> <work dir>
//       runs one scenario in this process
//
// Each scenario gets <work dir>\<name>\plugins\ with a copy of g6_python.dll, a g6_python.ini written for it and
// mods\python\ with the mods it runs. Scenarios: the five example mods with the scenarios and log lines of
// sdk/tests/python/test_script_layer.py, the stop event, failure isolation (load error, script error, watchdog),
// the embedded environment and _g6 contract, enable=0, the single-player guard (closed at start and tripped
// mid-run), hot reload plus unload/load in one process, a bad config, and the in-game Python UI (gta6mod.ui over
// runtime/src/ui_retained.cpp, linked into this exe as it is into gta6mod_runtime.dll): the trainer menu and every
// widget against the mock UI with transcript parity (sdk/tests/ui/*.ui.txt), the overlay off, and a UI mod hot
// reloaded next to another. Mock UI frames are drawn after every tick on a separate thread (the render thread).
#include "gta6mod/gta6mod.h"
#include "mock_world.hpp"
#include "ui_retained.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

// The "UI" export the env_check mod resolves through _g6.ui_proc: g6_python resolves g6_ui_* names in the module
// that holds the host table, which is this exe here (g_api below) and gta6mod_runtime.dll in game.
extern "C" __declspec(dllexport) int G6_CALL g6_ui_harness_probe(void) { return 42; }

namespace {

int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        if (cond) { ++g_pass; }                                                                                    \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

struct LogLine { int level; std::string source, message; };

const g6_host_api*   g_mock = nullptr;
g6_host_api          g_api{};
std::vector<LogLine> g_logs;
bool                 g_guard_closed = false;
int                  g_registered = 0;
std::uint64_t        g_native_calls = 0;

const char* level_name(int l) { return l == 0 ? "DEBUG" : l == 1 ? "INFO" : l == 2 ? "WARN" : "ERROR"; }

void G6_CALL h_log(g6_log_level lvl, const char* who, const char* msg) {
    g_logs.push_back({ static_cast<int>(lvl), who ? who : "", msg ? msg : "" });
    std::printf("[%s] %s: %s\n", level_name(lvl), who ? who : "", msg ? msg : "");
}
g6_status G6_CALL h_invoke(g6_native_handle h, g6_native_call* c) {
    ++g_native_calls;
    return g_guard_closed ? G6_E_ONLINE_SESSION : g_mock->native_invoke(h, c);
}
g6_status G6_CALL h_invoke_hash(std::uint64_t hash, g6_native_call* c) {
    ++g_native_calls;
    return g_guard_closed ? G6_E_ONLINE_SESSION : g_mock->native_invoke_hash(hash, c);
}
g6_status G6_CALL h_register(const g6_script_desc* d, g6_script_id* out) {
    const g6_status s = g_mock->script_register(d, out);
    if (s == G6_OK) ++g_registered;
    return s;
}
g6_status G6_CALL h_unregister(g6_script_id id) {
    const g6_status s = g_mock->script_unregister(id);
    if (s == G6_OK) --g_registered;
    return s;
}

// ------------------------------------------------------------------ UI table the plugin's host table hands out
// The mock's g6_ui_api with frame_register wrapped: every registered draw callback is recorded, so a scenario can
// check that none points into Python (g6_python.dll, python312.dll, or a ctypes thunk outside any module). With
// g_ui_off the table refuses registrations like the runtime's stub ([ui] overlay=0).
g6_ui_api                g_ui_table{};
bool                     g_ui_off = false;
std::vector<const void*> g_ui_draws;

g6_status G6_CALL h_frame_register(const g6_ui_frame_desc* d, g6_ui_id* out) {
    if (g_ui_off) { if (out) *out = 0; return G6_E_NOT_IMPLEMENTED; }
    if (d) g_ui_draws.push_back(reinterpret_cast<const void*>(d->draw));
    return g6_ui_get(g_mock)->frame_register(d, out);
}
const g6_ui_api* G6_CALL h_ui(void) { return &g_ui_table; }

HMODULE module_of(const void* p) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(p), &m);
    return m;
}

// {mock ms: transcript} of a run-length UI log (g6mock::UiLog text; one frame per 16 ms inside a run).
std::map<std::uint64_t, std::string> ui_frames(const std::string& text) {
    std::map<std::uint64_t, std::string> out;
    std::uint64_t first = 0, last = 0;
    std::string cur;
    bool have = false;
    auto flush = [&] {
        if (have)
            for (std::uint64_t m = first; m <= last; m += 16) out[m] = cur;
    };
    for (std::size_t at = 0; at < text.size();) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (line.rfind("@ ", 0) == 0) {
            flush();
            have = true;
            cur.clear();
            char* end = nullptr;
            first = last = std::strtoull(line.c_str() + 2, &end, 10);
            if (end && *end == '-') last = std::strtoull(end + 1, nullptr, 10);
        } else {
            cur += line + "\n";
        }
    }
    flush();
    return out;
}

// ------------------------------------------------------------------ paths and files
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool write_file(const std::string& path, const std::string& text) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, wide(path).c_str(), L"wb") != 0 || !f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void make_dirs(const std::string& path) {
    std::string cur;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '\\' || path[i] == '/') {
            if (!cur.empty() && cur.back() != ':') CreateDirectoryW(wide(cur).c_str(), nullptr);
        }
        if (i < path.size()) cur += path[i];
    }
}

bool copy_file(const std::string& from, const std::string& to) {
    return CopyFileW(wide(from).c_str(), wide(to).c_str(), FALSE) != 0;
}

// ------------------------------------------------------------------ scenarios
struct Args { std::string plugin, sdk, home, work; };

struct Run;
struct Scenario {
    const char*                                         name;
    std::vector<std::string>                            mods;    // "examples/x.py" | "tests/x.py[:as.py]"
    std::vector<std::pair<const char*, const char*>>    events;  // mock_world kinds
    std::uint64_t                                       run_ms;
    std::int64_t                                        stop_at; // -1: at the end
    std::string                                         ini;     // extra [python] lines
    std::function<void(Run&)>                           on_frame;
    std::function<void(Run&)>                           check;
    g6_status                                           expect_load = G6_OK;
    std::vector<std::pair<const char*, const char*>>    ui_events = {};   // mock_world UI kinds ("ui-click", ...)
    bool                                                ui_off = false;   // the host table's UI refuses frames
};

struct Run {
    Args                     a;
    std::string              dir, plugins, mods;
    HMODULE                  plugin = nullptr;
    g6_plugin_load_fn        load = nullptr;
    g6_plugin_stop_fn        stop = nullptr;
    g6_plugin_unload_fn      unload = nullptr;
    bool                     stopped = false;

    std::uint64_t now() const { return g6mock::now_ms(); }

    std::vector<std::string> lines(const std::string& source) const {
        std::vector<std::string> out;
        for (const auto& l : g_logs)
            if (l.source == source) out.push_back(std::string("[") + level_name(l.level) + "] " + l.source + ": " + l.message);
        return out;
    }
    int count(const char* text, int level = -1) const {
        int n = 0;
        for (const auto& l : g_logs)
            if ((level < 0 || l.level == level) && l.message.find(text) != std::string::npos) ++n;
        return n;
    }
    int errors() const {
        int n = 0;
        for (const auto& l : g_logs) n += l.level >= G6_LOG_ERROR ? 1 : 0;
        return n;
    }
    void expect_lines(const std::string& source, const std::vector<std::string>& want) const {
        const auto got = lines(source);
        bool same = got == want;
        CHECK(same, "%s: %zu log line(s), expected %zu", source.c_str(), got.size(), want.size());
        if (!same) {
            for (const auto& g : got) std::printf("    got:  %s\n", g.c_str());
            for (const auto& w : want) std::printf("    want: %s\n", w.c_str());
        }
    }
    void expect_clean() const { CHECK(errors() == 0 && g6mock::world_errors() == 0, "%d error(s) logged", errors()); }
    bool python_loaded() const { return GetModuleHandleW(L"python312.dll") != nullptr; }

    g6mock::UiLog ui;   // every mock UI frame drawn after a tick

    // The UI checks shared by the UI scenarios: nothing unbalanced or unconsumed, every frame unregistered by the
    // unload, and no draw callback that could run Python on the render thread.
    void expect_ui_clean() const {
        const std::string problems = g6mock::ui_problems(true);
        CHECK(problems.empty(), "mock UI: %s", problems.c_str());
        CHECK(g6uiret::live_frames() == 0, "%u retained frame(s) left after unload", g6uiret::live_frames());
        CHECK(!g_ui_draws.empty(), "no UI frame was registered");
        const HMODULE self = GetModuleHandleW(nullptr);
        for (const void* d : g_ui_draws) {
            const HMODULE m = module_of(d);
            CHECK(m == self, "a UI draw callback (%p) is not in g6_python_test.exe (module %p: %s)", d,
                  static_cast<void*>(m), m ? "another module" : "no module, e.g. a ctypes thunk");
        }
    }
    // The UI log against sdk/tests/ui/<name>: equal, except the frames in `allowed` (ms -> the expected transcript).
    void expect_ui_log(const char* name, const std::map<std::uint64_t, std::string>& allowed = {}) const {
        bool ok = false;
        const std::string want_text = g6mock::read_ui_log(a.sdk + "\\tests\\ui\\" + name, ok);
        CHECK(ok, "cannot read sdk\\tests\\ui\\%s", name);
        const auto got = ui_frames(ui.text()), want = ui_frames(want_text);
        CHECK(got.size() == want.size(), "%zu UI frame(s) drawn, %s has %zu", got.size(), name, want.size());
        int differing = 0;
        for (const auto& [ms, text] : want) {
            const auto g = got.find(ms);
            const auto al = allowed.find(ms);
            const std::string& expect = al != allowed.end() ? al->second : text;
            if (g != got.end() && g->second == expect) continue;
            if (differing++ < 2)
                std::printf("    UI frame at %llu ms%s; got:\n%s    want:\n%s", static_cast<unsigned long long>(ms),
                            al != allowed.end() ? " (with its allowed difference)" : "",
                            g != got.end() ? g->second.c_str() : "(none)\n", expect.c_str());
        }
        CHECK(differing == 0, "%d of %zu UI frame(s) differ from %s", differing, want.size(), name);
    }
};

// `text` without the first occurrence of `line` (an allowed difference of one UI frame).
std::string without(std::string text, const std::string& line) {
    if (const auto p = text.find(line); p != std::string::npos) text.erase(p, line.size());
    return text;
}

// The scenario's own lines come first: GetPrivateProfileString returns the first occurrence of a key.
std::string ini_for(const Run& r, const Scenario& s) {
    return "[python]\n" + s.ini + "enable=1\nhome=" + r.a.home + "\nsdk_path=" + r.a.sdk +
           "\\python\nmods_dir=mods\\python\nallow_unverified=0\n";
}

bool stage_mod(const Run& r, const std::string& spec) {
    std::string src = spec, as;
    if (const auto colon = spec.find(':'); colon != std::string::npos) { src = spec.substr(0, colon); as = spec.substr(colon + 1); }
    std::string from = src.rfind("examples/", 0) == 0 ? r.a.sdk + "\\examples\\python\\" + src.substr(9)
                                                       : r.a.sdk + "\\python\\host\\tests\\mods\\" + src.substr(6);
    if (as.empty()) as = from.substr(from.find_last_of('\\') + 1);
    return copy_file(from, r.mods + "\\" + as);
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> v;
    // ---- the five examples (sdk/tests/python/test_script_layer.py TestExampleMods)
    v.push_back({ "god_mode", { "examples/god_mode.py" },
                  { { "damage", "50@496" }, { "press", "INPUT_DETONATE@1008" }, { "damage", "50@2000" },
                    { "press", "INPUT_DETONATE@3008" }, { "damage", "30@3504" } },
                  4000, -1, "", nullptr, [](Run& r) {
                      r.expect_lines("god_mode", { "[INFO] god_mode: god mode ON", "[INFO] god_mode: god mode OFF" });
                      const auto p = g6mock::player();
                      CHECK(p.health == 170, "player health %d, expected 170", p.health);
                      CHECK(!p.invincible && p.can_ragdoll, "god mode flags left on");
                      CHECK(r.count("mod god_mode loaded") == 1, "no load line");
                      r.expect_clean();
                  } });
    v.push_back({ "never_wanted", { "examples/never_wanted.py" },
                  { { "press", "INPUT_VEH_HEADLIGHT@496" }, { "wanted", "3@1504" }, { "press", "INPUT_VEH_HEADLIGHT@3008" } },
                  4000, -1, "", nullptr, [](Run& r) {
                      r.expect_lines("never_wanted", { "[INFO] never_wanted: never wanted ON (max wanted level was 5)",
                                                       "[INFO] never_wanted: cleared wanted level 3",
                                                       "[INFO] never_wanted: never wanted OFF (max wanted level restored to 5)" });
                      CHECK(g6mock::wanted_level() == 0 && g6mock::max_wanted_level() == 5, "wanted %d/%d",
                            g6mock::wanted_level(), g6mock::max_wanted_level());
                      r.expect_clean();
                  } });
    v.push_back({ "waypoint_teleport", { "examples/waypoint_teleport.py" },
                  { { "press", "INPUT_VEH_DUCK@496" }, { "waypoint", "250,-400,0@1008" }, { "press", "INPUT_VEH_DUCK@1504" } },
                  3000, -1, "", nullptr, [](Run& r) {
                      r.expect_lines("waypoint_teleport", { "[WARN] waypoint_teleport: no waypoint set",
                                                            "[INFO] waypoint_teleport: teleported to 250.0 -400.0 13.0" });
                      const auto p = g6mock::player();
                      CHECK(p.x == 250.f && p.y == -400.f && p.z == 13.f, "player at %.2f %.2f %.2f", p.x, p.y, p.z);
                      r.expect_clean();
                  } });
    v.push_back({ "vehicle_spawner", { "examples/vehicle_spawner.py" },
                  { { "press", "INPUT_CONTEXT_SECONDARY@496" }, { "damage", "150@2000" } },
                  3000, -1, "", nullptr, [](Run& r) {
                      r.expect_lines("vehicle_spawner", { "[INFO] vehicle_spawner: spawned adder and seated the player",
                                                          "[INFO] vehicle_spawner: repaired vehicle (850 -> 1000)" });
                      const auto p = g6mock::player();
                      CHECK(p.vehicle != 0 && p.vehicle_health == 1000, "vehicle 0x%llx health %d",
                            static_cast<unsigned long long>(p.vehicle), p.vehicle_health);
                      CHECK(std::fabs(p.y - (-1995.f)) < 1e-3f, "vehicle y %.3f, expected -1995", p.y);
                      CHECK(p.health == 50, "driver health %d, expected 50", p.health);
                      r.expect_clean();
                  } });
    v.push_back({ "time_control", { "examples/time_control.py" },
                  { { "press", "INPUT_MULTIPLAYER_INFO@496" }, { "press", "INPUT_MULTIPLAYER_INFO@1008" },
                    { "press", "INPUT_SPECIAL_ABILITY@1504" }, { "press", "INPUT_MULTIPLAYER_INFO@1600+1200" } },
                  3000, -1, "", nullptr, [](Run& r) {
                      r.expect_lines("time_control", { "[INFO] time_control: time scale 0.50", "[INFO] time_control: time scale 0.20",
                                                       "[INFO] time_control: clock 13:00",
                                                       "[INFO] time_control: time scale reset to 1.00" });
                      CHECK(g6mock::time_scale() == 1.f, "time scale %.2f", g6mock::time_scale());
                      CHECK(g6mock::clock_seconds() == 13 * 3600, "clock %lld s", static_cast<long long>(g6mock::clock_seconds()));
                      r.expect_clean();
                  } });
    // ---- the stop event (g6_plugin_stop) switches god mode off; ticks after it run no mod code
    v.push_back({ "stop_event", { "examples/god_mode.py" },
                  { { "press", "INPUT_DETONATE@496" }, { "damage", "50@2000" } }, 3008, 1504, "", nullptr, [](Run& r) {
                      r.expect_lines("god_mode", { "[INFO] god_mode: god mode ON", "[INFO] god_mode: god mode OFF" });
                      const auto p = g6mock::player();
                      CHECK(!p.invincible && p.health == 150, "after stop: invincible %d health %d", p.invincible ? 1 : 0, p.health);
                      CHECK(r.count("stop event: 1 mod(s) stopped; 0 cleanup failure(s)") == 1, "no stop summary");
                      CHECK(r.count("Python host shut down") == 1, "no shutdown line");
                      r.expect_clean();
                  } });
    // ---- failure isolation: load error, script error, runaway loop (watchdog, 300 ms) -> only that mod is disabled
    v.push_back({ "isolation", { "tests/good.py", "tests/bad_load.py", "tests/bad_tick.py", "tests/runaway.py" }, {},
                  1600, -1, "tick_budget_ms=300\n", nullptr, [](Run& r) {
                      CHECK(r.count("broken at import", G6_LOG_ERROR) >= 1, "bad_load error not logged");
                      CHECK(r.count("mod bad_load disabled: load failed") == 1, "bad_load not disabled");
                      CHECK(r.count("broken in a tick", G6_LOG_ERROR) >= 1, "bad_tick error not logged");
                      CHECK(r.count("mod bad_tick disabled: script bad_tick.run failed") == 1, "bad_tick not disabled");
                      CHECK(r.count("bad_tick cleanup ran") == 1, "bad_tick cleanup did not run");
                      CHECK(r.count("TickBudgetExceeded: ran longer than 300 ms", G6_LOG_ERROR) == 1, "watchdog did not fire once");
                      CHECK(r.count("mod runaway disabled: tick failed") == 1, "runaway not disabled");
                      CHECK(r.count("runaway cleanup ran") == 1, "runaway cleanup did not run");
                      CHECK(r.count("good tick 90") == 1, "good mod stopped ticking");
                      CHECK(r.count("good stopped after") == 1, "good mod's stop handler did not run");
                      CHECK(r.count("stop event: 1 mod(s) stopped") == 1, "stop event did not cover exactly the good mod");
                  } });
    // ---- environment and _g6 contract, checked by a mod from inside the interpreter
    v.push_back({ "environment", { "tests/env_check.py" }, {}, 200, -1, "", nullptr, [](Run& r) {
                      CHECK(r.count("env ok") == 1, "env_check did not report ok");
                      CHECK(r.count("hello from print") == 1, "print() did not reach the log");
                      CHECK(r.count("isolated=1, no_site=1") == 1, "boot line does not show an isolated interpreter");
                      r.expect_clean();
                  } });
    // ---- off by default: enable=0 registers nothing and never loads python312.dll
    v.push_back({ "disabled", { "examples/god_mode.py" }, { { "press", "INPUT_DETONATE@496" } }, 1000, -1, "enable=0\n", nullptr,
                  [](Run& r) {
                      CHECK(g_registered == 0, "%d script(s) registered", g_registered);
                      CHECK(!r.python_loaded(), "python312.dll loaded although enable=0");
                      CHECK(r.count("disabled ([python] enable=1 not set") == 1, "no disabled line");
                      CHECK(r.lines("god_mode").empty(), "a mod ran");
                      r.expect_clean();
                  } });
    // ---- guard closed from the start: Python never starts (re-checked every 500 ms of wall time)
    v.push_back({ "guard_closed", { "examples/god_mode.py" }, {}, 800, -1, "guard_retry_ms=500\n",
                  [](Run& r) { if (r.now() == 0) g_guard_closed = true; },
                  [](Run& r) {
                      CHECK(!r.python_loaded(), "python312.dll loaded while the guard is closed");
                      CHECK(r.count("single-player guard closed") == 1, "guard line not logged exactly once");
                      CHECK(r.lines("god_mode").empty(), "a mod ran");
                  } });
    // ---- guard trips mid-run: every mod pauses, stop handlers are skipped (natives would be refused)
    v.push_back({ "guard_trip", { "examples/god_mode.py", "tests/good.py" }, { { "press", "INPUT_DETONATE@496" } }, 2000, -1, "",
                  [](Run& r) { if (r.now() == 1008) g_guard_closed = true; },
                  [](Run& r) {
                      r.expect_lines("god_mode", { "[INFO] god_mode: god mode ON" });
                      CHECK(r.count("G6_E_ONLINE_SESSION (single-player guard closed); every Python mod is paused") == 1,
                            "no pause line");
                      CHECK(r.count("good stopped after") == 0, "a stop handler ran while the guard was closed");
                      CHECK(r.count("scripts cancelled without stop handlers (guard closed)") == 1, "stop summary missing");
                      CHECK(r.count(" disabled") == 0, "a mod was disabled instead of paused");
                  } });
    // ---- hot reload (file replaced at 480 ms), then unload + load in the same process (interpreter reused)
    v.push_back({ "reload", { "tests/live_v1.py:live.py" }, {}, 1600, -1, "hot_reload=1\nreload_poll_ms=0\n",
                  [](Run& r) {
                      if (r.now() == 480) {
                          CHECK(copy_file(r.a.sdk + "\\python\\host\\tests\\mods\\live_v2.py", r.mods + "\\live.py"), "copy v2");
                      }
                      if (r.now() == 800) {
                          r.stop();
                          r.unload();
                          CHECK(g_registered == 0, "tick still registered after unload");
                          const g6_status st = r.load(&g_api);
                          CHECK(st == G6_OK, "second g6_plugin_load returned %d", static_cast<int>(st));
                      }
                  },
                  [](Run& r) {
                      r.expect_lines("live", { "[INFO] live: v1 running", "[INFO] live: v1 stopped", "[INFO] live: version two running",
                                               "[INFO] live: version two stopped", "[INFO] live: version two running",
                                               "[INFO] live: version two stopped" });
                      CHECK(r.count("mod live: file changed, reloading") == 1, "no reload line");
                      CHECK(r.count("reusing the interpreter") == 1, "second load did not reuse the interpreter");
                      r.expect_clean();
                  } });
    // ---- a bad config refuses to load and registers nothing
    v.push_back({ "bad_config", {}, {}, 100, -1, "tick_budget_ms=5\n", nullptr, [](Run& r) {
                      CHECK(g_registered == 0, "%d script(s) registered", g_registered);
                      CHECK(r.count("tick_budget_ms='5'", G6_LOG_ERROR) == 1, "bad value not reported");
                      CHECK(!r.python_loaded(), "python312.dll loaded");
                  }, G6_E_INVALID_ARG });
    // ---- in-game Python UI: gta6mod.ui -> EmbeddedUi -> g6_ui_retained_* (this exe) -> mock UI on the render thread.
    // The trainer menu with the Luau run's flags: every frame equal to the Luau transcript.
    v.push_back({ "ui_trainer", { "examples/trainer_menu.py" },
                  { { "damage", "30@240" }, { "damage", "50@1008" }, { "wanted", "3@2000" } }, 3000, -1, "", nullptr,
                  [](Run& r) {
                      r.expect_lines("trainer_menu", { "[INFO] trainer_menu: god mode ON",
                                                       "[INFO] trainer_menu: never wanted ON (max wanted level was 5)",
                                                       "[INFO] trainer_menu: cleared wanted level 3", "[INFO] trainer_menu: healed",
                                                       "[INFO] trainer_menu: god mode OFF",
                                                       "[INFO] trainer_menu: never wanted OFF (max wanted level restored to 5)" });
                      const auto p = g6mock::player();
                      CHECK(p.health == 200 && !p.invincible, "player health %d invincible %d (healed by god mode, the "
                            "damage at 1008 blocked, god mode off after the stop event)", p.health, p.invincible ? 1 : 0);
                      CHECK(g6mock::wanted_level() == 0 && g6mock::max_wanted_level() == 5, "wanted %d/%d",
                            g6mock::wanted_level(), g6mock::max_wanted_level());
                      r.expect_ui_log("trainer_menu.ui.txt");
                      CHECK(g_ui_draws.size() == 1, "%zu frame registration(s)", g_ui_draws.size());
                      r.expect_ui_clean();
                      r.expect_clean();
                  },
                  G6_OK, { { "ui-click", "God mode@496" }, { "ui-click", "Never wanted@1504" }, { "ui-click", "Heal@2496" } } });
    // Every widget (twin of the Luau ui_widgets scenario): equal to the Luau transcript except at 96 ms, where the
    // tree opened by that frame's click shows its contents one frame later (the program recorded none).
    v.push_back({ "ui_widgets", { "tests/ui_widgets.py" }, {}, 1500, -1, "", nullptr,
                  [](Run& r) {
                      CHECK(r.count("PASS ui_widgets") == 1, "ui_widgets did not pass (see its FAIL line)");
                      bool ok = false;
                      auto want = ui_frames(g6mock::read_ui_log(r.a.sdk + "\\tests\\ui\\ui_widgets.ui.txt", ok));
                      CHECK(ok && want.count(96) == 1, "ui_widgets.ui.txt has no frame at 96 ms");
                      r.expect_ui_log("ui_widgets.ui.txt", { { 96, without(want[96], "      text inside\n") } });
                      r.expect_ui_clean();
                      r.expect_clean();
                  },
                  G6_OK,
                  { { "ui-click", "Heal@96" }, { "ui-click", "God mode@96" }, { "ui-set", "Wanted=4@96" }, { "ui-set", "Speed=2.5@96" },
                    { "ui-set", "Name=Jason@96" }, { "ui-set", "Weather=Fog@96" }, { "ui-click", "Advanced@96" },
                    { "ui-click", "Second@96" }, { "ui-set", "Tint=0.1,0.2,0.3,0.4@96" }, { "ui-menu", "0@496" },
                    { "ui-menu", "1@800" } } });
    // The overlay off ([ui] overlay=0: frame_register -> G6_E_NOT_IMPLEMENTED): the menu is skipped, the mod runs.
    v.push_back({ "ui_off", { "examples/trainer_menu.py" }, { { "wanted", "3@96" } }, 500, -1, "", nullptr,
                  [](Run& r) {
                      CHECK(r.count("no in-game UI (overlay off") == 1, "no overlay-off line");
                      CHECK(r.count("mod trainer_menu loaded") == 1, "the mod did not load");
                      CHECK(g_ui_draws.empty() && g6uiret::live_frames() == 0 && r.ui.frames() == 0, "a frame was registered");
                      CHECK(g6mock::wanted_level() == 3, "wanted %d (nothing clears it while the menu is off)", g6mock::wanted_level());
                      r.expect_clean();
                  },
                  G6_OK, {}, true });
    // Hot reload of a UI mod next to another UI mod: only the reloaded mod's frame is replaced.
    v.push_back({ "ui_reload", { "examples/trainer_menu.py", "tests/ui_live_v1.py:ui_live.py" }, {}, 1200, -1,
                  "hot_reload=1\nreload_poll_ms=0\n",
                  [](Run& r) {
                      if (r.now() == 480) {
                          CHECK(r.ui.last().find("text live v1") != std::string::npos, "v1 frame not drawn before the reload");
                          CHECK(copy_file(r.a.sdk + "\\python\\host\\tests\\mods\\ui_live_v2.py", r.mods + "\\ui_live.py"), "copy v2");
                      }
                  },
                  [](Run& r) {
                      CHECK(r.count("mod ui_live: file changed, reloading") == 1, "no reload line");
                      const std::string& last = r.ui.last();
                      CHECK(last.find("frame trainer_menu") != std::string::npos && last.find("text live version two") != std::string::npos &&
                                last.find("live v1") == std::string::npos,
                            "last frame after the reload:\n%s", last.c_str());
                      CHECK(g_ui_draws.size() == 3, "%zu frame registration(s), expected 3 (trainer, v1, v2)", g_ui_draws.size());
                      r.expect_ui_clean();
                      r.expect_clean();
                  } });
    return v;
}

int run_scenario(const Scenario& s, const Args& a) {
    Run r;
    r.a = a;
    r.dir = a.work + "\\" + s.name;
    remove_tree(wide(r.dir));
    r.plugins = r.dir + "\\plugins";
    r.mods = r.plugins + "\\mods\\python";
    make_dirs(r.mods);
    const std::string plugin = r.plugins + "\\g6_python.dll";
    CHECK(copy_file(a.plugin, plugin), "copy %s", a.plugin.c_str());
    CHECK(write_file(r.plugins + "\\g6_python.ini", ini_for(r, s)), "write ini");
    for (const auto& m : s.mods) CHECK(stage_mod(r, m), "stage %s", m.c_str());

    g_mock = g6_get_host_api();
    g_api = *g_mock;
    g_api.log = h_log;
    g_api.native_invoke = h_invoke;
    g_api.native_invoke_hash = h_invoke_hash;
    g_api.script_register = h_register;
    g_api.script_unregister = h_unregister;
    CHECK(g6mock::install(g_mock), "mock world install");
    g_ui_off = s.ui_off;
    g_ui_draws.clear();
    if (const g6_ui_api* mui = g6_ui_get(g_mock)) {
        g_ui_table = *mui;
        g_ui_table.frame_register = h_frame_register;
        g_api.ui = h_ui;
    }
    CHECK(g6_ui_get(&g_api) != nullptr, "mock UI table");
    g6uiret::bind(&g_api);   // as gta6mod_runtime.dll binds its own table

    std::vector<g6mock::Event> events;
    for (const auto& [kind, spec] : s.events) {
        g6mock::Event ev{};
        std::string err;
        CHECK(g6mock::parse_event(kind, spec, ev, err), "%s", err.c_str());
        events.push_back(ev);
    }
    std::vector<g6mock::UiEvent> ui_events;
    for (const auto& [kind, spec] : s.ui_events) {
        g6mock::UiEvent ev{};
        std::string err;
        CHECK(g6mock::parse_ui_event(kind, spec, ev, err), "%s", err.c_str());
        ui_events.push_back(ev);
    }

    r.plugin = LoadLibraryW(wide(plugin).c_str());
    CHECK(r.plugin != nullptr, "LoadLibrary(g6_python.dll) failed: %lu", GetLastError());
    if (!r.plugin) return 1;
    auto query = reinterpret_cast<g6_plugin_query_fn>(GetProcAddress(r.plugin, "g6_plugin_query"));
    r.load = reinterpret_cast<g6_plugin_load_fn>(GetProcAddress(r.plugin, "g6_plugin_load"));
    r.stop = reinterpret_cast<g6_plugin_stop_fn>(GetProcAddress(r.plugin, "g6_plugin_stop"));
    r.unload = reinterpret_cast<g6_plugin_unload_fn>(GetProcAddress(r.plugin, "g6_plugin_unload"));
    CHECK(query && r.load && r.stop && r.unload, "missing plugin exports");
    if (!query || !r.load || !r.stop || !r.unload) return 1;
    const g6_plugin_info* info = query();
    CHECK(info && info->size >= sizeof(g6_plugin_info) && info->api_major == G6_API_VERSION_MAJOR &&
              std::strcmp(info->name, "python-host") == 0, "plugin info");
    CHECK(!r.python_loaded(), "python312.dll loaded by LoadLibrary of the plugin (must be delay-loaded)");

    const g6_status st = r.load(&g_api);
    CHECK(st == s.expect_load, "g6_plugin_load returned %d, expected %d", static_cast<int>(st), static_cast<int>(s.expect_load));
    if (st == G6_OK) {
        while (g6mock::now_ms() < s.run_ms) {
            const std::uint64_t t = g6mock::now_ms();
            for (const auto& ev : events) if (ev.at == t) g6mock::fire(ev);
            for (const auto& ev : ui_events) if (ev.at == t) g6mock::fire_ui(ev);
            if (s.on_frame) s.on_frame(r);
            if (s.stop_at >= 0 && t == static_cast<std::uint64_t>(s.stop_at)) r.stop();
            g6_mock_step(16);
            // One overlay frame after the tick, on another thread (as the render thread): it replays what the
            // Python frame functions recorded during the tick and never enters Python.
            std::thread render([&r, t] { g6mock::draw_ui_frame(t, r.ui); });
            render.join();
            g6mock::end_tick();
        }
        r.stop();    // the runtime raises the stop event before unloading
        r.unload();
        CHECK(g_registered == 0, "%d script(s) still registered after unload", g_registered);
    }
    if (s.check) s.check(r);
    std::printf("%llu ms, %llu native calls; ", static_cast<unsigned long long>(g6mock::now_ms()),
                static_cast<unsigned long long>(g_native_calls));
    g6mock::print_summary(stdout);
    // The plugin pinned itself once Python ran; FreeLibrary is a no-op then, and the process ends here anyway.
    return g_fail ? 1 : 0;
}

int child(const std::string& name, const Args& a) {
    for (const auto& s : scenarios()) {
        if (name != s.name) continue;
        const int rc = run_scenario(s, a);
        std::printf("%s %s (%d check(s) passed, %d failed)\n", rc ? "FAIL" : "PASS", s.name, g_pass, g_fail);
        std::fflush(stdout);
        if (std::FILE* f = nullptr; _wfopen_s(&f, wide(a.work + "\\" + s.name + "\\result.txt").c_str(), L"w") == 0 && f) {
            std::fprintf(f, "%d %d\n", g_pass, g_fail);
            std::fclose(f);
        }
        return rc;
    }
    std::printf("unknown scenario %s\n", name.c_str());
    return 2;
}

int parent(const char* self, const Args& a) {
    make_dirs(a.work);
    int passed = 0, failed = 0, checks = 0;
    for (const auto& s : scenarios()) {
        std::wstring cmd = L"\"" + wide(self) + L"\" --scenario " + wide(s.name) + L" \"" + wide(a.plugin) + L"\" \"" + wide(a.sdk) +
                           L"\" \"" + wide(a.home) + L"\" \"" + wide(a.work) + L"\"";
        std::printf("=== %s\n", s.name);
        std::fflush(stdout);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            std::printf("FAIL %s: CreateProcess error %lu\n", s.name, GetLastError());
            ++failed;
            continue;
        }
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 3);
            WaitForSingleObject(pi.hProcess, 5000);
            std::printf("FAIL %s: timed out after 120 s\n", s.name);
        }
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        int p = 0, f = 0;
        if (std::FILE* rf = nullptr; _wfopen_s(&rf, wide(a.work + "\\" + s.name + "\\result.txt").c_str(), L"r") == 0 && rf) {
            if (fscanf_s(rf, "%d %d", &p, &f) != 2) p = f = 0;
            std::fclose(rf);
        }
        checks += p;
        if (code == 0 && f == 0 && p > 0) ++passed;
        else { ++failed; std::printf("FAIL %s: exit code %lu\n", s.name, static_cast<unsigned long>(code)); }
    }
    std::printf("g6_python_test: %d of %d scenario(s) passed, %d check(s) passed%s\n", passed, passed + failed, checks,
                failed ? "; FAILED" : "");
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 7 && std::strcmp(argv[1], "--scenario") == 0) {
        const Args a{ argv[3], argv[4], argv[5], argv[6] };
        return child(argv[2], a);
    }
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <g6_python.dll> <sdk dir> <python home> <work dir>\n"
                             "       %s --scenario NAME <g6_python.dll> <sdk dir> <python home> <work dir>\n", argv[0], argv[0]);
        return 2;
    }
    const Args a{ argv[1], argv[2], argv[3], argv[4] };
    return parent(argv[0], a);
}
