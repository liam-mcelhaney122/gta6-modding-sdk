// g6ui.dll: Dear ImGui overlay on the host's D3D12 swap chain (see g6ui.h, sdk/runtime/loader/README.md "UI overlay").
//
// Threads: Present runs on the host's presenting thread (the "render thread"); the WndProc on the window's
// thread; start/stop on the runtime's init/shutdown thread. g_lock (a CRITICAL_SECTION, recursive) guards the
// ImGui context, the bound-swap-chain state and the frame registry. Detours check g_active before and after
// taking it, so stop() only has to clear g_active and take the lock once to know no thread draws any more.
#define G6UI_BUILDING 1
#include "g6ui.h"
#include "ui_logic.hpp"

#include "hook.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include <atomic>
#include <cfloat>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace g6ui {
namespace {

namespace lg = logic;
namespace hook = g6rt::hook;

// COM vtable slots (stable ABI): IDXGISwapChain = IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) + ...
constexpr int kSlotPresent = 8;          // IDXGISwapChain::Present
constexpr int kSlotResizeBuffers = 13;   // IDXGISwapChain::ResizeBuffers
constexpr int kSlotExecute = 10;         // ID3D12CommandQueue::ExecuteCommandLists (IUnknown 3, ID3D12Object 4, DeviceChild 1, 2 tile calls)
constexpr UINT kFrames = 3;              // our command allocators / ImGui frame resources in flight
constexpr UINT kRtvSlots = 16;           // DXGI_MAX_SWAP_CHAIN_BUFFERS
constexpr DWORD kFenceWaitMs = 250;      // a frame whose ring slot is still busy after this is skipped
constexpr ULONGLONG kSwitchAfterMs = 2000;   // another swap chain takes over after the bound one is silent this long
constexpr UINT kMaxCands = 8;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// ------------------------------------------------------------------ globals
g6ui_config g_cfg{};
std::atomic<bool> g_active{ false };    // hooks published and overlay running
bool g_published = false;               // start() succeeded (owned by the start/stop thread)

struct Lock {
    CRITICAL_SECTION cs;
    Lock() noexcept { InitializeCriticalSectionAndSpinCount(&cs, 1000); }
    ~Lock() { DeleteCriticalSection(&cs); }
};
Lock g_lockobj;
struct Guard {
    Guard() noexcept { EnterCriticalSection(&g_lockobj.cs); }
    ~Guard() { LeaveCriticalSection(&g_lockobj.cs); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

hook::InlineHook g_hooks[3];            // 0 Present, 1 ResizeBuffers, 2 ExecuteCommandLists
void* g_targets[3] = {};
const char* const kHookNames[3] = { "IDXGISwapChain::Present", "IDXGISwapChain::ResizeBuffers", "ID3D12CommandQueue::ExecuteCommandLists" };
PresentFn g_orig_present = nullptr;
ResizeFn g_orig_resize = nullptr;
ExecuteFn g_orig_execute = nullptr;

thread_local int t_internal = 0;        // our own D3D12 calls: the ExecuteCommandLists detour passes them through
thread_local int t_in_frame = 0;        // inside a mod frame callback: widget functions are live

std::atomic<std::uint64_t> s_present{ 0 }, s_resize{ 0 }, s_ecl{ 0 }, s_drawn{ 0 }, s_skipped{ 0 }, s_callbacks{ 0 }, s_gated{ 0 };
std::atomic<std::uint64_t> g_present_seq{ 1 };
char g_last_error[256] = {};

void logf(int level, const char* fmt, ...) noexcept {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (level >= 2) std::snprintf(g_last_error, sizeof g_last_error, "%s", buf);
    if (g_cfg.log) g_cfg.log(level, buf);
}

template <class T> void release(T*& p) noexcept {
    if (p) { p->Release(); p = nullptr; }
}

// ------------------------------------------------------------------ frame registry
struct Entry {
    g6_ui_id id = 0;
    std::uint32_t flags = 0;
    char name[64] = {};
    g6_ui_frame_fn draw = nullptr;
    void* user = nullptr;
    void* owner = nullptr;
    bool active = true;
    std::uint32_t errors = 0;
};
std::vector<Entry> g_entries;
g6_ui_id g_next_id = 1;
int g_dispatching = 0;                  // > 0 while callbacks run (entries are only marked inactive then)

void sweep_entries() noexcept {
    if (g_dispatching) return;
    for (std::size_t i = 0; i < g_entries.size();)
        if (!g_entries[i].active) g_entries.erase(g_entries.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
}

// ------------------------------------------------------------------ bound swap chain
struct Bound {
    IDXGISwapChain* sc = nullptr;       // identity only (no reference held)
    HWND hwnd = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    UINT width = 0, height = 0, buffers = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    UINT rtv_inc = 0;
    bool srv_used = false;
    ID3D12CommandAllocator* alloc[kFrames] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    UINT64 slot_value[kFrames] = {};
    UINT64 render_count = 0;
    bool dx12_backend = false;
    ID3D12Resource* readback = nullptr;
    ULONGLONG last_present_ms = 0;
};
Bound g_b;
ImGuiContext* g_ctx = nullptr;
bool g_win32_backend = false;
HWND g_win32_hwnd = nullptr;
bool g_visible = false;
std::uint32_t g_binds = 0, g_backend_inits = 0, g_queue_source = 0, g_recoveries = 0;
std::uint64_t g_last_draw_cmds = 0;
std::uint64_t g_frame_no = 0;
LARGE_INTEGER g_qpf{}, g_last_frame_qpc{};
IDXGISwapChain* g_rejected[8] = {};     // swap chains that are not D3D12 (GetBuffer(ID3D12Resource) failed)

// pixel probe (tests)
struct Probe {
    bool requested = false, pending = false, done = false;
    UINT x = 0, y = 0;
    UINT64 fence = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t rgba = 0;
};
Probe g_probe;

// WndProc subclass
std::atomic<WNDPROC> g_orig_wndproc{ nullptr };
HWND g_wndproc_hwnd = nullptr;
LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l);

// ExecuteCommandLists candidates (queue matching); references held while listed
SRWLOCK g_cand_lock = SRWLOCK_INIT;
struct Cand { ID3D12CommandQueue* q; ID3D12Device* dev; std::uint32_t tid; std::uint64_t seq; };
Cand g_cands[kMaxCands] = {};
UINT g_ncands = 0;
std::atomic<bool> g_want_queue{ false };
std::atomic<ID3D12Device*> g_want_device{ nullptr };   // compare only; nullptr = any device
std::uint32_t g_ambiguous_logs = 0;

void clear_cands() noexcept {
    AcquireSRWLockExclusive(&g_cand_lock);
    for (UINT i = 0; i < g_ncands; ++i) release(g_cands[i].q);
    g_ncands = 0;
    ReleaseSRWLockExclusive(&g_cand_lock);
}

// ------------------------------------------------------------------ ImGui allocator (shared with plugins)
void* imgui_alloc(size_t size, void*) { return std::malloc(size); }
void imgui_free(void* p, void*) { std::free(p); }

// ImGui's recoverable user errors (unbalanced Begin/End, ...): dispatch() reports which callback, this
// says what ImGui found. Rate-limited; ImGui would otherwise printf or assert.
void imgui_error(ImGuiContext*, void*, const char* msg) {
    static std::atomic<std::uint32_t> n{ 0 };
    if (n.fetch_add(1) < 20) logf(2, "ImGui: %s", msg);
}

void imgui_assert(const char* expr, const char* file, int line) {
    static std::atomic<std::uint32_t> n{ 0 };
    if (n.fetch_add(1) < 20) logf(2, "ImGui check failed: %s (%s:%d)", expr, file, line);
}

// ------------------------------------------------------------------ teardown of the bound state
// Waits (bounded) for our submitted work, then releases everything tied to the bound device. With
// wait_gpu == false (device removed) nothing is waited for.
void wait_idle(bool wait_gpu) noexcept {
    if (!wait_gpu || !g_b.fence || !g_b.event || g_b.fence_value == 0) return;
    if (g_b.fence->GetCompletedValue() >= g_b.fence_value) return;
    if (SUCCEEDED(g_b.fence->SetEventOnCompletion(g_b.fence_value, g_b.event)) &&
        WaitForSingleObject(g_b.event, 2000) != WAIT_OBJECT_0)
        logf(2, "GPU did not finish the overlay's last frame within 2 s; releasing its objects anyway");
}

void shutdown_dx12_backend() noexcept {
    if (g_b.dx12_backend && g_ctx) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui_ImplDX12_Shutdown();
    }
    g_b.dx12_backend = false;
    g_b.srv_used = false;
}

void unbind(bool wait_gpu, const char* why) noexcept {
    if (!g_b.sc && !g_b.queue && !g_b.device) return;
    logf(1, "overlay detached from swap chain %p (%s)", static_cast<void*>(g_b.sc), why);
    wait_idle(wait_gpu);
    shutdown_dx12_backend();
    release(g_b.readback);
    for (auto& a : g_b.alloc) release(a);
    release(g_b.list);
    release(g_b.fence);
    if (g_b.event) CloseHandle(g_b.event);
    release(g_b.rtv_heap);
    release(g_b.srv_heap);
    release(g_b.queue);
    release(g_b.device);
    g_probe = Probe{};
    g_b = Bound{};
}

void shutdown_imgui() noexcept {
    if (!g_ctx) return;
    ImGui::SetCurrentContext(g_ctx);
    if (g_win32_backend) ImGui_ImplWin32_Shutdown();
    g_win32_backend = false;
    g_win32_hwnd = nullptr;
    ImGui::DestroyContext(g_ctx);
    g_ctx = nullptr;
}

// Restores the window procedure when nobody subclassed after us; otherwise ours stays and passes through.
void restore_wndproc() noexcept {
    if (!g_wndproc_hwnd) return;
    const WNDPROC orig = g_orig_wndproc.load();
    if (IsWindow(g_wndproc_hwnd)) {
        const LONG_PTR cur = GetWindowLongPtrW(g_wndproc_hwnd, GWLP_WNDPROC);
        if (cur == reinterpret_cast<LONG_PTR>(&ui_wndproc)) {
            SetWindowLongPtrW(g_wndproc_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
            g_orig_wndproc.store(nullptr);
            logf(1, "window procedure of %p restored", static_cast<void*>(g_wndproc_hwnd));
        } else {
            logf(2, "window %p was subclassed after the overlay; its procedure stays chained (inactive pass-through)",
                 static_cast<void*>(g_wndproc_hwnd));
            return;   // g_orig_wndproc stays: ui_wndproc keeps forwarding to it
        }
    } else {
        g_orig_wndproc.store(nullptr);
    }
    g_wndproc_hwnd = nullptr;
}

bool install_wndproc(HWND hwnd) noexcept {
    if (g_wndproc_hwnd == hwnd) return true;
    restore_wndproc();
    if (g_wndproc_hwnd) return false;   // could not restore the old one: do not stack a second subclass
    SetLastError(0);
    const LONG_PTR prev = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!prev) { logf(2, "GetWindowLongPtrW(GWLP_WNDPROC) of %p failed (%lu): no input", static_cast<void*>(hwnd), GetLastError()); return false; }
    g_orig_wndproc.store(reinterpret_cast<WNDPROC>(prev));
    SetLastError(0);
    const LONG_PTR old = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ui_wndproc));
    if (!old && GetLastError() != 0) {
        g_orig_wndproc.store(nullptr);
        logf(2, "SetWindowLongPtrW(GWLP_WNDPROC) on %p failed (%lu): no input", static_cast<void*>(hwnd), GetLastError());
        return false;
    }
    if (old != prev) g_orig_wndproc.store(reinterpret_cast<WNDPROC>(old));
    g_wndproc_hwnd = hwnd;
    logf(1, "input: window %p subclassed (toggle key VK 0x%02X)", static_cast<void*>(hwnd), g_cfg.toggle_vk);
    return true;
}

