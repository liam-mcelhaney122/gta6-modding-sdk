/*
 * gta6mod.h — stable C ABI of the GTA VI modding runtime.
 *
 * This header is the contract between three layers:
 *
 *   platform   : loader + hooks inside a host process (future: PC build, or a
 *                VIctory-style host). Implemented per platform. NOT in this repo yet.
 *   bindings   : per-game-build data generated from re-db (native hashes,
 *                handler RVAs, anchors). Swapped on every title update.
 *   public API : THIS FILE. Must stay binary-compatible across game builds,
 *                so plugins (C/C++, Luau, Python) keep working after updates.
 *
 * Rules
 *  - Plain C, no STL, no exceptions across the boundary.
 *  - Every struct that can grow starts with `uint32_t size` and callers set it.
 *  - The host exposes a function table (g6_host_api); plugins never link
 *    against runtime symbols directly, so a plugin built today loads on a newer
 *    runtime as long as G6_API_VERSION_MAJOR matches.
 *  - Natives are addressed by their 64-bit hash. The runtime resolves a hash to
 *    a handler through the GAME'S OWN resolver (see research/01: YimMenuV2 /
 *    RedHook2 approach) or through re-db handler RVAs. Plugins never see RVAs.
 *  - Single-player only. The runtime refuses to initialise in an online
 *    session (G6_E_ONLINE_SESSION).
 *
 * Status: scaffold. Only the offline "mock" backend (runtime/src/mock_backend.c)
 * implements this today. Native hashes in the generated bindings are
 * UNVERIFIED for GTA VI until re-db marks them verified.
 */
#ifndef GTA6MOD_H
#define GTA6MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G6_API_VERSION_MAJOR 0
#define G6_API_VERSION_MINOR 4  /* 0.2: optional g6_plugin_stop export; 0.3: g6_host_api.ui (UI layer);
                                   0.4: g6_native_call.string_slots / text_slots (script objects) */

#if defined(_WIN32)
#  define G6_CALL __cdecl
#  if defined(G6_BUILDING_RUNTIME)
#    define G6_EXPORT __declspec(dllexport)
#  else
#    define G6_EXPORT __declspec(dllimport)
#  endif
#else
#  define G6_CALL
#  define G6_EXPORT __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */
typedef enum g6_status {
    G6_OK                    = 0,
    G6_E_INVALID_ARG         = 1,
    G6_E_UNKNOWN_NATIVE      = 2,  /* hash not present in this game build      */
    G6_E_UNVERIFIED_NATIVE   = 3,  /* hash known only as an unverified guess   */
    G6_E_TOO_MANY_ARGS       = 4,
    G6_E_NOT_ON_SCRIPT_THREAD= 5,  /* natives must run on the game script thread */
    G6_E_UNSUPPORTED_BUILD   = 6,  /* exe hash not in the address manifest     */
    G6_E_ONLINE_SESSION      = 7,  /* refused: online session detected         */
    G6_E_NOT_IMPLEMENTED     = 8,
    G6_E_NO_SUCH_ANCHOR      = 9,
    G6_E_INTERNAL            = 100
} g6_status;

/* ------------------------------------------------------------------------- */
/* Native calls                                                              */
/* ------------------------------------------------------------------------- */
#define G6_MAX_NATIVE_ARGS 32
#define G6_MAX_NATIVE_RETS 4   /* entity and Vector3 returns use 2 slots (16 bytes) */

