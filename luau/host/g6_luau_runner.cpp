// g6_luau_runner — run Luau mods offline against the mock runtime (gta6mod_mock.dll).
//   g6_luau_runner <lib_dir> <mod_init.luau> [ms=2000] [--press CTL@MS[+HOLD]]... [--damage N@MS]...
//                  [--wanted N@MS]... [--waypoint X,Y,Z@MS]... [--unload-at MS] [--expect-clean]
//                  [--expect-log TEXT]... [--expect-live N] [--ui-click LABEL@MS]... [--ui-set LABEL=VALUE@MS]...
//                  [--ui-menu 0|1@MS]... [--expect-ui TEXT]... [--print-ui] [--ui-log FILE] [--expect-ui-log FILE]
// The fake world (natives and scenario events) is runtime/src/mock_world.cpp, shared with the Python host test;
// it has the same semantics as the Python mock world
// (sdk/python/gta6mod/runtime/mock.py; the contract is "Mock world" in sdk/docs/script_layer.md)
// so a Luau mod and its Python twin behave the same: player ped 0x100 at (-1000, -2000, 15),
// spawned handles from 0x101, models load 50 ms after REQUEST_MODEL, CREATE_VEHICLE returns 0
// until then, a pressed control is held for its +HOLD ms (one tick by default), flat ground at z = 12 once
// collision is loaded (100 ms after the first request for a cell), wanted level 0 / max 5, clock
// 12:00:00. Each native is registered under its seed (GTA V) hash and, where re-db has one, the
// observed GTA VI hash.
// Scenario events fire on the tick whose time equals MS (use multiples of 16):
//   --press CTL@MS[+HOLD] action CTL pressed on control group 0 at MS and held HOLD ms (default: that tick
//                         only), then just released on the first tick at or after MS+HOLD; CTL is an
//                         INPUT_* name (hashed with g6_joaat) or a number (decimal, or 0x hex script key)
//   --damage N@MS         player ped (and the vehicle it is in) lose N health unless invincible
//   --wanted N@MS         wanted level = N (a crime: the max wanted level is not applied)
//   --waypoint X,Y,Z@MS   set the map waypoint
//   --unload-at MS        raise the host's stop event (g6_luau_stop, what g6_plugin_stop does) at MS; ticks go on
//   --expect-clean        exit 1 if anything was logged at ERROR (failed script, failed load)
//   --expect-log TEXT     exit 1 unless some log message contains TEXT (repeatable: all must appear)
//   --expect-live N       exit 1 unless exactly N scripts are still live when the run ends
// Mock UI (gta6mod_mock.dll records the g6_ui_api calls; sdk/docs/script_layer.md "Mock UI"; the driver is
// runtime/src/mock_world.cpp, shared with the Python host test): after every tick the runner draws one UI frame
// (g6_mock_ui_frame), menu open unless --ui-menu says otherwise.
//   --ui-click LABEL@MS   the next widget labelled LABEL in the frame at MS is clicked (button, checkbox, tree, tab)
//   --ui-set L=V@MS       the next widget labelled L takes value V (checkbox 1/0, slider, text, combo index/item, r,g,b,a)
//   --ui-menu 0|1@MS      hide/show the menu (hidden: only frames registered with { always = true } run)
//   --expect-ui TEXT      exit 1 unless some frame's transcript contains TEXT (repeatable)
//   --print-ui            print the last frame's transcript at the end
//   --ui-log FILE         write every frame's transcript, run-length encoded by mock time (sdk/tests/ui/*.ui.txt)
//   --expect-ui-log FILE  exit 1 unless that log equals FILE (transcript parity with the other hosts)
// --expect-clean also fails on unbalanced UI calls (g6_mock_ui_errors), interactions nothing consumed and frames
// left registered after destroy.
#include "g6_luau.h"
#include "mock_world.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Expect { std::string text; bool seen; };

