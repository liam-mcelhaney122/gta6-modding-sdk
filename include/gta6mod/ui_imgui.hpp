// gta6mod/ui_imgui.hpp: draw with Dear ImGui directly inside a g6 UI frame callback.
//
// The overlay (g6ui.dll) owns the ImGui context. A plugin compiles its own copy of ImGui's core
// (imgui.cpp, imgui_draw.cpp, imgui_tables.cpp, imgui_widgets.cpp from sdk/third_party/imgui, the exact
// version G6_UI_IMGUI_VERSION) with the same options as the overlay: force-include gta6mod/imgui_config.h
// (/FI"gta6mod/imgui_config.h", or /DIMGUI_USER_CONFIG="gta6mod/imgui_config.h"). No backend is needed.
// At the top of every frame callback:
//
//   if (!g6::ui::imgui_attach(ui_api)) return;   // wrong version/layout or no context: draw nothing
//   ImGui::Begin("My window"); ... ImGui::End();
//
// imgui_attach points this DLL's ImGui at the overlay's context (ImGui::SetCurrentContext) and its
// allocator (ImGui::SetAllocatorFunctions), so memory allocated here is freed by the same heap. It checks
// what IMGUI_CHECKVERSION() checks, but returns false instead of asserting.
#pragma once
#include "gta6mod/gta6mod.h"
#include "imgui.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

static_assert(IMGUI_VERSION_NUM == G6_UI_IMGUI_VERSION_NUM,
              "build this plugin with the Dear ImGui version the gta6mod overlay uses (sdk/third_party/imgui)");

namespace g6::ui {

// True when the overlay's context matches this build of ImGui and is now current. `why` (optional) says why not.
inline bool imgui_attach(const g6_ui_api* ui, char* why = nullptr, std::size_t cap = 0) {
    auto fail = [&](const char* m) {
        if (why && cap) std::snprintf(why, cap, "%s", m);
        return false;
    };
    if (!ui) return fail("no UI table");
    g6_ui_imgui_info info{};
    info.size = sizeof info;
    if (ui->imgui_info(&info) != G6_OK || !info.context) return fail("the overlay has no ImGui context (overlay off or not bound yet)");
    if (info.version_num != IMGUI_VERSION_NUM || !info.version || std::strcmp(info.version, IMGUI_VERSION) != 0)
        return fail("Dear ImGui version differs from the overlay's");
    if (info.sizeof_io != sizeof(ImGuiIO) || info.sizeof_style != sizeof(ImGuiStyle) || info.sizeof_vec2 != sizeof(ImVec2) ||
        info.sizeof_vec4 != sizeof(ImVec4) || info.sizeof_draw_vert != sizeof(ImDrawVert) || info.sizeof_draw_idx != sizeof(ImDrawIdx))
        return fail("Dear ImGui data layout differs from the overlay's (imconfig / IMGUI_USER_CONFIG options)");
    if (!info.alloc_fn || !info.free_fn) return fail("the overlay gave no allocator");
    ImGui::SetAllocatorFunctions(info.alloc_fn, info.free_fn, info.alloc_user);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(info.context));
    return true;
}

}  // namespace g6::ui