/*
 * One native invocation, in 8-byte slots (GTA VI ctx ABI; findings/phase5/23_handle_abi.md).
 * Encoding rules (see g6_arg_* helpers):
 *   int/BOOL/Hash/Player  : sign- or zero-extended into one slot
 *   float                 : IEEE-754 bits in the low 32 bits, upper 32 zero
 *   const char*           : pointer value (caller keeps the string alive)
 *   script String   ('S') : a const char* (UTF-8, NUL-terminated, or NULL) in the slot and the slot's bit
 *                           set in string_slots; the runtime passes the native a script String object
 *                           holding a copy of the text (gta6mod/script_objects.h, G-120)
 *   script text     ('T') : the same with text_slots: the native gets a script text object
 *   Entity/Ped/Vehicle/Object/Pickup/PersChar
 *                         : 16-byte opaque handle (g6_entity), TWO slots (lo, hi)
 *   ScenarioPoint  ('h')  : 16-byte opaque non-entity handle (g6_handle16), TWO
 *                           slots (lo, hi); returned in ret[0..1]
 *   Vector3 param  ('w')  : one packed 16-byte vector {x,y,z,w}, TWO slots:
 *                           x | y<<32 in the first, z in the low half of the
 *                           second (g6_arg_vec3_lo/hi, g6_arg_vec3_packed).
 *                           Measured per native: 254 float triples in 204
 *                           natives are read this way; TASK_GOTO_ENTITY_OFFSET
 *                           reads three float slots, so the signature, not a
 *                           global rule, decides ('v' = three float slots).
 *   Vector3 return        : one packed 16-byte vector {x,y,z,w} in ret[0..1]
 *   entity return         : 16-byte handle in ret[0..1]
 *   pointer out-params    : pointer value; Vector3* out-params point at a
 *                           16-byte-ALIGNED g6_vec3_packed (the game stores
 *                           it with vmovaps, which faults when misaligned),
 *                           entity out-params at a g6_entity
 * Evidence: findings/phase5/23_handle_abi.md (correction 2026-11-25).
 */
typedef struct g6_native_call {
    uint32_t size;                          /* = sizeof(g6_native_call) */
    uint32_t argc;
    uint64_t args[G6_MAX_NATIVE_ARGS];
    uint64_t ret[G6_MAX_NATIVE_RETS];
    /* API 0.4. Bit i set: args[i] holds a const char* (UTF-8, NUL-terminated) that the runtime replaces with a
     * script object for this call only, and restores afterwards; NULL stays a null object.
     *   string_slots: a script String ('S'); text_slots: a script text object ('T').
     * The runtime keeps one immutable object per distinct text for the session (as the game keeps a string
     * literal for its program's lifetime), so a native that stores the pointer keeps a valid object.
     * Callers built against API < 0.4 pass size == G6_NATIVE_CALL_V3_SIZE: both masks then count as 0. A runtime
     * that cannot build the objects for this game build returns G6_E_NO_SUCH_ANCHOR (the anchors behind them,
     * script.string.* and script.text.*, did not resolve or check out) without calling the native. */
    uint32_t string_slots;
    uint32_t text_slots;
} g6_native_call;
#define G6_NATIVE_CALL_V3_SIZE ((uint32_t)offsetof(g6_native_call, string_slots))

typedef struct g6_vec3 { float x, y, z; } g6_vec3;

#if defined(__cplusplus)
#  define G6_ALIGNAS(n) alignas(n)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define G6_ALIGNAS(n) _Alignas(n)
#elif defined(_MSC_VER)
#  define G6_ALIGNAS(n) __declspec(align(n))
#else
#  define G6_ALIGNAS(n) __attribute__((aligned(n)))
#endif
/* In-game Vec3V: what a Vector3* out-param receives (one 16-byte aligned store; w is unspecified). */
typedef struct g6_vec3_packed { G6_ALIGNAS(16) float x; float y, z, w; } g6_vec3_packed;
/* GTA VI script entity handle: a 128-bit opaque key (lo == 0 means "no entity").
   Pass back exactly what a native returned; do not construct or do arithmetic on it. */
typedef struct g6_entity { uint64_t lo, hi; } g6_entity;
/* GTA VI 16-byte opaque handle that is NOT an entity (signature code 'h'; today: ScenarioPoint, whose lo is the
   key of the game's scenario-point map). Same rules as g6_entity: pass back exactly what a native returned; the
   game may not read hi, but it is carried so the value round-trips unchanged (G-099,
   tools/phase5/natives_param_types.py). */
