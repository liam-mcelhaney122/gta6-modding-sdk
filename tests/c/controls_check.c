/* controls.h: constants equal g6_joaat of their names, and the index table holds every named action.
 * Compiled as C (this file) and as C++ (controls_check_cpp.cpp, which provides no table: linkage check). */
#define G6_CONTROLS_WANT_TABLE
#include "gta6mod/controls.h"
#include "gta6mod/gta6mod.h"

#include <stdio.h>

static int find(uint32_t key) {
    for (uint32_t i = 0; i < G6_INPUT_ACTION_COUNT; ++i)
        if (g6_input_action_hashes[i] == key) return (int)i;
    return -1;
}

int controls_check_cpp(void); /* controls_check_cpp.cpp */

int main(void) {
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s\n", #c); ++fails; } } while (0)
    CHECK(G6_INPUT_JUMP == 0xD9D0E1C0u);                      /* the RDR2 key of INPUT_JUMP */
    CHECK(g6_joaat("INPUT_JUMP") == G6_INPUT_JUMP);
    CHECK(g6_joaat("INPUT_CONTEXT") == G6_INPUT_CONTEXT);
    CHECK(g6_joaat("INPUT_FRONTEND_ACCEPT") == G6_INPUT_FRONTEND_ACCEPT);
    CHECK(find(G6_INPUT_JUMP) >= 0 && find(G6_INPUT_CONTEXT) >= 0);
    CHECK(find(0u) < 0);
    CHECK(G6_CONTROL_GROUP_PLAYER == 0 && G6_CONTROL_GROUP_CAMERA == 1 && G6_CONTROL_GROUP_FRONTEND == 2);
    CHECK(controls_check_cpp() == (int)G6_INPUT_ACTION_COUNT);  /* C++ TU sees the C table */
    printf(fails ? "controls_check: %d failure(s)\n" : "controls_check: OK\n", fails);
    return fails != 0;
}
