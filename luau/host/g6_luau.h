/*
 * g6_luau — Luau scripting host for the GTA VI modding runtime.
 *
 * The Luau host is itself a gta6mod plugin: it only talks to the game through
 * the g6_host_api table (include/gta6mod/gta6mod.h), so it is portable to any
 * runtime backend (offline mock today, in-game runtime later).
 *
 * Luau modules exposed to scripts (via require):
 *   "@gta6/native"   resolve(hexHash, sig, name, status) -> handle
 *                    call(handle, ...) -> results (per signature string)
 *                    joaat(string) -> number
 *   "@gta6/natives"  generated typed wrappers (sdk/luau/lib/natives.luau)
 *   "@gta6/script"   register(name, fn), wait(ms), log(level, msg), now() -> ms
 *   "./x"            module x.luau next to the requiring mod's init.luau
 *
 * Each script runs in its own sandboxed Luau thread (coroutine); wait()
 * yields to the scheduler, which resumes it on a later game tick.
 */
#ifndef G6_LUAU_H
#define G6_LUAU_H

#include "gta6mod/gta6mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct g6_luau_config {
    uint32_t    size;              /* = sizeof(g6_luau_config) */
    const char* lib_dir;           /* folder holding natives.luau (sdk/luau/lib) */
    const char* mods_dir;          /* each subfolder with init.luau is a mod */
    int         allow_unverified;  /* permit natives not verified for this build */
} g6_luau_config;

typedef struct g6_luau_host g6_luau_host;

g6_luau_host* g6_luau_create(const g6_host_api* api, const g6_luau_config* cfg);
void          g6_luau_destroy(g6_luau_host* host);
/* Load and run one mod file (top-level code; usually calls script.register). 0 = ok. */
int           g6_luau_run_file(g6_luau_host* host, const char* path);
/* Load every <mods_dir>/<name>/init.luau. Returns number of mods loaded. */
int           g6_luau_load_mods(g6_luau_host* host);
/* Resume every script whose wait has elapsed. Call once per game tick. */
void          g6_luau_tick(g6_luau_host* host);
int           g6_luau_script_count(const g6_luau_host* host);

#ifdef __cplusplus
}
#endif
#endif
