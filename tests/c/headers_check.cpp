/* Compile-only check of the generated C++ headers (offsets/classes/patterns).
 * clang-cl /nologo /O2 /W4 /WX /std:c++17 /I..\include /c ..\tests\c\headers_check.cpp
 */
#include "gta6mod/offsets.hpp"
#include "gta6mod/patterns.hpp"

int main() {
    return static_cast<int>(g6::offsets::cObject::set_object_physics_params)
         + static_cast<int>(g6::offsets::scrNativeCallContext_GTA_VI::args)
         + static_cast<int>(g6::patterns::count);
}
