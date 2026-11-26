/* Compile-only check of the generated C++ headers (offsets/classes/patterns/natives).
 * clang-cl /nologo /O2 /W4 /WX /std:c++17 /I..\include /c ..\tests\c\headers_check.cpp
 */
#include "gta6mod/natives.hpp"
#include "gta6mod/offsets.hpp"
#include "gta6mod/patterns.hpp"

/* Vector3* out-params receive one vmovaps store: 16 bytes, 16-byte aligned. */
static_assert(sizeof(g6_vec3_packed) == 16 && alignof(g6_vec3_packed) == 16, "g6_vec3_packed layout");

/* UI layer (gta6mod.h, API 0.3), x64: the sizes sdk/python/gta6mod/runtime/cabi.py mirrors (sdk/tests/python/test_ui.py). */
#include "gta6mod/ui.hpp"
static_assert(sizeof(g6_ui_frame) == 32 && sizeof(g6_ui_frame_desc) == 32, "g6_ui_frame / g6_ui_frame_desc layout");
static_assert(sizeof(g6_ui_api) == 8 + 25 * sizeof(void*), "g6_ui_api: header + 25 entry points");
static_assert(sizeof(g6_host_api) == 8 + 12 * sizeof(void*), "g6_host_api: header + 12 entry points (0.3 adds ui)");

int main() {
    return static_cast<int>(g6::offsets::cObject::set_object_physics_params)
         + static_cast<int>(g6::offsets::scrNativeCallContext_GTA_VI::args)
         + static_cast<int>(g6::patterns::count);
}
