// g6_luau.cpp — Luau host implementation (see g6_luau.h).
// Builds against Luau (https://github.com/luau-lang/luau) VM + Compiler, tag pinned by
// G6_LUAU_TAG in sdk/CMakeLists.txt. Build without cmake: sdk/luau/build.cmd.

#include "g6_luau.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kHostKey = "g6.host";
constexpr int         kNativeRefTag = 1;  // userdata tag: native.call only accepts what native.resolve made

struct NativeRef {
    uint64_t         hash = 0;
    g6_native_handle handle = nullptr;
    bool             verified = false;
    char             sig[48] = {};
    char             name[64] = {};
};

struct Script {
    std::string name;
    int         thread_ref = 0;
    lua_State*  thread = nullptr;
    uint64_t    wake_at = 0;
    bool        started = false;
    bool        done = false;
    bool        cancelled = false;  // cancelled while running: finished by g6_luau_tick after it yields
};

struct UiFrame {
    g6_luau_host* host = nullptr;   // owner
    std::string name;
    int fn_ref = 0;
    int thread_ref = 0;
    lua_State* thread = nullptr;
    g6_ui_id id = 0;
    int errors = 0;
    bool removed = false;
};


} // namespace

struct g6_luau_host {
    const g6_host_api*  api = nullptr;
    lua_State*          L = nullptr;
    std::string         lib_dir, mods_dir;
    bool                allow_unverified = false;
    std::vector<Script> scripts;
    std::unordered_map<std::string, int> module_cache;  // name -> registry ref
    std::string         current_mod_dir;
    std::string         current_script;  // log source while a script thread runs
    std::vector<int>    stop_refs;       // script.onStop handlers (registry refs)
    bool                stopped = false; // the stop event was raised
    // UI (@gta6/ui): frame callbacks run on the host's render thread, so every VM entry takes `vm`.
    std::recursive_timed_mutex vm;
    const g6_ui_api*    ui = nullptr;    // g6_ui_get(api); NULL on a host older than API 0.3
    std::vector<std::unique_ptr<UiFrame>> ui_frames;
    bool                ui_in_frame = false;
    bool                ui_unavailable_logged = false;
    uint64_t            ui_skipped = 0;  // frames whose Luau menus were skipped (VM busy)
    // ui.objective: game time at which g6_luau_tick calls _UILOG_CLEAR_CACHED_OBJECTIVE (0 = none pending)
    uint64_t            objective_clear_at = 0;
};

static g6_luau_host* host_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    auto* h = static_cast<g6_luau_host*>(lua_tolightuserdata(L, -1));
    lua_pop(L, 1);
    return h;
}

static void host_log(g6_luau_host* h, g6_log_level lvl, const std::string& who, const std::string& msg) {
    if (h && h->api && h->api->log) h->api->log(lvl, who.c_str(), msg.c_str());
}

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Compile + load a chunk; leaves the function on the stack. Returns 0 on success.
static int load_chunk(lua_State* L, const std::string& source, const std::string& chunkname) {
    size_t bytecode_size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecode_size);
    int rc = luau_load(L, ("=" + chunkname).c_str(), bytecode, bytecode_size, 0);
    std::free(bytecode);
    return rc;
}

static uint32_t joaat(const char* s) { return g6_joaat(s); }

// ------------------------------------------------------------------ native lib
static int l_native_resolve(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* hex = luaL_checkstring(L, 1);
    const char* sig = luaL_checkstring(L, 2);
    const char* name = luaL_optstring(L, 3, "?");
    const char* status = luaL_optstring(L, 4, "unverified");

    auto* ref = static_cast<NativeRef*>(lua_newuserdatatagged(L, sizeof(NativeRef), kNativeRefTag));
    new (ref) NativeRef();
    ref->hash = std::strtoull(hex, nullptr, 16);
    ref->verified = std::strcmp(status, "verified") == 0;
    std::snprintf(ref->sig, sizeof ref->sig, "%s", sig);
    std::snprintf(ref->name, sizeof ref->name, "%s", name);
    if (h->api->native_resolve(ref->hash, &ref->handle) != G6_OK) ref->handle = nullptr;  // error at call time
    return 1;
}

// GTA VI entity handles are 128-bit opaque keys; Luau numbers cannot hold them, so they
// travel as 16-byte buffers (lo, hi little-endian). Mods pass them back unchanged.
static void push_entity(lua_State* L, uint64_t lo, uint64_t hi) {
    void* b = lua_newbuffer(L, 16);
    std::memcpy(b, &lo, 8);
    std::memcpy(static_cast<char*>(b) + 8, &hi, 8);
}

// An entity argument: a 16-byte buffer exactly as a native returned it, or a number (lo only, hi = 0).
static void check_entity(lua_State* L, int arg, const char* name, uint64_t& lo, uint64_t& hi) {
    lo = hi = 0;
    if (lua_isbuffer(L, arg)) {
        size_t len = 0;
        const void* b = luaL_checkbuffer(L, arg, &len);
        if (len != 16) luaL_error(L, "%s: entity handle buffer must be 16 bytes", name);
        std::memcpy(&lo, b, 8);
        std::memcpy(&hi, static_cast<const char*>(b) + 8, 8);
    } else {
        lo = (uint64_t)luaL_checknumber(L, arg);
    }
}

// A 16-byte non-entity handle argument ('h', ScenarioPoint): only a 16-byte buffer as a native returned it. A
// number cannot carry the 64-bit key in its low half, so it is refused rather than truncated.
static void check_handle16(lua_State* L, int arg, const char* name, uint64_t& lo, uint64_t& hi) {
    size_t len = 0;
    if (!lua_isbuffer(L, arg)) luaL_error(L, "%s: expected a 16-byte handle buffer", name);
    const void* b = luaL_checkbuffer(L, arg, &len);
    if (len != 16) luaL_error(L, "%s: 16-byte handle buffer must be 16 bytes", name);
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, static_cast<const char*>(b) + 8, 8);
}

