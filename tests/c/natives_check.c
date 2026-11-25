/* Compile + smoke test of the generated C bindings against the mock runtime DLL. */
#include "gta6mod/natives.h"

#include <stdio.h>

/* Vector3* out-params receive one vmovaps store: 16 bytes, 16-byte aligned. */
_Static_assert(sizeof(g6_vec3_packed) == 16 && _Alignof(g6_vec3_packed) == 16, "g6_vec3_packed layout");

static g6_entity g_seen;
static g6_vec3   g_pos = { 1.5f, -2.0f, 30.25f };
static uint32_t  g_set_argc;
static uint64_t  g_set_flags[4];
static float     g_set_tail;
static int       g_out_aligned;

/* GTA VI ABI: the entity is two slots, the Vector3 return is one packed 16-byte vector. */
static void G6_CALL fake_coords(g6_native_call* c, void* user) {
    (void)user;
    g_seen = g6_entity_make(c->args[0], c->args[1]);
    g6_set_vec3_packed(c->ret, g_pos);
}

/* SET_ENTITY_COORDS 'ewbbbbf>n': entity 0-1, packed position 2-3, four BOOLs 4-7, float 8. */
static void G6_CALL fake_set_coords(g6_native_call* c, void* user) {
    (void)user;
    g_set_argc = c->argc;
    g_seen = g6_entity_make(c->args[0], c->args[1]);
    g_pos = g6_slots_vec3_packed(c->args + 2);
    for (int i = 0; i < 4; ++i) g_set_flags[i] = c->args[4 + i];
    g_set_tail = g6_ret_float(c->args[8]);
}

/* GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD 'wFV>b': packed coord 0-1, float* 2, Vector3* 3 (vmovaps in game). */
static void G6_CALL fake_ground(g6_native_call* c, void* user) {
    (void)user;
    const g6_vec3 p = g6_slots_vec3_packed(c->args);
    float* gz = (float*)(uintptr_t)c->args[2];
    g6_vec3_packed* n = (g6_vec3_packed*)(uintptr_t)c->args[3];
    g_out_aligned = n && ((uintptr_t)n & 15u) == 0;
    if (gz) *gz = p.z - 100.0f;
    if (n && g_out_aligned) { n->x = p.x; n->y = p.y; n->z = 1.0f; n->w = 0.0f; }
    c->ret[0] = 1;
}

int main(void) {
    const g6_host_api* api = g6_get_host_api();
    if (!api || api->version_major != G6_API_VERSION_MAJOR) { puts("FAIL api"); return 1; }
    g6_mock_register_native(0x3FEF770D40960D5Aull, fake_coords, NULL);
    g6_mock_register_native(0x06843DA7060A026Bull, fake_set_coords, NULL);
    g6_mock_register_native(0x2A29CA9A6319E6ABull, fake_ground, NULL);

    g6_vec3 p = G6_ENTITY_GET_ENTITY_COORDS(api, g6_entity_make(0x100, 0xABCDull), 1);
    g6_mock_step(250);
    int32_t t = G6_MISC_GET_GAME_TIMER(api);
    printf("coords %.2f %.2f %.2f timer %d natives %d\n", p.x, p.y, p.z, t, G6_NATIVE_COUNT);
    if (p.x != 1.5f || p.y != -2.0f || p.z != 30.25f || t != 250) { puts("FAIL values"); return 1; }
    if (g_seen.lo != 0x100 || g_seen.hi != 0xABCD) { puts("FAIL entity slots"); return 1; }

    /* Round trip through the packed Vector3 parameter: SET then GET must give the same position. */
    const g6_vec3 target = { -1234.5f, 678.25f, 42.125f };
    G6_ENTITY_SET_ENTITY_COORDS(api, g6_entity_make(0x101, 0x77ull), target, 1, 0, 1, 0, 2.5f);
    if (g_set_argc != 9) { printf("FAIL set argc %u (expected 9)\n", g_set_argc); return 1; }
    if (g_seen.lo != 0x101 || g_seen.hi != 0x77) { puts("FAIL set entity slots"); return 1; }
    if (g_set_flags[0] != 1 || g_set_flags[1] != 0 || g_set_flags[2] != 1 || g_set_flags[3] != 0 || g_set_tail != 2.5f) {
        puts("FAIL set slots 4-8");
        return 1;
    }
    p = G6_ENTITY_GET_ENTITY_COORDS(api, g6_entity_make(0x101, 0x77ull), 1);
    printf("set/get %.3f %.3f %.3f\n", p.x, p.y, p.z);
    if (p.x != target.x || p.y != target.y || p.z != target.z) { puts("FAIL set/get round trip"); return 1; }

    /* Vector3* out-param: a 16-byte aligned g6_vec3_packed. */
    float gz = 0.0f;
    g6_vec3_packed normal = { 0.0f, 0.0f, 0.0f, 0.0f };
    const g6_vec3 probe = { 3.0f, 4.0f, 112.0f };
    int found = G6_MISC_GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD(api, probe, &gz, &normal);
    if (!found || !g_out_aligned || gz != 12.0f || normal.x != 3.0f || normal.y != 4.0f || normal.z != 1.0f) {
        puts("FAIL ground probe out-params");
        return 1;
    }
    puts("OK");
    return 0;
}