// ------------------------------------------------------------------ ImGui setup
bool ensure_context(HWND hwnd) noexcept {
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        ImGui::SetAllocatorFunctions(imgui_alloc, imgui_free, nullptr);
        g6_imgui_assert_hook = imgui_assert;
        g_ctx = ImGui::CreateContext();
        if (!g_ctx) return false;
        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // nothing written into the game folder
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigErrorRecovery = true;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;   // reported through imgui_error instead (ImGui would printf)
        io.ConfigErrorRecoveryEnableTooltip = false;
        g_ctx->ErrorCallback = imgui_error;
        ImGui::StyleColorsDark();
    }
    ImGui::SetCurrentContext(g_ctx);
    if (g_win32_backend && g_win32_hwnd != hwnd) {
        ImGui_ImplWin32_Shutdown();
        g_win32_backend = false;
    }
    if (!g_win32_backend) {
        if (!ImGui_ImplWin32_Init(hwnd)) { logf(3, "ImGui_ImplWin32_Init failed"); return false; }
        g_win32_backend = true;
        g_win32_hwnd = hwnd;
    }
    return true;
}

void srv_alloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    // The 1.91 backend allocates exactly one descriptor (the font texture); the heap has one slot.
    IM_ASSERT(!g_b.srv_used);
    g_b.srv_used = true;
    *cpu = info->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = info->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) { g_b.srv_used = false; }