static int l_native_call(lua_State* L) {
    g6_luau_host* h = host_of(L);
    auto* ref = static_cast<NativeRef*>(lua_touserdatatagged(L, 1, kNativeRefTag));
    if (!ref) luaL_error(L, "native.call: expected a native handle from native.resolve");
    if (!ref->verified && !h->allow_unverified)
        luaL_error(L, "%s: native not verified for this game build (enable allow_unverified to try)", ref->name);
    if (!ref->handle)
        luaL_error(L, "%s (0x%016llX): not present in this game build", ref->name, (unsigned long long)ref->hash);

    const char* sig = ref->sig;
    const char* gt = std::strchr(sig, '>');
    const bool raw = !gt;                      /* "" = generic u64 wrapper */
    if (raw) gt = sig;

    g6_native_call c{};
    c.size = sizeof c;
    int64_t       int_outs[8] = {};
    g6_vec3_packed vec_outs[4] = {};  // 16-byte aligned: the game stores Vector3* outs with vmovaps
    g6_entity     ent_outs[4] = {};
    char          out_codes[12] = {};
    void*         out_ptrs[12] = {};
    int n_out = 0, n_int = 0, n_vec = 0, n_ent = 0, arg = 2;

    if (raw) {
        for (int i = 2; i <= lua_gettop(L); ++i) {
            if (c.argc >= G6_MAX_NATIVE_ARGS) luaL_error(L, "%s: too many arguments", ref->name);
            c.args[c.argc++] = g6_arg_u64((uint64_t)luaL_checknumber(L, i));
        }
    }
    for (const char* p = sig; p < gt; ++p) {
        if (c.argc + 3 > G6_MAX_NATIVE_ARGS) luaL_error(L, "%s: too many arguments", ref->name);
        switch (*p) {
        case 'i': c.args[c.argc++] = g6_arg_int((int64_t)luaL_checknumber(L, arg++)); break;
        case 'u':
            if (lua_type(L, arg) == LUA_TSTRING) c.args[c.argc++] = joaat(lua_tostring(L, arg++));
            else c.args[c.argc++] = g6_arg_u32((uint32_t)(int64_t)luaL_checknumber(L, arg++));
            break;
        case 'q': c.args[c.argc++] = lua_isnil(L, arg) ? (++arg, 0) : g6_arg_u64((uint64_t)luaL_checknumber(L, arg++)); break;
        case 'f': c.args[c.argc++] = g6_arg_float((float)luaL_checknumber(L, arg++)); break;
        case 'b': c.args[c.argc++] = g6_arg_bool(lua_toboolean(L, arg++)); break;
        case 's': c.args[c.argc++] = lua_isnil(L, arg) ? (++arg, 0) : g6_arg_ptr(luaL_checkstring(L, arg++)); break;
        case 'S': case 'T':  /* script String / text object (G-120): the runtime builds it from the text */
            if (*p == 'S') c.string_slots |= 1u << c.argc; else c.text_slots |= 1u << c.argc;
            c.args[c.argc++] = lua_isnil(L, arg) ? (++arg, 0) : g6_arg_ptr(luaL_checkstring(L, arg++));
            break;
        case 'w': {  /* packed 16-byte Vector3 {x,y,z,w}: two slots */
            const float* v = luaL_checkvector(L, arg++);
            const g6_vec3 packed{ v[0], v[1], v[2] };
            c.args[c.argc++] = g6_arg_vec3_lo(packed);
            c.args[c.argc++] = g6_arg_vec3_hi(packed);
            break;
        }
        case 'v': {  /* legacy three float slots */
            const float* v = luaL_checkvector(L, arg++);
            c.args[c.argc++] = g6_arg_float(v[0]);
            c.args[c.argc++] = g6_arg_float(v[1]);
            c.args[c.argc++] = g6_arg_float(v[2]);
            break;
        }
        case 'e': {  /* 16-byte entity handle: a 16-byte buffer (as returned), or a number (lo only) */
            uint64_t lo = 0, hi = 0;
            check_entity(L, arg++, ref->name, lo, hi);
            c.args[c.argc++] = lo;
            c.args[c.argc++] = hi;
            break;
        }
        case 'h': {  /* 16-byte non-entity handle (ScenarioPoint): a 16-byte buffer only */
            uint64_t lo = 0, hi = 0;
            check_handle16(L, arg++, ref->name, lo, hi);
            c.args[c.argc++] = lo;
            c.args[c.argc++] = hi;
            break;
        }
        case 'X': {  /* in/out Entity*: scratch filled from the argument, read back after the call */
            if (n_ent >= 4 || n_out >= 12) luaL_error(L, "%s: too many out-params", ref->name);
            check_entity(L, arg++, ref->name, ent_outs[n_ent].lo, ent_outs[n_ent].hi);
            out_codes[n_out] = 'X'; out_ptrs[n_out++] = &ent_outs[n_ent];
            c.args[c.argc++] = g6_arg_ptr(&ent_outs[n_ent++]);
            break;
        }
        case 'I': case 'F': case 'B':
            if (n_int >= 8 || n_out >= 12) luaL_error(L, "%s: too many out-params", ref->name);
            out_codes[n_out] = *p; out_ptrs[n_out++] = &int_outs[n_int];
            c.args[c.argc++] = g6_arg_ptr(&int_outs[n_int++]);
            break;
        case 'V':
            if (n_vec >= 4 || n_out >= 12) luaL_error(L, "%s: too many out-params", ref->name);
            out_codes[n_out] = 'V'; out_ptrs[n_out++] = &vec_outs[n_vec];
            c.args[c.argc++] = g6_arg_ptr(&vec_outs[n_vec++]);
            break;
        case 'E':
            if (n_ent >= 4 || n_out >= 12) luaL_error(L, "%s: too many out-params", ref->name);
            out_codes[n_out] = 'E'; out_ptrs[n_out++] = &ent_outs[n_ent];
            c.args[c.argc++] = g6_arg_ptr(&ent_outs[n_ent++]);
            break;
        default: luaL_error(L, "%s: unknown signature code '%c'", ref->name, *p);
        }
    }

    g6_status s = h->api->native_invoke(ref->handle, &c);
    if (s != G6_OK) luaL_error(L, "%s: native call failed (status %d)", ref->name, (int)s);

    int nret = 0;
    if (raw) { lua_pushnumber(L, (double)c.ret[0]); return 1; }
    switch (gt[1]) {
    case 'n': break;
    case 'i': lua_pushnumber(L, (double)(int32_t)(uint32_t)c.ret[0]); nret = 1; break;
    case 'u': lua_pushnumber(L, (double)(uint32_t)c.ret[0]); nret = 1; break;
    case 'q': lua_pushnumber(L, (double)c.ret[0]); nret = 1; break;
    case 'f': lua_pushnumber(L, g6_ret_float(c.ret[0])); nret = 1; break;
    case 'b': lua_pushboolean(L, (int)(c.ret[0] & 0xFF)); nret = 1; break;
    case 's': if (c.ret[0]) lua_pushstring(L, (const char*)(uintptr_t)c.ret[0]); else lua_pushnil(L); nret = 1; break;
    case 'v': lua_pushvector(L, g6_ret_float(c.ret[0]), g6_ret_float(c.ret[1]), g6_ret_float(c.ret[2])); nret = 1; break;
    case 'w': { g6_vec3 v = g6_ret_vec3_packed(c.ret); lua_pushvector(L, v.x, v.y, v.z); nret = 1; break; }
    case 'e': case 'h': push_entity(L, c.ret[0], c.ret[1]); nret = 1; break;  /* both: 16 bytes lo, hi */
    default: luaL_error(L, "%s: unknown return code", ref->name);
    }
    for (int i = 0; i < n_out; ++i) {
        switch (out_codes[i]) {
        case 'I': lua_pushnumber(L, (double)(int32_t)*(int64_t*)out_ptrs[i]); break;
        case 'F': lua_pushnumber(L, g6_ret_float((uint64_t)*(int64_t*)out_ptrs[i])); break;
        case 'B': lua_pushboolean(L, (int)(*(int64_t*)out_ptrs[i] & 0xFF)); break;
        case 'V': { auto* v = (g6_vec3_packed*)out_ptrs[i]; lua_pushvector(L, v->x, v->y, v->z); break; }
        case 'E': case 'X': { auto* e = (g6_entity*)out_ptrs[i]; push_entity(L, e->lo, e->hi); break; }
        }
        ++nret;
    }
    return nret;
}

