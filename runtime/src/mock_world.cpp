// mock_world.cpp — fake natives, scenario events and the mock UI driver shared by g6_luau_runner and g6_python_test.
// Moved verbatim from luau/host/g6_luau_runner.cpp (2026-11-26); semantics: sdk/docs/script_layer.md "Mock world".
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1  // std::sscanf parses --waypoint specs (bounded %f / %llu conversions)
#endif
#include "mock_world.hpp"

#include "gta6mod/script_objects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace g6mock {
namespace {

struct Ent {
    uint64_t h;
    bool     vehicle;
    uint32_t model;
    float    x, y, z, heading;
    int32_t  health, max_health;
    bool     invincible, can_ragdoll, frozen, visible;
    float    vx, vy, vz;
    uint64_t in_vehicle;  // peds: handle of the vehicle they are in, 0 = none
    bool     script_owned; // created by a script (mission entity): DELETE_* removes it
};
struct Model { uint32_t hash; uint64_t loaded_at; };
struct Cell { long x, y; uint64_t loaded_at; };
struct Held { int32_t ctl; uint64_t until; };

constexpr uint64_t kModelLoadMs = 50;
constexpr uint64_t kCollisionLoadMs = 100;
constexpr float    kGroundZ = 12.f;
constexpr uint64_t kPlayer = 0x100;
constexpr double   kPi = 3.14159265358979323846;

const g6_host_api*   g_mock = nullptr;   // the mock DLL table (game clock)
std::vector<Ent>     g_ents;
std::vector<Model>   g_models;
std::vector<Cell>    g_cells;
std::vector<int32_t> g_just_pressed;     // pad 0 controls pressed this tick
std::vector<Held>    g_held;             // pad 0 controls held, until their release time
std::vector<int32_t> g_just_released;    // pad 0 controls released this tick
uint64_t             g_next_handle = kPlayer;
int                  g_world_errors = 0;
int32_t              g_wanted = 0, g_max_wanted = 5;
bool                 g_waypoint = false;
float                g_wp[3] = {};
int64_t              g_clock = 12 * 3600;  // seconds since midnight
bool                 g_clock_paused = false;
float                g_time_scale = 1.f;
std::string          g_objective;          // _UILOG cached objective text (at most 511 bytes)

uint64_t world_now() { return g_mock->game_time_ms(); }

uint64_t spawn(bool vehicle, uint32_t model, float x, float y, float z, float heading) {
    const int32_t hp = vehicle ? 1000 : 200;
    g_ents.push_back({ g_next_handle, vehicle, model, x, y, z, heading, hp, hp, false, true, false, true, 0.f, 0.f, 0.f, 0, false });
    return g_next_handle++;
}
// Entity = 2 slots (lo, hi), findings/phase5/23_handle_abi.md. Mock handles have hi == 0.
Ent* ent_at(const uint64_t* slots) {
    for (auto& e : g_ents) if (e.h == slots[0] && slots[1] == 0) return &e;
    return nullptr;
}
Ent* ent(const uint64_t* slots) { return ent_at(slots); }
Ent* by_handle(uint64_t h) {
    for (auto& e : g_ents) if (e.h == h) return &e;
    return nullptr;
}
Model* model(uint32_t hash) {
    for (auto& m : g_models) if (m.hash == hash) return &m;
    return nullptr;
}
float  f(uint64_t slot) { return g6_ret_float(slot); }
int32_t i32(uint64_t slot) { return (int32_t)(uint32_t)slot; }
void   ret_vec(g6_native_call* c, float x, float y, float z) { g6_set_vec3_packed(c->ret, g6_vec3{ x, y, z }); }
int64_t norm_day(int64_t s) { return ((s % 86400) + 86400) % 86400; }
Cell*  cell(float x, float y) {
    const long cx = std::lround(x), cy = std::lround(y);
    for (auto& k : g_cells) if (k.x == cx && k.y == cy) return &k;
    return nullptr;
}
void   set_pos(Ent& e, float x, float y, float z) {
    e.x = x; e.y = y; e.z = z;
    if (e.vehicle)  // peds in a vehicle move with it
        for (auto& p : g_ents) if (p.in_vehicle == e.h) { p.x = x; p.y = y; p.z = z; }
}

// ---- system / player
void G6_CALL n_wait(g6_native_call*, void*) {}
void G6_CALL n_get_hash_key(g6_native_call* c, void*) {
    const char* s = (const char*)(uintptr_t)c->args[0];
    c->ret[0] = g6_joaat(s ? s : "");
}
void G6_CALL n_game_timer(g6_native_call* c, void*) { c->ret[0] = world_now() & 0xFFFFFFFFu; }
void G6_CALL n_player_ped(g6_native_call* c, void*) { c->ret[0] = kPlayer; c->ret[1] = 0; }
void G6_CALL n_player_id(g6_native_call* c, void*) { c->ret[0] = 0; }

// ---- entity
void G6_CALL n_exists(g6_native_call* c, void*) { c->ret[0] = ent(c->args) ? 1 : 0; }
void G6_CALL n_coords(g6_native_call* c, void*) {
    const Ent* e = ent(c->args);
    ret_vec(c, e ? e->x : 0.f, e ? e->y : 0.f, e ? e->z : 0.f);  // packed Vector3 return
}
// Vector3 params are one packed 16-byte vector in two slots ('w'), findings/phase5/23_handle_abi.md.
g6_vec3 vec_at(const uint64_t* slots) { return g6_slots_vec3_packed(slots); }
void G6_CALL n_set_coords(g6_native_call* c, void*) {  // 'ewbbbbf': entity 0-1, packed pos 2-3, BOOLs 4-7, float 8
    const g6_vec3 p = vec_at(c->args + 2);
    if (Ent* e = ent(c->args)) set_pos(*e, p.x, p.y, p.z);
}
void G6_CALL n_heading(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = g6_arg_float(e ? e->heading : 0.f); }
void G6_CALL n_set_heading(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->heading = f(c->args[2]); }
void G6_CALL n_health(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = e ? (uint64_t)(int64_t)e->health : 0; }
void G6_CALL n_max_health(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = e ? (uint64_t)(int64_t)e->max_health : 0; }
void G6_CALL n_set_health(g6_native_call* c, void*) {  // (e, int, e killer, q, q)
    if (Ent* e = ent(c->args)) e->health = std::clamp(i32(c->args[2]), 0, e->max_health);
}
void G6_CALL n_set_invincible(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->invincible = (c->args[2] & 0xFF) != 0; }
void G6_CALL n_freeze(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->frozen = (c->args[2] & 0xFF) != 0; }
void G6_CALL n_set_visible(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->visible = (c->args[2] & 0xFF) != 0; }
void G6_CALL n_is_visible(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = (e && e->visible) ? 1 : 0; }
void G6_CALL n_set_velocity(g6_native_call* c, void*) {
    const g6_vec3 v = vec_at(c->args + 2);
    if (Ent* e = ent(c->args)) { e->vx = v.x; e->vy = v.y; e->vz = v.z; }
}
void G6_CALL n_speed(g6_native_call* c, void*) {
    const Ent* e = ent(c->args);
    c->ret[0] = g6_arg_float(e ? std::sqrt(e->vx * e->vx + e->vy * e->vy + e->vz * e->vz) : 0.f);
}
void G6_CALL n_forward(g6_native_call* c, void*) {
    const Ent* e = ent(c->args);
    if (!e) { ret_vec(c, 0.f, 0.f, 0.f); return; }
    const double r = e->heading * kPi / 180.0;
    ret_vec(c, (float)-std::sin(r), (float)std::cos(r), 0.f);
}
void G6_CALL n_model_of(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = e ? e->model : 0; }
void G6_CALL n_is_dead(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = (e && e->health <= 0) ? 1 : 0; }
void G6_CALL n_offset(g6_native_call* c, void*) {  // pos + right*x + forward*y + up*z
    const Ent* e = ent(c->args);
    if (!e) { ret_vec(c, 0.f, 0.f, 0.f); return; }
    const double r = e->heading * kPi / 180.0;
    const g6_vec3 o = vec_at(c->args + 2);
    const double ox = o.x, oy = o.y, oz = o.z;
    const double rx = std::cos(r), ry = std::sin(r), fx = -std::sin(r), fy = std::cos(r);
    ret_vec(c, (float)(e->x + rx * ox + fx * oy), (float)(e->y + ry * ox + fy * oy), (float)(e->z + oz));
}

// ---- ped / vehicle
void G6_CALL n_set_can_ragdoll(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->can_ragdoll = (c->args[2] & 0xFF) != 0; }
void G6_CALL n_in_any_vehicle(g6_native_call* c, void*) {
    const Ent* e = ent(c->args);
    c->ret[0] = (e && e->in_vehicle && by_handle(e->in_vehicle)) ? 1 : 0;
}
void G6_CALL n_vehicle_using(g6_native_call* c, void*) {
    const Ent* e = ent(c->args);
    c->ret[0] = (e && e->in_vehicle && by_handle(e->in_vehicle)) ? e->in_vehicle : 0;
    c->ret[1] = 0;
}
void G6_CALL n_set_into_vehicle(g6_native_call* c, void*) {  // (e ped, e vehicle, int seat): seat ignored
    Ent* p = ent(c->args);
    const Ent* v = ent_at(c->args + 2);
    if (!p || !v) return;
    p->in_vehicle = v->h;
    p->x = v->x; p->y = v->y; p->z = v->z;
}
void G6_CALL n_create_vehicle(g6_native_call* c, void*) {
    const uint32_t m = (uint32_t)c->args[0];
    const Model* md = model(m);
    if (!md || world_now() < md->loaded_at) { c->ret[0] = 0; c->ret[1] = 0; return; }  // game returns 0 when the model is not loaded
    c->ret[0] = spawn(true, m, f(c->args[1]), f(c->args[2]), f(c->args[3]), f(c->args[4]));
    c->ret[1] = 0;
    g_ents.back().script_owned = true;
}
// In/out Entity* ('X'): the game reads the 16-byte handle at the pointer and writes 16 zero bytes back
// (findings/phase5/30_script_layer_evidence.md, G-084). Same semantics as the Python mock.
void G6_CALL n_delete_entity(g6_native_call* c, void*) {  // DELETE_ENTITY / _PED / _VEHICLE / _OBJECT
    auto* p = (g6_entity*)(uintptr_t)c->args[0];
    if (!p) return;
    const uint64_t slots[2] = { p->lo, p->hi };
    if (const Ent* e = ent_at(slots); e && e->script_owned) {
        const uint64_t h = e->h;
        g_ents.erase(g_ents.begin() + (e - g_ents.data()));
        for (auto& q : g_ents) if (q.in_vehicle == h) q.in_vehicle = 0;
    }
    p->lo = 0; p->hi = 0;
}
void G6_CALL n_no_longer_needed(g6_native_call* c, void*) {  // SET_*_AS_NO_LONGER_NEEDED
    auto* p = (g6_entity*)(uintptr_t)c->args[0];
    if (!p) return;
    const uint64_t slots[2] = { p->lo, p->hi };
    if (Ent* e = ent_at(slots); e && e->script_owned) { e->script_owned = false; p->lo = 0; p->hi = 0; }
}
void G6_CALL n_set_fixed(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->health = e->max_health; }

// ---- streaming / world
void G6_CALL n_request_model(g6_native_call* c, void*) {
    const uint32_t m = (uint32_t)c->args[0];
    if (!model(m)) g_models.push_back({ m, world_now() + kModelLoadMs });
}
void G6_CALL n_model_loaded(g6_native_call* c, void*) {
    const Model* m = model((uint32_t)c->args[0]);
    c->ret[0] = (m && world_now() >= m->loaded_at) ? 1 : 0;
}
void G6_CALL n_release_model(g6_native_call*, void*) {}
void G6_CALL n_model_valid(g6_native_call* c, void*) { c->ret[0] = ((uint32_t)c->args[0] != 0) ? 1 : 0; }
void G6_CALL n_request_collision(g6_native_call* c, void*) {
    const g6_vec3 p = vec_at(c->args);
    const float x = p.x, y = p.y;
    if (!cell(x, y)) g_cells.push_back({ std::lround(x), std::lround(y), world_now() + kCollisionLoadMs });
}
void G6_CALL n_ground_z(g6_native_call* c, void*) {  // 'wFV>b': packed coord 0-1, float* 2, Vector3* 3
    const g6_vec3 p = vec_at(c->args);
    const Cell* k = cell(p.x, p.y);
    const bool found = k && world_now() >= k->loaded_at && p.z > kGroundZ;
    auto* gz = (int64_t*)(uintptr_t)c->args[2];
    auto* nv = (g6_vec3_packed*)(uintptr_t)c->args[3];
    if (gz) *gz = (int64_t)g6_arg_float(found ? kGroundZ : 0.f);
    if (nv && ((uintptr_t)nv & 15u) != 0) {  // the game stores it with vmovaps, which would fault
        std::fprintf(stderr, "[ERROR] GET_GROUND_Z_AND_NORMAL: Vector3* out-param not 16-byte aligned\n");
        ++g_world_errors;
        nv = nullptr;
    }
    if (nv) { nv->x = 0.f; nv->y = 0.f; nv->z = found ? 1.f : 0.f; nv->w = 0.f; }
    c->ret[0] = found ? 1 : 0;
}
void G6_CALL n_waypoint_active(g6_native_call* c, void*) { c->ret[0] = g_waypoint ? 1 : 0; }
void G6_CALL n_waypoint_coords(g6_native_call* c, void*) {
    if (g_waypoint) ret_vec(c, g_wp[0], g_wp[1], g_wp[2]); else ret_vec(c, 0.f, 0.f, 0.f);
}
void G6_CALL n_set_time_scale(g6_native_call* c, void*) { g_time_scale = f(c->args[0]); }

// ---- clock
void G6_CALL n_clock_hours(g6_native_call* c, void*) { c->ret[0] = (uint64_t)(g_clock / 3600); }
void G6_CALL n_clock_minutes(g6_native_call* c, void*) { c->ret[0] = (uint64_t)(g_clock / 60 % 60); }
void G6_CALL n_clock_seconds(g6_native_call* c, void*) { c->ret[0] = (uint64_t)(g_clock % 60); }
void G6_CALL n_set_clock(g6_native_call* c, void*) {
    g_clock = norm_day((int64_t)i32(c->args[0]) * 3600 + (int64_t)i32(c->args[1]) * 60 + i32(c->args[2]));
}
void G6_CALL n_add_clock(g6_native_call* c, void*) {
    g_clock = norm_day(g_clock + (int64_t)i32(c->args[0]) * 3600 + (int64_t)i32(c->args[1]) * 60 + i32(c->args[2]));
}
void G6_CALL n_pause_clock(g6_native_call* c, void*) { g_clock_paused = (c->args[0] & 0xFF) != 0; }

// ---- pad
bool in_list(const std::vector<int32_t>& v, const g6_native_call* c) {
    return i32(c->args[0]) == 0 && std::find(v.begin(), v.end(), i32(c->args[1])) != v.end();
}
bool is_held(const g6_native_call* c) {
    if (i32(c->args[0]) != 0) return false;
    for (const Held& h : g_held) if (h.ctl == i32(c->args[1])) return true;
    return false;
}
void G6_CALL n_just_pressed(g6_native_call* c, void*) { c->ret[0] = in_list(g_just_pressed, c) ? 1 : 0; }
void G6_CALL n_pressed(g6_native_call* c, void*) { c->ret[0] = is_held(c) ? 1 : 0; }
void G6_CALL n_just_released(g6_native_call* c, void*) { c->ret[0] = in_list(g_just_released, c) ? 1 : 0; }
void G6_CALL n_released(g6_native_call* c, void*) { c->ret[0] = is_held(c) ? 0 : 1; }
void G6_CALL n_nop(g6_native_call*, void*) {}

// ---- wanted level
// GET/CLEAR_PLAYER_WANTED_LEVEL take a ped ('e', 2 slots): the game resolves it with the ped resolver and a null or
// non-player ped reads 0 / clears nothing (re-db script-vm.param_types.json, G-090). Same as the Python mock.
bool is_player_ped(const uint64_t* slots) { return slots[0] == kPlayer && slots[1] == 0; }
void G6_CALL n_wanted(g6_native_call* c, void*) { c->ret[0] = is_player_ped(c->args) ? (uint64_t)(int64_t)g_wanted : 0; }
void G6_CALL n_clear_wanted(g6_native_call* c, void*) { if (is_player_ped(c->args)) g_wanted = 0; }
void G6_CALL n_max_wanted(g6_native_call* c, void*) { c->ret[0] = (uint64_t)(int64_t)g_max_wanted; }
void G6_CALL n_set_max_wanted(g6_native_call* c, void*) {
    g_max_wanted = i32(c->args[0]);
    g_wanted = std::min(g_wanted, g_max_wanted);
}

// ---- _UILOG cached objective (G-087): 'T>n', 'q>n', '>n'. The text object is read the game's way
// (g6_script_text_read: +0x18, virtual +0x18); SET keeps at most 511 bytes, PRINT shows a non-empty text and logs
// "objective shown: TEXT (MS ms)", CLEAR empties it and logs "objective cleared" when there was one.
void world_log(const std::string& msg) { if (g_mock && g_mock->log) g_mock->log(G6_LOG_INFO, "mock", msg.c_str()); }
void G6_CALL n_objective_set(g6_native_call* c, void*) {
    const char* t = g6_script_text_read(reinterpret_cast<const void*>(static_cast<uintptr_t>(c->args[0])));
    g_objective = t ? std::string(t, std::min<size_t>(std::strlen(t), 511)) : std::string();
}
void G6_CALL n_objective_print(g6_native_call* c, void*) {
    if (!g_objective.empty()) world_log("objective shown: " + g_objective + " (" + std::to_string(i32(c->args[0])) + " ms)");
}
// _BUSYSPINNER_SET_TEXT ('S>n'): logs "busyspinner: TEXT" with the text read from the String the game's way.
void G6_CALL n_busyspinner_text(g6_native_call* c, void*) {
    const char* t = g6_script_string_text(reinterpret_cast<const void*>(static_cast<uintptr_t>(c->args[0])));
    world_log(std::string("busyspinner: ") + (t ? t : "(null)"));
}
void G6_CALL n_objective_clear(g6_native_call*, void*) {
    if (!g_objective.empty()) world_log("objective cleared");
    g_objective.clear();
}

// Scenario points ('h', 16-byte non-entity handles, G-099): a non-zero registry id gives lo = 0x5C<<56 | id,
// hi = 0x5C5C; id 0 gives 16 zero bytes. DOES_SCENARIO_POINT_EXIST needs both halves (round-trip check).
constexpr uint64_t kScenarioLo = 0x5C00000000000000ull, kScenarioHi = 0x5C5Cull;
void G6_CALL n_registered_scenario_point(g6_native_call* c, void*) {
    const uint32_t id = (uint32_t)c->args[0];
    c->ret[0] = id ? (kScenarioLo | id) : 0;
    c->ret[1] = id ? kScenarioHi : 0;
}
void G6_CALL n_scenario_point_exists(g6_native_call* c, void*) {
    const uint64_t lo = c->args[0], hi = c->args[1];
    c->ret[0] = ((uint32_t)lo != 0 && (lo >> 32) == (kScenarioLo >> 32) && hi == kScenarioHi) ? 1 : 0;
}

// CTL of --press: an INPUT_* name (hashed like GET_HASH_KEY) or a u32 (decimal or 0x hex).
bool parse_control(const char* spec, int32_t& out) {
    const std::string v(spec, std::strcspn(spec, "@"));
    if (v.empty()) return false;
    if ((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z')) {
        out = (int32_t)g6_joaat(v.c_str());
        return true;
    }
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v.c_str(), &end, 0);
    if (end == v.c_str() || *end != '\0' || n > 0xFFFFFFFFull) return false;
    out = (int32_t)(uint32_t)n;
    return true;
}

// "+HOLD" after "@MS" of --press: hold time in ms (absent: 0). False when present but not a number.
bool parse_hold(const char* spec, uint64_t& hold) {
    hold = 0;
    const char* a = std::strchr(spec, '@');
    const char* p = a ? std::strchr(a, '+') : nullptr;
    if (!p) return true;
    if (p[1] < '0' || p[1] > '9') return false;
    char* end = nullptr;
    hold = std::strtoull(p + 1, &end, 10);
    return *end == '\0';
}

// "N@MS" -> (N, MS); "X,Y,Z@MS" for waypoints.
bool parse_at(const char* spec, uint64_t& at) {
    const char* a = std::strchr(spec, '@');
    at = a ? std::strtoull(a + 1, nullptr, 10) : 0;
    return true;
}

} // namespace