bool init_dx12_backend() noexcept {
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g_b.device;
    ii.CommandQueue = g_b.queue;
    ii.NumFramesInFlight = static_cast<int>(kFrames);
    ii.RTVFormat = g_b.format;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_b.srv_heap;
    ii.SrvDescriptorAllocFn = srv_alloc;
    ii.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&ii)) { logf(3, "ImGui_ImplDX12_Init failed"); return false; }
    g_b.dx12_backend = true;
    g_b.render_count = 0;   // the backend's frame index restarts with it
    ++g_backend_inits;
    return true;
}

// ------------------------------------------------------------------ binding
bool is_rejected(IDXGISwapChain* sc) noexcept {
    for (auto* r : g_rejected) if (r == sc) return true;
    return false;
}
void reject(IDXGISwapChain* sc, const char* why) noexcept {
    for (auto*& r : g_rejected) if (!r) { r = sc; break; }
    logf(2, "swap chain %p ignored: %s", static_cast<void*>(sc), why);
}

// The command queue the swap chain presents on. Fills `out` with a reference, or returns false (not yet).
bool find_queue(IDXGISwapChain3* sc, ID3D12CommandQueue** out) noexcept {
    *out = nullptr;
    if (g_cfg.queue_source == G6UI_QUEUE_AUTO) {
        ID3D12CommandQueue* q = nullptr;
        // DXGI hands back the object the swap chain was created with (for D3D12: the command queue).
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q))) && q) {
            const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
            if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                *out = q;
                g_queue_source = 1;
                return true;
            }
            q->Release();
        }
    }
    // ExecuteCommandLists match: ask the detour for candidates, decide on a later Present.
    if (!g_want_queue.load()) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) && dev) {
            g_want_device.store(dev);
            dev->Release();   // compare only
        } else {
            g_want_device.store(nullptr);
        }
        g_want_queue.store(true);
        return false;
    }
    AcquireSRWLockExclusive(&g_cand_lock);
    lg::QueueCand view[kMaxCands] = {};
    ID3D12Device* want = g_want_device.load();
    for (UINT i = 0; i < g_ncands; ++i)
        view[i] = { g_cands[i].q, (!want || g_cands[i].dev == want) ? 1u : 0u, g_cands[i].tid, g_cands[i].seq };
    const int pick = lg::choose_queue(view, g_ncands, GetCurrentThreadId(), g_present_seq.load());
    if (pick >= 0) {
        *out = g_cands[pick].q;
        g_cands[pick].q = nullptr;   // reference moves to the caller
    }
    ReleaseSRWLockExclusive(&g_cand_lock);
    if (pick == lg::kQueueAmbiguous && g_ambiguous_logs++ < 5)
        logf(2, "several DIRECT queues executed on other threads since the last Present; waiting for a frame that singles one out");
    if (pick < 0) return false;
    g_want_queue.store(false);
    clear_cands();
    g_queue_source = 2;
    return true;
}

bool create_objects() noexcept {
    HRESULT hr;
    D3D12_DESCRIPTOR_HEAP_DESC rd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(hr = g_b.device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_b.rtv_heap)))) { logf(3, "RTV heap: 0x%08lX", hr); return false; }
    g_b.rtv_inc = g_b.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC sd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(hr = g_b.device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_b.srv_heap)))) { logf(3, "SRV heap: 0x%08lX", hr); return false; }
    for (auto& a : g_b.alloc)
        if (FAILED(hr = g_b.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { logf(3, "allocator: 0x%08lX", hr); return false; }
    if (FAILED(hr = g_b.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_b.alloc[0], nullptr, IID_PPV_ARGS(&g_b.list)))) {
        logf(3, "command list: 0x%08lX", hr);
        return false;
    }
    g_b.list->Close();
    if (FAILED(hr = g_b.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_b.fence)))) { logf(3, "fence: 0x%08lX", hr); return false; }
    g_b.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_b.event != nullptr;
}

// Binds the overlay to `sc` (identity, queue, device, our objects, ImGui backends, WndProc).
// False: not now (no queue yet) or refused; the frame is not drawn.
bool bind(IDXGISwapChain* sc, IDXGISwapChain3* sc3, const DXGI_SWAP_CHAIN_DESC1& d, HWND hwnd) noexcept {
    ID3D12Resource* probe_buffer = nullptr;
    if (FAILED(sc3->GetBuffer(0, IID_PPV_ARGS(&probe_buffer)))) { reject(sc, "GetBuffer(ID3D12Resource) failed: not a D3D12 swap chain"); return false; }
    probe_buffer->Release();
    ID3D12CommandQueue* q = nullptr;
    if (!find_queue(sc3, &q)) return false;
    unbind(true, "another swap chain");
    g_b.sc = sc;
    g_b.hwnd = hwnd;
    g_b.queue = q;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&g_b.device)))) { logf(3, "queue GetDevice failed"); unbind(false, "no device"); return false; }
    g_b.width = d.Width;
    g_b.height = d.Height;
    g_b.buffers = d.BufferCount;
    g_b.format = d.Format;
    if (!create_objects() || !ensure_context(hwnd)) { unbind(true, "object creation failed"); return false; }
    if (!init_dx12_backend()) { unbind(true, "backend init failed"); return false; }
    install_wndproc(hwnd);
    ++g_binds;
    logf(1, "overlay bound: swap chain %p, window %p, %ux%u format %d, %u buffers, queue %p (%s), device %p",
         static_cast<void*>(sc), static_cast<void*>(hwnd), d.Width, d.Height, static_cast<int>(d.Format), d.BufferCount,
         static_cast<void*>(q), g_queue_source == 1 ? "swap chain's own queue" : "ExecuteCommandLists match",
         static_cast<void*>(g_b.device));
    return true;
}

// ------------------------------------------------------------------ drawing
const char* vk_name(std::uint32_t vk) noexcept {
    static char buf[64];
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = static_cast<LONG>(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) lp |= 1 << 24;
    if (!GetKeyNameTextA(lp, buf, sizeof buf)) std::snprintf(buf, sizeof buf, "VK 0x%02X", vk);
    return buf;
}

int call_seh(g6_ui_frame_fn fn, const g6_ui_frame* f, void* user, DWORD* code) noexcept {
    __try {
        fn(f, user);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return 0;
    }
}