const g6_host_api*   g_mock = nullptr;   // the mock DLL's table
g6_host_api          g_api{};            // what the Luau host sees: mock + counting log/invoke
uint64_t             g_native_calls = 0;
int                  g_errors = 0;
std::vector<Expect>  g_expect;

void G6_CALL counting_log(g6_log_level lvl, const char* who, const char* msg) {
    if (lvl >= G6_LOG_ERROR) ++g_errors;
    if (msg) for (auto& e : g_expect) if (std::strstr(msg, e.text.c_str())) e.seen = true;
    g_mock->log(lvl, who, msg);
}
g6_status G6_CALL counting_invoke(g6_native_handle h, g6_native_call* c) {
    ++g_native_calls;
    return g_mock->native_invoke(h, c);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <lib_dir> <init.luau> [ms] [--press CTL@MS[+HOLD]]... [--damage N@MS]... [--wanted N@MS]..."
                             " [--waypoint X,Y,Z@MS]... [--unload-at MS] [--expect-clean] [--expect-log TEXT]..."
                             " [--expect-live N] [--ui-click LABEL@MS]... [--ui-set LABEL=VALUE@MS]... [--ui-menu 0|1@MS]..."
                             " [--expect-ui TEXT]... [--print-ui] [--ui-log FILE] [--expect-ui-log FILE]\n", argv[0]);
        return 2;
    }
    uint64_t run_ms = 2000;
    bool expect_clean = false;
    bool unload = false;
    uint64_t unload_at = 0;
    int expect_live = -1;
    std::vector<g6mock::Event> events;
    std::vector<g6mock::UiEvent> ui_events;
    std::vector<std::string> expect_ui;
    bool print_ui = false;
    const char* ui_log_path = nullptr;
    const char* expect_ui_log = nullptr;
    for (int i = 3; i < argc; ++i) {
        const bool has_val = i + 1 < argc;
        const char* opt = argv[i];
        if ((!std::strcmp(opt, "--press") || !std::strcmp(opt, "--damage") || !std::strcmp(opt, "--wanted") ||
             !std::strcmp(opt, "--waypoint")) && has_val) {
            g6mock::Event ev{};
            std::string err;
            if (!g6mock::parse_event(opt + 2, argv[++i], ev, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 2;
            }
            events.push_back(ev);
        } else if ((!std::strcmp(opt, "--ui-click") || !std::strcmp(opt, "--ui-set") || !std::strcmp(opt, "--ui-menu")) && has_val) {
            g6mock::UiEvent ev{};
            std::string err;
            if (!g6mock::parse_ui_event(opt + 2, argv[++i], ev, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 2;
            }
            ui_events.push_back(ev);
        } else if (!std::strcmp(opt, "--expect-ui") && has_val) {
            expect_ui.push_back(argv[++i]);
        } else if (!std::strcmp(opt, "--print-ui")) {
            print_ui = true;
        } else if (!std::strcmp(opt, "--ui-log") && has_val) {
            ui_log_path = argv[++i];
        } else if (!std::strcmp(opt, "--expect-ui-log") && has_val) {
            expect_ui_log = argv[++i];
        } else if (!std::strcmp(opt, "--press-ms") && has_val) {  // older form: control 51 at N ms
            events.push_back({ std::strtoull(argv[++i], nullptr, 10), g6mock::Ev::Press, 51, 0.f, 0.f, 0.f, 0 });
        } else if (!std::strcmp(opt, "--unload-at") && has_val) {
            unload = true;
            unload_at = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(opt, "--expect-live") && has_val) {
            expect_live = std::atoi(argv[++i]);
        } else if (!std::strcmp(opt, "--expect-clean")) {
            expect_clean = true;
        } else if (!std::strcmp(opt, "--expect-log") && has_val) {
            g_expect.push_back({ argv[++i], false });
        } else if (opt[0] != '-') {
            run_ms = std::strtoull(opt, nullptr, 10);
        } else {
            std::fprintf(stderr, "unknown option %s\n", opt);
            return 2;
        }
    }

    g_mock = g6_get_host_api();
    if (!g_mock || g_mock->version_major != G6_API_VERSION_MAJOR || g_mock->size < sizeof(g6_host_api)) {
        std::fprintf(stderr, "gta6mod_mock.dll: incompatible host API\n");
        return 1;
    }
    g_api = *g_mock;
    g_api.log = counting_log;
    g_api.native_invoke = counting_invoke;
    // &g_api: the world's own log lines (objective shown/cleared) count for --expect-log
    if (!g6mock::install(&g_api)) { std::fprintf(stderr, "mock world: native registration failed\n"); return 1; }

    g6_luau_config cfg = { sizeof(g6_luau_config), argv[1], ".", 1 /* mock: allow unverified */ };
    g6_luau_host* host = g6_luau_create(&g_api, &cfg);
    if (!host) { std::fprintf(stderr, "g6_luau_create failed\n"); return 1; }
    if (g6_luau_run_file(host, argv[2]) != 0) { g6_luau_destroy(host); return 1; }

    g6mock::UiLog ui_log;
    while (g6mock::now_ms() < run_ms) {
        const uint64_t t = g6mock::now_ms();
        for (const g6mock::Event& ev : events) if (ev.at == t) g6mock::fire(ev);
        for (const g6mock::UiEvent& ev : ui_events) if (ev.at == t) g6mock::fire_ui(ev);
        if (unload && g6mock::now_ms() == unload_at) {
            const int failed = g6_luau_stop(host);
            std::printf("stop event at %llu ms: %d handler(s) failed; %d live scripts\n",
                        (unsigned long long)g6mock::now_ms(), failed, g6_luau_script_count(host));
        }
        g6_luau_tick(host);
        g6_mock_step(16);
        g6mock::draw_ui_frame(t, ui_log);
        g6mock::end_tick();
    }
    g_errors += g6mock::world_errors();
    std::printf("ran %llu ms; %llu native calls; %d live scripts; %d errors; %zu entities\n",
                (unsigned long long)g6mock::now_ms(), (unsigned long long)g_native_calls, g6_luau_script_count(host), g_errors,
                g6mock::entity_count());
    g6mock::print_summary(stdout);
    const int live = g6_luau_script_count(host);
    g6_luau_destroy(host);
    if (print_ui) std::printf("last UI frame:\n%s", ui_log.last().c_str());
    if (ui_log_path) {
        std::FILE* f = std::fopen(ui_log_path, "wb");
        const std::string text = ui_log.text();
        const bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (f) std::fclose(f);
        if (!ok) { std::fprintf(stderr, "FAIL: cannot write %s\n", ui_log_path); return 1; }
    }
    if (expect_clean) {
        if (const std::string ui = g6mock::ui_problems(true); !ui.empty()) { std::fprintf(stderr, "FAIL: %s", ui.c_str()); return 1; }
    }
    if (expect_clean && g_errors) { std::fprintf(stderr, "FAIL: %d error(s) logged\n", g_errors); return 1; }
    if (expect_live >= 0 && live != expect_live) {
        std::fprintf(stderr, "FAIL: %d live scripts, expected %d\n", live, expect_live);
        return 1;
    }
    int missing = 0;
    for (const auto& e : g_expect)
        if (!e.seen) { std::fprintf(stderr, "FAIL: no log message containing \"%s\"\n", e.text.c_str()); ++missing; }
    for (const auto& e : expect_ui)
        if (!ui_log.contains(e)) { std::fprintf(stderr, "FAIL: no UI frame containing \"%s\"\n", e.c_str()); ++missing; }
    if (expect_ui_log) {
        if (const std::string diff = g6mock::compare_ui_log(ui_log.text(), expect_ui_log); !diff.empty()) {
            std::fprintf(stderr, "FAIL: UI transcript differs from %s: %s\n", expect_ui_log, diff.c_str());
            ++missing;
        }
    }
    return missing ? 1 : 0;
}
