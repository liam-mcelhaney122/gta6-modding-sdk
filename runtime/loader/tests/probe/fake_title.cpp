// fake_title.cpp - stand-in for the title image in the g6probe mock-world test.
// It exports __XMemAllocationHooks__ (what the runtime and g6probe use to recognise the title module)
// and holds the "global" data the probe reaches through anchors: the local player ped pointer, the
// gravity vector, the generic archetype table, the static ped store record and the game timer block. The test fills them;
// their RVAs are of course not the game's, so the test runs with [probe] anchor_rva_check=0 (and once
// with 1, to see every anchor refused).
#include <cstdint>
#include <cstring>

extern "C" __declspec(dllexport) std::uint64_t __XMemAllocationHooks__ = 0;

namespace {
alignas(16) float g_gravity[4] = { 0.0f, -9.8f, 0.0f, 0.0f };
void* g_player_ped = nullptr;
alignas(16) std::uint8_t g_arch_table[32 * 0x238];
alignas(16) std::uint8_t g_static_ped_record[0x238];
alignas(16) std::uint32_t g_game_timer[4] = {};   // +0 time ms, +4 frame count
} // namespace

extern "C" __declspec(dllexport) void* fake_anchor(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "player.ped.local") == 0) return &g_player_ped;
    if (std::strcmp(name, "world.physics.gravity") == 0) return g_gravity;
    if (std::strcmp(name, "ecs.archetype_table") == 0) return g_arch_table;
    if (std::strcmp(name, "ecs.ped.row_stride") == 0) return g_static_ped_record + 0x14;
    if (std::strcmp(name, "ecs.ped.index_table") == 0) return g_static_ped_record + 0x20;
    if (std::strcmp(name, "ecs.ped.columns") == 0) return g_static_ped_record + 0x30;
    if (std::strcmp(name, "world.game_timer") == 0) return g_game_timer;
    return nullptr;
}

extern "C" __declspec(dllexport) void** fake_player_ped_slot() { return &g_player_ped; }
extern "C" __declspec(dllexport) std::uint8_t* fake_arch_table() { return g_arch_table; }
extern "C" __declspec(dllexport) std::uint8_t* fake_static_ped_record() { return g_static_ped_record; }
extern "C" __declspec(dllexport) float* fake_gravity() { return g_gravity; }
extern "C" __declspec(dllexport) std::uint32_t* fake_game_timer() { return g_game_timer; }
