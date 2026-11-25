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
#define G6_API_VERSION_MINOR 1

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
 *   Entity/Ped/Vehicle/Object/Pickup/PersChar
 *                         : 16-byte opaque handle (g6_entity), TWO slots (lo, hi)
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
} g6_native_call;

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
} g6_host_api;

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
