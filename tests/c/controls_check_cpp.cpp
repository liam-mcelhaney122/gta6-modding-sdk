// C++ side of controls_check: the header compiles as C++ and links to the table defined in the C TU.
#include "gta6mod/controls.h"

static_assert(G6_INPUT_JUMP == 0xD9D0E1C0u, "INPUT_JUMP key");

extern "C" int controls_check_cpp(void) {
    int n = 0;
    for (uint32_t h : g6_input_action_hashes) n += h != 0;
    return n;
}
