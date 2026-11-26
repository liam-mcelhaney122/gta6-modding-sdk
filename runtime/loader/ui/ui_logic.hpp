// Pure decisions of the overlay (no Windows, D3D or ImGui calls), so the offline test can check them
// directly: which command queue belongs to the swap chain, what a window message does to the menu
// state, and the client -> back-buffer mouse scale.
#pragma once
#include <cstdint>

namespace g6ui::logic {

// ------------------------------------------------------------------ queue match
// A DIRECT command queue seen in ExecuteCommandLists. The overlay keeps a reference on it while it
// is a candidate, so `queue` stays valid. Plain data so the test API can pass arrays of it.
struct QueueCand {
    const void*   queue;
    std::uint32_t device_match;  // 1: same ID3D12Device as the swap chain (or the swap chain's device is unknown)
    std::uint32_t tid;           // thread of its last ExecuteCommandLists
    std::uint64_t seq;           // Present counter value at that call (seq == current: since the last Present)
};

inline constexpr int kQueueNone = -1;       // no fresh candidate on the swap chain's device
inline constexpr int kQueueAmbiguous = -2;  // several fresh candidates, none singled out by the thread

// The swap chain's queue among the candidates: the DIRECT queues on its device that executed since the
// previous Present. One left: that one. Several: the one that executed on the presenting thread, if it
// is the only such. Otherwise none is chosen and the overlay waits (it never guesses).
inline int choose_queue(const QueueCand* c, std::uint32_t n, std::uint32_t present_tid, std::uint64_t seq) noexcept {
    int fresh = 0, fresh_idx = kQueueNone, same_thread = 0, same_idx = kQueueNone;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!c[i].queue || !c[i].device_match || c[i].seq != seq) continue;
        ++fresh;
        fresh_idx = static_cast<int>(i);
        if (c[i].tid == present_tid) { ++same_thread; same_idx = static_cast<int>(i); }
    }
    if (fresh == 0) return kQueueNone;
    if (fresh == 1) return fresh_idx;
    return same_thread == 1 ? same_idx : kQueueAmbiguous;
}

// ------------------------------------------------------------------ window messages
enum class Msg : std::uint8_t {
    other,
    toggle_down,         // the toggle key, first press (not an auto-repeat)
    toggle_repeat,       // the toggle key, auto-repeat
    toggle_up,
    key,                 // WM_KEYDOWN/UP, WM_CHAR, WM_DEADCHAR (other keys)
    sys_key,             // WM_SYSKEYDOWN/UP, WM_SYSCHAR: also passed on, so Alt+Tab / Alt+F4 keep working
    mouse,               // WM_MOUSEMOVE .. WM_MOUSEHWHEEL, WM_MOUSELEAVE
    set_cursor,          // WM_SETCURSOR
    raw_input,           // WM_INPUT
    focus_lost,          // WM_KILLFOCUS, WM_ACTIVATEAPP(FALSE), WM_ACTIVATE(WA_INACTIVE)
};

// Message numbers (winuser.h), repeated so this header needs no <windows.h>.
inline constexpr std::uint32_t kWmActivate = 0x0006, kWmKillFocus = 0x0008, kWmActivateApp = 0x001C, kWmSetCursor = 0x0020,
                               kWmInput = 0x00FF, kWmKeyDown = 0x0100, kWmKeyUp = 0x0101, kWmChar = 0x0102,
                               kWmDeadChar = 0x0103, kWmSysKeyDown = 0x0104, kWmSysKeyUp = 0x0105, kWmSysChar = 0x0106,
                               kWmMouseFirst = 0x0200, kWmMouseLast = 0x020E, kWmMouseLeave = 0x02A3;

inline Msg classify(std::uint32_t msg, std::uint64_t wparam, std::int64_t lparam, std::uint32_t toggle_vk) noexcept {
    switch (msg) {
    case kWmKeyDown:
        if ((wparam & 0xFFFF) == toggle_vk) return (lparam & (1ll << 30)) ? Msg::toggle_repeat : Msg::toggle_down;
        return Msg::key;
    case kWmKeyUp:
        return (wparam & 0xFFFF) == toggle_vk ? Msg::toggle_up : Msg::key;
    case kWmChar: case kWmDeadChar: return Msg::key;
    case kWmSysKeyDown: case kWmSysKeyUp: case kWmSysChar: return Msg::sys_key;
    case kWmSetCursor: return Msg::set_cursor;
    case kWmInput: return Msg::raw_input;
    case kWmMouseLeave: return Msg::mouse;
    case kWmKillFocus: return Msg::focus_lost;
    case kWmActivateApp: return wparam == 0 ? Msg::focus_lost : Msg::other;
    case kWmActivate: return (wparam & 0xFFFF) == 0 ? Msg::focus_lost : Msg::other;
    default:
        return msg >= kWmMouseFirst && msg <= kWmMouseLast ? Msg::mouse : Msg::other;
    }
}

struct Action {
    bool to_imgui = false;   // feed ImGui's Win32 handler
    bool swallow = false;    // do not call the host's window procedure
    bool def_proc = false;   // when swallowed: still DefWindowProc (WM_INPUT cleanup)
    bool toggled = false;    // visibility changed
    bool release = false;    // drop ImGui's held keys/buttons and the mouse capture
};

// Applies `m` to the menu state. Hidden: everything goes to the host except the toggle key.
// Shown: keyboard and mouse go to ImGui only (system keys to both); focus loss hides the menu.
inline Action decide(bool& visible, Msg m) noexcept {
    Action a;
    switch (m) {
    case Msg::toggle_down:
        visible = !visible;
        a.toggled = true;
        a.release = !visible;
        a.swallow = true;
        return a;
    case Msg::toggle_repeat:
    case Msg::toggle_up:
        a.swallow = true;
        return a;
    case Msg::focus_lost:
        if (visible) { visible = false; a.toggled = true; a.release = true; }
        return a;   // the host still sees it
    default:
        break;
    }
    if (!visible) return a;
    switch (m) {
    case Msg::key: case Msg::mouse: a.to_imgui = true; a.swallow = true; break;
    case Msg::sys_key: a.to_imgui = true; break;
    case Msg::set_cursor: a.to_imgui = true; break;   // swallowed only when ImGui set the cursor
    case Msg::raw_input: a.swallow = true; a.def_proc = true; break;
    default: break;
    }
    return a;
}

// ------------------------------------------------------------------ coordinates
// A client-area coordinate in back-buffer pixels (DXGI stretches the back buffer over the client area).
inline float client_to_bb(float v, std::uint32_t client, std::uint32_t bb) noexcept {
    if (client == 0 || bb == 0 || client == bb) return v;
    return v * static_cast<float>(bb) / static_cast<float>(client);
}

}  // namespace g6ui::logic
