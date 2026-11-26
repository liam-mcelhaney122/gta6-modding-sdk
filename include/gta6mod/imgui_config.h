// Dear ImGui build options shared by the overlay (g6ui.dll) and every plugin that compiles ImGui itself.
// Use it through the compiler: /DIMGUI_USER_CONFIG="<gta6mod/imgui_config.h>" (or "gta6mod/imgui_config.h"
// with sdk/include on the include path). Nothing here changes ImGui's data layout; the layout checks in
// gta6mod/ui_imgui.hpp still compare sizes.
#pragma once

// The IME hooks in imgui.cpp call ImmSetCompositionWindow on the game window from the render thread,
// which may wait for the window's thread while it waits for the overlay lock. The overlay never
// needs IME composition, so the default Win32 IME functions (and the imm32 import) are compiled out.
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS

// IM_ASSERT: never assert()/abort() inside the game. A failed check calls g6_imgui_assert_hook when set
// (the overlay logs it), otherwise it is ignored like an NDEBUG build. ImGui's recoverable user errors
// (unbalanced Begin/End, ...) go through io.ConfigErrorRecovery instead, which the overlay enables.
#ifdef __cplusplus
inline void (*g6_imgui_assert_hook)(const char* expr, const char* file, int line) = nullptr;
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : (g6_imgui_assert_hook ? g6_imgui_assert_hook(#_EXPR, __FILE__, __LINE__) : (void)0))
#endif