static int l_native_joaat(lua_State* L) {
    lua_pushnumber(L, (double)joaat(luaL_checkstring(L, 1)));
    return 1;
}

// ------------------------------------------------------------------ script lib
static int l_script_wait(lua_State* L) {
    double ms = luaL_optnumber(L, 1, 0.0);
    lua_pushnumber(L, ms < 0 ? 0 : ms);
    return lua_yield(L, 1);
}

static int l_script_now(lua_State* L) {
    g6_luau_host* h = host_of(L);
    lua_pushnumber(L, (double)h->api->game_time_ms());
    return 1;
}

static int l_script_log(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* lvl = luaL_checkstring(L, 1);
    const char* msg = luaL_checkstring(L, 2);
    g6_log_level level = G6_LOG_INFO;
    if (!std::strcmp(lvl, "debug")) level = G6_LOG_DEBUG;
    else if (!std::strcmp(lvl, "warn")) level = G6_LOG_WARN;
    else if (!std::strcmp(lvl, "error")) level = G6_LOG_ERROR;
    // Optional third argument: the log source (e.g. a mod name); default is the running script's name.
    const char* source = luaL_optstring(L, 3, nullptr);
    if (source && *source) host_log(h, level, source, msg);
    else host_log(h, level, h->current_script.empty() ? std::string("luau") : h->current_script, msg);
    return 0;
}

