// trainer_mock_test.exe <gta6mod_mock.dll> <g6trainer.dll>
// Runs the C++ trainer plugin against the mock runtime: fake natives for one player ped, script ticks from
// g6_mock_step and menu frames from g6_mock_ui_frame, clicks through g6_mock_ui_click.
#include "gta6mod/gta6mod.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (cond) { ++g_pass; }                                                                                \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

// mock world
constexpr std::uint64_t kPed = 0x100;
bool g_invincible = false, g_ragdoll = true;
std::int32_t g_health = 150, g_max_health = 200, g_wanted = 0, g_max_wanted = 5;
int g_natives = 0;

bool is_player(const g6_native_call* c) { return c->args[0] == kPed && c->args[1] == 0; }
void G6_CALL n_ped(g6_native_call* c, void*) { ++g_natives; c->ret[0] = kPed; c->ret[1] = 0; }
void G6_CALL n_coords(g6_native_call* c, void*) { ++g_natives; g6_set_vec3_packed(c->ret, g6_vec3{ -1000.f, -2000.f, 15.f }); }
void G6_CALL n_invincible(g6_native_call* c, void*) { ++g_natives; if (is_player(c)) g_invincible = (c->args[2] & 0xFF) != 0; }
void G6_CALL n_ragdoll(g6_native_call* c, void*) { ++g_natives; if (is_player(c)) g_ragdoll = (c->args[2] & 0xFF) != 0; }
void G6_CALL n_max_health(g6_native_call* c, void*) { ++g_natives; c->ret[0] = static_cast<std::uint32_t>(g_max_health); }
void G6_CALL n_set_health(g6_native_call* c, void*) { ++g_natives; if (is_player(c)) g_health = static_cast<std::int32_t>(c->args[2]); }
void G6_CALL n_wanted(g6_native_call* c, void*) { ++g_natives; c->ret[0] = is_player(c) ? static_cast<std::uint32_t>(g_wanted) : 0u; }
void G6_CALL n_clear_wanted(g6_native_call* c, void*) { ++g_natives; if (is_player(c)) g_wanted = 0; }
void G6_CALL n_max_wanted(g6_native_call* c, void*) { ++g_natives; c->ret[0] = static_cast<std::uint32_t>(g_max_wanted); }
void G6_CALL n_set_max_wanted(g6_native_call* c, void*) { ++g_natives; g_max_wanted = static_cast<std::int32_t>(c->args[0]); }

template <class F> F get(HMODULE m, const char* n) { return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, n))); }

using StepFn = g6_status(G6_CALL*)(std::uint32_t);
using FrameFn = std::int32_t(G6_CALL*)(void);
using TrFn = const char*(G6_CALL*)(void);
using ClickFn = g6_status(G6_CALL*)(const char*);
using CountFn = std::int32_t(G6_CALL*)(void);
StepFn mock_step;
FrameFn ui_frame;
TrFn ui_transcript;
ClickFn ui_click;