void fire(const Event& ev) {
    switch (ev.kind) {
    case Ev::Press: {
        g_just_pressed.push_back(ev.n);
        const uint64_t until = world_now() + ev.hold;
        auto it = std::find_if(g_held.begin(), g_held.end(), [&](const Held& h) { return h.ctl == ev.n; });
        if (it != g_held.end()) it->until = until; else g_held.push_back({ ev.n, until });
        break;
    }
    case Ev::Damage: {
        Ent* p = by_handle(kPlayer);
        if (!p) break;
        // ev.n is validated as 0..INT32_MAX at parse time; subtract in 64 bits so the clamp sees the true value.
        auto hurt = [&](Ent* e) { e->health = (int32_t)std::max<int64_t>(0, (int64_t)e->health - ev.n); };
        if (!p->invincible) hurt(p);
        if (p->in_vehicle)
            if (Ent* v = by_handle(p->in_vehicle); v && !v->invincible) hurt(v);
        break;
    }
    case Ev::Wanted: g_wanted = ev.n; break;
    case Ev::Waypoint: g_waypoint = true; g_wp[0] = ev.x; g_wp[1] = ev.y; g_wp[2] = ev.z; break;
    }
}

bool install(const g6_host_api* mock) {
    g_mock = mock;
    g_ents.clear();
    g_models.clear();
    g_cells.clear();
    g_just_pressed.clear();
    g_held.clear();
    g_just_released.clear();
    g_next_handle = kPlayer;
    g_world_errors = 0;
    g_wanted = 0; g_max_wanted = 5;
    g_waypoint = false;
    g_wp[0] = g_wp[1] = g_wp[2] = 0.f;
    g_clock = 12 * 3600;
    g_clock_paused = false;
    g_time_scale = 1.f;
    spawn(false, g6_joaat("player_zero"), -1000.f, -2000.f, 15.f, 0.f);  // player ped, handle 0x100
    const struct { uint64_t hash; g6_mock_native_fn fn; } fakes[] = {
        // seed (GTA V) hashes, kept for the raw-native examples and tests
        { 0x4EDE34FBADD967A6ull, n_wait },           { 0xD24D37CC275948CCull, n_get_hash_key },
        { 0x9CD27B0045628463ull, n_game_timer },     { 0xD80958FC74E988A6ull, n_player_ped },
        { 0x4F8644AF03D0E0D6ull, n_player_id },      { 0x7239B21A38F536BAull, n_exists },
        { 0x3FEF770D40960D5Aull, n_coords },         { 0x06843DA7060A026Bull, n_set_coords },
        { 0xE83D4F9BA2A38914ull, n_heading },        { 0xEEF059FAD016D209ull, n_health },
        { 0x963D27A58DF860ACull, n_request_model },  { 0x98A4EB5D89A0C952ull, n_model_loaded },
        { 0xE532F5D78798DAABull, n_release_model },  { 0xAF35D0D2583051B0ull, n_create_vehicle },
        { 0x580417101DDB492Full, n_just_pressed },
        // observed GTA VI hashes used by the script layer (sdk/docs/script_layer.md)
        { 0x096275889B8E0EE0ull, n_player_ped },     { 0xD42BD6EB2E0F1677ull, n_exists },
        { 0x47E385B0D957C8D4ull, n_player_id },  // GET_PLAYER_INDEX (observed; the handler returns 0)
        { 0xA86D5F069399F44Dull, n_coords },         { 0xC230DD956E2F5507ull, n_heading },
        { 0xCF2B9C0645C4651Bull, n_set_heading },    { 0x82368787EA73C0F7ull, n_health },
        { 0x15D757606D170C3Cull, n_max_health },     { 0xAC2767ED8BDFAB15ull, n_set_health },
        { 0xA5C38736C426FCB8ull, n_set_invincible }, { 0x7D9EFB7AD6B19754ull, n_freeze },
        { 0x1794B4FCC84D812Full, n_set_visible },    { 0xFFC96ECB7FA404CAull, n_is_visible },
        { 0x1C99BB7B6E96D16Full, n_set_velocity },   { 0xFB6BA510A533DF81ull, n_speed },
        { 0x2412D9C05BB09B97ull, n_forward },        { 0xDA76A9F39210D365ull, n_model_of },
        { 0x7D5B1F88E7504BBAull, n_is_dead },        { 0x1899F328B0E12848ull, n_offset },
        { 0xB128377056A54E2Aull, n_set_can_ragdoll }, { 0x997ABD671D25CA0Bull, n_in_any_vehicle },
        { 0x6094AD011A2EA87Dull, n_vehicle_using },  { 0xF75B0D629E1C063Dull, n_set_into_vehicle },
        { 0x79811282A9D1AE56ull, n_set_fixed },      { 0xFA28FE3A6246FC30ull, n_request_model },
        { 0x1283B8B89DD5D1B6ull, n_model_loaded },   { 0x4AD96EF928BD4F9Aull, n_release_model },
        { 0x392C8D8E07B70EFCull, n_model_valid },    { 0x0A3720F162A033C9ull, n_request_collision },
        { 0x202B1BBFC6AB5EE4ull, n_waypoint_active }, { 0x29B30D07C3F7873Bull, n_waypoint_coords },
        { 0x2A29CA9A6319E6ABull, n_ground_z },       { 0x9682AF6050854856ull, n_set_time_scale },
        { 0x4F67E8ECA7D3F667ull, n_game_timer },     { 0xC82CF208C2B19199ull, n_clock_hours },
        { 0x4E162231B823DBBFull, n_clock_minutes },  { 0xB6101ABE62B5F080ull, n_clock_seconds },
        { 0x3A52C59FFB2DEED8ull, n_set_clock },      { 0xAB7C251C7701D336ull, n_add_clock },
        { 0x4D1A590C92BF377Eull, n_pause_clock },    { 0xF3A21BCD95725A4Aull, n_pressed },
        { 0x50F940259D3841E6ull, n_just_released },  { 0x648EE3E7F38877DDull, n_released },
        { 0xFE99B66D079CF6BCull, n_nop },
        { 0xABC532F9098BFD9Dull, n_wanted },         { 0x4E4B996C928C7AA6ull, n_clear_wanted },
        { 0xD04CFAD1E2B7984Aull, n_max_wanted },     { 0x28A4BD2CEE236E19ull, n_set_max_wanted },
        // scenario points ('h', G-099): _GET_REGISTERED_SCENARIO_POINT, DOES_SCENARIO_POINT_EXIST
        { 0xE3A0EFAFF3DFE27Bull, n_registered_scenario_point }, { 0x841475AC96E794D1ull, n_scenario_point_exists },
        // in/out Entity* natives (G-084)
        { 0x4CD38C78BD19A497ull, n_delete_entity },  { 0xCC0EF140F99365C5ull, n_delete_entity },
        { 0xE20A909D8C4A70F8ull, n_delete_entity },  { 0x931914268722C263ull, n_delete_entity },
        { 0x4971D2F8162B9674ull, n_no_longer_needed }, { 0x2595DD4236549CE3ull, n_no_longer_needed },
        { 0x629BFA74418D6239ull, n_no_longer_needed }, { 0x3AE22DEB5BA5A3E6ull, n_no_longer_needed },
        // _UILOG cached objective (G-087): SET ('T'), PRINT, CLEAR
        { 0x0CB5D1E14473D8A5ull, n_objective_set }, { 0xE9990552DEC71600ull, n_objective_print },
        { 0xDFF0D417277B41F8ull, n_objective_clear }, { 0x7F78CD75CC4539E4ull, n_busyspinner_text },
    };
    for (const auto& fk : fakes)
        if (g6_mock_register_native(fk.hash, fk.fn, nullptr) != G6_OK) return false;
    return true;
}