void dispatch(const g6_ui_frame& fr, bool visible, bool gate_open) {
    ++g_dispatching;
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        if (!g_entries[i].active) continue;
        if (!visible && !(g_entries[i].flags & G6_UI_FRAME_ALWAYS)) continue;
        if (!gate_open) continue;
        const g6_ui_id id = g_entries[i].id;
        const g6_ui_frame_fn fn = g_entries[i].draw;
        void* const user = g_entries[i].user;
        ImGui::PushID(static_cast<int>(id));
        ImGuiErrorRecoveryState before;
        ImGui::ErrorRecoveryStoreState(&before);
        DWORD code = 0;
        t_in_frame = 1;
        const int ok = call_seh(fn, &fr, user, &code);
        t_in_frame = 0;
        s_callbacks.fetch_add(1);
        ImGuiErrorRecoveryState after;
        ImGui::ErrorRecoveryStoreState(&after);
        const bool unbalanced = std::memcmp(&before, &after, sizeof before) != 0;
        if (unbalanced) ImGui::ErrorRecoveryTryToRecoverState(&before);
        ImGui::PopID();
        Entry* e = nullptr;   // re-find: the callback may have registered (vector growth) or unregistered
        for (auto& x : g_entries) if (x.id == id) e = &x;
        if (unbalanced) {
            ++g_recoveries;
            if (e && e->errors++ < 5)
                logf(2, "ui frame '%s' left ImGui scopes open (begin/end, tree, tab, push); repaired", e->name);
        }
        if (!ok) {
            ++g_recoveries;
            if (e) {
                logf(3, "ui frame '%s' raised exception 0x%08lX; it is unregistered", e->name, code);
                e->active = false;
            }
        }
    }
    --g_dispatching;
    sweep_entries();
}

void status_window(bool gate_open, const char* why) {
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("gta6mod", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        std::size_t n = 0;
        for (const auto& e : g_entries) n += e.active ? 1 : 0;
        ImGui::Text("gta6mod overlay (Dear ImGui %s) - %s hides", IMGUI_VERSION, vk_name(g_cfg.toggle_vk));
        ImGui::Text("%zu mod UI(s) registered", n);
        if (!gate_open) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "mod UIs paused: %s", why);
    }
    ImGui::End();
}

bool format_is_8bit_rgba(DXGI_FORMAT f) noexcept {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void draw_frame(IDXGISwapChain3* sc3) {
    bool any_always = false;
    for (const auto& e : g_entries) if (e.active && (e.flags & G6_UI_FRAME_ALWAYS)) any_always = true;
    const bool visible = g_visible;
    if (!visible && !any_always && !g_probe.requested) return;

    const UINT slot = static_cast<UINT>(g_b.render_count % kFrames);
    if (g_b.fence->GetCompletedValue() < g_b.slot_value[slot]) {
        g_b.fence->SetEventOnCompletion(g_b.slot_value[slot], g_b.event);
        if (WaitForSingleObject(g_b.event, kFenceWaitMs) != WAIT_OBJECT_0) { s_skipped.fetch_add(1); return; }
    }

    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& g = *g_ctx;
    ImGui_ImplDX12_NewFrame();
    const int queued_before = g.InputEventsQueue.Size;
    ImGui_ImplWin32_NewFrame();
    // The Win32 backend works in client pixels; DXGI stretches the back buffer over the client area.
    RECT rc{};
    GetClientRect(g_b.hwnd, &rc);
    const UINT cw = static_cast<UINT>(rc.right - rc.left), ch = static_cast<UINT>(rc.bottom - rc.top);
    for (int i = queued_before; i < g.InputEventsQueue.Size; ++i) {   // events NewFrame itself added (cursor fallback)
        ImGuiInputEvent& ev = g.InputEventsQueue[i];
        if (ev.Type == ImGuiInputEventType_MousePos && ev.MousePos.PosX != -FLT_MAX) {
            ev.MousePos.PosX = lg::client_to_bb(ev.MousePos.PosX, cw, g_b.width);
            ev.MousePos.PosY = lg::client_to_bb(ev.MousePos.PosY, ch, g_b.height);
        }
    }
    io.DisplaySize = ImVec2(static_cast<float>(g_b.width), static_cast<float>(g_b.height));
    io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
    io.MouseDrawCursor = visible;
    if (visible) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard);
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    const float scale = static_cast<float>(g_b.height) / 1080.f;
    io.FontGlobalScale = scale > 1.f ? static_cast<float>(static_cast<int>(scale * 4.f + 0.5f)) / 4.f : 1.f;
    ImGui::NewFrame();

    char why[200] = "open";
    const bool gate_open = g_cfg.gate ? g_cfg.gate(why, sizeof why) != 0 : true;
    if (!gate_open) s_gated.fetch_add(1);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g6_ui_frame fr{};
    fr.size = sizeof fr;
    fr.menu_open = visible ? 1u : 0u;
    fr.display_w = io.DisplaySize.x;
    fr.display_h = io.DisplaySize.y;
    fr.delta_s = g_last_frame_qpc.QuadPart && g_qpf.QuadPart
                     ? static_cast<float>(static_cast<double>(now.QuadPart - g_last_frame_qpc.QuadPart) / static_cast<double>(g_qpf.QuadPart))
                     : 0.f;
    fr.frame = g_frame_no++;
    g_last_frame_qpc = now;
    dispatch(fr, visible, gate_open);
    if (visible) status_window(gate_open, why);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    std::uint64_t cmds = 0;
    for (int i = 0; i < dd->CmdListsCount; ++i) cmds += static_cast<std::uint64_t>(dd->CmdLists[i]->CmdBuffer.Size);
    g_last_draw_cmds = cmds;
    const bool probe = g_probe.requested && g_probe.x < g_b.width && g_probe.y < g_b.height && format_is_8bit_rgba(g_b.format);
    if ((dd->TotalVtxCount == 0 && !probe) || dd->DisplaySize.x <= 0.f || dd->DisplaySize.y <= 0.f) return;

    ID3D12Resource* bb = nullptr;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    if (index >= kRtvSlots || FAILED(sc3->GetBuffer(index, IID_PPV_ARGS(&bb)))) { s_skipped.fetch_add(1); return; }
    if (probe && !g_b.readback) {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g_b.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_b.readback));
    }
    ID3D12CommandAllocator* alloc = g_b.alloc[slot];
    alloc->Reset();
    g_b.list->Reset(alloc, nullptr);
    const D3D12_RESOURCE_BARRIER to_rt = transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_b.list->ResourceBarrier(1, &to_rt);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_b.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * g_b.rtv_inc;
    g_b.device->CreateRenderTargetView(bb, nullptr, rtv);
    g_b.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_b.srv_heap };
    g_b.list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(dd, g_b.list);
    const bool do_probe = probe && g_b.readback;
    if (do_probe) {
        const D3D12_RESOURCE_BARRIER to_src = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_b.list->ResourceBarrier(1, &to_src);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_b.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { g_b.format, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX box{ g_probe.x, g_probe.y, 0, g_probe.x + 1, g_probe.y + 1, 1 };
        g_b.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        const D3D12_RESOURCE_BARRIER back = transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        g_b.list->ResourceBarrier(1, &back);
    } else {
        const D3D12_RESOURCE_BARRIER back = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        g_b.list->ResourceBarrier(1, &back);
    }
    const HRESULT closed = g_b.list->Close();
    bb->Release();
    if (FAILED(closed)) { logf(3, "overlay command list Close failed 0x%08lX", closed); s_skipped.fetch_add(1); return; }
    ID3D12CommandList* lists[] = { g_b.list };
    g_b.queue->ExecuteCommandLists(1, lists);   // t_internal: the detour passes it through
    g_b.queue->Signal(g_b.fence, ++g_b.fence_value);
    g_b.slot_value[slot] = g_b.fence_value;
    ++g_b.render_count;
    s_drawn.fetch_add(1);
    if (do_probe) {
        g_probe.requested = false;
        g_probe.pending = true;
        g_probe.fence = g_b.fence_value;
        g_probe.format = g_b.format;
    }
}