static int l_script_cancel(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    for (auto& s : h->scripts) {
        if (s.done || s.name != name) continue;
        if (s.thread == L || h->current_script == s.name) {
            s.cancelled = true;  // its thread is executing: g6_luau_tick retires it when the resume returns
        } else {
            s.done = true;
            lua_unref(h->L, s.thread_ref);
            s.thread = nullptr;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int l_script_on_stop(lua_State* L) {
    g6_luau_host* h = host_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (h->stopped) luaL_error(L, "script.onStop: the stop event was already raised");
    lua_pushvalue(L, 1);
    h->stop_refs.push_back(lua_ref(L, -1));
    lua_pop(L, 1);
    return 0;
}

static int l_script_register(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    for (auto& s : h->scripts)
        if (s.name == name && !s.done) luaL_error(L, "script '%s' already registered", name);

    lua_State* T = lua_newthread(L);
    int ref = lua_ref(L, -1);
    luaL_sandboxthread(T);
    lua_pushvalue(L, 2);
    lua_xmove(L, T, 1);
    lua_pop(L, 1);  // the thread (kept alive by ref)

    Script s;
    s.name = name;
    s.thread = T;
    s.thread_ref = ref;
    s.wake_at = h->api->game_time_ms();
    h->scripts.push_back(std::move(s));
    return 0;
}

// ------------------------------------------------------------------ ui lib (@gta6/ui)
// Frame callbacks (ui.onFrame) run where the host draws: in-game the overlay's render thread, offline inside
// g6_mock_ui_frame. The VM is shared with the script scheduler, so a callback first takes the VM lock; the
// render thread waits at most kUiLockWaitMs for it and skips the mod's menu for that frame otherwise. A
// callback runs with lua_pcall on its own thread: script.wait (a yield) is an error there, and natives
// return G6_E_NOT_ON_SCRIPT_THREAD in-game. ui.defer(fn) hands work to the next tick, where natives work.
constexpr auto kUiLockWaitMs = std::chrono::milliseconds(4);
constexpr int kUiMaxErrors = 10;           // a frame callback that fails this often in a row is removed


static const g6_ui_api* ui_of(lua_State* L, const char* fn) {
    g6_luau_host* h = host_of(L);
    if (!h->ui) luaL_error(L, "ui.%s: the host has no UI layer (runtime older than API 0.3)", fn);
    if (!h->ui_in_frame) luaL_error(L, "ui.%s: only inside a ui.onFrame callback", fn);
    return h->ui;
}

static void G6_CALL ui_frame_thunk(const g6_ui_frame* f, void* user) {
    auto* uf = static_cast<UiFrame*>(user);
    g6_luau_host* h = uf->host;
    std::unique_lock<std::recursive_timed_mutex> lk(h->vm, std::defer_lock);
    if (!lk.try_lock_for(kUiLockWaitMs)) { ++h->ui_skipped; return; }
    if (uf->removed || !uf->thread || h->stopped) return;
    lua_State* T = uf->thread;
    lua_settop(T, 0);
    lua_getref(T, uf->fn_ref);
    lua_createtable(T, 0, 5);
    lua_pushboolean(T, f->menu_open != 0); lua_setfield(T, -2, "menuOpen");
    lua_pushnumber(T, f->display_w); lua_setfield(T, -2, "width");
    lua_pushnumber(T, f->display_h); lua_setfield(T, -2, "height");
    lua_pushnumber(T, f->delta_s); lua_setfield(T, -2, "dt");
    lua_pushnumber(T, static_cast<double>(f->frame)); lua_setfield(T, -2, "frame");
    const std::string saved = h->current_script;
    h->current_script = uf->name;
    h->ui_in_frame = true;
    const int st = lua_pcall(T, 1, 0, 0);
    h->ui_in_frame = false;
    h->current_script = saved;
    if (st == LUA_OK) { uf->errors = 0; return; }
    const char* msg = lua_tostring(T, -1);
    host_log(h, G6_LOG_ERROR, uf->name, std::string("ui frame failed: ") + (msg ? msg : "error"));
    lua_settop(T, 0);
    if (++uf->errors >= kUiMaxErrors) {
        uf->removed = true;   // unregistered by the next tick (not from inside the host's frame)
        host_log(h, G6_LOG_ERROR, uf->name, "ui frame failed " + std::to_string(kUiMaxErrors) + " times in a row; removed");
    }
}

static void ui_release(g6_luau_host* h, UiFrame& uf, bool unregister) {
    if (unregister && uf.id && h->ui) h->ui->frame_unregister(uf.id);   // waits for a frame in progress
    uf.id = 0;
    uf.removed = true;
    if (uf.fn_ref) lua_unref(h->L, uf.fn_ref);
    if (uf.thread_ref) lua_unref(h->L, uf.thread_ref);
    uf.fn_ref = uf.thread_ref = 0;
    uf.thread = nullptr;
}

// Frames removed after errors are unregistered here, on the script thread (g6_luau_tick).
static void ui_sweep(g6_luau_host* h) {
    for (auto it = h->ui_frames.begin(); it != h->ui_frames.end();) {
        if ((*it)->removed) {
            ui_release(h, **it, true);
            it = h->ui_frames.erase(it);
        } else {
            ++it;
        }
    }
}

// ui.onFrame(name, fn [, opts]) -> boolean. opts.always: also while the menu is hidden (HUD).
static int l_ui_on_frame(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    bool always = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "always");
        always = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    if (h->stopped) luaL_error(L, "ui.onFrame: the stop event was already raised");
    for (const auto& f : h->ui_frames)
        if (!f->removed && f->name == name) luaL_error(L, "ui.onFrame: a frame named '%s' is already registered", name);
    if (!h->ui) { lua_pushboolean(L, 0); return 1; }
    auto uf = std::make_unique<UiFrame>();
    uf->host = h;
    uf->name = name;
    lua_pushvalue(L, 2);
    uf->fn_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_State* T = lua_newthread(L);
    luaL_sandboxthread(T);
    uf->thread_ref = lua_ref(L, -1);
    uf->thread = T;
    lua_pop(L, 1);
    const g6_ui_frame_desc d{ sizeof(g6_ui_frame_desc), always ? G6_UI_FRAME_ALWAYS : 0u, uf->name.c_str(), ui_frame_thunk, uf.get() };
    const g6_status s = h->ui->frame_register(&d, &uf->id);
    if (s != G6_OK) {
        if (!h->ui_unavailable_logged) {
            h->ui_unavailable_logged = true;
            host_log(h, G6_LOG_INFO, name, s == G6_E_NOT_IMPLEMENTED ? "no in-game UI (overlay off: [ui] overlay=0)"
                                                                     : "ui frame not registered (status " + std::to_string(s) + ")");
        }
        ui_release(h, *uf, false);
        lua_pushboolean(L, 0);
        return 1;
    }
    h->ui_frames.push_back(std::move(uf));
    lua_pushboolean(L, 1);
    return 1;
}

// ui.remove(name) -> boolean
static int l_ui_remove(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    for (auto& f : h->ui_frames) {
        if (f->removed || f->name != name) continue;
        if (h->ui_in_frame) f->removed = true;   // inside a frame: unregistered by the next tick
        else ui_release(h, *f, true);
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int l_ui_available(lua_State* L) {
    lua_pushboolean(L, host_of(L)->ui != nullptr);
    return 1;
}

// ui.defer(fn): run fn as a one-shot script on the next tick (natives work there; it may wait).
// ui.objective / ui.clearObjective: the game's objective line through the generated bindings (@gta6/natives, so the
// verification policy and the 'T' marshalling are the usual ones). Natives: call from a script, not a frame.
static int l_require(lua_State* L);

static void push_uilog(lua_State* L, const char* fn) {
    lua_pushcfunction(L, l_require, "require");
    lua_pushstring(L, "@gta6/natives");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "UILOG");
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    lua_remove(L, -2);
}

static int l_ui_objective(lua_State* L) {
    g6_luau_host* h = host_of(L);
    const char* text = luaL_checkstring(L, 1);
    const double ms = luaL_optnumber(L, 2, 7500.0);
    push_uilog(L, "_UILOG_SET_CACHED_OBJECTIVE");
    lua_pushstring(L, text);
    lua_call(L, 1, 0);
    push_uilog(L, "_UILOG_PRINT_CACHED_OBJECTIVE");
    // the 'q' slot is read as a 32-bit int by the handler: pass -1 as 0xFFFFFFFF (a negative double has no u64 value)
    lua_pushnumber(L, ms < 0 ? 4294967295.0 : ms);
    lua_call(L, 1, 0);
    h->objective_clear_at = ms < 0 ? 0 : h->api->game_time_ms() + static_cast<uint64_t>(ms);
    if (ms >= 0 && h->objective_clear_at == 0) h->objective_clear_at = 1;
    return 0;
}

static int l_ui_clear_objective(lua_State* L) {
    g6_luau_host* h = host_of(L);
    h->objective_clear_at = 0;
    push_uilog(L, "_UILOG_CLEAR_CACHED_OBJECTIVE");
    lua_call(L, 0, 0);
    return 0;
}

static int l_ui_defer(lua_State* L) {
    g6_luau_host* h = host_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* T = lua_newthread(L);
    const int ref = lua_ref(L, -1);
    luaL_sandboxthread(T);
    lua_pushvalue(L, 1);
    lua_xmove(L, T, 1);
    lua_pop(L, 1);
    Script s;
    s.name = h->current_script.empty() ? "ui.defer" : h->current_script + ".defer";
    s.thread = T;
    s.thread_ref = ref;
    s.wake_at = 0;
    h->scripts.push_back(std::move(s));
    return 0;
}

static std::uint32_t window_flags(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return 0;
    static const struct { const char* key; std::uint32_t flag; } map[] = {
        { "noTitleBar", G6_UI_WINDOW_NO_TITLE_BAR }, { "noResize", G6_UI_WINDOW_NO_RESIZE }, { "noMove", G6_UI_WINDOW_NO_MOVE },
        { "noCollapse", G6_UI_WINDOW_NO_COLLAPSE }, { "autoResize", G6_UI_WINDOW_AUTO_RESIZE },
        { "noBackground", G6_UI_WINDOW_NO_BACKGROUND }, { "noInputs", G6_UI_WINDOW_NO_INPUTS },
    };
    std::uint32_t f = 0;
    for (const auto& m : map) {
        lua_getfield(L, idx, m.key);
        if (lua_toboolean(L, -1)) f |= m.flag;
        lua_pop(L, 1);
    }
    return f;
}

static bool opt_number(lua_State* L, int idx, const char* key, double& out) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, key);
    const bool ok = lua_isnumber(L, -1) != 0;
    if (ok) out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return ok;
}

// Applies opts.x/y (position) and opts.width/height (size), first use only.
static void window_placement(lua_State* L, const g6_ui_api* ui, int idx) {
    double x = 0, y = 0, w = 0, ht = 0;
    if (opt_number(L, idx, "x", x) && opt_number(L, idx, "y", y)) ui->set_next_window_pos(static_cast<float>(x), static_cast<float>(y), G6_UI_COND_FIRST_USE);
    if (opt_number(L, idx, "width", w) && opt_number(L, idx, "height", ht))
        ui->set_next_window_size(static_cast<float>(w), static_cast<float>(ht), G6_UI_COND_FIRST_USE);
}

static bool opt_flag(lua_State* L, int idx, const char* key) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, key);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

