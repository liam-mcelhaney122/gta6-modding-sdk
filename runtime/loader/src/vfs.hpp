// mods/ overlay (design §5, hook-free path): a fiDeviceRelative rooted at the mods folder is
// appended to existing mount points with the engine's own MountAs/MountGlobal, so the layered
// lookup (newest device first) finds mod files before the game's. Every engine call runs on the
// script thread inside the Run hook. See build_consts.hpp for the observed ABI.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gta6mod/gta6mod.h"

namespace g6rt::vfs {

// Resolved anchors (null = not resolved). Filled by runtime.cpp.
struct Anchors {
    const std::uint8_t* mount_global = nullptr;     // fs.mount.mount_global
    const std::uint8_t* rel_init = nullptr;         // fs.device.relative.init
    const std::uint8_t* rel_mount_as = nullptr;     // fs.device.relative.mount_as
    const std::uint8_t* rel_vtable = nullptr;       // fs.device.relative.vtable (after deref)
    const std::uint8_t* rel_vtable_site = nullptr;  // fs.device.relative.vtable signature match (lea in the ctor)
    const std::uint8_t* base_ctor = nullptr;        // fs.device.base.ctor
    const std::uint8_t* mount_table = nullptr;      // fs.mount.table (holds the entry array pointer)
    // device vtables, for naming devices in the mount-table dump only
    const std::uint8_t* vt_base = nullptr;
    const std::uint8_t* vt_local = nullptr;
    const std::uint8_t* vt_packfile = nullptr;
    const std::uint8_t* vt_memory = nullptr;
    const std::uint8_t* vt_growbuffer = nullptr;
    const std::uint8_t* vt_count = nullptr;
    const std::uint8_t* vt_dstorage = nullptr;
    const std::uint8_t* vt_rawstreamer = nullptr;
};

using CtorFn = void* (*)(void* self);
using InitFn = void (*)(void* self, const char* root, bool read_only, void* parent);
using MountAsFn = bool (*)(void* self, const char* mount_point);
using GetAttrFn = std::uint32_t (*)(void* self, const char* path);

struct Engine {
    CtorFn ctor = nullptr;
    InitFn init = nullptr;
    MountAsFn mount_as = nullptr;
    const void* rel_vtable = nullptr;
    std::uint8_t* mount_table = nullptr;   // address of the pointer to the entry array
    std::uint32_t dev_size = 0;
    bool ok = false;
    char why[256] = {};
};

// Checks the code shapes the ABI was derived from (ctor bytes, Init/MountAs field offsets, dtor
// size, MountGlobal's table references) and fills `e`. `base/size` = game image (vtable pointers
// that are file VAs are rebased, for the offline test). Never calls anything.
bool derive(const Anchors& a, const std::uint8_t* base, std::size_t size, const std::uint8_t* text_lo,
            const std::uint8_t* text_hi, Engine& e) noexcept;

// "host_prefix|game_prefix": host path -> game-side path. Pure.
bool map_path(const char* host, const char* map, char* out, std::size_t cap, const char** why) noexcept;

// Logs the mount table (prefix, flag, device count, device kinds, Relative roots). Read-only, SEH-guarded.
void dump_mounts(const Engine& e, const Anchors& a, const char* when) noexcept;

enum class Result : int { mounted = 0, refused = 1, faulted = 2 };
// One Relative device for (game_root, mount_point) in `dev_mem` (>= e.dev_size bytes, zeroed here).
// Script thread only. A fault means the engine state is unknown: the caller disables the overlay.
Result mount_one(const Engine& e, const char* game_root, const char* mount_point, std::uint8_t* dev_mem,
                 char* why, std::size_t cap) noexcept;

// ---- runtime glue
struct Config {
    bool enabled = false;          // [vfs] mode=overlay
    char path_map[600] = {};       // [vfs] path_map  (UNKNOWN until verified live: empty = refuse)
    char mount_points[256] = {};   // [vfs] mount_points
};
void configure(const Config& c, const Engine& e, const Anchors& a) noexcept;
// vfs_add_overlay: validates and queues; mounting happens in process_pending().
g6_status request(const char* host_root, std::int32_t priority) noexcept;
// Script thread, inside the tick window. Mounts queued overlays once the session is single-player.
void process_pending(bool single_player) noexcept;
// First Run call in observe/full mode: dump the table once, scripts exist by then.
void dump_once(const char* when) noexcept;

} // namespace g6rt::vfs
