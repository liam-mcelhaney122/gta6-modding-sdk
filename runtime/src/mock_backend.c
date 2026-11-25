/*
 * gta6mod_mock — offline implementation of the gta6mod.h C ABI.
 *
 * Purpose: let the Python (ctypes) and Luau hosts, and C/C++ plugins, be built
 * and tested against the real ABI before any in-game runtime exists. Natives
 * are fake handlers registered through g6_mock_register_native(); the game
 * clock advances through g6_mock_step().
 *
 * The real runtime will replace exactly this file's responsibilities:
 *   native_resolve/invoke  -> game's own native resolver + script-thread call
 *   script_register/tick   -> hooked script-thread tick, fibers for wait()
 *   anchor_address         -> address manifest (exe SHA-256 -> RVAs) + pattern fallback
 *   vfs_add_overlay        -> fiDevice / file-layer hooks (mods/ overlay)
 */
#define G6_BUILDING_RUNTIME 1
#include "gta6mod/gta6mod.h"

#include <stdio.h>
#include <string.h>

#define MOCK_NATIVE_SLOTS 1024u   /* power of two */
#define MOCK_MAX_SCRIPTS  64u

typedef struct mock_native {
    uint64_t          hash;      /* 0 = empty slot */
    g6_mock_native_fn fn;
    void*             user;
} mock_native;

typedef struct mock_script {
    int               used;
    g6_script_desc    desc;
    char              name[64];
} mock_script;

static mock_native g_natives[MOCK_NATIVE_SLOTS];
static uint32_t    g_native_count;
static mock_script g_scripts[MOCK_MAX_SCRIPTS];
static uint32_t    g_clock_ms;
static int         g_in_tick;

static mock_native* find_slot(uint64_t hash, int for_insert) {
    uint32_t i = (uint32_t)(hash ^ (hash >> 32)) & (MOCK_NATIVE_SLOTS - 1u);
    for (uint32_t n = 0; n < MOCK_NATIVE_SLOTS; ++n, i = (i + 1u) & (MOCK_NATIVE_SLOTS - 1u)) {
        if (g_natives[i].hash == hash) return &g_natives[i];
        if (g_natives[i].hash == 0) return for_insert ? &g_natives[i] : NULL;
    }
    return NULL;
}

/* --------------------------------------------------------------- host API */
static g6_status G6_CALL host_get_build_info(g6_build_info* out) {
    if (!out || out->size < sizeof(g6_build_info)) return G6_E_INVALID_ARG;
    memset(out->exe_sha256, 0, sizeof out->exe_sha256);
    out->game_version     = "1.0.2.1";
    out->platform         = "mock";
    out->natives_known    = g_native_count;
    out->natives_verified = 0;
    return G6_OK;
}

static void G6_CALL host_log(g6_log_level level, const char* plugin, const char* message) {
    static const char* names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    fprintf(stderr, "[%s] %s: %s\n", (unsigned)level < 4u ? names[level] : "?", plugin ? plugin : "?", message ? message : "");
}

static g6_status G6_CALL host_native_resolve(uint64_t hash, g6_native_handle* out) {
    if (!out) return G6_E_INVALID_ARG;
    mock_native* n = find_slot(hash, 0);
    if (!n) { *out = NULL; return G6_E_UNKNOWN_NATIVE; }
    *out = (g6_native_handle)n;
    return G6_OK;
}

static g6_status G6_CALL host_native_invoke(g6_native_handle h, g6_native_call* call) {
    if (!h || !call || call->size < sizeof(g6_native_call)) return G6_E_INVALID_ARG;
    if (call->argc > G6_MAX_NATIVE_ARGS) return G6_E_TOO_MANY_ARGS;
    mock_native* n = (mock_native*)h;
    memset(call->ret, 0, sizeof call->ret);
    n->fn(call, n->user);
    return G6_OK;
}

static g6_status G6_CALL host_native_invoke_hash(uint64_t hash, g6_native_call* call) {
    g6_native_handle h;
    g6_status s = host_native_resolve(hash, &h);
    return s == G6_OK ? host_native_invoke(h, call) : s;
}

