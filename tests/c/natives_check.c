/* Compile + smoke test of the generated C bindings against the mock runtime DLL. */
#include "gta6mod/natives.h"

#include <stdio.h>

static g6_entity g_seen;

/* GTA VI ABI: the entity is two slots, the Vector3 return is one packed 16-byte vector. */
static void G6_CALL fake_coords(g6_native_call* c, void* user) {
    (void)user;
    g_seen = g6_entity_make(c->args[0], c->args[1]);
    g6_vec3 v = { 1.5f, -2.0f, 30.25f };
    g6_set_vec3_packed(c->ret, v);
}

int main(void) {
    const g6_host_api* api = g6_get_host_api();
    if (!api || api->version_major != G6_API_VERSION_MAJOR) { puts("FAIL api"); return 1; }
    g6_mock_register_native(0x3FEF770D40960D5Aull, fake_coords, NULL);
    g6_vec3 p = G6_ENTITY_GET_ENTITY_COORDS(api, g6_entity_make(0x100, 0xABCDull), 1);
    g6_mock_step(250);
    int32_t t = G6_MISC_GET_GAME_TIMER(api);
    printf("coords %.2f %.2f %.2f timer %d natives %d\n", p.x, p.y, p.z, t, G6_NATIVE_COUNT);
    if (p.x != 1.5f || p.y != -2.0f || p.z != 30.25f || t != 250) { puts("FAIL values"); return 1; }
    if (g_seen.lo != 0x100 || g_seen.hi != 0xABCD) { puts("FAIL entity slots"); return 1; }
    puts("OK");
    return 0;
}