bool parse_event(const char* kind, const char* spec, Event& out, std::string& err) {
    out = Event{ 0, Ev::Press, 0, 0.f, 0.f, 0.f, 0 };
    if (!kind || !spec) { err = "missing value"; return false; }
    parse_at(spec, out.at);
    if (!std::strcmp(kind, "press")) {
        int32_t ctl = 0;
        uint64_t hold = 0;
        if (!parse_control(spec, ctl) || !parse_hold(spec, hold)) {
            err = std::string("bad --press ") + spec + " (want INPUT_NAME@MS[+HOLD] or N@MS[+HOLD])";
            return false;
        }
        out.kind = Ev::Press;
        out.n = ctl;
        out.hold = hold;
        return true;
    }
    if (!std::strcmp(kind, "damage") || !std::strcmp(kind, "wanted")) {
        char* end = nullptr;
        const long long n = std::strtoll(spec, &end, 10);
        if (end == spec || (*end != '@' && *end != '\0') || n < 0 || n > INT32_MAX) {
            err = std::string("bad --") + kind + " " + spec + " (want N@MS with 0 <= N <= 2147483647)";
            return false;
        }
        out.kind = kind[0] == 'd' ? Ev::Damage : Ev::Wanted;
        out.n = (int32_t)n;
        return true;
    }
    if (!std::strcmp(kind, "waypoint")) {
        float x = 0.f, y = 0.f, z = 0.f;
        unsigned long long at = 0;
        if (std::sscanf(spec, "%f,%f,%f@%llu", &x, &y, &z, &at) != 4) {
            err = std::string("bad --waypoint ") + spec + " (want X,Y,Z@MS)";
            return false;
        }
        out.kind = Ev::Waypoint;
        out.at = (uint64_t)at;
        out.x = x; out.y = y; out.z = z;
        return true;
    }
    err = std::string("unknown event kind ") + kind;
    return false;
}