typedef struct g6_handle16 { uint64_t lo, hi; } g6_handle16;
/* GTA V scrVector layout (each component padded to 8 bytes). NOT the GTA VI layout: no GTA VI native
   was found to use it; Vector3* out-params take a g6_vec3_packed. Kept for source compatibility only. */
typedef struct g6_vec3_slots { float x; uint32_t _px; float y; uint32_t _py; float z; uint32_t _pz; } g6_vec3_slots;

/* Opaque resolved-native handle: resolve once, invoke many times. */
typedef struct g6_native_handle_s* g6_native_handle;

/* ------------------------------------------------------------------------- */
/* Scripts                                                                   */
/* ------------------------------------------------------------------------- */
typedef void (G6_CALL *g6_script_tick_fn)(void* user);

typedef struct g6_script_desc {
    uint32_t          size;                 /* = sizeof(g6_script_desc) */
    const char*       name;                 /* UTF-8, unique per plugin  */
    g6_script_tick_fn tick;                 /* called on the script thread every frame */
    void*             user;
} g6_script_desc;

typedef uint32_t g6_script_id;

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */
typedef enum g6_log_level { G6_LOG_DEBUG = 0, G6_LOG_INFO = 1, G6_LOG_WARN = 2, G6_LOG_ERROR = 3 } g6_log_level;

/* ------------------------------------------------------------------------- */
/* Build information                                                         */
/* ------------------------------------------------------------------------- */
typedef struct g6_build_info {
    uint32_t    size;                       /* = sizeof(g6_build_info) */
    const char* game_version;               /* e.g. "1.0.2.1"          */
    const char* platform;                   /* "mock", "xbox-scarlett-host", "pc" */
    uint8_t     exe_sha256[32];
    uint32_t    natives_known;              /* hashes the bindings layer knows */
    uint32_t    natives_verified;           /* of those, verified for this build */
} g6_build_info;

/* ------------------------------------------------------------------------- */
/* UI layer (Dear ImGui overlay on the host's swap chain)                   */
/* ------------------------------------------------------------------------- */
/*
 * One C ABI for every language (C/C++ plugins, the Luau host, Python). Get the table with
 * g6_ui_get(host_api). Model:
 *   - A mod registers a frame callback (frame_register). The overlay calls it once per presented
 *     frame, on the host's RENDER thread, inside an ImGui frame, while the menu is shown
 *     (toggle key, default Insert) or always with G6_UI_FRAME_ALWAYS (HUD; no input then).
 *   - Widget functions are valid only inside a frame callback. Anywhere else they do nothing and
 *     return 0. They follow ImGui's rules: call end() after every begin(); tree_pop() only when
 *     tree_node() returned 1; end_tab_bar()/end_tab_item() only when their begin returned 1. An
 *     unbalanced callback is repaired by the overlay (ImGui error recovery) and logged.
 *   - Natives cannot be called from a frame callback (not the script thread: G6_E_NOT_ON_SCRIPT_THREAD).
 *     Keep state in variables the script tick reads, or hand work to the next tick.
 *   - A frame callback must not block on, or SendMessage to, the game window: the overlay holds its
 *     lock, and the window's message handler waits for it while the menu has input.
 * Off by default: the in-game runtime loads the overlay only with [ui] overlay=1 and [runtime]
 * mode=full; otherwise frame_register returns G6_E_NOT_IMPLEMENTED. Mods' callbacks are skipped
 * while the single-player guard is closed (like native calls).
 */
#define G6_UI_ABI_MAJOR 1
#define G6_UI_ABI_MINOR 0
/* The Dear ImGui release the overlay is built with (sdk/third_party/imgui/G6_VENDOR.txt). A plugin that
   draws with ImGui directly (gta6mod/ui_imgui.hpp) must be built with exactly this version. */