// ui.beginWindow(title [, opts]) -> (drawn, open). Call ui.endWindow() whatever it returned.
static int l_ui_begin_window(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "beginWindow");
    const char* title = luaL_checkstring(L, 1);
    window_placement(L, ui, 2);
    std::int32_t open = 1;
    const bool closable = opt_flag(L, 2, "closable");
    const std::int32_t drawn = ui->begin(title, closable ? &open : nullptr, window_flags(L, 2));
    lua_pushboolean(L, drawn != 0);
    lua_pushboolean(L, open != 0);
    return 2;
}
static int l_ui_end_window(lua_State* L) { ui_of(L, "endWindow")->end(); return 0; }

// Calls the function at `fn` (no arguments) and then `end`, also when it errors (the error is rethrown).
static void scoped_call(lua_State* L, int fn, void (G6_CALL* end)(void)) {
    lua_pushvalue(L, fn);
    const int st = lua_pcall(L, 0, 0, 0);
    if (end) end();
    if (st != LUA_OK) lua_error(L);
}

// ui.window(title, fn [, opts]) -> (drawn, open): fn runs when the window's contents are visible.
static int l_ui_window(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "window");
    const char* title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    window_placement(L, ui, 3);
    std::int32_t open = 1;
    const bool closable = opt_flag(L, 3, "closable");
    const std::int32_t drawn = ui->begin(title, closable ? &open : nullptr, window_flags(L, 3));
    if (drawn) scoped_call(L, 2, ui->end);
    else ui->end();
    lua_pushboolean(L, drawn != 0);
    lua_pushboolean(L, open != 0);
    return 2;
}

static int l_ui_set_next_window_pos(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "setNextWindowPos");
    ui->set_next_window_pos(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                            lua_toboolean(L, 3) ? G6_UI_COND_ALWAYS : G6_UI_COND_FIRST_USE);
    return 0;
}
static int l_ui_set_next_window_size(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "setNextWindowSize");
    ui->set_next_window_size(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                             lua_toboolean(L, 3) ? G6_UI_COND_ALWAYS : G6_UI_COND_FIRST_USE);
    return 0;
}

// ui.text(...): the arguments through tostring, separated by spaces.
static int l_ui_text(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "text");
    std::string s;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* p = luaL_tolstring(L, i, &len);
        if (i > 1) s += ' ';
        s.append(p, len);
        lua_pop(L, 1);
    }
    ui->text(s.c_str());
    return 0;
}

