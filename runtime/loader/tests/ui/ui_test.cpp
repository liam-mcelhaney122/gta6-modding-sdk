// ui_test.exe: offline test of g6ui.dll on this process's own D3D12 device and swap chain (hidden window).
//   ui_test.exe <g6ui.dll> <ui_test_plugin.dll> [--warp]
// The overlay is started exactly as the runtime starts it; this program plays the host: it clears the back
// buffer to blue, submits on its queue and calls Present. Checks: hook install/rollback/removal (bytes at the
// three entry points), queue matching (ExecuteCommandLists and the swap chain's own queue), ImGui output in the
// back buffer (pixel readback), input toggle/capture/click through the subclassed WndProc, HUD frames while
// hidden, focus loss, ResizeBuffers (size and format), swap chain re-creation, the gate, ImGui error recovery
// and SEH containment of a faulting callback, and a plugin drawing with its own ImGui copy on the shared context.
#include "g6ui.h"
#include "ui_logic.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

struct ui_test_plugin_stats {
    std::uint32_t attach_ok, attach_fail, clicks, menu_frames, hud_frames;
    float btn_x, btn_y, display_w, display_h;
    char why[160];
};

using StartFn = g6_status(G6_CALL*)(const g6ui_config*);
using StopFn = void(G6_CALL*)(void);
using ApiFn = const g6_ui_api*(G6_CALL*)(void);
using TestFn = const g6ui_test_api*(G6_CALL*)(void);
using OwnedFn = g6_status(G6_CALL*)(const g6_ui_frame_desc*, void*, g6_ui_id*);
using UnownFn = void(G6_CALL*)(void*);
using PlugInstall = int (*)(const g6_ui_api*, int);
using PlugUninstall = void (*)(void);
using PlugStats = void (*)(ui_test_plugin_stats*);

StartFn g6ui_start_ = nullptr;
StopFn g6ui_stop_ = nullptr;
const g6_ui_api* g_ui = nullptr;
const g6ui_test_api* g_t = nullptr;
OwnedFn g6ui_owned_ = nullptr;
UnownFn g6ui_unown_ = nullptr;
PlugStats plug_stats = nullptr;

// ------------------------------------------------------------------ host window
int g_host_mouse = 0, g_host_keys = 0;
LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) ++g_host_mouse;
    if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR) ++g_host_keys;
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ host device
struct Host {
    HWND hwnd = nullptr;
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* sc = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    UINT w = 640, h = 480;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool warp = false;
} H;

void wait_gpu() {
    H.queue->Signal(H.fence, ++H.fv);
    if (H.fence->GetCompletedValue() < H.fv) {
        H.fence->SetEventOnCompletion(H.fv, H.ev);
        WaitForSingleObject(H.ev, 5000);
    }
}

bool make_swapchain() {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = H.w;
    d.Height = H.h;
    d.Format = H.fmt;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(H.factory->CreateSwapChainForHwnd(H.queue, H.hwnd, &d, nullptr, nullptr, &sc1))) return false;
    const bool ok = SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&H.sc)));
    sc1->Release();
    return ok;
}

bool make_host(bool warp) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = host_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"g6ui_test_host";
    RegisterClassExW(&wc);
    RECT r{ 0, 0, 640, 480 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    H.hwnd = CreateWindowExW(0, wc.lpszClassName, L"g6ui test host", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);   // never shown
    if (!H.hwnd || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&H.factory)))) return false;
    HRESULT hr = E_FAIL;
    if (!warp) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&H.device));
    if (FAILED(hr)) {
        IDXGIAdapter* a = nullptr;
        if (FAILED(H.factory->EnumWarpAdapter(IID_PPV_ARGS(&a)))) return false;
        hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&H.device));
        a->Release();
        H.warp = true;
    }
    if (FAILED(hr)) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(H.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&H.queue)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(H.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&H.rtv)))) return false;
    if (FAILED(H.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&H.alloc)))) return false;
    if (FAILED(H.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, H.alloc, nullptr, IID_PPV_ARGS(&H.list)))) return false;
    H.list->Close();
    if (FAILED(H.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&H.fence)))) return false;
    H.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return make_swapchain();
}