void run(int ticks) {
    for (int i = 0; i < ticks; ++i) {
        mock_step(16);
        ui_frame();
    }
}
bool shows(const char* text) { return std::strstr(ui_transcript(), text) != nullptr; }

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf("usage: trainer_mock_test <gta6mod_mock.dll> <g6trainer.dll>\n"); return 2; }
    HMODULE mock = LoadLibraryA(argv[1]);
    HMODULE plug = LoadLibraryA(argv[2]);
    if (!mock || !plug) { std::printf("cannot load %s / %s\n", argv[1], argv[2]); return 2; }
    auto host = get<const g6_host_api*(G6_CALL*)(void)>(mock, "g6_get_host_api");
    auto reg = get<g6_status(G6_CALL*)(std::uint64_t, g6_mock_native_fn, void*)>(mock, "g6_mock_register_native");
    mock_step = get<StepFn>(mock, "g6_mock_step");
    ui_frame = get<FrameFn>(mock, "g6_mock_ui_frame");
    ui_transcript = get<TrFn>(mock, "g6_mock_ui_transcript");
    ui_click = get<ClickFn>(mock, "g6_mock_ui_click");
    auto ui_errors = get<CountFn>(mock, "g6_mock_ui_errors");
    auto ui_registered = get<CountFn>(mock, "g6_mock_ui_registered");
    auto query = get<g6_plugin_query_fn>(plug, "g6_plugin_query");
    auto load = get<g6_plugin_load_fn>(plug, "g6_plugin_load");
    auto stop = get<g6_plugin_stop_fn>(plug, "g6_plugin_stop");
    auto unload = get<g6_plugin_unload_fn>(plug, "g6_plugin_unload");
    if (!host || !reg || !mock_step || !ui_frame || !ui_transcript || !ui_click || !ui_errors || !ui_registered || !query || !load || !stop || !unload) {
        std::printf("missing exports\n");
        return 2;
    }
    const struct { std::uint64_t h; g6_mock_native_fn fn; } natives[] = {
        { 0x096275889B8E0EE0ull, n_ped },        { 0xA86D5F069399F44Dull, n_coords },      { 0xA5C38736C426FCB8ull, n_invincible },
        { 0xB128377056A54E2Aull, n_ragdoll },    { 0x15D757606D170C3Cull, n_max_health },  { 0xAC2767ED8BDFAB15ull, n_set_health },
        { 0xABC532F9098BFD9Dull, n_wanted },     { 0x4E4B996C928C7AA6ull, n_clear_wanted }, { 0xD04CFAD1E2B7984Aull, n_max_wanted },
        { 0x28A4BD2CEE236E19ull, n_set_max_wanted },
    };
    for (const auto& n : natives) reg(n.h, n.fn, nullptr);

    const g6_host_api* api = host();
    CHECK(g6_ui_get(api) != nullptr, "mock exposes the UI table through g6_ui_get");
    const g6_plugin_info* info = query();
    CHECK(info && std::strcmp(info->name, "trainer") == 0 && info->api_major == G6_API_VERSION_MAJOR, "plugin info");
    CHECK(load(api) == G6_OK, "plugin loads");
    CHECK(ui_registered() == 1, "menu registered (%d)", ui_registered());

    run(3);
    CHECK(shows("begin gta6mod trainer") && shows("checkbox God mode = 0") && shows("checkbox Never wanted = 0") && shows("button Heal"),
          "menu drawn:\n%s", ui_transcript());
    CHECK(shows("text Player: -1000.0 -2000.0 15.0"), "coordinates shown:\n%s", ui_transcript());
    CHECK(shows("text Status: ok"), "status ok:\n%s", ui_transcript());

    const int calls_before_frame = g_natives;
    ui_frame();
    CHECK(g_natives == calls_before_frame, "the menu itself calls no natives");

    ui_click("God mode");
    run(2);
    CHECK(g_invincible && !g_ragdoll && g_health == 200, "God mode on: invincible %d ragdoll %d health %d", g_invincible, g_ragdoll, g_health);
    CHECK(shows("checkbox God mode = 1"), "checkbox shows on");

    g_health = 50;
    ui_click("Heal");
    run(2);
    CHECK(g_health == 200, "Heal button (health %d)", g_health);

    ui_click("Never wanted");
    run(2);
    CHECK(g_max_wanted == 0, "never wanted: max wanted 0 (%d)", g_max_wanted);
    g_wanted = 3;
    run(2);
    CHECK(g_wanted == 0, "never wanted clears a wanted level (%d)", g_wanted);
    ui_click("Never wanted");
    run(2);
    CHECK(g_max_wanted == 5, "never wanted off restores the max (%d)", g_max_wanted);

    g_wanted = 2;
    run(2);
    CHECK(shows("text Wanted level: 2"), "wanted level shown:\n%s", ui_transcript());

    stop();
    CHECK(!g_invincible && g_ragdoll, "stop event turns god mode off");
    unload();
    CHECK(ui_registered() == 0, "unload unregisters the menu");
    CHECK(ui_errors() == 0, "no unbalanced UI calls (%d)", ui_errors());
    std::printf("trainer_mock_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
