// Fiber-per-script scheduler. Every function except register/unregister must run on the
// thread that drives tick_all() (the game's script thread inside the Run hook).
#pragma once
#include <cstddef>
#include <cstdint>

#include "gta6mod/gta6mod.h"

namespace g6rt::sched {

inline constexpr std::size_t kMaxScripts = 64;

void set_fiber_stack(std::size_t reserve_bytes) noexcept;
g6_status register_script(const g6_script_desc* desc, void* owner, g6_script_id* out) noexcept;
g6_status unregister_script(g6_script_id id) noexcept;
void unregister_owner(void* owner) noexcept;

// Yield the calling script fiber until game time >= now + ms. Only valid on a script fiber.
g6_status wait(std::uint32_t ms) noexcept;

// Resume every due script once (each tick() call runs until it returns or waits).
void tick_all(std::uint64_t now_ms) noexcept;

bool in_script_fiber() noexcept;
void* current_owner() noexcept;
std::size_t active_count() noexcept;

// Delete every fiber. Only when no tick_all() is running (shutdown path).
void delete_all() noexcept;

} // namespace g6rt::sched
