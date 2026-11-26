/* g6ui.dll: the gta6mod UI overlay. Dear ImGui drawn on the host's D3D12 swap chain from a hook on
 * IDXGISwapChain::Present, input from a subclassed WndProc of the swap chain's window.
 *
 * Loaded by gta6mod_runtime.dll (never from plugins\) only with [ui] overlay=1 and [runtime] mode=full,
 * pinned, never unloaded. The runtime hands plugins the g6_ui_api table (gta6mod.h) through
 * g6_host_api.ui; this header is the runtime <-> g6ui.dll contract plus the offline test surface.
 */
#ifndef G6UI_H
#define G6UI_H

#include <stddef.h>
#include <stdint.h>

#include "gta6mod/gta6mod.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(G6UI_BUILDING)
#  define G6UI_EXPORT __declspec(dllexport)
#else
#  define G6UI_EXPORT __declspec(dllimport)
#endif

enum g6ui_queue_source {
    G6UI_QUEUE_AUTO = 0,   /* the swap chain's own queue (GetDevice(ID3D12CommandQueue)), else ExecuteCommandLists match */
    G6UI_QUEUE_ECL = 1     /* ExecuteCommandLists match only */
};

typedef struct g6ui_config {
    uint32_t size;                 /* = sizeof(g6ui_config) */
    uint32_t toggle_vk;            /* virtual key that shows/hides the menu; 0 = VK_INSERT */
    uint32_t queue_source;         /* g6ui_queue_source */
    uint32_t start_visible;        /* 1 = menu shown at start (tests) */
    /* level: 0 debug, 1 info, 2 warn, 3 error. May be NULL (no log). Called from any thread. */
    void (*log)(int level, const char* message);
    /* 1 = mods' frame callbacks may run this frame; 0 = closed, `why` says why. NULL = always open.
       Called on the render thread once per drawn frame. */
    int (*gate)(char* why, size_t cap);
    int32_t test_fail_hook;        /* tests only: pretend hook n (0 Present, 1 ResizeBuffers, 2 ExecuteCommandLists)
                                      failed to install, to exercise the rollback; -1 = none */
} g6ui_config;

/* Finds the D3D12 swap chain / command queue entry points (dummy device and swap chain on a hidden
   window), then installs the three inline hooks; publishes them only when all three are in place,
   else rolls back the ones already written. Synchronous. G6_OK, or G6_E_INTERNAL with the reason logged. */
G6UI_EXPORT g6_status G6_CALL g6ui_start(const g6ui_config* cfg);
/* Unpublishes, waits for a frame in progress, restores the hooked bytes and the WndProc (when it is
   still ours), releases the GPU objects and the ImGui context. The DLL stays loaded (trampolines kept). */
G6UI_EXPORT void G6_CALL g6ui_stop(void);
/* The g6_ui_api table (valid whether or not the overlay runs; frame_register works before start). */
G6UI_EXPORT const g6_ui_api* G6_CALL g6ui_api(void);
/* frame_register with an owner tag (the runtime passes the plugin), for g6ui_unregister_owner. */
G6UI_EXPORT g6_status G6_CALL g6ui_frame_register_owned(const g6_ui_frame_desc* desc, void* owner, g6_ui_id* out);
/* Removes every frame callback registered with `owner`; waits for a frame in progress. */
G6UI_EXPORT void G6_CALL g6ui_unregister_owner(void* owner);

/* ------------------------------------------------------------------ offline tests */
typedef struct g6ui_stats {
    uint32_t size;
    uint32_t hooks_published;      /* 1 while all three hooks are installed and active */
    uint64_t present_calls;        /* Present detour entries (active or not) */
    uint64_t resize_calls;
    uint64_t ecl_calls;
    uint64_t frames_drawn;         /* frames whose ImGui output was submitted */
    uint64_t frames_skipped;       /* frames not drawn: fence wait timed out, no queue yet, ... */
    uint64_t callbacks_run;        /* mod frame callbacks invoked */
    uint64_t callbacks_gated;      /* frames whose mod callbacks were skipped by the gate */
    uint64_t draw_cmds;            /* ImDrawCmd count of the last drawn frame */
    uint32_t binds;                /* swap chain (re)binds */
    uint32_t backend_inits;        /* ImGui DX12 backend initialisations (format changes re-init) */
    uint32_t queue_source;         /* 0 none yet, 1 swap chain query, 2 ExecuteCommandLists match */
    uint32_t visible;              /* menu shown */
    uint32_t wndproc_installed;
    uint32_t bb_width, bb_height;  /* bound back buffer size */
    uint32_t rtv_format;           /* DXGI_FORMAT of the bound swap chain */
    uint32_t recoveries;           /* frame callbacks repaired by ImGui error recovery or SEH */
    void*    bound_swapchain;
    void*    bound_queue;
    char     last_error[256];
} g6ui_stats;

typedef struct g6ui_test_api {
    uint32_t size;
    int  (*stats)(g6ui_stats* out);
    /* Copy back-buffer pixel (x, y) right after the overlay drew into it, on the next drawn frame. */
    void (*request_probe)(uint32_t x, uint32_t y);
    /* 1 = the probe finished: rgba = R | G<<8 | B<<16 | A<<24 (8-bit formats only); 0 = not yet. */
    int  (*probe_result)(uint32_t* rgba);
    /* Pure-logic checks used by the test (ui_logic.hpp). */
    int  (*choose_queue)(const void* cands, uint32_t n, uint32_t present_tid, uint64_t seq);
    uint32_t (*translate_window_flags)(uint32_t g6_flags);
} g6ui_test_api;

G6UI_EXPORT const g6ui_test_api* G6_CALL g6ui_get_test_api(void);

#ifdef __cplusplus
}
#endif
#endif
