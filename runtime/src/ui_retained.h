// ui_retained — the g6_ui_api for language hosts that must not run on the render thread (the in-game Python host).
//
// A host builds each UI frame on the game script thread as a *program* (a flat list of widget commands with the
// values the mod passed) and submits it here. One native frame callback per program, registered through the host
// table's g6_ui_api, replays the latest program on the overlay's render thread, inside the ImGui frame. What the
// user does there (clicks, edited values, opened trees, selected tabs, closed windows) is stored in this module
// and handed back to the host by poll() on the next script tick. The render thread never calls into the host's
// language: everything it touches is C++ in this module and the g6_ui_api.
//
// Design, wire format and rules: sdk/docs/python_host.md, "Python UI". Exported (C ABI, cdecl) by the module that
// owns the host table: gta6mod_runtime.dll in game, g6_python_test.exe offline. Language hosts find the exports
// with GetProcAddress on that module (g6_python: _g6.ui_proc, which resolves only g6_ui_* names).
//
// Threads: create/destroy/submit/poll from the script thread (create must run inside the plugin's tick or load,
// so the runtime tags the frame with the calling plugin and drops it if the plugin unloads without destroy);
// the frame callback runs on the render thread. Each frame slot has its own lock, held only to swap a program
// pointer or merge results, never across a g6_ui_api call. destroy waits (in frame_unregister) for a frame in
// progress; it must not be called from inside a frame callback.
#pragma once

#include "gta6mod/gta6mod.h"

#include <stddef.h>
#include <stdint.h>

#if defined(G6_UI_RETAINED_IMPL)
#  define G6_UI_RETAINED_EXPORT __declspec(dllexport)
#else
#  define G6_UI_RETAINED_EXPORT
#endif

#define G6_UI_RETAINED_VERSION  1u            /* wire format version (program and poll result) */
#define G6_UI_RETAINED_MAX_FRAMES 64u         /* frame slots per process */
#define G6_UI_RETAINED_MAX_PROGRAM (8u << 20) /* bytes per submitted program */

#ifdef __cplusplus
extern "C" {
#endif

/* G6_UI_RETAINED_VERSION, so a host can refuse a module that speaks another format. */
G6_UI_RETAINED_EXPORT uint32_t  G6_CALL g6_ui_retained_version(void);
/* Register one frame (flags: G6_UI_FRAME_ALWAYS or 0) with the bound host's g6_ui_api. G6_E_NOT_IMPLEMENTED when
   that host has no overlay (frame_register refused it), G6_E_INTERNAL when every slot is taken. */
G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_create(const char* name, uint32_t flags, uint32_t* out_handle);
/* Unregister the frame (waits for a frame in progress) and free its slot. */
G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_destroy(uint32_t handle);
/* Validate and publish a program; the render thread replays it from its next frame on. A malformed program is
   refused with G6_E_INVALID_ARG and the previous one stays. Delivered value overrides are dropped (the program
   carries them now). */
G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_submit(uint32_t handle, const void* program, size_t size);
/* Write the results gathered since the last poll into `out` and mark them delivered. When `cap` is too small
   nothing is consumed: *out_size is the size needed and the call returns G6_E_INVALID_ARG. */
G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_poll(uint32_t handle, void* out, size_t cap, size_t* out_size);

#ifdef __cplusplus
}

namespace g6uiret {
// The host table whose g6_ui_get() the frames are registered with (looked up on every create, so an overlay that
// starts after bind() is seen). The runtime binds its own table; the Python host test binds the table it hands
// the plugin. nullptr unbinds (create then returns G6_E_NOT_IMPLEMENTED).
void bind(const g6_host_api* host) noexcept;
// Slots in use (tests).
unsigned live_frames() noexcept;
} // namespace g6uiret
#endif