// Reads {r, g, b [, a]} (0..1) at idx into rgba (a defaults to 1).
static void check_color(lua_State* L, int idx, const char* fn, float rgba[4]) {
    if (!lua_istable(L, idx)) luaL_error(L, "ui.%s: color must be a table {r, g, b [, a]}", fn);
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_isnumber(L, -1)) rgba[i] = static_cast<float>(lua_tonumber(L, -1));
        else if (i < 3) luaL_error(L, "ui.%s: color component %d is not a number", fn, i + 1);
        else rgba[i] = 1.f;
        lua_pop(L, 1);
    }
}
static void push_color(lua_State* L, const float rgba[4]) {
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        lua_pushnumber(L, rgba[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

static int l_ui_text_colored(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "textColored");
    float c[4];
    check_color(L, 1, "textColored", c);
    ui->text_colored(c[0], c[1], c[2], c[3], luaL_checkstring(L, 2));
    return 0;
}

static int l_ui_button(lua_State* L) {
    lua_pushboolean(L, ui_of(L, "button")->button(luaL_checkstring(L, 1)) != 0);
    return 1;
}

// ui.checkbox(label, value) -> (value, changed)
static int l_ui_checkbox(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "checkbox");
    std::int32_t v = lua_toboolean(L, 2) ? 1 : 0;
    const std::int32_t changed = ui->checkbox(luaL_checkstring(L, 1), &v);
    lua_pushboolean(L, v != 0);
    lua_pushboolean(L, changed != 0);
    return 2;
}

static std::int32_t check_i32(lua_State* L, int idx) {
    const double d = luaL_checknumber(L, idx);
    if (!(d >= -2147483648.0 && d <= 2147483647.0)) luaL_error(L, "argument #%d is outside the 32-bit integer range", idx);
    return static_cast<std::int32_t>(d);
}

// ui.sliderInt(label, value, min, max) -> (value, changed)
static int l_ui_slider_int(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "sliderInt");
    std::int32_t v = check_i32(L, 2);
    const std::int32_t changed = ui->slider_int(luaL_checkstring(L, 1), &v, check_i32(L, 3), check_i32(L, 4));
    lua_pushnumber(L, v);
    lua_pushboolean(L, changed != 0);
    return 2;
}

// ui.sliderFloat(label, value, min, max [, format]) -> (value, changed)
static int l_ui_slider_float(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "sliderFloat");
    float v = static_cast<float>(luaL_checknumber(L, 2));
    const std::int32_t changed = ui->slider_float(luaL_checkstring(L, 1), &v, static_cast<float>(luaL_checknumber(L, 3)),
                                                  static_cast<float>(luaL_checknumber(L, 4)), luaL_optstring(L, 5, nullptr));
    lua_pushnumber(L, v);
    lua_pushboolean(L, changed != 0);
    return 2;
}

// ui.inputText(label, text [, maxBytes = 256]) -> (text, changed)
static int l_ui_input_text(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "inputText");
    size_t len = 0;
    const char* text = luaL_checklstring(L, 2, &len);
    const double maxd = luaL_optnumber(L, 3, 256);
    if (!(maxd >= 1 && maxd <= 65536)) luaL_error(L, "ui.inputText: maxBytes must be 1..65536");
    std::string buf(static_cast<size_t>(maxd) + 1, '\0');
    std::memcpy(buf.data(), text, len < buf.size() - 1 ? len : buf.size() - 1);
    const std::int32_t changed = ui->input_text(luaL_checkstring(L, 1), buf.data(), buf.size());
    lua_pushstring(L, buf.c_str());
    lua_pushboolean(L, changed != 0);
    return 2;
}

// ui.combo(label, index (1-based), items) -> (index, changed)
static int l_ui_combo(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "combo");
    const char* label = luaL_checkstring(L, 1);
    std::int32_t cur = check_i32(L, 2) - 1;
    luaL_checktype(L, 3, LUA_TTABLE);
    const int n = lua_objlen(L, 3);
    if (n > 4096) luaL_error(L, "ui.combo: at most 4096 items");
    std::vector<std::string> items(static_cast<size_t>(n));
    std::vector<const char*> ptrs(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, 3, i + 1);
        size_t len = 0;
        const char* s = luaL_tolstring(L, -1, &len);
        items[static_cast<size_t>(i)].assign(s, len);
        lua_pop(L, 2);
        ptrs[static_cast<size_t>(i)] = items[static_cast<size_t>(i)].c_str();
    }
    const std::int32_t changed = ui->combo(label, &cur, ptrs.data(), n);
    lua_pushnumber(L, cur + 1);
    lua_pushboolean(L, changed != 0);
    return 2;
}

static int l_ui_separator(lua_State* L) { ui_of(L, "separator")->separator(); return 0; }
static int l_ui_same_line(lua_State* L) { ui_of(L, "sameLine")->same_line(); return 0; }

static int l_ui_tree_node(lua_State* L) {
    lua_pushboolean(L, ui_of(L, "treeNode")->tree_node(luaL_checkstring(L, 1)) != 0);
    return 1;
}
static int l_ui_tree_pop(lua_State* L) { ui_of(L, "treePop")->tree_pop(); return 0; }
// ui.tree(label, fn) -> open
static int l_ui_tree(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "tree");
    const char* label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const bool open = ui->tree_node(label) != 0;
    if (open) scoped_call(L, 2, ui->tree_pop);
    lua_pushboolean(L, open);
    return 1;
}

static int l_ui_begin_tab_bar(lua_State* L) {
    lua_pushboolean(L, ui_of(L, "beginTabBar")->begin_tab_bar(luaL_checkstring(L, 1)) != 0);
    return 1;
}
static int l_ui_end_tab_bar(lua_State* L) { ui_of(L, "endTabBar")->end_tab_bar(); return 0; }
static int l_ui_begin_tab_item(lua_State* L) {
    lua_pushboolean(L, ui_of(L, "beginTabItem")->begin_tab_item(luaL_checkstring(L, 1)) != 0);
    return 1;
}
static int l_ui_end_tab_item(lua_State* L) { ui_of(L, "endTabItem")->end_tab_item(); return 0; }
// ui.tabBar(id, fn) -> visible
static int l_ui_tab_bar(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "tabBar");
    const char* id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const bool open = ui->begin_tab_bar(id) != 0;
    if (open) scoped_call(L, 2, ui->end_tab_bar);
    lua_pushboolean(L, open);
    return 1;
}
// ui.tab(label, fn) -> selected
static int l_ui_tab(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "tab");
    const char* label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const bool open = ui->begin_tab_item(label) != 0;
    if (open) scoped_call(L, 2, ui->end_tab_item);
    lua_pushboolean(L, open);
    return 1;
}

