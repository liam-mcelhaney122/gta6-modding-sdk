// g6_luau.cpp — Luau host implementation (see g6_luau.h).
// Builds against Luau (https://github.com/luau-lang/luau) VM + Compiler, tag pinned by
// G6_LUAU_TAG in sdk/CMakeLists.txt. Build without cmake: sdk/luau/build.cmd.

#include "g6_luau.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
        case 'w': {  /* packed 16-byte Vector3 {x,y,z,w}: two slots */
            const float* v = luaL_checkvector(L, arg++);
            const g6_vec3 p{ v[0], v[1], v[2] };
            c.args[c.argc++] = g6_arg_vec3_lo(p);
            c.args[c.argc++] = g6_arg_vec3_hi(p);
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
            if (lua_isbuffer(L, arg)) {
                size_t len = 0;
                const void* b = luaL_checkbuffer(L, arg++, &len);
                if (len != 16) luaL_error(L, "%s: entity handle buffer must be 16 bytes", ref->name);
                std::memcpy(&lo, b, 8);
                std::memcpy(&hi, static_cast<const char*>(b) + 8, 8);
            } else {
                lo = (uint64_t)luaL_checknumber(L, arg++);
            }
            c.args[c.argc++] = lo;
            c.args[c.argc++] = hi;
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
    case 'e': push_entity(L, c.ret[0], c.ret[1]); nret = 1; break;
    default: luaL_error(L, "%s: unknown return code", ref->name);
    }
    for (int i = 0; i < n_out; ++i) {
        switch (out_codes[i]) {
        case 'I': lua_pushnumber(L, (double)(int32_t)*(int64_t*)out_ptrs[i]); break;
        case 'F': lua_pushnumber(L, g6_ret_float((uint64_t)*(int64_t*)out_ptrs[i])); break;
        case 'B': lua_pushboolean(L, (int)(*(int64_t*)out_ptrs[i] & 0xFF)); break;
        case 'V': { auto* v = (g6_vec3_packed*)out_ptrs[i]; lua_pushvector(L, v->x, v->y, v->z); break; }
        case 'E': { auto* e = (g6_entity*)out_ptrs[i]; push_entity(L, e->lo, e->hi); break; }
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
    } else if (name == "@gta6/script") {
        static const luaL_Reg fns[] = {{"register", l_script_register}, {"wait", l_script_wait}, {"now", l_script_now}, {"log", l_script_log}, {"cancel", l_script_cancel}, {nullptr, nullptr}};
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

    h->L = luaL_newstate();
    luaL_openlibs(h->L);
    lua_pushlightuserdata(h->L, h);
    lua_setfield(h->L, LUA_REGISTRYINDEX, kHostKey);
    lua_pushcfunction(h->L, l_require, "require");
    lua_setglobal(h->L, "require");
    luaL_sandbox(h->L);  // freeze globals/builtins; each script thread gets its own sandbox env
    return h;
}

extern "C" void g6_luau_destroy(g6_luau_host* h) {
    if (!h) return;
    if (h->L) lua_close(h->L);
    delete h;
}

extern "C" int g6_luau_run_file(g6_luau_host* h, const char* path) {
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
    const uint64_t now = h->api->game_time_ms();
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