#define G6_UI_IMGUI_VERSION     "1.91.9b"
#define G6_UI_IMGUI_VERSION_NUM 19191

typedef uint32_t g6_ui_id;

typedef struct g6_ui_frame {
    uint32_t size;          /* = sizeof(g6_ui_frame) */
    uint32_t menu_open;     /* 1 while the menu is shown and has the mouse and keyboard */
    float    display_w;     /* back buffer size in pixels (ImGui's DisplaySize) */
    float    display_h;
    float    delta_s;       /* seconds since the previous overlay frame */
    uint32_t _pad;
    uint64_t frame;         /* overlay frame counter */
} g6_ui_frame;

typedef void (G6_CALL *g6_ui_frame_fn)(const g6_ui_frame* frame, void* user);

#define G6_UI_FRAME_ALWAYS 0x1u    /* also while the menu is hidden (HUD); the overlay gives it no input then */

typedef struct g6_ui_frame_desc {
    uint32_t       size;    /* = sizeof(g6_ui_frame_desc) */
    uint32_t       flags;   /* G6_UI_FRAME_* */
    const char*    name;    /* UTF-8, for the log; copied */
    g6_ui_frame_fn draw;
    void*          user;
} g6_ui_frame_desc;

/* begin() window flags: stable values, translated to ImGui's by the implementation. */
#define G6_UI_WINDOW_NO_TITLE_BAR  0x01u
#define G6_UI_WINDOW_NO_RESIZE     0x02u
#define G6_UI_WINDOW_NO_MOVE       0x04u
#define G6_UI_WINDOW_NO_COLLAPSE   0x08u
#define G6_UI_WINDOW_AUTO_RESIZE   0x10u
#define G6_UI_WINDOW_NO_BACKGROUND 0x20u
#define G6_UI_WINDOW_NO_INPUTS     0x40u
/* color_edit flags */
#define G6_UI_COLOR_NO_ALPHA       0x01u
/* set_next_window_pos / set_next_window_size condition */
#define G6_UI_COND_ALWAYS          0u
#define G6_UI_COND_FIRST_USE       1u

/* For plugins that use ImGui directly: the overlay's context and allocator, plus what
   IMGUI_CHECKVERSION() compares. gta6mod/ui_imgui.hpp checks these before ImGui::SetCurrentContext. */
typedef struct g6_ui_imgui_info {
    uint32_t    size;              /* = sizeof(g6_ui_imgui_info), set by the caller */
    uint32_t    version_num;       /* IMGUI_VERSION_NUM of the overlay */
    const char* version;           /* IMGUI_VERSION of the overlay */
    uint32_t    sizeof_io, sizeof_style, sizeof_vec2, sizeof_vec4, sizeof_draw_vert, sizeof_draw_idx;
    void*       context;           /* ImGuiContext*; NULL while the overlay has no context */
    void*       (*alloc_fn)(size_t size, void* user);   /* ImGuiMemAllocFunc */
    void        (*free_fn)(void* ptr, void* user);      /* ImGuiMemFreeFunc */
    void*       alloc_user;
} g6_ui_imgui_info;

