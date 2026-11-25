// g6_luau_plugin.cpp — exposes the Luau host as a gta6mod plugin DLL.
// The runtime loads plugins, calls g6_plugin_load(api); we create a Luau VM,
// load <game>/mods/luau/*/init.luau, and drive the scheduler from one tick.
#include "g6_luau.h"

#include <cstdlib>

#if defined(_WIN32)
#  define G6_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define G6_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

static const g6_host_api* g_api = nullptr;
static g6_luau_host* g_host = nullptr;
static g6_script_id  g_tick_id = 0;

static void G6_CALL luau_tick(void*) { if (g_host) g6_luau_tick(g_host); }

G6_PLUGIN_EXPORT const g6_plugin_info* G6_CALL g6_plugin_query(void) {
    static const g6_plugin_info info = { sizeof(g6_plugin_info), "luau-host", "0.1.0", G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR };
    return &info;
}

G6_PLUGIN_EXPORT g6_status G6_CALL g6_plugin_load(const g6_host_api* api) {
    if (!api || api->version_major != G6_API_VERSION_MAJOR) return G6_E_UNSUPPORTED_BUILD;
    const char* lib = std::getenv("G6_LUAU_LIB");    // default: mods/luau/_lib
    const char* mods = std::getenv("G6_LUAU_MODS");  // default: mods/luau
    const char* unv = std::getenv("G6_ALLOW_UNVERIFIED");
    g6_luau_config cfg = { sizeof(g6_luau_config), lib ? lib : "mods/luau/_lib", mods ? mods : "mods/luau", unv && *unv == '1' };
    g_host = g6_luau_create(api, &cfg);
    if (!g_host) return G6_E_INTERNAL;
    g6_luau_load_mods(g_host);
    g6_script_desc d = { sizeof(g6_script_desc), "luau-scheduler", luau_tick, nullptr };
    g6_status s = api->script_register(&d, &g_tick_id);
    if (s != G6_OK) { g6_luau_destroy(g_host); g_host = nullptr; return s; }
    g_api = api;
    return G6_OK;
}

G6_PLUGIN_EXPORT void G6_CALL g6_plugin_unload(void) {
    if (g_api && g_tick_id) g_api->script_unregister(g_tick_id);  // stop ticks before the VM goes away
    g_api = nullptr;
    g_tick_id = 0;
    g6_luau_destroy(g_host);
    g_host = nullptr;
}
