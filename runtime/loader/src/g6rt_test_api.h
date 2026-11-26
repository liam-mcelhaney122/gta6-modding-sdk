/* Offline self-test surface of gta6mod_runtime.dll (exported as g6rt_get_test_api).
 * Nothing here touches the game; the runtime itself never calls these. */
#ifndef G6RT_TEST_API_H
#define G6RT_TEST_API_H
#include <stddef.h>
#include <stdint.h>

#include "gta6mod/gta6mod.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct g6rt_insn_info {
    uint32_t len, op_off, map, op, rip_relative, disp_off, branch, rel_off, rel_size, terminator, imm_size;
} g6rt_insn_info;

/* branch values: 0 none, 1 jmp8, 2 jmp32, 3 call32, 4 jcc8, 5 jcc32, 6 loop8 */

typedef struct g6rt_test_api {
    uint32_t size;
    int (*decode)(const uint8_t* p, size_t n, g6rt_insn_info* out);
    /* counts[i] = matches of sigs[i] in [base, base+size); first[i] = offset of the first one */
    int (*scan)(const uint8_t* base, size_t size, const char* const* sigs, size_t n, uint32_t* counts, size_t* first);
    int (*build_trampoline)(const uint8_t* src, uint64_t src_addr, size_t src_avail, size_t min_bytes, uint64_t tramp_addr,
                            uint8_t* out, size_t cap, size_t* out_len, size_t* stolen, char* err, size_t errcap);
    void* (*hook_create)(void* target, void* detour, int allow_rel32, void** original, char* err, size_t errcap);
    int (*hook_enable)(void* h, char* err, size_t errcap);
    int (*hook_disable)(void* h, char* err, size_t errcap);
    void (*hook_destroy)(void* h);
    int (*hook_info)(void* h, int* kind, size_t* stolen, size_t* patch_len);
    int (*verify_selfid)(const uint8_t* fn, const uint8_t* lo, const uint8_t* hi, uint64_t hash, uint64_t* id);
    /* image laid out at RVAs (e.g. a file mapped section by section): headers + exports + imphash */
    int (*check_image)(const uint8_t* base, size_t size, char* report, size_t cap);
    /* every patterns.hpp anchor over the executable sections + manifest RVA cross-check + required anchors */
    int (*resolve_anchors)(const uint8_t* base, size_t size, char* report, size_t cap);
    /* static self-id check of every manifest native handler; binds the table to `base` */
    int (*verify_handlers)(const uint8_t* base, size_t size, uint32_t* ok, uint32_t* bad);
    /* run the fiber scheduler once on the calling thread, which acts as the script thread */
    int (*sched_tick)(uint64_t now_ms);
    /* game clock: find the rip load in a native handler (kind 0 = mov eax,[rip], 1 = vmovss xmm0,[rip]) */
    int (*clock_find_load)(const uint8_t* fn, const uint8_t* lo, const uint8_t* hi, int kind, const uint8_t** target,
                           const uint8_t** core, char* why, size_t cap);
    /* feed a sample sequence to a fresh clock::Mixer; -1 if its output ever went backwards. state: 0 candidate 1 trusted 2 rejected */
    int (*clock_mix)(int has_game, const uint32_t* game, const uint64_t* wall, size_t n, uint64_t* out_now, int* state,
                     uint32_t* n_trusted, uint32_t* n_anomaly, uint32_t* n_rejected);
    /* anchors + clock block + vfs ABI checks over an image laid out at its RVAs */
    int (*resolve_engine)(const uint8_t* base, size_t size, char* report, size_t cap, uint32_t* clock_rva, int* vfs_ok);
    int (*vfs_map_path)(const char* host, const char* map, char* out, size_t cap);
    /* vfs::mount_one with caller-supplied engine functions. returns 0 mounted, 1 refused, 2 faulted */
    int (*vfs_mount_fake)(void* ctor, void* init, void* mount_as, const void* rel_vtable, void* mount_table_slot, uint32_t dev_size,
                          const char* root, const char* mount_point, uint8_t* dev_mem, char* why, size_t cap);
    /* guard: address class (0 loopback, 1 link-local, 2 unspecified, 3 private, 4 public) of a 4- or 16-byte address */
    int (*guard_classify)(int v6, const uint8_t* addr);
    int (*guard_trips)(int addr_class, uint32_t tcp_state);
    /* fresh guard::Gate; ops: 0 attest off, 1 attest on, 2 clean scan at t, 3 failed scan, 4 trip, 5 open(t) -> out[i] */
    int (*guard_gate_run)(const int* ops, const uint64_t* t, size_t n, int* out);
    /* one GetExtendedTcpTable pass (v4 + v6) filtered to the calling process; returns 1 if both tables were read */
    int (*guard_scan_self)(uint32_t* own_rows, uint32_t* tripping, unsigned long* error);
    /* late-load patch safety (pure): 1 if rip is in [lo, lo+n); bytes no thread may execute while patching */
    int (*hook_rip_in_range)(uint64_t rip, uint64_t lo, size_t n);
    size_t (*hook_guarded_len)(size_t patch_len, size_t stolen);
    /* last enable/disable freeze: other threads suspended, attempts, last blocking thread and its RIP */
    int (*hook_freeze_info)(void* h, uint32_t* threads, uint32_t* attempts, uint32_t* blocking_tid, uint64_t* blocking_rip);
    /* stop event (g6_plugin_stop) through teardown's plugin sequence with n fake plugins (stops[i] may be null):
       queue the stop script, run one scheduler tick at now_ms if `tick` (this thread = script thread), stop the
       rest on this thread, unload all. Returns 1 if the stop script was queued, 0 if not, -1 on bad input. */
    int (*plugin_stop_run)(const g6_plugin_stop_fn* stops, const g6_plugin_unload_fn* unloads, size_t n, int tick,
                           uint64_t now_ms);
    /* G-093: the scrThread fields as the Run hook reads them (+0x2b8 u32 name hash, +0x344 u8 state, +0x348 u32 id) */
    void (*thread_fields)(const uint8_t* thread, uint32_t* name, uint32_t* state, uint32_t* id);
    /* G-095: a fresh script-thread binder (binder.hpp). mode 0 stable, 1 hash, 2 observe; 0 ms = default.
       binder_run is one Run observation: returns 1 if that call may tick; gen and bound receive the generation and bound id
       after it. binder_counts: binds, unbinds, forgotten threads, tracked threads. */
    void* (*binder_create)(int mode, uint32_t hash, uint32_t observe_ms, uint32_t absent_ms);
    int (*binder_run)(void* b, uint32_t id, uint32_t name, uint8_t state, uint64_t now_ms, uint32_t* gen, uint32_t* bound);
    void (*binder_counts)(void* b, uint32_t* binds, uint32_t* unbinds, uint32_t* gone, uint32_t* tracked);
    void (*binder_destroy)(void* b);
    /* G-120 script objects. objects_resolve: anchors over an image laid out at its RVAs -> the String type and
       element record RVAs and whether the text path checks out; 1 if both kinds are available. objects_call:
       configure the store with these records (unconfigured afterwards), convert call's string_slots/text_slots,
       run fn(call, user) on the converted call, restore; returns the g6_status of the conversion.
       objects_stats: interned objects so far. */
    int (*objects_resolve)(const uint8_t* base, size_t size, uint32_t* type_rva, uint32_t* elem_rva, int* text_ok,
                           char* report, size_t cap);
    int (*objects_call)(const void* type, const void* elem, int text_ok, g6_native_call* call,
                        void (*fn)(g6_native_call*, void*), void* user);
    void (*objects_stats)(uint32_t* strings, uint32_t* texts, uint32_t* temporaries, size_t* bytes);
} g6rt_test_api;

typedef const g6rt_test_api* (G6_CALL *g6rt_get_test_api_fn)(void);

#ifdef __cplusplus
}
#endif
#endif