// One Present of `sc` while active, under g_lock.
void on_present(IDXGISwapChain* sc) {
    if (is_rejected(sc)) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    DXGI_SWAP_CHAIN_DESC1 d{};
    HWND hwnd = nullptr;
    sc3->GetDesc1(&d);
    sc3->GetHwnd(&hwnd);
    const ULONGLONG now = GetTickCount64();
    if (g_b.sc && g_b.sc != sc) {
        // A window has one flip-model swap chain at a time: a new one on the bound window replaces it.
        if (hwnd == g_b.hwnd) unbind(true, "a new swap chain on the same window");
        else if (now - g_b.last_present_ms < kSwitchAfterMs) { sc3->Release(); return; }   // not the bound one
        else unbind(true, "the bound swap chain stopped presenting");
    }
    if (g_b.sc == sc && g_b.hwnd != hwnd) unbind(true, "window changed");
    if (g_b.device && g_b.device->GetDeviceRemovedReason() != S_OK) {
        logf(3, "device removed (0x%08lX); overlay detached until a new swap chain presents", g_b.device->GetDeviceRemovedReason());
        unbind(false, "device removed");
        sc3->Release();
        return;
    }
    if (!g_b.sc) {
        if (!hwnd || !bind(sc, sc3, d, hwnd)) { s_skipped.fetch_add(1); sc3->Release(); return; }
    }
    g_b.last_present_ms = now;
    if (d.Width != g_b.width || d.Height != g_b.height || d.Format != g_b.format || d.BufferCount != g_b.buffers) {
        logf(1, "swap chain resized: %ux%u format %d -> %ux%u format %d, %u buffers", g_b.width, g_b.height,
             static_cast<int>(g_b.format), d.Width, d.Height, static_cast<int>(d.Format), d.BufferCount);
        const bool fmt = d.Format != g_b.format;
        g_b.width = d.Width;
        g_b.height = d.Height;
        g_b.buffers = d.BufferCount;
        if (fmt) {
            wait_idle(true);
            shutdown_dx12_backend();
            g_b.format = d.Format;
            if (!init_dx12_backend()) { unbind(true, "backend re-init failed"); sc3->Release(); return; }
        }
    }
    if (g_b.width && g_b.height) draw_frame(sc3);
    sc3->Release();
}

// ------------------------------------------------------------------ detours
HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    s_present.fetch_add(1, std::memory_order_relaxed);
    if (sc && g_active.load(std::memory_order_acquire) && !(flags & DXGI_PRESENT_TEST)) {
        Guard lk;
        if (g_active.load()) {
            t_internal = 1;
            on_present(sc);
            t_internal = 0;
            g_present_seq.fetch_add(1);
        }
    }
    return g_orig_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE resize_detour(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    s_resize.fetch_add(1, std::memory_order_relaxed);
    if (sc && g_active.load(std::memory_order_acquire)) {
        Guard lk;
        if (g_active.load() && g_b.sc == sc) wait_idle(true);   // our commands referenced the old buffers
    }
    return g_orig_resize(sc, count, w, h, fmt, flags);
}

void note_queue(ID3D12CommandQueue* q) noexcept {
    const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    if (d.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev))) && dev) dev->Release();   // compare only; the queue keeps it alive
    const std::uint32_t tid = GetCurrentThreadId();
    const std::uint64_t seq = g_present_seq.load();
    AcquireSRWLockExclusive(&g_cand_lock);
    bool found = false;
    for (UINT i = 0; i < g_ncands; ++i)
        if (g_cands[i].q == q) { g_cands[i].tid = tid; g_cands[i].seq = seq; found = true; }
    if (!found && g_ncands < kMaxCands) {
        q->AddRef();
        g_cands[g_ncands++] = { q, dev, tid, seq };
    }
    ReleaseSRWLockExclusive(&g_cand_lock);
}

void STDMETHODCALLTYPE execute_detour(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    s_ecl.fetch_add(1, std::memory_order_relaxed);
    if (q && !t_internal && g_want_queue.load(std::memory_order_relaxed) && g_active.load(std::memory_order_relaxed)) note_queue(q);
    g_orig_execute(q, n, lists);
}

LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC orig = g_orig_wndproc.load();
    auto forward = [&]() -> LRESULT { return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l); };
    if (!g_active.load(std::memory_order_acquire)) return forward();
    const lg::Msg cls = lg::classify(m, static_cast<std::uint64_t>(w), static_cast<std::int64_t>(l), g_cfg.toggle_vk);
    if (cls == lg::Msg::other) return forward();
    lg::Action act;
    LRESULT handled = 0;
    {
        Guard lk;
        if (!g_active.load()) return forward();
        act = lg::decide(g_visible, cls);
        if (act.toggled) logf(0, "menu %s", g_visible ? "shown" : "hidden");
        if (g_ctx && g_win32_backend && h == g_win32_hwnd) {
            ImGui::SetCurrentContext(g_ctx);
            if (act.release) {
                ImGui::GetIO().AddFocusEvent(false);   // drops held keys and mouse buttons
                if (GetCapture() == h) ReleaseCapture();
            }
            if (act.to_imgui) {
                LPARAM l2 = l;
                if (m == WM_MOUSEMOVE && g_b.width && g_b.height) {   // client pixels -> back-buffer pixels
                    RECT rc{};
                    GetClientRect(h, &rc);
                    const float x = lg::client_to_bb(static_cast<float>(static_cast<short>(LOWORD(l))), static_cast<std::uint32_t>(rc.right), g_b.width);
                    const float y = lg::client_to_bb(static_cast<float>(static_cast<short>(HIWORD(l))), static_cast<std::uint32_t>(rc.bottom), g_b.height);
                    l2 = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
                }
                handled = ImGui_ImplWin32_WndProcHandler(h, m, w, l2);
            }
        }
    }
    if (cls == lg::Msg::set_cursor && act.to_imgui && handled) return TRUE;
    if (act.swallow) return act.def_proc ? DefWindowProcW(h, m, w, l) : 0;
    return forward();
}