void end_tick() {
    g_just_pressed.clear();
    g_just_released.clear();
    for (auto it = g_held.begin(); it != g_held.end();) {
        if (it->until <= world_now()) { g_just_released.push_back(it->ctl); it = g_held.erase(it); }
        else ++it;
    }
}

uint64_t now_ms() { return world_now(); }
int world_errors() { return g_world_errors; }
std::size_t entity_count() { return g_ents.size(); }

Player player() {
    Player out{};
    const Ent* p = by_handle(kPlayer);
    if (!p) return out;
    out.exists = true;
    out.health = p->health;
    out.max_health = p->max_health;
    out.invincible = p->invincible;
    out.can_ragdoll = p->can_ragdoll;
    out.x = p->x; out.y = p->y; out.z = p->z;
    if (const Ent* v = p->in_vehicle ? by_handle(p->in_vehicle) : nullptr) {
        out.vehicle = v->h;
        out.vehicle_health = v->health;
    }
    return out;
}
int32_t wanted_level() { return g_wanted; }
int32_t max_wanted_level() { return g_max_wanted; }
float time_scale() { return g_time_scale; }
int64_t clock_seconds() { return g_clock; }
bool clock_paused() { return g_clock_paused; }

void print_summary(std::FILE* out) {
    if (const Ent* p = by_handle(kPlayer))
        std::fprintf(out, "world: player health %d/%d invincible %d ragdoll %d at %.1f %.1f %.1f; wanted %d/%d; "
                          "clock %02lld:%02lld:%02lld%s; time scale %.2f\n",
                     p->health, p->max_health, p->invincible ? 1 : 0, p->can_ragdoll ? 1 : 0, p->x, p->y, p->z,
                     g_wanted, g_max_wanted, (long long)(g_clock / 3600), (long long)(g_clock / 60 % 60),
                     (long long)(g_clock % 60), g_clock_paused ? " (paused)" : "", g_time_scale);
}

} // namespace g6mock

