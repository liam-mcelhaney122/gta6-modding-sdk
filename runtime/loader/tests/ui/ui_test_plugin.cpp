// ui_test_plugin.dll: draws with its OWN copy of Dear ImGui on the overlay's context (gta6mod/ui_imgui.hpp),
// the way a C++ plugin would. Driven by ui_test.exe through the exports below.
#include "gta6mod/gta6mod.h"
#include "gta6mod/ui_imgui.hpp"

#include <windows.h>

#include <cstdint>

namespace {

const g6_ui_api* g_ui = nullptr;
g6_ui_id g_menu = 0, g_hud = 0;
std::uint32_t g_attach_ok = 0, g_attach_fail = 0, g_clicks = 0, g_menu_frames = 0, g_hud_frames = 0;
float g_btn_x = 0, g_btn_y = 0;
float g_display_w = 0, g_display_h = 0;
char g_why[160] = {};

void G6_CALL menu_frame(const g6_ui_frame* f, void*) {
    if (!g6::ui::imgui_attach(g_ui, g_why, sizeof g_why)) { ++g_attach_fail; return; }
    ++g_attach_ok;
    ++g_menu_frames;
    g_display_w = f->display_w;
    g_display_h = f->display_h;
    ImGui::SetNextWindowPos(ImVec2(20, 60), ImGuiCond_Always);
    ImGui::Begin("ui_test imgui", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Click me")) ++g_clicks;
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    g_btn_x = (lo.x + hi.x) * 0.5f;
    g_btn_y = (lo.y + hi.y) * 0.5f;
    ImGui::Text("clicks: %u", g_clicks);
    ImGui::End();
    // Probe targets drawn on top of everything: red at (100..140, 200..240) and at the bottom-right corner.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(100, 200), ImVec2(140, 240), IM_COL32(255, 0, 0, 255));
    dl->AddRectFilled(ImVec2(f->display_w - 40, f->display_h - 40), ImVec2(f->display_w - 10, f->display_h - 10), IM_COL32(255, 0, 0, 255));
}

void G6_CALL hud_frame(const g6_ui_frame*, void*) {
    if (!g6::ui::imgui_attach(g_ui)) return;
    ++g_hud_frames;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(200, 300), ImVec2(240, 340), IM_COL32(0, 255, 0, 255));
}

}  // namespace

extern "C" {

struct ui_test_plugin_stats {
    std::uint32_t attach_ok, attach_fail, clicks, menu_frames, hud_frames;
    float btn_x, btn_y, display_w, display_h;
    char why[160];
};

__declspec(dllexport) int ui_test_plugin_install(const g6_ui_api* ui, int with_hud) {
    g_ui = ui;
    const g6_ui_frame_desc m{ sizeof(g6_ui_frame_desc), 0, "imgui-direct menu", menu_frame, nullptr };
    if (ui->frame_register(&m, &g_menu) != G6_OK) return 0;
    if (with_hud) {
        const g6_ui_frame_desc h{ sizeof(g6_ui_frame_desc), G6_UI_FRAME_ALWAYS, "imgui-direct hud", hud_frame, nullptr };
        if (ui->frame_register(&h, &g_hud) != G6_OK) return 0;
    }
    return 1;
}

__declspec(dllexport) void ui_test_plugin_uninstall(void) {
    if (g_ui && g_menu) g_ui->frame_unregister(g_menu);
    if (g_ui && g_hud) g_ui->frame_unregister(g_hud);
    g_menu = g_hud = 0;
}

__declspec(dllexport) void ui_test_plugin_stats(ui_test_plugin_stats* o) {
    o->attach_ok = g_attach_ok;
    o->attach_fail = g_attach_fail;
    o->clicks = g_clicks;
    o->menu_frames = g_menu_frames;
    o->hud_frames = g_hud_frames;
    o->btn_x = g_btn_x;
    o->btn_y = g_btn_y;
    o->display_w = g_display_w;
    o->display_h = g_display_h;
    std::memcpy(o->why, g_why, sizeof g_why);
}

}  // extern "C"
