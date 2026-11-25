// plugin_check — load g6_luau.dll the way a runtime does (g6_plugin_query / load / unload)
// against the mock runtime, run the example mods for ~2 s, then unload.
//   plugin_check <g6_luau.dll> <lib_dir> <mods_dir>
// Passes when the plugin loads, its scheduler tick drives the Luau scripts (where_am_i
// logs twice), nothing is logged at ERROR, and the tick is unregistered on unload.
#include "gta6mod/gta6mod.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const g6_host_api* g_mock = nullptr;
g6_host_api        g_api{};
int                g_errors = 0;
int                g_where = 0;
int                g_registered = 0;

void G6_CALL n_player_ped(g6_native_call* c, void*) { c->ret[0] = 0x100; }
void G6_CALL n_coords(g6_native_call* c, void*) { g6_set_vec3_packed(c->ret, g6_vec3{ 1.f, 2.f, 3.f }); }
void G6_CALL n_just_pressed(g6_native_call* c, void*) { c->ret[0] = 0; }
void G6_CALL n_false(g6_native_call* c, void*) { c->ret[0] = 0; }

void G6_CALL counting_log(g6_log_level lvl, const char* who, const char* msg) {
    if (lvl >= G6_LOG_ERROR) ++g_errors;
    if (msg && std::strstr(msg, "player at 1.0 2.0 3.0")) ++g_where;
    g_mock->log(lvl, who, msg);
}
g6_status G6_CALL counting_register(const g6_script_desc* d, g6_script_id* out) {
    g6_status s = g_mock->script_register(d, out);
    if (s == G6_OK) ++g_registered;
    return s;
}
g6_status G6_CALL counting_unregister(g6_script_id id) {
    g6_status s = g_mock->script_unregister(id);
    if (s == G6_OK) --g_registered;
    return s;
}

int fail(const char* what) { std::fprintf(stderr, "FAIL: %s\n", what); return 1; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <g6_luau.dll> <lib_dir> <mods_dir>\n", argv[0]); return 2; }
    g_mock = g6_get_host_api();
    g_api = *g_mock;
    g_api.log = counting_log;
    g_api.script_register = counting_register;
    g_api.script_unregister = counting_unregister;
    g6_mock_register_native(0xD80958FC74E988A6ull, n_player_ped, nullptr);
    g6_mock_register_native(0x3FEF770D40960D5Aull, n_coords, nullptr);
    g6_mock_register_native(0x580417101DDB492Full, n_just_pressed, nullptr);
    // script-layer examples (luau/examples/*): observed PLAYER_PED_ID and IS_PED_IN_ANY_VEHICLE (vehicle_spawner's repair loop)
    g6_mock_register_native(0x096275889B8E0EE0ull, n_player_ped, nullptr);
    g6_mock_register_native(0x997ABD671D25CA0Bull, n_false, nullptr);

    _putenv_s("G6_LUAU_LIB", argv[2]);
    _putenv_s("G6_LUAU_MODS", argv[3]);
    _putenv_s("G6_ALLOW_UNVERIFIED", "1");  // the example uses GTA V seed natives (unverified)

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) return fail("LoadLibrary g6_luau.dll");
    auto query = reinterpret_cast<g6_plugin_query_fn>(reinterpret_cast<void*>(GetProcAddress(dll, "g6_plugin_query")));
    auto load = reinterpret_cast<g6_plugin_load_fn>(reinterpret_cast<void*>(GetProcAddress(dll, "g6_plugin_load")));
    auto unload = reinterpret_cast<g6_plugin_unload_fn>(reinterpret_cast<void*>(GetProcAddress(dll, "g6_plugin_unload")));
    if (!query || !load || !unload) return fail("plugin exports");
    const g6_plugin_info* info = query();
    if (!info || info->api_major != G6_API_VERSION_MAJOR) return fail("plugin info");
    std::printf("plugin %s %s (api %u.%u)\n", info->name, info->version, info->api_major, info->api_minor);

    if (load(&g_api) != G6_OK) return fail("g6_plugin_load");
    if (g_registered != 1) return fail("scheduler tick not registered");
    for (int t = 0; t <= 2016; t += 16) g6_mock_step(16);
    unload();
    if (g_registered != 0) return fail("scheduler tick still registered after unload");
    g6_mock_step(16);  // no tick may reach the destroyed host
    FreeLibrary(dll);

    std::printf("where_am_i logs %d, errors %d\n", g_where, g_errors);
    if (g_errors) return fail("errors logged");
    if (g_where != 2) return fail("expected two where_am_i logs (t=0 and t=2000)");
    std::puts("OK");
    return 0;
}
