// probe_layout.hpp - every build-dependent value g6probe uses, with its provenance. GTA VI 1.0.2.1 only.
//
// Nothing here is a guess. Each value is copied from re-db/builds/1.0.2.1 (anchors.json,
// types/parts/gameplay-fields.json, types/parts/entity-types.json) or findings/phase5/24, and its
// confidence label is kept. Addresses are never hardcoded: data anchors are resolved at run time by
// the runtime's pattern scan (g6_host_api::anchor_address). The RVAs below (image base 0x140000000)
// only cross-check that live result ([probe] anchor_rva_check=1, the default).
//
// Kinds of values: "rva" = offset from the title image base; "off" = field offset inside an object.
#pragma once
#include <cstdint>

namespace g6probe::layout {

inline constexpr const char* kGameBuild = "1.0.2.1";

struct DataAnchor {
    const char* name;      // re-db anchor name, resolved through anchor_address
    std::uint32_t rva;     // re-db manifest RVA (cross-check only)
    const char* what;
    const char* source;
};

inline constexpr DataAnchor kPlayerPedLocal{ "player.ped.local", 0x0ab56e38u,
    "CPed* of the local player (global pointer)",
    "re-db anchors.json (entity-types, corrected in findings/phase5/24 follow-up), confidence high" };
inline constexpr DataAnchor kGravity{ "world.physics.gravity", 0x06dd5290u,
    "Vec3V f32[4]; static image value (0, -9.8, 0, 0); which axis is up is unknown (G-011)",
    "re-db anchors.json (gameplay-fields), confidence medium" };
inline constexpr DataAnchor kArchetypeTable{ "ecs.archetype_table", 0x0abbf3b0u,
    "CEcsArchetypeRecord[32], stride 0x238 (generic component store)",
    "re-db anchors.json (gameplay-fields), confidence high" };
inline constexpr DataAnchor kPedRowStride{ "ecs.ped.row_stride", 0x0abbeeccu,
    "u32 slots_per_row of the static ped store (record 0x14abbeeb8 + 0x14)",
    "re-db anchors.json (gameplay-fields), confidence medium" };
inline constexpr DataAnchor kPedIndexTable{ "ecs.ped.index_table", 0x0abbeed8u,
    "u16* row_index of the static ped store (record + 0x20)",
    "re-db anchors.json (gameplay-fields), confidence medium" };
inline constexpr DataAnchor kPedColumns{ "ecs.ped.columns", 0x0abbeee8u,
    "CEcsColumn* columns of the static ped store (record + 0x30)",
    "re-db anchors.json (gameplay-fields), confidence medium" };

// The game timer block (G-016). An xref-chain anchor (no signature of its own): GET_GAME_TIMER handler
// 0x143e85810 -> call 0x1405f2ef0 (mov eax,[rip]; ret) -> the rip operand, the same path the runtime decodes
// for game_time_ms (sdk/runtime/loader/README.md "Game clock"). GET_GAME_TIMER_F667 stays logged every
// sample as the fallback when this anchor is gated.
inline constexpr DataAnchor kGameTimer{ "world.game_timer", 0x0abbef40u,
    "game timer block: +0 u32 game time in ms (GET_GAME_TIMER returns it sign-extended), +4 u32 frame count",
    "re-db anchors.json (game-timer), confidence high" };

inline constexpr const DataAnchor* kAllAnchors[] = {
    &kPlayerPedLocal, &kGravity, &kArchetypeTable, &kPedRowStride, &kPedIndexTable, &kPedColumns, &kGameTimer,
};

namespace timer {
// world.game_timer fields: +0 read by GET_GAME_TIMER's core 0x1405f2ef0, +4 by GET_FRAME_COUNT's core
// 0x143eb1ef0 (re-db anchors.json evidence; build_consts.hpp kClockFrameCountOff).
inline constexpr std::uint32_t kTimeMsOff = 0x0;
inline constexpr std::uint32_t kFrameCountOff = 0x4;
} // namespace timer

// The title module: the runtime identifies it by this export (README "Architecture" step 1).
inline constexpr const char* kTitleExport = "__XMemAllocationHooks__";

namespace entity {
// CEntity+0x30 u8 entity_type: 3 = ped, 2 = vehicle. gameplay-fields.json, confidence high
// (write 0x144f15322, read 0x1402ba4e7).
inline constexpr std::uint32_t kTypeOff = 0x30;
inline constexpr std::uint8_t kTypePed = 3;
// CEntity+0x9c u32 ECS id: findings/phase5/24 [observed] (read in entity.health.get_pair 0x1402ba4fe;
// every component header reader masks the owner and reads +0x9c, gameplay-fields.json notes).
inline constexpr std::uint32_t kEcsIdOff = 0x9c;
inline constexpr std::uint32_t kReadSpan = 0xa0;   // bytes read from the entity: up to and including +0x9c
} // namespace entity

namespace ecs {
// findings/phase5/24 "Lookup" [observed]: arch = (id >> 17) & 0x1f, row = id & 0x1ffff.
inline constexpr std::uint32_t kArchShift = 17;
inline constexpr std::uint32_t kArchMask = 0x1f;
inline constexpr std::uint32_t kRowMask = 0x1ffff;
inline constexpr std::uint32_t kPedArch = 3;            // ped archetype (generic record 3 and the static ped store)
// CEcsArchetypeRecord (gameplay-fields.json, confidence high), 0x238 bytes.
inline constexpr std::uint32_t kRecordSize = 0x238;
inline constexpr std::uint32_t kRecSlotsPerRow = 0x14;  // u32
inline constexpr std::uint32_t kRecRowIndex = 0x20;     // u16*
inline constexpr std::uint32_t kRecColumns = 0x30;      // CEcsColumn*
inline constexpr std::uint32_t kRecTypeToColumn = 0x38; // u8[0x200], 0xff = none (generic records only)
inline constexpr std::uint32_t kTypeToColumnLen = 0x200;
// CEcsColumn {u8* base @0; u64 stride @8}, 0x10 bytes (gameplay-fields.json, confidence high).
inline constexpr std::uint32_t kColumnSize = 0x10;
inline constexpr std::uint16_t kAbsentIndex = 0xffff;
inline constexpr std::uint8_t kAbsentColumn = 0xff;
// Bound used by the probe (not a game fact): a row longer than the selector array cannot be indexed.
inline constexpr std::uint32_t kMaxSlotsPerRow = 0x200;
// The three static ped store anchors are fields of one record: index_table - row_stride and
// columns - row_stride must equal these (0x20 - 0x14, 0x30 - 0x14).
inline constexpr std::uint32_t kIndexMinusStride = kRecRowIndex - kRecSlotsPerRow;
inline constexpr std::uint32_t kColumnsMinusStride = kRecColumns - kRecSlotsPerRow;

// Components read for the player ped. size = pool element size = the live column stride, which the
// probe requires to match before it reads a field (findings/phase5/24 round 2 "Validation").
struct Component {
    const char* name;
    std::uint32_t column_or_type;   // static ped store: column; generic store: component type T
    std::uint32_t size;
    bool generic;                   // true: generic record 3, column = type_to_column[T]
};
inline constexpr Component kPedLaw{ "CPedComp_col4a", 0x4a, 0xcce0, false };   // confidence high
inline constexpr Component kPedCol32{ "CPedComp_col32", 0x32, 0xb80, false };  // confidence medium
inline constexpr Component kPedCol0f{ "CPedComp_col0f", 0x0f, 0x1d30, false }; // confidence medium
inline constexpr Component kGenHealth{ "CEcsComp_sel4d", 0x15, 0x8b0, true };  // confidence high
inline constexpr Component kGenProofs{ "CEcsComp_sel4f", 0x17, 0x28, true };   // confidence high

// Fields (gameplay-fields.json).
inline constexpr std::uint32_t kLawFlags14 = 0x14;      // u32, bit 5 = wanted disabled [medium]
inline constexpr std::uint32_t kWantedScore = 0x5f28;   // i32 [high]
inline constexpr std::uint32_t kWantedLevel = 0x5f2c;   // i32 [high]
inline constexpr std::uint32_t kCol32Flags = 0x234;     // u32, bit 28 = gravity disabled (SET_PED_GRAVITY) [medium]
inline constexpr std::uint32_t kCol32GravityBit = 28;
inline constexpr std::uint32_t kCol0fFlags = 0x15d4;    // u32, bit 11 = infinite ammo clip [medium]
inline constexpr std::uint32_t kHealthHost = 0x20;      // CEcsComp_sel4d+0x20 -> CHealthHost* [high]
inline constexpr std::uint32_t kHostHealthData = 0x68;  // CHealthHost+0x68 -> CHealthData* [high]
inline constexpr std::uint32_t kDataMaxHealth = 0x38;   // f32 cache, recomputed by 0x1408dc4a0 [high]
inline constexpr std::uint32_t kDataHealth = 0x40;      // f32 cache [high]
inline constexpr std::uint32_t kProofFlags = 0x10;      // u32, bit 4 = invincible [high]
} // namespace ecs

} // namespace g6probe::layout
