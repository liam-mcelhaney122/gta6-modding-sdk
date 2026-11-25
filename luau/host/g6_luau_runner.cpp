// g6_luau_runner — run Luau mods offline against the mock runtime (gta6mod_mock.dll).
//   g6_luau_runner <lib_dir> <mod_init.luau> [ms=2000] [--press CTL@MS]... [--damage N@MS]...
//                  [--wanted N@MS]... [--waypoint X,Y,Z@MS]... [--expect-clean] [--expect-log TEXT]...
// Registers fake natives with the same semantics as the Python mock world
// (sdk/python/gta6mod/runtime/mock.py; the contract is "Mock world" in sdk/docs/script_layer.md)
// so a Luau mod and its Python twin behave the same: player ped 0x100 at (-1000, -2000, 15),
// spawned handles from 0x101, models load 50 ms after REQUEST_MODEL, CREATE_VEHICLE returns 0
// until then, a pressed control is "just pressed" for one tick only, flat ground at z = 12 once
// collision is loaded (100 ms after the first request for a cell), wanted level 0 / max 5, clock
// 12:00:00. Each native is registered under its seed (GTA V) hash and, where re-db has one, the
// observed GTA VI hash.
// Scenario events fire on the tick whose time equals MS (use multiples of 16):
//   --press CTL@MS        control CTL "just pressed" on pad 0 for that tick
//   --damage N@MS         player ped (and the vehicle it is in) lose N health unless invincible
//   --wanted N@MS         wanted level = N (a crime: the max wanted level is not applied)
//   --waypoint X,Y,Z@MS   set the map waypoint
//   --expect-clean        exit 1 if anything was logged at ERROR (failed script, failed load)
//   --expect-log TEXT     exit 1 unless some log message contains TEXT (repeatable: all must appear)
#include "g6_luau.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
};
struct Model { uint32_t hash; uint64_t loaded_at; };
struct Cell { long x, y; uint64_t loaded_at; };
enum class Ev { Press, Damage, Wanted, Waypoint };
struct Event { uint64_t at; Ev kind; int32_t n; float x, y, z; };
struct Expect { std::string text; bool seen; };

constexpr uint64_t kModelLoadMs = 50;
constexpr uint64_t kCollisionLoadMs = 100;
constexpr float    kGroundZ = 12.f;
constexpr uint64_t kPlayer = 0x100;
constexpr double   kPi = 3.14159265358979323846;

const g6_host_api*   g_mock = nullptr;   // the mock DLL's table
g6_host_api          g_api{};            // what the Luau host sees: mock + counting log/invoke
std::vector<Ent>     g_ents;
std::vector<Model>   g_models;
std::vector<Cell>    g_cells;
std::vector<int32_t> g_pressed;          // controls "just pressed" on pad 0 this tick
uint64_t             g_next_handle = kPlayer;
uint64_t             g_native_calls = 0;
int                  g_errors = 0;
std::vector<Expect>  g_expect;
int32_t              g_wanted = 0, g_max_wanted = 5;
bool                 g_waypoint = false;
float                g_wp[3] = {};
int64_t              g_clock = 12 * 3600;  // seconds since midnight
bool                 g_clock_paused = false;
float                g_time_scale = 1.f;

uint64_t now_ms() { return g_mock->game_time_ms(); }

uint64_t spawn(bool vehicle, uint32_t model, float x, float y, float z, float heading) {
    const int32_t hp = vehicle ? 1000 : 200;
    g_ents.push_back({ g_next_handle, vehicle, model, x, y, z, heading, hp, hp, false, true, false, true, 0.f, 0.f, 0.f, 0 });
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
void G6_CALL n_game_timer(g6_native_call* c, void*) { c->ret[0] = now_ms() & 0xFFFFFFFFu; }
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
    if (!md || now_ms() < md->loaded_at) { c->ret[0] = 0; c->ret[1] = 0; return; }  // game returns 0 when the model is not loaded
    c->ret[0] = spawn(true, m, f(c->args[1]), f(c->args[2]), f(c->args[3]), f(c->args[4]));
    c->ret[1] = 0;
}
void G6_CALL n_set_fixed(g6_native_call* c, void*) { if (Ent* e = ent(c->args)) e->health = e->max_health; }

