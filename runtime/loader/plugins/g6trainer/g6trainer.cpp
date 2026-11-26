// g6trainer.dll: example C++ plugin with an in-game menu (gta6mod/ui.hpp over the g6_ui_api C ABI).
// Menu (Insert): "God mode" and "Never wanted" checkboxes, a "Heal" button, the player's coordinates and
// wanted level. Twins: sdk/luau/examples/trainer_menu, sdk/examples/python/trainer_menu.py.
//
// Threads: the menu runs on the render thread, where natives are refused, so it only writes requests
// (atomics). The script tick (game script thread) applies them with natives and publishes what the menu
// shows. Natives are addressed through g6_host_api by hash; the hashes and argument layouts are the ones
// the script layer uses (sdk/include/gta6mod/natives.hpp, sdk/luau/lib/game.luau), all [observed] in 1.0.2.1.
#include "gta6mod/gta6mod.h"
#include "gta6mod/ui.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

// natives (names.luau / natives.hpp, build 1.0.2.1)
constexpr std::uint64_t kPlayerPedId = 0x096275889B8E0EE0ull;         // Ped PLAYER_PED_ID_0EE0()
constexpr std::uint64_t kGetEntityCoords = 0xA86D5F069399F44Dull;     // Vector3 GET_ENTITY_COORDS_F44D(Entity, BOOL alive, BOOL realCoords)
constexpr std::uint64_t kSetEntityInvincible = 0xA5C38736C426FCB8ull; // void SET_ENTITY_INVINCIBLE(Entity, BOOL)
constexpr std::uint64_t kSetPedCanRagdoll = 0xB128377056A54E2Aull;    // void SET_PED_CAN_RAGDOLL(Ped, BOOL)
constexpr std::uint64_t kGetEntityMaxHealth = 0x15D757606D170C3Cull;  // int GET_ENTITY_MAX_HEALTH(Entity, BOOL)
constexpr std::uint64_t kSetEntityHealth = 0xAC2767ED8BDFAB15ull;     // void SET_ENTITY_HEALTH(Entity, int, Entity killer, a5, a6)
constexpr std::uint64_t kGetWantedLevel = 0xABC532F9098BFD9Dull;      // int GET_PLAYER_WANTED_LEVEL(Ped)
constexpr std::uint64_t kClearWantedLevel = 0x4E4B996C928C7AA6ull;    // void CLEAR_PLAYER_WANTED_LEVEL(Ped)
constexpr std::uint64_t kGetMaxWantedLevel = 0xD04CFAD1E2B7984Aull;   // int GET_MAX_WANTED_LEVEL()
constexpr std::uint64_t kSetMaxWantedLevel = 0x28A4BD2CEE236E19ull;   // void SET_MAX_WANTED_LEVEL(int)

const g6_host_api* g_api = nullptr;

// Plugins never link the runtime (gta6mod.h rules), so no g6_status_str here.
const char* status_text(g6_status s) {
    switch (s) {
    case G6_OK: return "ok";
    case G6_E_NOT_ON_SCRIPT_THREAD: return "not on the script thread";
    case G6_E_ONLINE_SESSION: return "single-player guard closed";
    case G6_E_UNKNOWN_NATIVE: return "native not in this build";
    case G6_E_UNVERIFIED_NATIVE: return "native handler not verified";
    case G6_E_UNSUPPORTED_BUILD: return "unsupported game build";
    case G6_E_NOT_IMPLEMENTED: return "not implemented";
    default: return "error";
    }
}
g6::ui::Ui g_ui;
g6::ui::Frame g_menu;
g6_script_id g_script = 0;

// menu -> script tick
std::atomic<bool> g_want_god{ false }, g_want_never_wanted{ false }, g_heal{ false };
// script tick -> menu
struct View {
    bool valid = false;
    float x = 0, y = 0, z = 0;
    int wanted = 0;
    char status[128] = "waiting for the first script tick";
};
std::mutex g_view_lock;
View g_view;

// script-thread state
bool g_god_on = false, g_nw_on = false;
int g_saved_max_wanted = 0;
g6_entity g_god_ped{};

void log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_api) g_api->log(G6_LOG_INFO, "trainer", buf);
}

struct Call {
    g6_native_call c{};
    Call() { c.size = sizeof c; }
    Call& u(std::uint64_t v) { if (c.argc < G6_MAX_NATIVE_ARGS) c.args[c.argc++] = v; return *this; }
    Call& e(g6_entity v) { return u(v.lo).u(v.hi); }
    g6_status run(std::uint64_t hash) { return g_api->native_invoke_hash(hash, &c); }
};

g6_status g_last = G6_OK;
bool ok(g6_status s) {
    if (s != G6_OK) g_last = s;
    return s == G6_OK;
}

void set_god(g6_entity ped, bool on) {
    ok(Call().e(ped).u(g6_arg_bool(on)).run(kSetEntityInvincible));
    ok(Call().e(ped).u(g6_arg_bool(!on)).run(kSetPedCanRagdoll));
}

void heal(g6_entity ped) {
    Call mh;
    if (!ok(mh.e(ped).u(0).run(kGetEntityMaxHealth))) return;
    const std::int32_t max = static_cast<std::int32_t>(mh.c.ret[0]);
    ok(Call().e(ped).u(g6_arg_int(max)).e(g6_entity{}).u(0).u(0).run(kSetEntityHealth));
    log("healed to %d", max);
}