// ui.colorEdit(label, color [, opts]) -> (color, changed). opts.noAlpha hides the alpha channel.
static int l_ui_color_edit(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "colorEdit");
    float c[4];
    check_color(L, 2, "colorEdit", c);
    const std::int32_t changed = ui->color_edit(luaL_checkstring(L, 1), c, opt_flag(L, 3, "noAlpha") ? G6_UI_COLOR_NO_ALPHA : 0u);
    push_color(L, c);
    lua_pushboolean(L, changed != 0);
    return 2;
}

// ui.plotLines(label, values [, opts]): opts.overlay (text), opts.min/opts.max (scale), opts.width/opts.height.
static int l_ui_plot_lines(lua_State* L) {
    const g6_ui_api* ui = ui_of(L, "plotLines");
    const char* label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = lua_objlen(L, 2);
    if (n > 65536) luaL_error(L, "ui.plotLines: at most 65536 values");
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        v[static_cast<size_t>(i)] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    double lo = 0, hi = 0, w = 0, ht = 0;
    opt_number(L, 3, "min", lo);
    opt_number(L, 3, "max", hi);
    opt_number(L, 3, "width", w);
    opt_number(L, 3, "height", ht);
    std::string overlay;
    bool has_overlay = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "overlay");
        if (lua_isstring(L, -1)) { overlay = lua_tostring(L, -1); has_overlay = true; }
        lua_pop(L, 1);
    }
    ui->plot_lines(label, v.data(), n, has_overlay ? overlay.c_str() : nullptr, static_cast<float>(lo), static_cast<float>(hi),
                   static_cast<float>(w), static_cast<float>(ht));
    return 0;
}

static const luaL_Reg kUiLib[] = {
    { "onFrame", l_ui_on_frame }, { "remove", l_ui_remove }, { "available", l_ui_available }, { "defer", l_ui_defer },
    { "window", l_ui_window }, { "beginWindow", l_ui_begin_window }, { "endWindow", l_ui_end_window },
    { "setNextWindowPos", l_ui_set_next_window_pos }, { "setNextWindowSize", l_ui_set_next_window_size },
    { "text", l_ui_text }, { "textColored", l_ui_text_colored }, { "button", l_ui_button }, { "checkbox", l_ui_checkbox },
    { "sliderInt", l_ui_slider_int }, { "sliderFloat", l_ui_slider_float }, { "inputText", l_ui_input_text },
    { "combo", l_ui_combo }, { "separator", l_ui_separator }, { "sameLine", l_ui_same_line },
    { "tree", l_ui_tree }, { "treeNode", l_ui_tree_node }, { "treePop", l_ui_tree_pop },
    { "tabBar", l_ui_tab_bar }, { "tab", l_ui_tab }, { "beginTabBar", l_ui_begin_tab_bar }, { "endTabBar", l_ui_end_tab_bar },
    { "beginTabItem", l_ui_begin_tab_item }, { "endTabItem", l_ui_end_tab_item },
    { "colorEdit", l_ui_color_edit }, { "plotLines", l_ui_plot_lines },
    { "objective", l_ui_objective }, { "clearObjective", l_ui_clear_objective },
    { nullptr, nullptr },
};

// ------------------------------------------------------------------ require
static void push_lib(lua_State* L, const luaL_Reg* fns) {
    lua_newtable(L);
    for (; fns->name; ++fns) {
        lua_pushcfunction(L, fns->func, fns->name);
        lua_setfield(L, -2, fns->name);
    }
    lua_setreadonly(L, -1, true);
}