typedef struct g6_ui_api {
    uint32_t size;                  /* = sizeof(g6_ui_api) */
    uint16_t abi_major;             /* G6_UI_ABI_MAJOR */
    uint16_t abi_minor;

    /* registration: any thread. unregister waits for a frame in progress (except from inside a callback). */
    g6_status (G6_CALL *frame_register)(const g6_ui_frame_desc* desc, g6_ui_id* out);
    g6_status (G6_CALL *frame_unregister)(g6_ui_id id);
    g6_status (G6_CALL *imgui_info)(g6_ui_imgui_info* out);

    /* widgets: only inside a frame callback. Strings are UTF-8; "label##id" works as in ImGui. */
    int32_t (G6_CALL *begin)(const char* title, int32_t* open /* may be NULL; set to 0 by the close button */,
                             uint32_t window_flags);              /* 1 = draw the contents; call end() always */
    void    (G6_CALL *end)(void);
    void    (G6_CALL *set_next_window_pos)(float x, float y, uint32_t cond);
    void    (G6_CALL *set_next_window_size)(float w, float h, uint32_t cond);
    void    (G6_CALL *text)(const char* text);
    void    (G6_CALL *text_colored)(float r, float g, float b, float a, const char* text);
    int32_t (G6_CALL *button)(const char* label);                 /* 1 = clicked this frame */
    int32_t (G6_CALL *checkbox)(const char* label, int32_t* value);   /* 1 = *value changed */
    int32_t (G6_CALL *slider_int)(const char* label, int32_t* value, int32_t min, int32_t max);
    int32_t (G6_CALL *slider_float)(const char* label, float* value, float min, float max,
                                    const char* format /* NULL = "%.3f" */);
    int32_t (G6_CALL *input_text)(const char* label, char* buf, size_t cap);  /* 1 = edited; buf stays NUL-terminated */
    int32_t (G6_CALL *combo)(const char* label, int32_t* current, const char* const* items, int32_t count);
    void    (G6_CALL *separator)(void);
    void    (G6_CALL *same_line)(void);
    int32_t (G6_CALL *tree_node)(const char* label);              /* 1 = open: call tree_pop() */
    void    (G6_CALL *tree_pop)(void);
    int32_t (G6_CALL *begin_tab_bar)(const char* id);             /* 1 = call end_tab_bar() */
    void    (G6_CALL *end_tab_bar)(void);
    int32_t (G6_CALL *begin_tab_item)(const char* label);         /* 1 = selected: call end_tab_item() */
    void    (G6_CALL *end_tab_item)(void);
    int32_t (G6_CALL *color_edit)(const char* label, float rgba[4], uint32_t flags);  /* 1 = changed */
    /* scale_min >= scale_max: automatic scale. width/height 0: automatic size. */
    void    (G6_CALL *plot_lines)(const char* label, const float* values, int32_t count, const char* overlay,
                                  float scale_min, float scale_max, float width, float height);
} g6_ui_api;

/* ------------------------------------------------------------------------- */
/* Host API function table (what plugins receive)                            */
/* ------------------------------------------------------------------------- */
typedef struct g6_host_api {
    uint32_t size;                          /* = sizeof(g6_host_api) */
    uint16_t version_major;
    uint16_t version_minor;

    g6_status (G6_CALL *get_build_info)(g6_build_info* out);
    void      (G6_CALL *log)(g6_log_level level, const char* plugin, const char* message);

    /* natives */
    g6_status (G6_CALL *native_resolve)(uint64_t hash, g6_native_handle* out);
    g6_status (G6_CALL *native_invoke)(g6_native_handle h, g6_native_call* call);
    g6_status (G6_CALL *native_invoke_hash)(uint64_t hash, g6_native_call* call);

    /* scripts: tick runs on the game's script thread; wait() yields the
       calling script fiber for at least `ms` game milliseconds. */
    g6_status (G6_CALL *script_register)(const g6_script_desc* desc, g6_script_id* out);
    g6_status (G6_CALL *script_unregister)(g6_script_id id);
    g6_status (G6_CALL *script_wait)(uint32_t ms);

    /* named engine anchors from re-db (e.g. "rage::sysMemAllocator::Allocate").
       Returns the live address in the host process. For advanced plugins only;
       anything reachable through natives should use natives. */
    g6_status (G6_CALL *anchor_address)(const char* name, uintptr_t* out);

    /* virtual file system overlay: files under mods_root shadow game files
       (OpenIV / RageOpenV "mods" folder model). */
    g6_status (G6_CALL *vfs_add_overlay)(const char* mods_root, int32_t priority);

    /* monotonic game time in ms (pauses with the game); drives script waits in
       language hosts that schedule their own coroutines (Luau, Python). */
    uint64_t  (G6_CALL *game_time_ms)(void);

    /* API 0.3: the UI layer (never NULL in a 0.3 runtime; read it through g6_ui_get, which checks
       `size` first, so a plugin also loads on a 0.2 runtime). */
    const g6_ui_api* (G6_CALL *ui)(void);
} g6_host_api;