void G6_CALL tick(void*) {
    g_last = G6_OK;
    Call pc;
    if (!ok(pc.run(kPlayerPedId))) {
        std::lock_guard<std::mutex> lk(g_view_lock);
        g_view.valid = false;
        std::snprintf(g_view.status, sizeof g_view.status, "natives refused: %s", status_text(g_last));
        return;
    }
    const g6_entity ped{ pc.c.ret[0], pc.c.ret[1] };

    const bool god = g_want_god.load();
    if (god != g_god_on || (god && (ped.lo != g_god_ped.lo || ped.hi != g_god_ped.hi))) {   // toggled, or a new player ped
        set_god(ped, god);
        if (god && !g_god_on) heal(ped);
        if (god != g_god_on) log("god mode %s", god ? "ON" : "OFF");
        g_god_on = god;
        g_god_ped = ped;
    }
    if (g_heal.exchange(false)) heal(ped);

    const bool nw = g_want_never_wanted.load();
    if (nw != g_nw_on) {
        if (nw) {
            Call mw;
            if (ok(mw.run(kGetMaxWantedLevel))) g_saved_max_wanted = static_cast<std::int32_t>(mw.c.ret[0]);
            ok(Call().u(0).run(kSetMaxWantedLevel));
            ok(Call().e(ped).run(kClearWantedLevel));
            log("never wanted ON (max wanted level was %d)", g_saved_max_wanted);
        } else {
            ok(Call().u(g6_arg_int(g_saved_max_wanted)).run(kSetMaxWantedLevel));
            log("never wanted OFF (max wanted level restored to %d)", g_saved_max_wanted);
        }
        g_nw_on = nw;
    }
    Call wl;
    int wanted = ok(wl.e(ped).run(kGetWantedLevel)) ? static_cast<std::int32_t>(wl.c.ret[0]) : 0;
    if (g_nw_on && wanted > 0) {
        ok(Call().e(ped).run(kClearWantedLevel));
        log("cleared wanted level %d", wanted);
        wanted = 0;
    }

    Call cc;
    const bool have_pos = ok(cc.e(ped).u(1).u(0).run(kGetEntityCoords));
    const g6_vec3 p = g6_ret_vec3_packed(cc.c.ret);
    std::lock_guard<std::mutex> lk(g_view_lock);
    g_view.valid = have_pos;
    g_view.x = p.x;
    g_view.y = p.y;
    g_view.z = p.z;
    g_view.wanted = wanted;
    if (g_last == G6_OK) std::snprintf(g_view.status, sizeof g_view.status, "ok");
    else std::snprintf(g_view.status, sizeof g_view.status, "last native call failed: %s", status_text(g_last));
}

void menu(const g6_ui_frame&) {
    g_ui.next_window_pos(40, 120);
    if (auto w = g_ui.window("gta6mod trainer", nullptr, G6_UI_WINDOW_AUTO_RESIZE)) {
        bool god = g_want_god.load(), nw = g_want_never_wanted.load();
        if (g_ui.checkbox("God mode", god)) g_want_god.store(god);
        if (g_ui.checkbox("Never wanted", nw)) g_want_never_wanted.store(nw);
        if (g_ui.button("Heal")) g_heal.store(true);
        g_ui.separator();
        View v;
        {
            std::lock_guard<std::mutex> lk(g_view_lock);
            v = g_view;
        }
        char line[160];
        if (v.valid) std::snprintf(line, sizeof line, "Player: %.1f %.1f %.1f", static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
        else std::snprintf(line, sizeof line, "Player: unknown");
        g_ui.text(line);
        std::snprintf(line, sizeof line, "Wanted level: %d", v.wanted);
        g_ui.text(line);
        std::snprintf(line, sizeof line, "Status: %s", v.status);
        g_ui.text(line);
    }
}

}  // namespace

extern "C" {

__declspec(dllexport) const g6_plugin_info* G6_CALL g6_plugin_query(void) {
    static const g6_plugin_info info = { sizeof(g6_plugin_info), "trainer", "0.1.0", G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR };
    return &info;
}

__declspec(dllexport) g6_status G6_CALL g6_plugin_load(const g6_host_api* api) {
    if (!api || api->version_major != G6_API_VERSION_MAJOR) return G6_E_UNSUPPORTED_BUILD;
    g_api = api;
    const g6_script_desc d{ sizeof(g6_script_desc), "trainer", tick, nullptr };
    const g6_status s = api->script_register(&d, &g_script);
    if (s != G6_OK) return s;
    g_ui = g6::ui::Ui(api);
    const g6_status us = g_menu.reset(g_ui, "trainer", menu);
    if (us != G6_OK) log("menu not registered (%s): the trainer runs without UI", status_text(us));
    return G6_OK;
}

// Stop event (API 0.2): switch everything off on the script thread while natives still work.
__declspec(dllexport) void G6_CALL g6_plugin_stop(void) {
    g_want_god.store(false);
    g_want_never_wanted.store(false);
    if (!g_api) return;
    Call pc;
    if (pc.run(kPlayerPedId) != G6_OK) return;
    const g6_entity ped{ pc.c.ret[0], pc.c.ret[1] };
    if (g_god_on) { set_god(ped, false); g_god_on = false; log("god mode OFF"); }
    if (g_nw_on) {
        Call().u(g6_arg_int(g_saved_max_wanted)).run(kSetMaxWantedLevel);
        g_nw_on = false;
        log("never wanted OFF (max wanted level restored to %d)", g_saved_max_wanted);
    }
}

__declspec(dllexport) void G6_CALL g6_plugin_unload(void) {
    g_menu.reset();   // waits for a menu frame in progress
    if (g_api && g_script) g_api->script_unregister(g_script);
    g_script = 0;
    g_api = nullptr;
}

}  // extern "C"
