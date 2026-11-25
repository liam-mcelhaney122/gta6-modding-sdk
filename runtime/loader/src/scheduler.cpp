#include "scheduler.hpp"

#include "log.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace g6rt::sched {
namespace {

enum State : int { kFree = 0, kActive = 1, kPendingDelete = 2, kDead = 3, kReserved = 4 };

struct Slot {
    std::atomic<int> state{ kFree };
    std::uint32_t generation = 0;
    g6_script_desc desc{};
    char name[64] = {};
    void* owner = nullptr;
    void* fiber = nullptr;
    std::uint64_t wake_ms = 0;
};

Slot g_slots[kMaxScripts];
CRITICAL_SECTION g_cs;
INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;
std::size_t g_stack_reserve = 1u << 20;
void* g_main_fiber = nullptr;
Slot* g_current = nullptr;
std::uint64_t g_now = 0;
bool g_logged_convert_fail = false;

BOOL CALLBACK init_cs(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_cs);
    return TRUE;
}
void lock() noexcept { InitOnceExecuteOnce(&g_cs_once, init_cs, nullptr, nullptr); EnterCriticalSection(&g_cs); }
void unlock() noexcept { LeaveCriticalSection(&g_cs); }

g6_script_id make_id(std::size_t index, std::uint32_t gen) noexcept {
    return static_cast<g6_script_id>((gen << 8) | static_cast<std::uint32_t>(index + 1));
}
Slot* slot_of(g6_script_id id) noexcept {
    const std::uint32_t idx = (id & 0xFF);
    if (idx == 0 || idx > kMaxScripts) return nullptr;
    Slot& s = g_slots[idx - 1];
    if (s.generation != (id >> 8)) return nullptr;
    return &s;
}

// Runs one tick() call; SEH only, no C++ objects in this frame.
bool run_tick_guarded(Slot* s, DWORD* code) noexcept {
    __try {
        s->desc.tick(s->desc.user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
    return true;
}

void WINAPI fiber_proc(void* p) {
    Slot* s = static_cast<Slot*>(p);
    for (;;) {
        if (s->state.load(std::memory_order_acquire) == kActive) {
            DWORD code = 0;
            if (!run_tick_guarded(s, &code)) {
                s->state.store(kDead, std::memory_order_release);
                log::write(log::Level::error, "sched", "script '%s' raised exception 0x%08lX; stopped (unregister to free it)",
                           s->name, code);
            }
        }
        s->wake_ms = 0;  // due again on the next tick
        SwitchToFiber(g_main_fiber);
    }
}

} // namespace

void set_fiber_stack(std::size_t reserve_bytes) noexcept {
    if (reserve_bytes >= (64u << 10) && reserve_bytes <= (64u << 20)) g_stack_reserve = reserve_bytes;
}

g6_status register_script(const g6_script_desc* desc, void* owner, g6_script_id* out) noexcept {
    if (!desc || desc->size < sizeof(g6_script_desc) || !desc->tick || !desc->name) return G6_E_INVALID_ARG;
    lock();
    for (std::size_t i = 0; i < kMaxScripts; ++i) {
        Slot& s = g_slots[i];
        if (s.state.load() != kFree) continue;
        s.state.store(kReserved);
        s.generation = (s.generation + 1) & 0xFFFFFF;
        if (s.generation == 0) s.generation = 1;
        s.desc = *desc;
        std::size_t k = 0;
        for (; k + 1 < sizeof s.name && desc->name[k]; ++k) s.name[k] = desc->name[k];
        s.name[k] = 0;
        s.desc.name = s.name;
        s.owner = owner;
        s.fiber = nullptr;
        s.wake_ms = 0;
        const g6_script_id id = make_id(i, s.generation);
        s.state.store(kActive, std::memory_order_release);
        unlock();
        if (out) *out = id;
        log::write(log::Level::info, "sched", "registered script '%s' id 0x%x", s.name, id);
        return G6_OK;
    }
    unlock();
    return G6_E_INTERNAL;
}

g6_status unregister_script(g6_script_id id) noexcept {
    lock();
    Slot* s = slot_of(id);
    if (!s) { unlock(); return G6_E_INVALID_ARG; }
    const int st = s->state.load();
    if (st != kActive && st != kDead) { unlock(); return G6_E_INVALID_ARG; }
    s->state.store(kPendingDelete, std::memory_order_release);
    unlock();
    return G6_OK;
}

void unregister_owner(void* owner) noexcept {
    lock();
    for (Slot& s : g_slots) {
        const int st = s.state.load();
        if ((st == kActive || st == kDead) && s.owner == owner) s.state.store(kPendingDelete, std::memory_order_release);
    }
    unlock();
}

g6_status wait(std::uint32_t ms) noexcept {
    if (!g_current || !g_main_fiber || GetCurrentFiber() != g_current->fiber) return G6_E_NOT_ON_SCRIPT_THREAD;
    g_current->wake_ms = g_now + ms;
    SwitchToFiber(g_main_fiber);
    return G6_OK;
}

void tick_all(std::uint64_t now_ms) noexcept {
    g_now = now_ms;
    bool converted = false;
    if (IsThreadAFiber()) {
        g_main_fiber = GetCurrentFiber();
    } else {
        g_main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (!g_main_fiber) {
            if (!g_logged_convert_fail) {
                g_logged_convert_fail = true;
                log::write(log::Level::error, "sched", "ConvertThreadToFiberEx failed (%lu); scripts not run", GetLastError());
            }
            return;
        }
        converted = true;
    }
    for (std::size_t i = 0; i < kMaxScripts; ++i) {
        Slot& s = g_slots[i];
        const int st = s.state.load(std::memory_order_acquire);
        if (st == kPendingDelete) {
            if (s.fiber) DeleteFiber(s.fiber);
            s.fiber = nullptr;
            lock();
            s.state.store(kFree);
            unlock();
            log::write(log::Level::info, "sched", "script '%s' removed", s.name);
            continue;
        }
        if (st != kActive) continue;
        if (!s.fiber) {
            s.fiber = CreateFiberEx(64u << 10, g_stack_reserve, FIBER_FLAG_FLOAT_SWITCH, fiber_proc, &s);
            if (!s.fiber) {
                log::write(log::Level::error, "sched", "CreateFiberEx failed for '%s' (%lu)", s.name, GetLastError());
                s.state.store(kDead);
                continue;
            }
        }
        if (s.wake_ms > now_ms) continue;
        g_current = &s;
        SwitchToFiber(s.fiber);
        g_current = nullptr;
    }
    if (converted) {
        ConvertFiberToThread();
        g_main_fiber = nullptr;
    }
}

bool in_script_fiber() noexcept { return g_current && GetCurrentFiber() == g_current->fiber; }
void* current_owner() noexcept { return in_script_fiber() ? g_current->owner : nullptr; }

std::size_t active_count() noexcept {
    std::size_t n = 0;
    for (const Slot& s : g_slots) n += s.state.load() == kActive ? 1u : 0u;
    return n;
}

void delete_all() noexcept {
    lock();
    for (Slot& s : g_slots) {
        if (s.state.load() == kFree) continue;
        if (s.fiber) DeleteFiber(s.fiber);
        s.fiber = nullptr;
        s.state.store(kFree);
    }
    unlock();
}

} // namespace g6rt::sched