// "@gta6/<name>" with name in [a-z_]+ is a library module: <lib_dir>/<name>.luau (e.g. names, game).
static bool is_lib_module(const std::string& name) {
    if (name.rfind("@gta6/", 0) != 0 || name.size() == 6) return false;
    for (size_t i = 6; i < name.size(); ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') || name[i] == '_')) return false;
    return true;
}

static int l_require(lua_State* L) {
    g6_luau_host* h = host_of(L);
    std::string name = luaL_checkstring(L, 1);

    auto cached = h->module_cache.find(name);
    if (cached != h->module_cache.end()) { lua_getref(L, cached->second); return 1; }

    if (name == "@gta6/native") {
        static const luaL_Reg fns[] = {{"resolve", l_native_resolve}, {"call", l_native_call}, {"joaat", l_native_joaat}, {nullptr, nullptr}};
        push_lib(L, fns);
    } else if (name == "@gta6/ui") {
        push_lib(L, kUiLib);
    } else if (name == "@gta6/script") {
        static const luaL_Reg fns[] = {{"register", l_script_register}, {"wait", l_script_wait}, {"now", l_script_now}, {"log", l_script_log}, {"cancel", l_script_cancel}, {"onStop", l_script_on_stop}, {nullptr, nullptr}};
        push_lib(L, fns);
    } else {
        std::string path;
        if (name == "@gta6/natives") path = (fs::path(h->lib_dir) / "natives.luau").string();
        else if (is_lib_module(name)) path = (fs::path(h->lib_dir) / (name.substr(6) + ".luau")).string();
        else if (name.rfind("./", 0) == 0) path = (fs::path(h->current_mod_dir) / (name.substr(2) + ".luau")).string();
        else luaL_error(L, "require: unknown module '%s'", name.c_str());

        std::string src;
        if (!read_file(path, src)) luaL_error(L, "require: cannot read %s", path.c_str());
        if (load_chunk(L, src, name) != 0) lua_error(L);
        lua_call(L, 0, 1);
    }
    lua_pushvalue(L, -1);
    h->module_cache[name] = lua_ref(L, -1);
    lua_pop(L, 1);
    return 1;
}

// ------------------------------------------------------------------ public API
extern "C" g6_luau_host* g6_luau_create(const g6_host_api* api, const g6_luau_config* cfg) {
    if (!api || !cfg || cfg->size < sizeof(g6_luau_config)) return nullptr;
    auto* h = new g6_luau_host();
    h->api = api;
    h->lib_dir = cfg->lib_dir ? cfg->lib_dir : ".";
    h->mods_dir = cfg->mods_dir ? cfg->mods_dir : "mods";
    h->allow_unverified = cfg->allow_unverified != 0;
    h->ui = g6_ui_get(api);

    h->L = luaL_newstate();
    luaL_openlibs(h->L);
    lua_pushlightuserdata(h->L, h);
    lua_setfield(h->L, LUA_REGISTRYINDEX, kHostKey);
    lua_pushcfunction(h->L, l_require, "require");
    lua_setglobal(h->L, "require");
    luaL_sandbox(h->L);  // freeze globals/builtins; each script thread gets its own sandbox env
    return h;
}

extern "C" int g6_luau_stop(g6_luau_host* h) {
    if (!h || !h->L || h->stopped) return 0;
    std::lock_guard<std::recursive_timed_mutex> lk(h->vm);
    h->stopped = true;
    int failed = 0;
    for (int ref : h->stop_refs) {
        lua_State* T = lua_newthread(h->L);
        luaL_sandboxthread(T);
        lua_getref(h->L, ref);
        lua_xmove(h->L, T, 1);
        h->current_script = "stop";
        const int st = lua_resume(T, nullptr, 0);
        h->current_script.clear();
        if (st == LUA_YIELD) {
            ++failed;
            host_log(h, G6_LOG_ERROR, "stop", "stop handler waited; it is not resumed (stop handlers must not wait)");
        } else if (st != LUA_OK) {
            ++failed;
            host_log(h, G6_LOG_ERROR, "stop", std::string("stop handler failed: ") + (lua_tostring(T, -1) ? lua_tostring(T, -1) : "error"));
        }
        lua_pop(h->L, 1);  // the thread
        lua_unref(h->L, ref);
    }
    h->stop_refs.clear();
    for (auto& s : h->scripts) {  // whatever the handlers left running is cancelled
        if (s.done) continue;
        s.done = true;
        lua_unref(h->L, s.thread_ref);
        s.thread = nullptr;
    }
    return failed;
}

extern "C" void g6_luau_destroy(g6_luau_host* h) {
    if (!h) return;
    // Unregister the UI frames without holding the VM lock: frame_unregister waits for a frame in progress,
    // which may itself be waiting (briefly) for the lock.
    for (auto& f : h->ui_frames)
        if (f->id && h->ui) { h->ui->frame_unregister(f->id); f->id = 0; }
    {
        std::lock_guard<std::recursive_timed_mutex> lk(h->vm);
        for (auto& f : h->ui_frames) ui_release(h, *f, false);
        h->ui_frames.clear();
        g6_luau_stop(h);
        if (h->L) lua_close(h->L);
        h->L = nullptr;
    }
    delete h;
}

extern "C" int g6_luau_run_file(g6_luau_host* h, const char* path) {
    std::lock_guard<std::recursive_timed_mutex> lk(h->vm);
    std::string src;
    if (!read_file(path, src)) { host_log(h, G6_LOG_ERROR, "luau", std::string("cannot read ") + path); return 1; }
    h->current_mod_dir = fs::path(path).parent_path().string();
    lua_State* T = lua_newthread(h->L);
    luaL_sandboxthread(T);
    int rc = load_chunk(T, src, fs::path(path).filename().string());
    if (rc == 0) rc = lua_pcall(T, 0, 0, 0);
    if (rc != 0) host_log(h, G6_LOG_ERROR, path, lua_tostring(T, -1) ? lua_tostring(T, -1) : "error");
    lua_pop(h->L, 1);  // the thread
    return rc;
}

extern "C" int g6_luau_load_mods(g6_luau_host* h) {
    int loaded = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(h->mods_dir, ec)) {
        fs::path init = entry.path() / "init.luau";
        if (entry.is_directory() && fs::exists(init) && g6_luau_run_file(h, init.string().c_str()) == 0) ++loaded;
    }
    return loaded;
}

extern "C" void g6_luau_tick(g6_luau_host* h) {
    std::lock_guard<std::recursive_timed_mutex> lk(h->vm);
    ui_sweep(h);
    const uint64_t now = h->api->game_time_ms();
    if (h->objective_clear_at && now >= h->objective_clear_at && !h->stopped) {  // ui.objective's timed clear
        h->objective_clear_at = 0;
        lua_State* T = lua_newthread(h->L);
        luaL_sandboxthread(T);
        lua_pushcfunction(T, l_ui_clear_objective, "ui.clearObjective");
        if (lua_pcall(T, 0, 0, 0) != LUA_OK)
            host_log(h, G6_LOG_ERROR, "ui.objective", lua_tostring(T, -1) ? lua_tostring(T, -1) : "clear failed");
        lua_pop(h->L, 1);
    }
    for (size_t i = 0; i < h->scripts.size(); ++i) {  // index loop: scripts may register scripts
        if (h->scripts[i].done || now < h->scripts[i].wake_at) continue;
        lua_State* T = h->scripts[i].thread;
        h->current_script = h->scripts[i].name;
        int st = lua_resume(T, nullptr, 0);
        h->current_script.clear();
        Script& cur = h->scripts[i];  // re-fetch: a resumed script may register scripts (vector growth)
        cur.started = true;
        if (st == LUA_YIELD && !cur.cancelled) {
            double ms = lua_gettop(T) > 0 ? lua_tonumber(T, -1) : 0.0;
            lua_settop(T, 0);
            cur.wake_at = now + (uint64_t)ms;
        } else {
            if (st != LUA_OK && st != LUA_YIELD) {  // a yield here means it was cancelled while running
                std::string err = lua_tostring(T, -1) ? lua_tostring(T, -1) : "error";
                host_log(h, G6_LOG_ERROR, cur.name, err + "\n" + lua_debugtrace(T));
            }
            cur.done = true;
            lua_unref(h->L, cur.thread_ref);
            cur.thread = nullptr;
        }
    }
}

extern "C" int g6_luau_script_count(const g6_luau_host* h) {
    int n = 0;
    for (const auto& s : h->scripts) n += s.done ? 0 : 1;
    return n;
}