/* The UI table, or NULL when the runtime predates API 0.3 or its UI ABI major differs. */
static inline const g6_ui_api* g6_ui_get(const g6_host_api* api) {
    const g6_ui_api* ui;
    if (!api || api->size < offsetof(g6_host_api, ui) + sizeof(api->ui) || !api->ui) return NULL;
    ui = api->ui();
    if (!ui || ui->abi_major != G6_UI_ABI_MAJOR || ui->size < sizeof(g6_ui_api)) return NULL;
    return ui;
}

/* ------------------------------------------------------------------------- */
/* Plugin entry points (a plugin DLL exports these)                          */
/* ------------------------------------------------------------------------- */
typedef struct g6_plugin_info {
    uint32_t    size;
    const char* name;
    const char* version;
    uint16_t    api_major;                  /* must equal G6_API_VERSION_MAJOR */
    uint16_t    api_minor;
} g6_plugin_info;

typedef const g6_plugin_info* (G6_CALL *g6_plugin_query_fn)(void);           /* "g6_plugin_query" */
typedef g6_status            (G6_CALL *g6_plugin_load_fn)(const g6_host_api*); /* "g6_plugin_load"  */
typedef void                 (G6_CALL *g6_plugin_unload_fn)(void);            /* "g6_plugin_unload"*/
/* Optional (API 0.2): the stop event. The runtime calls it once per loaded plugin before g6_plugin_unload,
   from a script fiber on the game script thread while ticks still run, so natives work there; the plugin
   switches its features off (toggles, spawned entities) without waiting (script_wait is not allowed to
   delay shutdown). When no script tick arrives in time (game paused or exiting), the runtime calls it from
   the shutdown thread instead, where natives return G6_E_NOT_ON_SCRIPT_THREAD. Plugins without the
   export are unloaded as before. */
typedef void                 (G6_CALL *g6_plugin_stop_fn)(void);              /* "g6_plugin_stop"  */

/* ------------------------------------------------------------------------- */
/* Runtime exports (the runtime DLL itself; used by embedded language hosts) */
/* ------------------------------------------------------------------------- */
G6_EXPORT const g6_host_api* G6_CALL g6_get_host_api(void);
G6_EXPORT const char*        G6_CALL g6_status_str(g6_status s);

/* Mock backend only: register a fake native implementation for offline tests. */
typedef void (G6_CALL *g6_mock_native_fn)(g6_native_call* call, void* user);
G6_EXPORT g6_status G6_CALL g6_mock_register_native(uint64_t hash, g6_mock_native_fn fn, void* user);
/* Mock backend only: advance the fake game clock and run every script tick once. */
G6_EXPORT g6_status G6_CALL g6_mock_step(uint32_t elapsed_ms);
/* Mock backend only: the UI layer records instead of drawing (sdk/docs/script_layer.md, "Mock UI").
   g6_mock_ui_frame runs every registered frame callback once (menu open unless set otherwise) and
   returns the number of callbacks run; g6_mock_ui_transcript is that frame's calls, one per line
   ("begin Trainer", "checkbox God mode = 1", ...). click/set queue one interaction for the next widget
   with exactly that label: click = button returns 1, checkbox flips, tree node/tab opens; set = the value
   ("1"/"0", a number, text, a combo index or "r,g,b,a"). Unbalanced begin/end in a callback counts in
   g6_mock_ui_errors and is logged at ERROR. */