// ---- streaming / world
void G6_CALL n_request_model(g6_native_call* c, void*) {
    const uint32_t m = (uint32_t)c->args[0];
    if (!model(m)) g_models.push_back({ m, now_ms() + kModelLoadMs });
}
void G6_CALL n_model_loaded(g6_native_call* c, void*) {
    const Model* m = model((uint32_t)c->args[0]);
    c->ret[0] = (m && now_ms() >= m->loaded_at) ? 1 : 0;
}
void G6_CALL n_release_model(g6_native_call*, void*) {}
void G6_CALL n_model_valid(g6_native_call* c, void*) { c->ret[0] = ((uint32_t)c->args[0] != 0) ? 1 : 0; }
void G6_CALL n_request_collision(g6_native_call* c, void*) {
    const g6_vec3 p = vec_at(c->args);
    const float x = p.x, y = p.y;
    if (!cell(x, y)) g_cells.push_back({ std::lround(x), std::lround(y), now_ms() + kCollisionLoadMs });
}
void G6_CALL n_ground_z(g6_native_call* c, void*) {  // 'wFV>b': packed coord 0-1, float* 2, Vector3* 3
    const g6_vec3 p = vec_at(c->args);
    const Cell* k = cell(p.x, p.y);
    const bool found = k && now_ms() >= k->loaded_at && p.z > kGroundZ;
    auto* gz = (int64_t*)(uintptr_t)c->args[2];
    auto* nv = (g6_vec3_packed*)(uintptr_t)c->args[3];
    if (gz) *gz = (int64_t)g6_arg_float(found ? kGroundZ : 0.f);
    if (nv && ((uintptr_t)nv & 15u) != 0) {  // the game stores it with vmovaps, which would fault
        std::fprintf(stderr, "[ERROR] GET_GROUND_Z_AND_NORMAL: Vector3* out-param not 16-byte aligned\n");
        ++g_errors;
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
void G6_CALL n_just_pressed(g6_native_call* c, void*) {  // also IS_CONTROL_PRESSED: a mock press lasts one tick
    const int32_t pad = i32(c->args[0]), ctl = i32(c->args[1]);
    c->ret[0] = 0;
    if (pad == 0) for (int32_t p : g_pressed) if (p == ctl) c->ret[0] = 1;
}
void G6_CALL n_false(g6_native_call* c, void*) { c->ret[0] = 0; }
void G6_CALL n_nop(g6_native_call*, void*) {}

// ---- wanted level
void G6_CALL n_wanted(g6_native_call* c, void*) { c->ret[0] = (uint64_t)(int64_t)g_wanted; }
void G6_CALL n_clear_wanted(g6_native_call*, void*) { g_wanted = 0; }
void G6_CALL n_max_wanted(g6_native_call* c, void*) { c->ret[0] = (uint64_t)(int64_t)g_max_wanted; }
void G6_CALL n_set_max_wanted(g6_native_call* c, void*) {
    g_max_wanted = i32(c->args[0]);
    g_wanted = std::min(g_wanted, g_max_wanted);
}

void fire(const Event& ev) {
    switch (ev.kind) {
    case Ev::Press: g_pressed.push_back(ev.n); break;
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

void G6_CALL counting_log(g6_log_level lvl, const char* who, const char* msg) {
    if (lvl >= G6_LOG_ERROR) ++g_errors;
    if (msg) for (auto& e : g_expect) if (std::strstr(msg, e.text.c_str())) e.seen = true;
    g_mock->log(lvl, who, msg);
}
g6_status G6_CALL counting_invoke(g6_native_handle h, g6_native_call* c) {
    ++g_native_calls;
    return g_mock->native_invoke(h, c);
}

// "N@MS" -> (N, MS); "X,Y,Z@MS" for waypoints.
bool parse_at(const char* spec, uint64_t& at) {
    const char* a = std::strchr(spec, '@');
    at = a ? std::strtoull(a + 1, nullptr, 10) : 0;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <lib_dir> <init.luau> [ms] [--press CTL@MS]... [--damage N@MS]... [--wanted N@MS]..."
                             " [--waypoint X,Y,Z@MS]... [--expect-clean] [--expect-log TEXT]...\n", argv[0]);
        return 2;
    }
    uint64_t run_ms = 2000;
    bool expect_clean = false;
    std::vector<Event> events;
    for (int i = 3; i < argc; ++i) {
        const bool has_val = i + 1 < argc;
        if (!std::strcmp(argv[i], "--press") && has_val) {
            const char* spec = argv[++i];
            uint64_t at; parse_at(spec, at);
            events.push_back({ at, Ev::Press, (int32_t)std::atoi(spec), 0.f, 0.f, 0.f });
        } else if (!std::strcmp(argv[i], "--press-ms") && has_val) {  // older form: control 51 at N ms
            events.push_back({ std::strtoull(argv[++i], nullptr, 10), Ev::Press, 51, 0.f, 0.f, 0.f });
        } else if ((!std::strcmp(argv[i], "--damage") || !std::strcmp(argv[i], "--wanted")) && has_val) {
            const Ev kind = argv[i][2] == 'd' ? Ev::Damage : Ev::Wanted;
            const char* spec = argv[++i];
            uint64_t at; parse_at(spec, at);
            char* end = nullptr;
            const long long n = std::strtoll(spec, &end, 10);
            if (end == spec || (*end != '@' && *end != '\0') || n < 0 || n > INT32_MAX) {
                std::fprintf(stderr, "bad %s %s (want N@MS with 0 <= N <= %d)\n", argv[i - 1], spec, INT32_MAX);
                return 2;
            }
            events.push_back({ at, kind, (int32_t)n, 0.f, 0.f, 0.f });
        } else if (!std::strcmp(argv[i], "--waypoint") && has_val) {
            const char* spec = argv[++i];
            float x = 0.f, y = 0.f, z = 0.f;
            unsigned long long at = 0;
            if (std::sscanf(spec, "%f,%f,%f@%llu", &x, &y, &z, &at) != 4) {
                std::fprintf(stderr, "bad --waypoint %s (want X,Y,Z@MS)\n", spec);
                return 2;
            }
            events.push_back({ (uint64_t)at, Ev::Waypoint, 0, x, y, z });
        } else if (!std::strcmp(argv[i], "--expect-clean")) {
            expect_clean = true;
        } else if (!std::strcmp(argv[i], "--expect-log") && has_val) {
            g_expect.push_back({ argv[++i], false });
        } else if (argv[i][0] != '-') {
            run_ms = std::strtoull(argv[i], nullptr, 10);
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    g_mock = g6_get_host_api();
    if (!g_mock || g_mock->version_major != G6_API_VERSION_MAJOR || g_mock->size < sizeof(g6_host_api)) {
        std::fprintf(stderr, "gta6mod_mock.dll: incompatible host API\n");
        return 1;
    }
    g_api = *g_mock;
    g_api.log = counting_log;
    g_api.native_invoke = counting_invoke;

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
        { 0x4D1A590C92BF377Eull, n_pause_clock },    { 0xF3A21BCD95725A4Aull, n_just_pressed },
        { 0x50F940259D3841E6ull, n_false },          { 0xFE99B66D079CF6BCull, n_nop },
        { 0xABC532F9098BFD9Dull, n_wanted },         { 0x4E4B996C928C7AA6ull, n_clear_wanted },
        { 0xD04CFAD1E2B7984Aull, n_max_wanted },     { 0x28A4BD2CEE236E19ull, n_set_max_wanted },
    };
    for (const auto& fk : fakes) g6_mock_register_native(fk.hash, fk.fn, nullptr);

    g6_luau_config cfg = { sizeof(g6_luau_config), argv[1], ".", 1 /* mock: allow unverified */ };
    g6_luau_host* host = g6_luau_create(&g_api, &cfg);
    if (!host) { std::fprintf(stderr, "g6_luau_create failed\n"); return 1; }
    if (g6_luau_run_file(host, argv[2]) != 0) { g6_luau_destroy(host); return 1; }

    while (now_ms() < run_ms) {
        for (const Event& ev : events) if (ev.at == now_ms()) fire(ev);
        g6_luau_tick(host);
        g6_mock_step(16);
        g_pressed.clear();
    }
    std::printf("ran %llu ms; %llu native calls; %d live scripts; %d errors; %zu entities\n",
                (unsigned long long)now_ms(), (unsigned long long)g_native_calls, g6_luau_script_count(host), g_errors, g_ents.size());
    if (const Ent* p = by_handle(kPlayer))
        std::printf("world: player health %d/%d invincible %d ragdoll %d at %.1f %.1f %.1f; wanted %d/%d; "
                    "clock %02lld:%02lld:%02lld%s; time scale %.2f\n",
                    p->health, p->max_health, p->invincible ? 1 : 0, p->can_ragdoll ? 1 : 0, p->x, p->y, p->z,
                    g_wanted, g_max_wanted, (long long)(g_clock / 3600), (long long)(g_clock / 60 % 60),
                    (long long)(g_clock % 60), g_clock_paused ? " (paused)" : "", g_time_scale);
    g6_luau_destroy(host);
    if (expect_clean && g_errors) { std::fprintf(stderr, "FAIL: %d error(s) logged\n", g_errors); return 1; }
    int missing = 0;
    for (const auto& e : g_expect)
        if (!e.seen) { std::fprintf(stderr, "FAIL: no log message containing \"%s\"\n", e.text.c_str()); ++missing; }
    return missing ? 1 : 0;
}