// ------------------------------------------------------------------ entry points (dummy swap chain)
bool module_of(const void* p, wchar_t* name, std::size_t cap, HMODULE* mod) noexcept {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) || !m)
        return false;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = std::wcsrchr(path, L'\\');
    std::swprintf(name, cap, L"%ls", base ? base + 1 : path);
    if (mod) *mod = m;
    return true;
}

LRESULT CALLBACK dummy_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Reads Present/ResizeBuffers from a dummy swap chain's vtable and ExecuteCommandLists from a dummy queue's,
// then checks that they lie in dxgi.dll and in a D3D12 runtime module.
bool find_targets() noexcept {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    wchar_t cls[64];
    std::swprintf(cls, 64, L"g6ui_dummy_%lu", GetCurrentProcessId());
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = dummy_proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    const ATOM atom = RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"g6ui", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
    bool ok = false;
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    const char* adapter = "default adapter";
    do {
        if (!hwnd) { logf(3, "dummy window: CreateWindowExW failed (%lu)", GetLastError()); break; }
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) { logf(3, "CreateDXGIFactory1 failed 0x%08lX", hr); break; }
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) {
            IDXGIAdapter* warp = nullptr;
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
                hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
                warp->Release();
                adapter = "WARP";
            }
            if (FAILED(hr)) { logf(3, "D3D12CreateDevice failed 0x%08lX (default and WARP)", hr); break; }
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { logf(3, "dummy queue 0x%08lX", hr); break; }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1))) { logf(3, "dummy swap chain 0x%08lX", hr); break; }
        void** svt = *reinterpret_cast<void***>(sc1);
        void** qvt = *reinterpret_cast<void***>(queue);
        g_targets[0] = svt[kSlotPresent];
        g_targets[1] = svt[kSlotResizeBuffers];
        g_targets[2] = qvt[kSlotExecute];
        const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        wchar_t m0[MAX_PATH], m1[MAX_PATH], m2[MAX_PATH];
        HMODULE h0 = nullptr, h1 = nullptr, h2 = nullptr;
        if (!module_of(g_targets[0], m0, MAX_PATH, &h0) || !module_of(g_targets[1], m1, MAX_PATH, &h1) ||
            !module_of(g_targets[2], m2, MAX_PATH, &h2)) {
            logf(3, "a swap chain or queue entry point is not inside a loaded module (vtable replaced by someone?)");
            break;
        }
        if (h0 != dxgi || h1 != dxgi) {
            logf(3, "Present/ResizeBuffers are in %ls / %ls, not dxgi.dll (vtable replaced by another overlay?); refusing", m0, m1);
            break;
        }
        if (_wcsnicmp(m2, L"d3d12", 5) != 0) {
            logf(3, "ExecuteCommandLists is in %ls, not a D3D12 runtime module; refusing", m2);
            break;
        }
        logf(1, "entry points (dummy device on %s): Present %p, ResizeBuffers %p in dxgi.dll; ExecuteCommandLists %p in %ls",
             adapter, g_targets[0], g_targets[1], g_targets[2], m2);
        ok = true;
    } while (false);
    release(sc1);
    release(queue);
    release(device);
    release(factory);
    if (hwnd) DestroyWindow(hwnd);
    if (atom) UnregisterClassW(cls, inst);
    return ok;
}

// ------------------------------------------------------------------ hooks
bool hooks_created = false;

bool create_hooks() noexcept {
    if (hooks_created) return true;
    void* const detours[3] = { reinterpret_cast<void*>(&present_detour), reinterpret_cast<void*>(&resize_detour),
                               reinterpret_cast<void*>(&execute_detour) };
    for (int i = 0; i < 3; ++i) {
        char err[256] = {};
        hook::Options opt;
        if (!g_hooks[i].create(g_targets[i], detours[i], opt, err, sizeof err)) {
            logf(3, "%s hook not prepared: %s", kHookNames[i], err);
            for (int k = 0; k < i; ++k) g_hooks[k].release_unsafe();   // never enabled: nothing can be inside
            return false;
        }
    }
    g_orig_present = reinterpret_cast<PresentFn>(g_hooks[0].original());
    g_orig_resize = reinterpret_cast<ResizeFn>(g_hooks[1].original());
    g_orig_execute = reinterpret_cast<ExecuteFn>(g_hooks[2].original());
    hooks_created = true;
    return true;
}

void disable_hooks(int upto) noexcept {
    for (int k = upto - 1; k >= 0; --k) {
        char err[256] = {};
        if (!g_hooks[k].enabled()) continue;
        if (g_hooks[k].disable(err, sizeof err)) logf(1, "%s hook removed (original bytes restored)", kHookNames[k]);
        else logf(3, "%s hook removal failed: %s (left installed, pass-through)", kHookNames[k], err);
    }
}

// ------------------------------------------------------------------ C ABI (g6_ui_api)
ImGuiWindowFlags window_flags(std::uint32_t f) noexcept {
    ImGuiWindowFlags r = ImGuiWindowFlags_None;
    if (f & G6_UI_WINDOW_NO_TITLE_BAR) r |= ImGuiWindowFlags_NoTitleBar;
    if (f & G6_UI_WINDOW_NO_RESIZE) r |= ImGuiWindowFlags_NoResize;
    if (f & G6_UI_WINDOW_NO_MOVE) r |= ImGuiWindowFlags_NoMove;
    if (f & G6_UI_WINDOW_NO_COLLAPSE) r |= ImGuiWindowFlags_NoCollapse;
    if (f & G6_UI_WINDOW_AUTO_RESIZE) r |= ImGuiWindowFlags_AlwaysAutoResize;
    if (f & G6_UI_WINDOW_NO_BACKGROUND) r |= ImGuiWindowFlags_NoBackground;
    if (f & G6_UI_WINDOW_NO_INPUTS) r |= ImGuiWindowFlags_NoInputs;
    return r;
}

bool live() noexcept { return t_in_frame && g_ctx; }

g6_status register_owned(const g6_ui_frame_desc* desc, void* owner, g6_ui_id* out) noexcept {
    if (!desc || desc->size < sizeof(g6_ui_frame_desc) || !desc->draw) return G6_E_INVALID_ARG;
    Guard lk;
    try {
        Entry e;
        e.id = g_next_id++;
        e.flags = desc->flags;
        std::snprintf(e.name, sizeof e.name, "%s", desc->name ? desc->name : "ui");
        e.draw = desc->draw;
        e.user = desc->user;
        e.owner = owner;
        g_entries.push_back(e);
        if (out) *out = e.id;
        logf(0, "ui frame '%s' registered (id %u%s)", e.name, e.id, (e.flags & G6_UI_FRAME_ALWAYS) ? ", always" : "");
    } catch (...) {
        return G6_E_INTERNAL;
    }
    return G6_OK;
}