G6_EXPORT int32_t     G6_CALL g6_mock_ui_frame(void);
G6_EXPORT const char* G6_CALL g6_mock_ui_transcript(void);
G6_EXPORT g6_status   G6_CALL g6_mock_ui_click(const char* label);
G6_EXPORT g6_status   G6_CALL g6_mock_ui_set(const char* label, const char* value);
G6_EXPORT void        G6_CALL g6_mock_ui_set_menu_open(int32_t open);
G6_EXPORT int32_t     G6_CALL g6_mock_ui_pending(void);   /* queued interactions not consumed yet */
G6_EXPORT int32_t     G6_CALL g6_mock_ui_errors(void);
G6_EXPORT int32_t     G6_CALL g6_mock_ui_registered(void);

/* ------------------------------------------------------------------------- */
/* Header-only helpers                                                       */
/* ------------------------------------------------------------------------- */
static inline uint64_t g6_arg_int(int64_t v)   { return (uint64_t)v; }
static inline uint64_t g6_arg_u64(uint64_t v)  { return v; }
static inline uint64_t g6_arg_u32(uint32_t v)  { return (uint64_t)v; }
static inline uint64_t g6_arg_bool(int v)      { return v ? 1u : 0u; }
static inline uint64_t g6_arg_float(float f)   { union { float f; uint32_t u; } c; c.f = f; return (uint64_t)c.u; }
static inline uint64_t g6_arg_ptr(const void* p){ return (uint64_t)(uintptr_t)p; }
static inline float    g6_ret_float(uint64_t s){ union { float f; uint32_t u; } c; c.u = (uint32_t)s; return c.f; }
/* Packed 16-byte Vector3 return {x,y,z,w}: x,y in ret[0], z in the low half of ret[1]. */
static inline g6_vec3  g6_ret_vec3_packed(const uint64_t* r) {
    g6_vec3 v; v.x = g6_ret_float(r[0]); v.y = g6_ret_float(r[0] >> 32); v.z = g6_ret_float(r[1]); return v;
}
static inline void     g6_set_vec3_packed(uint64_t* r, g6_vec3 v) {
    r[0] = g6_arg_float(v.x) | (g6_arg_float(v.y) << 32); r[1] = g6_arg_float(v.z);
}
/* Packed Vector3 *argument* ('w'): the same 16 bytes as a return, in two consecutive arg slots. */
static inline uint64_t g6_arg_vec3_lo(g6_vec3 v) { return g6_arg_float(v.x) | (g6_arg_float(v.y) << 32); }
static inline uint64_t g6_arg_vec3_hi(g6_vec3 v) { return g6_arg_float(v.z); }
static inline void     g6_arg_vec3_packed(uint64_t* slots, g6_vec3 v) { slots[0] = g6_arg_vec3_lo(v); slots[1] = g6_arg_vec3_hi(v); }
/* Read a packed Vector3 argument back (mock handlers): slots[0..1] as written by g6_arg_vec3_packed. */
static inline g6_vec3  g6_slots_vec3_packed(const uint64_t* slots) { return g6_ret_vec3_packed(slots); }
static inline g6_vec3  g6_vec3_from_packed(const g6_vec3_packed* p) { g6_vec3 v; v.x = p->x; v.y = p->y; v.z = p->z; return v; }
static inline int      g6_entity_valid(g6_entity e) { return e.lo != 0; }
static inline g6_entity g6_entity_make(uint64_t lo, uint64_t hi) { g6_entity e; e.lo = lo; e.hi = hi; return e; }
/* The scenario-point producers store 16 zero bytes when there is no point (G-099, sdk/docs/native-signatures.md). */
static inline int      g6_handle16_valid(g6_handle16 h) { return (h.lo | h.hi) != 0; }

/* Jenkins one-at-a-time hash as used by RAGE (GET_HASH_KEY): ASCII-lowercased. */
static inline uint32_t g6_joaat(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h += c; h += h << 10; h ^= h >> 6;
    }
    h += h << 3; h ^= h >> 11; h += h << 15;
    return h;
}

#ifdef __cplusplus
}
#endif
#endif /* GTA6MOD_H */
