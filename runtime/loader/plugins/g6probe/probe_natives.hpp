// probe_natives.hpp - the natives g6probe calls, checked against the generated bindings (natives.h).
//
// A native is callable only if natives.h lists the same hash with the same name and signature, a
// located handler (handler_rva != 0) and status "observed" or "verified". "unverified" (GTA V seed
// hashes without a 1.0.2.1 handler) is always refused. Setters are marked `write`: the probe calls
// them only from an armed stage-4 write probe (probe_core.hpp gate()).
#pragma once
#include "gta6mod/natives.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g6probe {

enum class N : int {
    player_ped_id, get_player_index, get_entity_coords, get_entity_heading, get_entity_health,
    get_entity_max_health, is_ped_in_any_vehicle, get_vehicle_ped_is_using, get_player_wanted_level,
    get_game_timer, get_frame_count, get_clock_hours, get_clock_minutes,
    set_entity_coords, set_entity_health, clear_player_wanted_level, add_to_clock_time,
    count
};

struct NativeSpec {
    N id;
    const char* name;     // natives.h name (GTA VI hash variant, e.g. PLAYER_PED_ID_0EE0)
    std::uint64_t hash;
    const char* sig;      // natives.h signature; the probe fills exactly these slots
    bool write;
};

// The GTA VI hash variants (suffix = last 4 hex digits) are used where the GTA V seed hash is
// "unverified" in natives.h (PLAYER_PED_ID, GET_ENTITY_COORDS, GET_ENTITY_HEADING, GET_ENTITY_HEALTH,
// GET_GAME_TIMER). Argument values follow sdk/python/gta6mod/game/entities.py.
inline constexpr NativeSpec kNatives[] = {
    { N::player_ped_id,             "PLAYER_PED_ID_0EE0",        0x096275889B8E0EE0ull, ">e",        false },
    { N::get_player_index,          "GET_PLAYER_INDEX",          0x47E385B0D957C8D4ull, ">i",        false },
    { N::get_entity_coords,         "GET_ENTITY_COORDS_F44D",    0xA86D5F069399F44Dull, "ebb>w",     false },
    { N::get_entity_heading,        "GET_ENTITY_HEADING_5507",   0xC230DD956E2F5507ull, "e>f",       false },
    { N::get_entity_health,         "GET_ENTITY_HEALTH_C0F7",    0x82368787EA73C0F7ull, "e>i",       false },
    { N::get_entity_max_health,     "GET_ENTITY_MAX_HEALTH",     0x15D757606D170C3Cull, "eb>i",      false },
    { N::is_ped_in_any_vehicle,     "IS_PED_IN_ANY_VEHICLE",     0x997ABD671D25CA0Bull, "ebqq>b",    false },
    { N::get_vehicle_ped_is_using,  "GET_VEHICLE_PED_IS_USING",  0x6094AD011A2EA87Dull, "eq>e",      false },
    { N::get_player_wanted_level,   "GET_PLAYER_WANTED_LEVEL",   0xABC532F9098BFD9Dull, "iq>i",      false },
    { N::get_game_timer,            "GET_GAME_TIMER_F667",       0x4F67E8ECA7D3F667ull, ">i",        false },
    { N::get_frame_count,           "GET_FRAME_COUNT",           0x77DFA958FCF100C1ull, ">i",        false },
    { N::get_clock_hours,           "GET_CLOCK_HOURS",           0xC82CF208C2B19199ull, ">i",        false },
    { N::get_clock_minutes,         "GET_CLOCK_MINUTES",         0x4E162231B823DBBFull, ">i",        false },
    { N::set_entity_coords,         "SET_ENTITY_COORDS",         0x06843DA7060A026Bull, "ewbbbbf>n", true  },
    { N::set_entity_health,         "SET_ENTITY_HEALTH",         0xAC2767ED8BDFAB15ull, "eieqq>n",   true  },
    { N::clear_player_wanted_level, "CLEAR_PLAYER_WANTED_LEVEL", 0x4E4B996C928C7AA6ull, "iq>n",      true  },
    { N::add_to_clock_time,         "ADD_TO_CLOCK_TIME",         0xAB7C251C7701D336ull, "iii>n",     true  },
};
static_assert(sizeof(kNatives) / sizeof(kNatives[0]) == static_cast<std::size_t>(N::count), "one spec per N");

inline const NativeSpec& spec(N id) noexcept { return kNatives[static_cast<int>(id)]; }

enum class BindState : int { ok = 0, missing = 1, name_mismatch = 2, sig_mismatch = 3, no_handler = 4, below_observed = 5 };

inline const char* bind_state_str(BindState s) noexcept {
    switch (s) {
    case BindState::ok: return "ok";
    case BindState::missing: return "hash not in the generated bindings (natives.h)";
    case BindState::name_mismatch: return "name differs from the generated bindings";
    case BindState::sig_mismatch: return "signature differs from the generated bindings";
    case BindState::no_handler: return "no located handler (handler_rva 0)";
    case BindState::below_observed: return "status below 'observed' (unverified natives are refused)";
    }
    return "?";
}

struct Binding {
    BindState state = BindState::missing;
    const char* status = "";      // natives.h status string
    std::uint32_t handler_rva = 0;
};

inline int status_rank(const char* s) noexcept {
    if (!s) return -1;
    if (std::strcmp(s, "verified") == 0) return 2;
    if (std::strcmp(s, "observed") == 0) return 1;
    if (std::strcmp(s, "unverified") == 0) return 0;
    return -1;
}

// `table`/`n` are g6_natives/G6_NATIVE_COUNT in the plugin; tests pass their own rows.
inline Binding bind(const NativeSpec& s, const g6_native_info* table, std::size_t n) noexcept {
    Binding b;
    for (std::size_t i = 0; i < n; ++i) {
        const g6_native_info& r = table[i];
        if (r.hash != s.hash) continue;
        b.status = r.status ? r.status : "";
        b.handler_rva = r.handler_rva;
        if (!r.name || std::strcmp(r.name, s.name) != 0) b.state = BindState::name_mismatch;
        else if (!r.sig || std::strcmp(r.sig, s.sig) != 0) b.state = BindState::sig_mismatch;
        else if (status_rank(r.status) < 1) b.state = BindState::below_observed;
        else if (r.handler_rva == 0) b.state = BindState::no_handler;
        else b.state = BindState::ok;
        return b;
    }
    return b;
}

} // namespace g6probe