static g6_status G6_CALL host_script_register(const g6_script_desc* desc, g6_script_id* out) {
    if (!desc || desc->size < sizeof(g6_script_desc) || !desc->tick || !desc->name) return G6_E_INVALID_ARG;
    for (uint32_t i = 0; i < MOCK_MAX_SCRIPTS; ++i) {
        if (!g_scripts[i].used) {
            g_scripts[i].used = 1;
            g_scripts[i].desc = *desc;
            size_t k = 0;
            for (; k + 1 < sizeof g_scripts[i].name && desc->name[k]; ++k) g_scripts[i].name[k] = desc->name[k];
            g_scripts[i].name[k] = 0;
            g_scripts[i].desc.name = g_scripts[i].name;
            if (out) *out = i + 1u;
            return G6_OK;
        }
    }
    return G6_E_INTERNAL;
}

static g6_status G6_CALL host_script_unregister(g6_script_id id) {
    if (id == 0 || id > MOCK_MAX_SCRIPTS || !g_scripts[id - 1].used) return G6_E_INVALID_ARG;
    g_scripts[id - 1].used = 0;
    return G6_OK;
}

static g6_status G6_CALL host_script_wait(uint32_t ms) {
    (void)ms;
    /* The real runtime runs each script on its own fiber and switches back to
       the game here. The mock has no fibers: language hosts (Python/Luau)
       implement wait() with their own coroutines on top of tick(). */
    return G6_E_NOT_IMPLEMENTED;
}

static g6_status G6_CALL host_anchor_address(const char* name, uintptr_t* out) {
    (void)name;
    if (out) *out = 0;
    return G6_E_NO_SUCH_ANCHOR; /* no game image in the mock */
}

static g6_status G6_CALL host_vfs_add_overlay(const char* mods_root, int32_t priority) {
    (void)mods_root; (void)priority;
    return G6_E_NOT_IMPLEMENTED;
}

static uint64_t G6_CALL host_game_time_ms(void) { return g_clock_ms; }

static const g6_host_api g_api = {
    sizeof(g6_host_api), G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR,
    host_get_build_info, host_log,
    host_native_resolve, host_native_invoke, host_native_invoke_hash,
    host_script_register, host_script_unregister, host_script_wait,
    host_anchor_address, host_vfs_add_overlay,
    host_game_time_ms,
};

/* ---------------------------------------------------------- DLL exports */
G6_EXPORT const g6_host_api* G6_CALL g6_get_host_api(void) { return &g_api; }

G6_EXPORT const char* G6_CALL g6_status_str(g6_status s) {
    switch (s) {
    case G6_OK: return "ok";
    case G6_E_INVALID_ARG: return "invalid argument";
    case G6_E_UNKNOWN_NATIVE: return "unknown native";
    case G6_E_UNVERIFIED_NATIVE: return "unverified native";
    case G6_E_TOO_MANY_ARGS: return "too many arguments";
    case G6_E_NOT_ON_SCRIPT_THREAD: return "not on script thread";
    case G6_E_UNSUPPORTED_BUILD: return "unsupported game build";
    case G6_E_ONLINE_SESSION: return "online session (refused)";
    case G6_E_NOT_IMPLEMENTED: return "not implemented";
    case G6_E_NO_SUCH_ANCHOR: return "no such anchor";
    default: return "internal error";
    }
}

G6_EXPORT g6_status G6_CALL g6_mock_register_native(uint64_t hash, g6_mock_native_fn fn, void* user) {
    if (hash == 0 || !fn) return G6_E_INVALID_ARG;
    mock_native* n = find_slot(hash, 1);
    if (!n) return G6_E_INTERNAL;
    if (n->hash == 0) { n->hash = hash; ++g_native_count; }
    n->fn = fn;
    n->user = user;
    return G6_OK;
}

G6_EXPORT g6_status G6_CALL g6_mock_step(uint32_t elapsed_ms) {
    if (g_in_tick) return G6_E_INVALID_ARG;
    g_in_tick = 1;
    for (uint32_t i = 0; i < MOCK_MAX_SCRIPTS; ++i)
        if (g_scripts[i].used) g_scripts[i].desc.tick(g_scripts[i].desc.user);
    g_clock_ms += elapsed_ms;
    g_in_tick = 0;
    return G6_OK;
}

/* Built-in fake GET_GAME_TIMER so the DLL is useful with zero setup. */
static void G6_CALL mock_get_game_timer(g6_native_call* c, void* user) { (void)user; c->ret[0] = g_clock_ms; }

#if defined(_WIN32)
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g6_mock_register_native(0x9CD27B0045628463ull, mock_get_game_timer, NULL);
    return TRUE;
}
#endif