g6_status G6_CALL api_frame_register(const g6_ui_frame_desc* desc, g6_ui_id* out) { return register_owned(desc, nullptr, out); }
g6_status G6_CALL api_frame_unregister(g6_ui_id id) {
    Guard lk;   // recursive: a callback may unregister itself
    for (auto& e : g_entries)
        if (e.id == id && e.active) { e.active = false; sweep_entries(); return G6_OK; }
    return G6_E_INVALID_ARG;
}
g6_status G6_CALL api_imgui_info(g6_ui_imgui_info* out) {
    if (!out || out->size < sizeof(g6_ui_imgui_info)) return G6_E_INVALID_ARG;
    out->version_num = IMGUI_VERSION_NUM;
    out->version = IMGUI_VERSION;
    out->sizeof_io = sizeof(ImGuiIO);
    out->sizeof_style = sizeof(ImGuiStyle);
    out->sizeof_vec2 = sizeof(ImVec2);
    out->sizeof_vec4 = sizeof(ImVec4);
    out->sizeof_draw_vert = sizeof(ImDrawVert);
    out->sizeof_draw_idx = sizeof(ImDrawIdx);
    out->context = g_ctx;
    out->alloc_fn = imgui_alloc;
    out->free_fn = imgui_free;
    out->alloc_user = nullptr;
    return g_ctx ? G6_OK : G6_E_NOT_IMPLEMENTED;
}

int32_t G6_CALL w_begin(const char* title, int32_t* open, std::uint32_t flags) {
    if (!live() || !title) return 0;
    bool o = open ? *open != 0 : true;
    const bool r = ImGui::Begin(title, open ? &o : nullptr, window_flags(flags));
    if (open) *open = o ? 1 : 0;
    return r ? 1 : 0;
}
void G6_CALL w_end(void) { if (live()) ImGui::End(); }
void G6_CALL w_next_pos(float x, float y, std::uint32_t cond) {
    if (live()) ImGui::SetNextWindowPos(ImVec2(x, y), cond == G6_UI_COND_FIRST_USE ? ImGuiCond_FirstUseEver : ImGuiCond_Always);
}
void G6_CALL w_next_size(float wd, float ht, std::uint32_t cond) {
    if (live()) ImGui::SetNextWindowSize(ImVec2(wd, ht), cond == G6_UI_COND_FIRST_USE ? ImGuiCond_FirstUseEver : ImGuiCond_Always);
}
void G6_CALL w_text(const char* t) { if (live() && t) ImGui::TextUnformatted(t); }
void G6_CALL w_text_colored(float r, float g, float b, float a, const char* t) {
    if (!live() || !t) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(r, g, b, a));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}
int32_t G6_CALL w_button(const char* label) { return live() && label && ImGui::Button(label) ? 1 : 0; }
int32_t G6_CALL w_checkbox(const char* label, int32_t* v) {
    if (!live() || !label || !v) return 0;
    bool b = *v != 0;
    const bool c = ImGui::Checkbox(label, &b);
    *v = b ? 1 : 0;
    return c ? 1 : 0;
}
int32_t G6_CALL w_slider_int(const char* label, int32_t* v, int32_t lo, int32_t hi) {
    if (!live() || !label || !v) return 0;
    int x = *v;
    const bool c = ImGui::SliderInt(label, &x, lo, hi);
    *v = x;
    return c ? 1 : 0;
}
int32_t G6_CALL w_slider_float(const char* label, float* v, float lo, float hi, const char* fmt) {
    return live() && label && v && ImGui::SliderFloat(label, v, lo, hi, fmt ? fmt : "%.3f") ? 1 : 0;
}
int32_t G6_CALL w_input_text(const char* label, char* buf, size_t cap) {
    if (!live() || !label || !buf || cap == 0) return 0;
    if (cap > INT_MAX) cap = INT_MAX;
    buf[cap - 1] = 0;
    return ImGui::InputText(label, buf, cap) ? 1 : 0;
}
int32_t G6_CALL w_combo(const char* label, int32_t* cur, const char* const* items, int32_t count) {
    if (!live() || !label || !cur || count < 0 || (count > 0 && !items)) return 0;
    for (int32_t i = 0; i < count; ++i) if (!items[i]) return 0;
    int x = *cur;
    const bool c = ImGui::Combo(label, &x, items, count);
    *cur = x;
    return c ? 1 : 0;
}
void G6_CALL w_separator(void) { if (live()) ImGui::Separator(); }
void G6_CALL w_same_line(void) { if (live()) ImGui::SameLine(); }
int32_t G6_CALL w_tree_node(const char* label) { return live() && label && ImGui::TreeNode(label) ? 1 : 0; }
void G6_CALL w_tree_pop(void) { if (live()) ImGui::TreePop(); }
int32_t G6_CALL w_begin_tab_bar(const char* id) { return live() && id && ImGui::BeginTabBar(id) ? 1 : 0; }
void G6_CALL w_end_tab_bar(void) { if (live()) ImGui::EndTabBar(); }
int32_t G6_CALL w_begin_tab_item(const char* label) { return live() && label && ImGui::BeginTabItem(label) ? 1 : 0; }
void G6_CALL w_end_tab_item(void) { if (live()) ImGui::EndTabItem(); }
int32_t G6_CALL w_color_edit(const char* label, float rgba[4], std::uint32_t flags) {
    if (!live() || !label || !rgba) return 0;
    return ImGui::ColorEdit4(label, rgba, (flags & G6_UI_COLOR_NO_ALPHA) ? ImGuiColorEditFlags_NoAlpha : 0) ? 1 : 0;
}
void G6_CALL w_plot_lines(const char* label, const float* values, int32_t count, const char* overlay, float smin, float smax,
                          float wd, float ht) {
    if (!live() || !label || (!values && count > 0) || count < 0) return;
    if (smin >= smax) smin = smax = FLT_MAX;
    ImGui::PlotLines(label, values, count, 0, overlay, smin, smax, ImVec2(wd, ht));
}

const g6_ui_api g_api = {
    sizeof(g6_ui_api), G6_UI_ABI_MAJOR, G6_UI_ABI_MINOR,
    api_frame_register, api_frame_unregister, api_imgui_info,
    w_begin, w_end, w_next_pos, w_next_size, w_text, w_text_colored, w_button, w_checkbox, w_slider_int, w_slider_float,
    w_input_text, w_combo, w_separator, w_same_line, w_tree_node, w_tree_pop, w_begin_tab_bar, w_end_tab_bar,
    w_begin_tab_item, w_end_tab_item, w_color_edit, w_plot_lines,
};