// One host frame: clear the back buffer to blue, submit, Present, wait for the GPU.
void frame() {
    ID3D12Resource* bb = nullptr;
    H.sc->GetBuffer(H.sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    H.alloc->Reset();
    H.list->Reset(H.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    H.list->ResourceBarrier(1, &b);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = H.rtv->GetCPUDescriptorHandleForHeapStart();
    H.device->CreateRenderTargetView(bb, nullptr, rtv);
    const float blue[4] = { 0.f, 0.f, 1.f, 1.f };
    H.list->ClearRenderTargetView(rtv, blue, 0, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    H.list->ResourceBarrier(1, &b);
    H.list->Close();
    ID3D12CommandList* l[] = { H.list };
    H.queue->ExecuteCommandLists(1, l);
    bb->Release();
    H.sc->Present(0, 0);
    wait_gpu();
}
void frames(int n) { for (int i = 0; i < n; ++i) frame(); }

g6ui_stats stats() {
    g6ui_stats s{};
    s.size = sizeof s;
    g_t->stats(&s);
    return s;
}
ui_test_plugin_stats pstats() {
    ui_test_plugin_stats s{};
    plug_stats(&s);
    return s;
}

// Pixel (x, y) of the back buffer right after the overlay drew (R | G<<8 | B<<16 | A<<24), or 0xDEADBEEF.
std::uint32_t probe(std::uint32_t x, std::uint32_t y) {
    g_t->request_probe(x, y);
    std::uint32_t v = 0;
    for (int i = 0; i < 10; ++i) {
        frame();
        if (g_t->probe_result(&v)) return v;
    }
    return 0xDEADBEEFu;
}
bool near_rgb(std::uint32_t v, int r, int g, int b) {
    auto d = [](int a, int c) { return a > c ? a - c : c - a; };
    return d(static_cast<int>(v & 0xFF), r) <= 2 && d(static_cast<int>((v >> 8) & 0xFF), g) <= 2 && d(static_cast<int>((v >> 16) & 0xFF), b) <= 2;
}
constexpr std::uint32_t kRed = 0, kBlue = 1, kGreen = 2;
bool is(std::uint32_t v, std::uint32_t c) {
    return c == kRed ? near_rgb(v, 255, 0, 0) : c == kBlue ? near_rgb(v, 0, 0, 255) : near_rgb(v, 0, 255, 0);
}

// ------------------------------------------------------------------ entry-point bytes
void* g_tgt[3] = {};
std::uint8_t g_orig_bytes[3][16];
void read_targets() {
    void** svt = *reinterpret_cast<void***>(H.sc);
    void** qvt = *reinterpret_cast<void***>(H.queue);
    g_tgt[0] = svt[8];
    g_tgt[1] = svt[13];
    g_tgt[2] = qvt[10];
}
void save_bytes() { for (int i = 0; i < 3; ++i) std::memcpy(g_orig_bytes[i], g_tgt[i], 16); }
int bytes_original() {
    int n = 0;
    for (int i = 0; i < 3; ++i) n += std::memcmp(g_orig_bytes[i], g_tgt[i], 16) == 0 ? 1 : 0;
    return n;
}

// ------------------------------------------------------------------ C ABI callbacks
int g_cabi_frames = 0;
int32_t g_god = 0, g_slider_i = 3, g_combo = 1;
float g_slider_f = 0.5f, g_color[4] = { 1, 0.5f, 0.25f, 1 };
char g_name[32] = "Niko";
const float g_plot[5] = { 1, 3, 2, 5, 4 };
void G6_CALL cabi_frame(const g6_ui_frame* f, void*) {
    ++g_cabi_frames;
    g_ui->set_next_window_pos(300, 20, G6_UI_COND_ALWAYS);
    int32_t open = 1;
    if (g_ui->begin("C ABI widgets", &open, G6_UI_WINDOW_AUTO_RESIZE | G6_UI_WINDOW_NO_COLLAPSE)) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "frame %llu %.0fx%.0f", static_cast<unsigned long long>(f->frame), f->display_w, f->display_h);
        g_ui->text(buf);
        g_ui->text_colored(1, 1, 0, 1, "yellow");
        g_ui->checkbox("God mode", &g_god);
        g_ui->same_line();
        g_ui->button("Heal");
        g_ui->slider_int("Wanted", &g_slider_i, 0, 5);
        g_ui->slider_float("Speed", &g_slider_f, 0.f, 1.f, nullptr);
        g_ui->input_text("Name", g_name, sizeof g_name);
        const char* items[] = { "Clear", "Rain", "Fog" };
        g_ui->combo("Weather", &g_combo, items, 3);
        g_ui->separator();
        if (g_ui->tree_node("Advanced")) { g_ui->text("inside"); g_ui->tree_pop(); }
        if (g_ui->begin_tab_bar("tabs")) {
            if (g_ui->begin_tab_item("One")) { g_ui->text("tab one"); g_ui->end_tab_item(); }
            if (g_ui->begin_tab_item("Two")) { g_ui->text("tab two"); g_ui->end_tab_item(); }
            g_ui->end_tab_bar();
        }
        g_ui->color_edit("Tint", g_color, 0);
        g_ui->plot_lines("Plot", g_plot, 5, "overlay", 0, 0, 0, 0);
    }
    g_ui->end();
}
int g_unbalanced_frames = 0;
void G6_CALL unbalanced_frame(const g6_ui_frame*, void*) {
    ++g_unbalanced_frames;
    g_ui->begin("left open", nullptr, 0);   // no end(): the overlay must repair it
    g_ui->tree_node("never popped");
}
int g_fault_frames = 0;
void G6_CALL faulting_frame(const g6_ui_frame*, void*) {
    ++g_fault_frames;
    volatile int* p = nullptr;
    *p = 1;   // access violation: contained, callback unregistered
}

// ------------------------------------------------------------------ gate / log
int g_gate_open = 1;
int test_gate(char* why, size_t cap) {
    if (!g_gate_open) std::snprintf(why, cap, "test gate closed");
    return g_gate_open;
}
int g_log_errors = 0;   // reported at the end (forced failures and the faulting callback log errors on purpose)
void test_log(int level, const char* m) {
    if (level >= 3) ++g_log_errors;
    std::printf("  [g6ui %d] %s\n", level, m);
}

void pure_logic_checks() {
    namespace lg = g6ui::logic;
    lg::QueueCand c[3] = { { &c[0], 1, 7, 5 }, { &c[1], 1, 8, 5 }, { &c[2], 0, 7, 5 } };
    CHECK(lg::choose_queue(c, 1, 7, 5) == 0, "single fresh candidate");
    CHECK(lg::choose_queue(c, 1, 7, 6) == lg::kQueueNone, "stale candidate is not chosen");
    CHECK(lg::choose_queue(c, 2, 8, 5) == 1, "two fresh: the one on the presenting thread");
    CHECK(lg::choose_queue(c, 2, 9, 5) == lg::kQueueAmbiguous, "two fresh, neither on the presenting thread: ambiguous");
    CHECK(lg::choose_queue(c + 2, 1, 7, 5) == lg::kQueueNone, "other device is never chosen");
    CHECK(lg::choose_queue(c, 3, 7, 5) == 0, "other-device queue does not make it ambiguous");
    CHECK(g_t->choose_queue(c, 2, 8, 5) == 1, "test API reaches the same function");
    bool vis = false;
    lg::Action a = lg::decide(vis, lg::classify(WM_KEYDOWN, VK_INSERT, 0, VK_INSERT));
    CHECK(vis && a.toggled && a.swallow && !a.release, "Insert shows the menu and is swallowed");
    a = lg::decide(vis, lg::classify(WM_KEYDOWN, VK_INSERT, 1ll << 30, VK_INSERT));
    CHECK(vis && !a.toggled && a.swallow, "auto-repeat does not toggle");
    a = lg::decide(vis, lg::classify(WM_MOUSEMOVE, 0, 0, VK_INSERT));
    CHECK(a.to_imgui && a.swallow, "mouse goes to ImGui only while shown");
    a = lg::decide(vis, lg::classify(WM_SYSKEYDOWN, VK_F4, 0, VK_INSERT));
    CHECK(a.to_imgui && !a.swallow, "system keys reach the host too");
    a = lg::decide(vis, lg::classify(WM_INPUT, 0, 0, VK_INSERT));
    CHECK(a.swallow && a.def_proc, "raw input swallowed with DefWindowProc cleanup");
    a = lg::decide(vis, lg::classify(WM_ACTIVATEAPP, 0, 0, VK_INSERT));
    CHECK(!vis && a.toggled && a.release && !a.swallow, "focus loss hides, releases, and is passed on");
    a = lg::decide(vis, lg::classify(WM_MOUSEMOVE, 0, 0, VK_INSERT));
    CHECK(!a.to_imgui && !a.swallow, "hidden: mouse goes to the host");
    CHECK(lg::client_to_bb(320.f, 640, 800) == 400.f && lg::client_to_bb(5.f, 0, 800) == 5.f, "client -> back-buffer scale");
    CHECK(g_t->translate_window_flags(G6_UI_WINDOW_AUTO_RESIZE | G6_UI_WINDOW_NO_TITLE_BAR) == ((1u << 6) | (1u << 0)),
          "window flags map to ImGuiWindowFlags_AlwaysAutoResize | NoTitleBar");
}

LPARAM key_lparam(bool up) { return up ? static_cast<LPARAM>(0xC0000001) : 1; }
void press_insert() {
    SendMessageW(H.hwnd, WM_KEYDOWN, VK_INSERT, key_lparam(false));
    SendMessageW(H.hwnd, WM_KEYUP, VK_INSERT, key_lparam(true));
}
void click_client(float x, float y) {
    const LPARAM pos = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
    SendMessageW(H.hwnd, WM_MOUSEMOVE, 0, pos);
    frames(2);
    SendMessageW(H.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    frames(2);
    SendMessageW(H.hwnd, WM_LBUTTONUP, 0, pos);
    frames(2);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf("usage: ui_test <g6ui.dll> <ui_test_plugin.dll> [--warp]\n"); return 2; }
    const bool want_warp = argc > 3 && std::strcmp(argv[3], "--warp") == 0;
    HMODULE ui = LoadLibraryA(argv[1]);
    HMODULE plug = LoadLibraryA(argv[2]);
    if (!ui || !plug) { std::printf("cannot load %s / %s (%lu)\n", argv[1], argv[2], GetLastError()); return 2; }
    g6ui_start_ = reinterpret_cast<StartFn>(GetProcAddress(ui, "g6ui_start"));
    g6ui_stop_ = reinterpret_cast<StopFn>(GetProcAddress(ui, "g6ui_stop"));
    g_ui = reinterpret_cast<ApiFn>(GetProcAddress(ui, "g6ui_api"))();
    g_t = reinterpret_cast<TestFn>(GetProcAddress(ui, "g6ui_get_test_api"))();
    g6ui_owned_ = reinterpret_cast<OwnedFn>(GetProcAddress(ui, "g6ui_frame_register_owned"));
    g6ui_unown_ = reinterpret_cast<UnownFn>(GetProcAddress(ui, "g6ui_unregister_owner"));
    auto plug_install = reinterpret_cast<PlugInstall>(GetProcAddress(plug, "ui_test_plugin_install"));
    auto plug_uninstall = reinterpret_cast<PlugUninstall>(GetProcAddress(plug, "ui_test_plugin_uninstall"));
    plug_stats = reinterpret_cast<PlugStats>(GetProcAddress(plug, "ui_test_plugin_stats"));
    if (!g6ui_start_ || !g6ui_stop_ || !g_ui || !g_t || !g6ui_owned_ || !g6ui_unown_ || !plug_install || !plug_uninstall || !plug_stats) {
        std::printf("missing exports\n");
        return 2;
    }
    CHECK(g_ui->size == sizeof(g6_ui_api) && g_ui->abi_major == G6_UI_ABI_MAJOR, "g6_ui_api table size/ABI");
    pure_logic_checks();
    if (!make_host(want_warp)) { std::printf("cannot create the D3D12 host (device, queue, swap chain)\n"); return 2; }
    std::printf("host device: %s\n", H.warp ? "WARP" : "hardware adapter");
    read_targets();
    save_bytes();

    g6ui_config cfg{};
    cfg.size = sizeof cfg;
    cfg.toggle_vk = VK_INSERT;
    cfg.queue_source = G6UI_QUEUE_ECL;
    cfg.log = test_log;
    cfg.gate = test_gate;

    // 1. rollback: hook #1 or #2 "fails" after the earlier ones were written; all bytes must come back.
    for (int k = 1; k <= 2; ++k) {
        cfg.test_fail_hook = k;
        CHECK(g6ui_start_(&cfg) == G6_E_INTERNAL, "forced failure of hook %d is reported", k);
        CHECK(bytes_original() == 3, "rollback after hook %d restored all three entry points (%d/3)", k, bytes_original());
        CHECK(stats().hooks_published == 0, "nothing published after a failed start");
    }
    cfg.test_fail_hook = -1;

    // 2. start, ExecuteCommandLists queue matching, hidden menu.
    CHECK(g6ui_start_(&cfg) == G6_OK, "start (ECL queue matching)");
    CHECK(bytes_original() == 0, "all three entry points patched (%d unchanged)", bytes_original());
    CHECK(stats().hooks_published == 1, "hooks published");
    const int32_t outside = g_ui->button("outside a frame");
    CHECK(outside == 0, "widgets outside a frame callback do nothing");
    g6_ui_id cabi_id = 0;
    const g6_ui_frame_desc cd{ sizeof(g6_ui_frame_desc), 0, "c-abi", cabi_frame, nullptr };
    CHECK(g6ui_owned_(&cd, &H, &cabi_id) == G6_OK && cabi_id != 0, "register C ABI frame (owned)");
    CHECK(plug_install(g_ui, 0) == 1, "plugin registers its ImGui frame");
    frames(4);
    g6ui_stats s = stats();
    CHECK(s.present_calls >= 4, "Present detour ran (%llu)", static_cast<unsigned long long>(s.present_calls));
    CHECK(s.ecl_calls >= 4, "ExecuteCommandLists detour ran (%llu)", static_cast<unsigned long long>(s.ecl_calls));
    CHECK(s.binds == 1 && s.queue_source == 2 && s.bound_queue == H.queue && s.bound_swapchain == H.sc,
          "bound to the test swap chain via ExecuteCommandLists (binds %u, source %u)", s.binds, s.queue_source);
    CHECK(s.wndproc_installed == 1 && GetWindowLongPtrW(H.hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&host_proc), "WndProc subclassed");
    CHECK(s.frames_drawn == 0 && g_cabi_frames == 0, "hidden menu, no HUD: nothing drawn, no callback");
    CHECK(s.bb_width == 640 && s.bb_height == 480 && s.rtv_format == DXGI_FORMAT_R8G8B8A8_UNORM, "bound size/format");

    // 3. show (Insert through the window procedure), draw, read pixels back.
    const int keys_before = g_host_keys;
    press_insert();
    CHECK(stats().visible == 1, "Insert shows the menu");
    CHECK(g_host_keys == keys_before, "toggle key not passed to the host");
    frames(3);
    s = stats();
    CHECK(s.frames_drawn >= 3 && g_cabi_frames >= 3 && pstats().menu_frames >= 3, "frames drawn %llu, C ABI frames %d, plugin frames %u",
          static_cast<unsigned long long>(s.frames_drawn), g_cabi_frames, pstats().menu_frames);
    CHECK(s.draw_cmds > 0, "ImGui produced draw commands (%llu)", static_cast<unsigned long long>(s.draw_cmds));
    CHECK(pstats().attach_ok > 0 && pstats().attach_fail == 0, "plugin attached its ImGui copy to the overlay context (%s)", pstats().why);
    g6_ui_imgui_info info{};
    info.size = sizeof info;
    CHECK(g_ui->imgui_info(&info) == G6_OK && info.context && info.version_num == G6_UI_IMGUI_VERSION_NUM &&
              std::strcmp(info.version, G6_UI_IMGUI_VERSION) == 0, "imgui_info: context and pinned version");
    std::uint32_t px = probe(120, 220);
    CHECK(is(px, kRed), "overlay pixel at (120,220) is red: 0x%08X", px);
    px = probe(630, 470);
    CHECK(is(px, kBlue), "untouched pixel at (630,470) keeps the host's blue: 0x%08X", px);

    // 4. input: a click on the plugin's button through WM_* messages; the host must not see the mouse.
    const int mouse_before = g_host_mouse;
    const ui_test_plugin_stats p0 = pstats();
    click_client(p0.btn_x, p0.btn_y);
    CHECK(pstats().clicks == 1, "ImGui button clicked through the subclassed WndProc (%u clicks at %.0f,%.0f)", pstats().clicks, p0.btn_x, p0.btn_y);
    CHECK(g_host_mouse == mouse_before, "mouse captured while the menu is shown (host saw %d)", g_host_mouse - mouse_before);

    // 5. hide: no callbacks, host gets input back, overlay pixels gone; HUD (ALWAYS) still drawn.
    press_insert();
    CHECK(stats().visible == 0, "Insert hides the menu");
    const int cabi_hidden = g_cabi_frames;
    frames(3);
    CHECK(g_cabi_frames == cabi_hidden, "no menu callbacks while hidden");
    SendMessageW(H.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    CHECK(g_host_mouse == mouse_before + 1, "host gets the mouse while hidden");
    px = probe(120, 220);
    CHECK(is(px, kBlue), "hidden: no overlay pixels (0x%08X)", px);
    plug_uninstall();
    CHECK(plug_install(g_ui, 1) == 1, "plugin registers menu + HUD (ALWAYS)");
    px = probe(220, 320);
    CHECK(is(px, kGreen) && pstats().hud_frames > 0, "HUD drawn while the menu is hidden (0x%08X)", px);

    // 6. focus loss hides and releases.
    press_insert();
    CHECK(stats().visible == 1, "shown again");
    SendMessageW(H.hwnd, WM_KILLFOCUS, 0, 0);
    CHECK(stats().visible == 0, "focus loss hides the menu");
    press_insert();

    // 7. gate closed: mod callbacks skipped, status only.
    g_gate_open = 0;
    const int cabi_gate = g_cabi_frames;
    frames(3);
    CHECK(g_cabi_frames == cabi_gate && stats().callbacks_gated >= 3, "gate closed: callbacks skipped (%llu gated frames)",
          static_cast<unsigned long long>(stats().callbacks_gated));
    g_gate_open = 1;
    frames(1);
    CHECK(g_cabi_frames > cabi_gate, "gate open again: callbacks run");

    // 8. ImGui error recovery and SEH containment.
    g6_ui_id ub = 0, fault = 0;
    const g6_ui_frame_desc ud{ sizeof(g6_ui_frame_desc), 0, "unbalanced", unbalanced_frame, nullptr };
    const g6_ui_frame_desc fd{ sizeof(g6_ui_frame_desc), 0, "faulting", faulting_frame, nullptr };
    CHECK(g_ui->frame_register(&ud, &ub) == G6_OK && g_ui->frame_register(&fd, &fault) == G6_OK, "register bad callbacks");
    const std::uint32_t rec0 = stats().recoveries;
    frames(3);
    CHECK(g_unbalanced_frames >= 3 && stats().recoveries >= rec0 + 3, "unbalanced callback repaired every frame (%u)", stats().recoveries - rec0);
    CHECK(g_fault_frames == 1, "faulting callback ran once and was unregistered (%d)", g_fault_frames);
    CHECK(g_ui->frame_unregister(fault) == G6_E_INVALID_ARG, "faulting callback is gone");
    CHECK(g_ui->frame_unregister(ub) == G6_OK, "unregister the unbalanced callback");
    px = probe(120, 220);
    CHECK(is(px, kRed), "still drawing after recovery (0x%08X)", px);

    // 9. ResizeBuffers: size (client stays 640x480, back buffer 800x600: mouse scaled), then format.
    wait_gpu();
    HRESULT hr = H.sc->ResizeBuffers(0, 800, 600, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr) && stats().resize_calls >= 1, "ResizeBuffers 800x600 through the hook (0x%08lX)", hr);
    frames(2);
    s = stats();
    CHECK(s.bb_width == 800 && s.bb_height == 600 && pstats().display_w == 800.f, "overlay follows the new size (%ux%u)", s.bb_width, s.bb_height);
    px = probe(800 - 25, 600 - 25);
    CHECK(is(px, kRed), "bottom-right probe at the new size is red (0x%08X)", px);
    const ui_test_plugin_stats p1 = pstats();
    click_client(p1.btn_x * 640.f / 800.f, p1.btn_y * 480.f / 600.f);
    CHECK(pstats().clicks == 2, "click lands after client->back-buffer scaling (%u)", pstats().clicks);
    const std::uint32_t inits = stats().backend_inits;
    wait_gpu();
    hr = H.sc->ResizeBuffers(0, 800, 600, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers to BGRA (0x%08lX)", hr);
    frames(2);
    CHECK(stats().backend_inits == inits + 1 && stats().rtv_format == DXGI_FORMAT_B8G8R8A8_UNORM, "format change re-initialises the ImGui DX12 backend");
    px = probe(120, 220);
    CHECK(is(px, kRed), "BGRA back buffer: overlay pixel red (0x%08X)", px);

    // 10. swap chain re-created on the same window: rebinds at once.
    wait_gpu();
    const std::uint32_t binds = stats().binds;
    const void* old_sc = H.sc;
    H.sc->Release();
    H.sc = nullptr;
    H.w = 640;
    H.h = 480;
    H.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(make_swapchain(), "new swap chain");
    frames(3);
    s = stats();
    // DXGI may hand out the same address again; then the binding (same queue, device, window) simply stays valid.
    CHECK(s.bound_swapchain == H.sc && (s.binds == binds + 1 || old_sc == static_cast<const void*>(H.sc)),
          "bound to the new swap chain (binds %u -> %u%s)", binds, s.binds, old_sc == static_cast<const void*>(H.sc) ? ", same address" : "");
    px = probe(120, 220);
    CHECK(is(px, kRed), "new swap chain: overlay pixel red (0x%08X)", px);

    // 11. owner unregistration (what the runtime does when a plugin unloads).
    const int cabi_own = g_cabi_frames;
    g6ui_unown_(&H);
    frames(2);
    CHECK(g_cabi_frames == cabi_own, "owner's frames unregistered");

    // 12. stop: bytes and WndProc restored, no more detour calls, host input back.
    g6ui_stop_();
    CHECK(bytes_original() == 3, "stop restored all three entry points (%d/3)", bytes_original());
    CHECK(GetWindowLongPtrW(H.hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&host_proc), "WndProc restored");
    const std::uint64_t pc = stats().present_calls;
    frames(3);
    CHECK(stats().present_calls == pc && stats().hooks_published == 0, "no detour calls after stop");
    const int keys2 = g_host_keys;
    press_insert();
    CHECK(g_host_keys == keys2 + 2 && stats().visible == 0, "Insert reaches the host after stop");

    // 13. restart with queue_source=auto (the swap chain's own queue when DXGI hands it out).
    cfg.queue_source = G6UI_QUEUE_AUTO;
    cfg.start_visible = 1;
    CHECK(g6ui_start_(&cfg) == G6_OK, "restart (auto queue source)");
    frames(3);
    s = stats();
    std::printf("auto queue source: %s\n", s.queue_source == 1 ? "IDXGISwapChain::GetDevice(ID3D12CommandQueue)" :
                                         s.queue_source == 2 ? "ExecuteCommandLists match (GetDevice gave no queue)" : "none");
    CHECK(s.queue_source != 0 && s.bound_queue == H.queue, "auto: bound to the host queue");
    px = probe(120, 220);
    CHECK(is(px, kRed), "auto: overlay pixel red (0x%08X)", px);
    plug_uninstall();
    g6ui_stop_();
    CHECK(bytes_original() == 3, "second stop restored the entry points");

    std::printf("ui_test (%s): %d passed, %d failed (overlay logged %d error line(s); the forced hook failures and the faulting "
                "callback log errors on purpose)\n", H.warp ? "WARP" : "hardware", g_pass, g_fail, g_log_errors);
    return g_fail ? 1 : 0;
}
