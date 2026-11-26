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
 *   "@gta6/script"   register(name, fn), wait(ms), log(level, msg [, source]), now() -> ms
 *                    (source replaces the running script's name as the log source)
 *                    cancel(name) -> boolean: the live script `name` is never resumed again.
 *                    Cancelling the running script ends it at its next wait (or return).
 *                    onStop(fn): fn runs once when the host raises the stop event
 *                    (g6_luau_stop: plugin stop/unload). @gta6/game mods register m.stop here.
 *   "@gta6/ui"       in-game menu over the g6_ui_api C ABI (types: sdk/luau/types/ui.luau):
 *                    onFrame(name, fn, opts) / remove / available / defer, window / beginWindow / endWindow,
 *                    text, textColored, button, checkbox, sliderInt, sliderFloat, inputText, combo,
 *                    separator, sameLine, tree / treeNode / treePop, tabBar / tab (+ unscoped begin/end),
 *                    colorEdit, plotLines. Frame functions run where the host draws (in-game: the
 *                    render thread) under the VM lock; natives and waits fail there; ui.defer(fn) runs
 *                    fn on the next tick.
 *   "@gta6/<lib>"    any other [a-z_]+ name: <lib_dir>/<lib>.luau, e.g. "@gta6/names"
 *                    (generated clean names) and "@gta6/game" (helper layer)
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
/* Raise the stop event: run every script.onStop handler once (registration order, each in its own thread,
   errors logged; a handler that waits is not resumed), then cancel every live script. Call it where
   natives work (the game script thread) before unloading. Idempotent; g6_luau_destroy raises it when it
   was not raised. Returns the number of handlers that failed. */
int           g6_luau_stop(g6_luau_host* host);
int           g6_luau_script_count(const g6_luau_host* host);

#ifdef __cplusplus
}
#endif
#endif