// ------------------------------------------------------------------ test API
int t_stats(g6ui_stats* o) {
    if (!o || o->size < sizeof(g6ui_stats)) return 0;
    Guard lk;
    std::memset(reinterpret_cast<char*>(o) + sizeof o->size, 0, sizeof *o - sizeof o->size);
    o->hooks_published = g_active.load() ? 1u : 0u;
    o->present_calls = s_present.load();
    o->resize_calls = s_resize.load();
    o->ecl_calls = s_ecl.load();
    o->frames_drawn = s_drawn.load();
    o->frames_skipped = s_skipped.load();
    o->callbacks_run = s_callbacks.load();
    o->callbacks_gated = s_gated.load();
    o->draw_cmds = g_last_draw_cmds;
    o->binds = g_binds;
    o->backend_inits = g_backend_inits;
    o->queue_source = g_queue_source;
    o->visible = g_visible ? 1u : 0u;
    o->wndproc_installed = g_wndproc_hwnd ? 1u : 0u;
    o->bb_width = g_b.width;
    o->bb_height = g_b.height;
    o->rtv_format = static_cast<std::uint32_t>(g_b.format);
    o->recoveries = g_recoveries;
    o->bound_swapchain = g_b.sc;
    o->bound_queue = g_b.queue;
    std::snprintf(o->last_error, sizeof o->last_error, "%s", g_last_error);
    return 1;
}
void t_request_probe(std::uint32_t x, std::uint32_t y) {
    Guard lk;
    g_probe = Probe{};
    g_probe.requested = true;
    g_probe.x = x;
    g_probe.y = y;
}
int t_probe_result(std::uint32_t* rgba) {
    Guard lk;
    if (g_probe.done) { if (rgba) *rgba = g_probe.rgba; return 1; }
    if (!g_probe.pending || !g_b.fence || !g_b.readback) return 0;
    if (g_b.fence->GetCompletedValue() < g_probe.fence) return 0;
    void* p = nullptr;
    const D3D12_RANGE r{ 0, 4 };
    if (FAILED(g_b.readback->Map(0, &r, &p)) || !p) return 0;
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    const D3D12_RANGE none{ 0, 0 };
    g_b.readback->Unmap(0, &none);
    if (g_probe.format == DXGI_FORMAT_B8G8R8A8_UNORM || g_probe.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
    g_probe.rgba = v;
    g_probe.done = true;
    g_probe.pending = false;
    if (rgba) *rgba = v;
    return 1;
}
int t_choose_queue(const void* cands, std::uint32_t n, std::uint32_t tid, std::uint64_t seq) {
    return lg::choose_queue(static_cast<const lg::QueueCand*>(cands), n, tid, seq);
}
std::uint32_t t_window_flags(std::uint32_t f) { return static_cast<std::uint32_t>(window_flags(f)); }

const g6ui_test_api g_test = { sizeof(g6ui_test_api), t_stats, t_request_probe, t_probe_result, t_choose_queue, t_window_flags };

}  // namespace
}  // namespace g6ui

using namespace g6ui;

extern "C" {

G6UI_EXPORT g6_status G6_CALL g6ui_start(const g6ui_config* cfg) {
    if (!cfg || cfg->size < sizeof(g6ui_config)) return G6_E_INVALID_ARG;
    if (g_published) return G6_OK;
    g_cfg = *cfg;
    if (!g_cfg.toggle_vk) g_cfg.toggle_vk = VK_INSERT;
    QueryPerformanceFrequency(&g_qpf);
    if (!g_targets[0] && !find_targets()) return G6_E_INTERNAL;
    if (!create_hooks()) return G6_E_INTERNAL;
    // Publish in order; any failure undoes the hooks already written (newest first).
    for (int i = 0; i < 3; ++i) {
        char err[256] = {};
        const bool forced = g_cfg.test_fail_hook == i;
        if (forced || !g_hooks[i].enable(err, sizeof err)) {
            logf(3, "%s hook NOT installed: %s; rolling back %d hook(s)", kHookNames[i], forced ? "test_fail_hook" : err, i);
            disable_hooks(i);
            return G6_E_INTERNAL;
        }
        const hook::FreezeStats& fs = g_hooks[i].last_freeze();
        logf(1, "%s hook installed at %p (%s patch, %zu bytes relocated; %u thread(s) suspended, %u attempt(s), %u ms)",
             kHookNames[i], g_targets[i], g_hooks[i].kind() == hook::PatchKind::rel32_via_relay ? "5-byte rel32" : "14-byte abs",
             g_hooks[i].stolen(), fs.threads, fs.attempts, fs.elapsed_ms);
    }
    {
        Guard lk;
        g_visible = g_cfg.start_visible != 0;
    }
    g_active.store(true, std::memory_order_release);
    g_published = true;
    logf(1, "overlay active: menu %s, toggle %s, queue source %s", g_visible ? "shown" : "hidden", vk_name(g_cfg.toggle_vk),
         g_cfg.queue_source == G6UI_QUEUE_ECL ? "ExecuteCommandLists match" : "auto");
    return G6_OK;
}

G6UI_EXPORT void G6_CALL g6ui_stop(void) {
    if (!g_published) return;
    g_active.store(false, std::memory_order_release);
    {
        Guard lk;   // a frame in progress finishes first; later ones see g_active == false
        g_want_queue.store(false);
        unbind(true, "overlay stopped");
        restore_wndproc();
        shutdown_imgui();
        g_visible = false;
        g_queue_source = 0;
    }
    clear_cands();
    disable_hooks(3);
    g_published = false;
    logf(1, "overlay stopped");
}

G6UI_EXPORT const g6_ui_api* G6_CALL g6ui_api(void) { return &g_api; }

G6UI_EXPORT g6_status G6_CALL g6ui_frame_register_owned(const g6_ui_frame_desc* desc, void* owner, g6_ui_id* out) {
    return register_owned(desc, owner, out);
}

G6UI_EXPORT void G6_CALL g6ui_unregister_owner(void* owner) {
    if (!owner) return;
    Guard lk;
    for (auto& e : g_entries)
        if (e.owner == owner && e.active) {
            e.active = false;
            logf(0, "ui frame '%s' unregistered with its plugin", e.name);
        }
    sweep_entries();
}

G6UI_EXPORT const g6ui_test_api* G6_CALL g6ui_get_test_api(void) { return &g_test; }

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        HMODULE pinned = nullptr;   // hooks and the WndProc may point into this module for the rest of the process
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    }
    return TRUE;
}