// ---------------------------------------------------------------- mock UI driver
namespace g6mock {

bool parse_ui_event(const char* kind, const char* spec, UiEvent& out, std::string& err) {
    out = UiEvent{ 0, UiEv::Click, {}, {} };
    if (!kind || !spec) { err = "missing value"; return false; }
    const std::string s = spec;
    const std::size_t at_pos = s.rfind('@');
    if (at_pos == std::string::npos || at_pos == 0 || at_pos + 1 >= s.size()) {
        err = std::string("bad --") + kind + " " + s + " (want ...@MS)";
        return false;
    }
    char* end = nullptr;
    out.at = std::strtoull(s.c_str() + at_pos + 1, &end, 10);
    if (!end || *end != '\0') { err = std::string("bad --") + kind + " " + s + " (MS is not a number)"; return false; }
    out.label = s.substr(0, at_pos);
    if (!std::strcmp(kind, "ui-click")) {
        out.kind = UiEv::Click;
        return true;
    }
    if (!std::strcmp(kind, "ui-set")) {
        const std::size_t eq = out.label.find('=');
        if (eq == std::string::npos || eq == 0) { err = "bad --ui-set " + s + " (want LABEL=VALUE@MS)"; return false; }
        out.kind = UiEv::Set;
        out.value = out.label.substr(eq + 1);
        out.label.resize(eq);
        return true;
    }
    if (!std::strcmp(kind, "ui-menu")) {
        if (out.label != "0" && out.label != "1") { err = "bad --ui-menu " + s + " (want 0@MS or 1@MS)"; return false; }
        out.kind = UiEv::Menu;
        return true;
    }
    err = std::string("unknown UI event kind ") + kind;
    return false;
}

void fire_ui(const UiEvent& ev) {
    switch (ev.kind) {
    case UiEv::Click: g6_mock_ui_click(ev.label.c_str()); break;
    case UiEv::Set: g6_mock_ui_set(ev.label.c_str(), ev.value.c_str()); break;
    case UiEv::Menu: g6_mock_ui_set_menu_open(ev.label == "1" ? 1 : 0); break;
    }
}

void UiLog::add(std::uint64_t ms, const std::string& transcript) {
    ++frames_;
    last_ = transcript;
    if (!runs_.empty() && !runs_.back().closed && runs_.back().transcript == transcript) runs_.back().last = ms;
    else runs_.push_back({ ms, ms, transcript, false });
}

bool UiLog::contains(const std::string& text) const {
    for (const Run& r : runs_)
        if (r.transcript.find(text) != std::string::npos) return true;
    return false;
}

std::string UiLog::text() const {
    std::string out;
    for (const Run& r : runs_) {
        out += "@ " + std::to_string(r.first);
        if (r.last != r.first) out += "-" + std::to_string(r.last);
        out += " ms\n" + r.transcript;
    }
    return out;
}

int draw_ui_frame(std::uint64_t ms, UiLog& log) {
    const int ran = g6_mock_ui_frame();
    if (ran > 0) log.add(ms, g6_mock_ui_transcript());
    else log.gap();
    return ran;
}

std::string ui_problems(bool check_registered) {
    std::string out;
    char line[200];
    if (const int n = g6_mock_ui_errors()) {
        std::snprintf(line, sizeof line, "%d unbalanced UI call(s)\n", n);
        out += line;
    }
    if (const int n = g6_mock_ui_pending()) {
        std::snprintf(line, sizeof line, "%d UI interaction(s) never reached a widget (label typo, or the widget was not drawn)\n", n);
        out += line;
    }
    if (check_registered) {
        if (const int n = g6_mock_ui_registered()) {
            std::snprintf(line, sizeof line, "%d UI frame(s) left registered after unload\n", n);
            out += line;
        }
    }
    return out;
}

std::string read_ui_log(const std::string& path, bool& ok) {
    ok = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    while (!text.empty() && text[0] == '#') {
        const std::size_t nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? text.size() : nl + 1);
    }
    ok = true;
    return text;
}

std::string compare_ui_log(const std::string& got, const std::string& path) {
    bool ok = false;
    const std::string want = read_ui_log(path, ok);
    if (!ok) return "cannot read " + path;
    if (want == got) return {};
    std::size_t a = 0, b = 0, line = 1;
    while (a < got.size() && b < want.size()) {
        const std::size_t ea = got.find('\n', a), eb = want.find('\n', b);
        const std::string la = got.substr(a, ea == std::string::npos ? std::string::npos : ea - a);
        const std::string lb = want.substr(b, eb == std::string::npos ? std::string::npos : eb - b);
        if (la != lb) return "line " + std::to_string(line) + ": got \"" + la + "\", " + path + " has \"" + lb + "\"";
        if (ea == std::string::npos || eb == std::string::npos) break;
        a = ea + 1;
        b = eb + 1;
        ++line;
    }
    return got.size() < want.size() ? "got " + std::to_string(line) + " line(s); " + path + " has more"
                                     : "got more lines than " + path + " (" + std::to_string(line) + ")";
}

} // namespace g6mock
