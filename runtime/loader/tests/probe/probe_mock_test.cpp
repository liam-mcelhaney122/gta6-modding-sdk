// probe_mock_test.cpp - runs g6probe.dll as a plugin against the offline mock runtime (gta6mod_mock.dll,
// sdk/runtime/src/mock_backend.c) and a fake title image (fake_title.dll) that holds the anchored data.
//
// The mock provides native dispatch, script ticks and the game clock. This driver registers fake
// natives for a small world (one player ped, coordinates, health, wanted level, clock, a vehicle) and
// replaces anchor_address in the host table so the probe's memory path reads fake_title.dll's globals
// (player ped, gravity, ECS stores) exactly as it reads the title's in game.
//
// Usage: probe_mock_test.exe <gta6mod_mock.dll> <g6probe.dll> <fake_title.dll> <work dir>
// Each scenario gets <work dir>\<name>\ with a copy of g6probe.dll, its g6probe.ini and the JSONL output.
#include "gta6mod/gta6mod.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) { ++g_pass; }                                       \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

// ------------------------------------------------------------------ hashes (natives.h, the probe's spec table)
constexpr std::uint64_t H_PLAYER_PED_ID = 0x096275889B8E0EE0ull;
constexpr std::uint64_t H_GET_PLAYER_INDEX = 0x47E385B0D957C8D4ull;
constexpr std::uint64_t H_GET_ENTITY_COORDS = 0xA86D5F069399F44Dull;
constexpr std::uint64_t H_GET_ENTITY_HEADING = 0xC230DD956E2F5507ull;
constexpr std::uint64_t H_GET_ENTITY_HEALTH = 0x82368787EA73C0F7ull;
constexpr std::uint64_t H_GET_ENTITY_MAX_HEALTH = 0x15D757606D170C3Cull;
constexpr std::uint64_t H_IS_PED_IN_ANY_VEHICLE = 0x997ABD671D25CA0Bull;
constexpr std::uint64_t H_GET_VEHICLE_PED_IS_USING = 0x6094AD011A2EA87Dull;
constexpr std::uint64_t H_GET_PLAYER_WANTED_LEVEL = 0xABC532F9098BFD9Dull;
constexpr std::uint64_t H_GET_GAME_TIMER = 0x4F67E8ECA7D3F667ull;
constexpr std::uint64_t H_GET_FRAME_COUNT = 0x77DFA958FCF100C1ull;
constexpr std::uint64_t H_GET_CLOCK_HOURS = 0xC82CF208C2B19199ull;
constexpr std::uint64_t H_GET_CLOCK_MINUTES = 0x4E162231B823DBBFull;
constexpr std::uint64_t H_SET_ENTITY_COORDS = 0x06843DA7060A026Bull;
constexpr std::uint64_t H_SET_ENTITY_HEALTH = 0xAC2767ED8BDFAB15ull;
constexpr std::uint64_t H_CLEAR_PLAYER_WANTED_LEVEL = 0x4E4B996C928C7AA6ull;
constexpr std::uint64_t H_ADD_TO_CLOCK_TIME = 0xAB7C251C7701D336ull;
constexpr std::uint64_t H_GET_PLAYERS_LAST_VEHICLE = 0x2F96E7720B0B19EAull;
constexpr std::uint64_t H_GET_CLOSEST_VEHICLE = 0x52F45D033645181Bull;
constexpr std::uint64_t H_GET_CLOSEST_PED = 0xC33AB876A77F8164ull;

// ------------------------------------------------------------------ the world
constexpr std::uint64_t kPedLo = 0x00000001000000A5ull, kPedHi = 0x7F3C00000000BEEFull;
constexpr std::uint64_t kVehLo = 0x0000000200000077ull, kVehHi = 0x7F3C00000000CAFEull;
constexpr std::uint64_t kOtherLo = 0x00000003000000B1ull, kOtherHi = 0x7F3C00000000D00Dull;   // a second ped
constexpr std::uint32_t kRow = 5, kStaticN = 0x4d, kGenericN = 0x3d;

struct World {
    double x = 100.0, y = 200.0, z = 30.0;
    float heading = 90.0f;
    int health = 180, max_health = 200;
    bool in_vehicle = false;
    bool had_vehicle = false;   // GET_PLAYERS_LAST_VEHICLE answers once the player sat in the vehicle
    double other_x = 103.0, veh_x = 105.0;   // the second ped and the parked vehicle, both at y 200, z 30
    int wanted = 2;
    int hours = 12, minutes = 0;
    std::uint32_t frames = 0;
    int set_coords = 0, set_health = 0, clear_wanted = 0, add_clock = 0;
    int bad_args = 0;
    int last_health_arg = 0;
};
World g_w;
const g6_host_api* g_mock = nullptr;

bool is_ped(const g6_native_call* c) { return c->argc >= 2 && c->args[0] == kPedLo && c->args[1] == kPedHi; }
bool is_handle(const g6_native_call* c, std::uint64_t lo, std::uint64_t hi) { return c->argc >= 2 && c->args[0] == lo && c->args[1] == hi; }
g6_vec3 veh_pos() {
    return g_w.in_vehicle ? g6_vec3{ static_cast<float>(g_w.x), static_cast<float>(g_w.y), static_cast<float>(g_w.z) }
                          : g6_vec3{ static_cast<float>(g_w.veh_x), 200.0f, 30.0f };
}
g6_vec3 other_pos() { return g6_vec3{ static_cast<float>(g_w.other_x), 200.0f, 30.0f }; }
float dist3(g6_vec3 a, g6_vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
void need(const g6_native_call* c, std::uint32_t argc) { if (c->argc != argc) ++g_w.bad_args; }

void G6_CALL n_ped(g6_native_call* c, void*) { need(c, 0); c->ret[0] = kPedLo; c->ret[1] = kPedHi; }
void G6_CALL n_player(g6_native_call* c, void*) { need(c, 0); c->ret[0] = 0; }
void G6_CALL n_coords(g6_native_call* c, void*) {
    need(c, 4);
    if (is_ped(c)) {
        g6_vec3 v{ static_cast<float>(g_w.x), static_cast<float>(g_w.y), static_cast<float>(g_w.z) };
        g6_set_vec3_packed(c->ret, v);
    } else if (is_handle(c, kVehLo, kVehHi)) {
        g6_set_vec3_packed(c->ret, veh_pos());
    } else if (is_handle(c, kOtherLo, kOtherHi)) {
        g6_set_vec3_packed(c->ret, other_pos());
    }
}
// GET_PLAYERS_LAST_VEHICLE ">e": no argument, the vehicle once the player has sat in it.
void G6_CALL n_last_vehicle(g6_native_call* c, void*) {
    need(c, 0);
    if (g_w.had_vehicle) { c->ret[0] = kVehLo; c->ret[1] = kVehHi; }
}
// GET_CLOSEST_VEHICLE "wfui>e": packed position in slots 0-1, radius slot 2, model 3 (0 = any), flags 4.
void G6_CALL n_closest_vehicle(g6_native_call* c, void*) {
    need(c, 5);
    if (c->argc != 5 || c->args[3] != 0) return;
    const g6_vec3 p = g6_slots_vec3_packed(&c->args[0]);
    float r = 0;
    std::memcpy(&r, &c->args[2], 4);
    if (dist3(p, veh_pos()) <= r) { c->ret[0] = kVehLo; c->ret[1] = kVehHi; }
}
// GET_CLOSEST_PED "wfbbEbbbi>b": slot 5 points at 16 bytes the handler fills (the handle, or zeros); returns found.
void G6_CALL n_closest_ped(g6_native_call* c, void*) {
    need(c, 10);
    if (c->argc != 10 || !c->args[5]) { ++g_w.bad_args; return; }
    const g6_vec3 p = g6_slots_vec3_packed(&c->args[0]);
    float r = 0;
    std::memcpy(&r, &c->args[2], 4);
    const bool found = dist3(p, other_pos()) <= r;
    const std::uint64_t h[2] = { found ? kOtherLo : 0, found ? kOtherHi : 0 };
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(c->args[5])), h, 16);
    c->ret[0] = found ? 1 : 0;
}
void G6_CALL n_heading(g6_native_call* c, void*) { need(c, 2); if (is_ped(c)) c->ret[0] = g6_arg_float(g_w.heading); }
void G6_CALL n_health(g6_native_call* c, void*) { need(c, 2); if (is_ped(c)) c->ret[0] = static_cast<std::uint64_t>(g_w.health); }
void G6_CALL n_max_health(g6_native_call* c, void*) { need(c, 3); if (is_ped(c)) c->ret[0] = static_cast<std::uint64_t>(g_w.max_health); }
void G6_CALL n_in_vehicle(g6_native_call* c, void*) { need(c, 5); if (is_ped(c)) c->ret[0] = g_w.in_vehicle ? 1 : 0; }
void G6_CALL n_vehicle(g6_native_call* c, void*) {
    need(c, 3);
    if (is_ped(c) && g_w.in_vehicle) { c->ret[0] = kVehLo; c->ret[1] = kVehHi; }
}
// GET/CLEAR_PLAYER_WANTED_LEVEL take the player ped (2 slots, G-090): any other argument reads 0 / clears nothing.
void G6_CALL n_wanted(g6_native_call* c, void*) { need(c, 2); c->ret[0] = is_ped(c) ? static_cast<std::uint64_t>(g_w.wanted) : 0; }
void G6_CALL n_timer(g6_native_call* c, void*) { need(c, 0); c->ret[0] = static_cast<std::uint32_t>(g_mock->game_time_ms()); }
void G6_CALL n_frames(g6_native_call* c, void*) { need(c, 0); c->ret[0] = g_w.frames; }
void G6_CALL n_hours(g6_native_call* c, void*) { need(c, 0); c->ret[0] = static_cast<std::uint64_t>(g_w.hours); }
void G6_CALL n_minutes(g6_native_call* c, void*) { need(c, 0); c->ret[0] = static_cast<std::uint64_t>(g_w.minutes); }
void G6_CALL n_set_coords(g6_native_call* c, void*) {
    need(c, 9);
    ++g_w.set_coords;
    if (!is_ped(c)) return;
    const g6_vec3 v = g6_slots_vec3_packed(&c->args[2]);   // packed {x,y,z} over slots 2-3 (findings/phase5/23)
    g_w.x = v.x;
    g_w.y = v.y;
    g_w.z = v.z;
}
void G6_CALL n_set_health(g6_native_call* c, void*) {
    need(c, 7);
    ++g_w.set_health;
    if (!is_ped(c)) return;
    g_w.last_health_arg = static_cast<int>(static_cast<std::int32_t>(c->args[2]));
    g_w.health = g_w.last_health_arg;
}
void G6_CALL n_clear_wanted(g6_native_call* c, void*) { need(c, 2); ++g_w.clear_wanted; if (is_ped(c)) g_w.wanted = 0; }
void G6_CALL n_add_clock(g6_native_call* c, void*) {
    need(c, 3);
    ++g_w.add_clock;
    g_w.hours = (g_w.hours + static_cast<int>(c->args[0])) % 24;
}

// ------------------------------------------------------------------ fake memory (the ECS model of findings/phase5/24)
using AnchorFn = void* (*)(const char*);
AnchorFn g_fake_anchor = nullptr;
std::uint8_t* g_ped = nullptr;
std::uint16_t* g_static_index = nullptr;
std::uint8_t* g_static_columns = nullptr;
std::uint8_t* g_law = nullptr;
std::uint32_t* g_timer_block = nullptr;   // fake_title's world.game_timer: +0 time ms, +4 frame count
std::uint8_t* g_col47 = nullptr;          // the player's static column 0x47 component (last-vehicle pointers)
std::uint8_t* g_vehicle_obj = nullptr;    // the vehicle entity those pointers name

std::uint8_t* alloc(std::size_t n) {
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}
template <class T> void put(std::uint8_t* p, std::size_t off, T v) { std::memcpy(p + off, &v, sizeof v); }
void set_column(std::uint8_t* cols, std::uint32_t c, void* base, std::uint64_t stride) {
    put(cols, c * 16u, base);
    put(cols, c * 16u + 8u, stride);
}

void build_memory(HMODULE fake) {
    g_fake_anchor = reinterpret_cast<AnchorFn>(reinterpret_cast<void*>(GetProcAddress(fake, "fake_anchor")));
    auto ped_slot = reinterpret_cast<void** (*)()>(reinterpret_cast<void*>(GetProcAddress(fake, "fake_player_ped_slot")));
    auto arch = reinterpret_cast<std::uint8_t* (*)()>(reinterpret_cast<void*>(GetProcAddress(fake, "fake_arch_table")));
    auto srec = reinterpret_cast<std::uint8_t* (*)()>(reinterpret_cast<void*>(GetProcAddress(fake, "fake_static_ped_record")));
    auto timer = reinterpret_cast<std::uint32_t* (*)()>(reinterpret_cast<void*>(GetProcAddress(fake, "fake_game_timer")));
    g_timer_block = timer();
    g_ped = alloc(0x100);
    put<std::uint8_t>(g_ped, 0x30, 3);
    put<std::uint32_t>(g_ped, 0x9c, (3u << 17) | kRow);
    *ped_slot() = g_ped;

    // static ped store: row_index, columns, three components
    std::uint8_t* rec = srec();
    g_static_index = reinterpret_cast<std::uint16_t*>(alloc((kRow + 1) * kStaticN * 2));
    for (std::uint32_t i = 0; i < (kRow + 1) * kStaticN; ++i) g_static_index[i] = 0xffff;
    std::uint16_t* r = g_static_index + kRow * kStaticN;
    r[0x4a] = 2;
    r[0x32] = 1;
    r[0x0f] = 0;
    r[0x47] = 0;
    g_static_columns = alloc(kStaticN * 16);
    g_law = alloc(3 * 0xcce0);
    std::uint8_t* c32 = alloc(2 * 0xb80);
    std::uint8_t* c0f = alloc(0x1d30);
    set_column(g_static_columns, 0x4a, g_law, 0xcce0);
    set_column(g_static_columns, 0x32, c32, 0xb80);
    set_column(g_static_columns, 0x0f, c0f, 0x1d30);
    g_col47 = alloc(0xb30);
    set_column(g_static_columns, 0x47, g_col47, 0xb30);
    g_vehicle_obj = alloc(0x100);
    put<std::uint8_t>(g_vehicle_obj, 0x30, 2);
    put<std::uint32_t>(g_vehicle_obj, 0x9c, (2u << 17) | 9u);
    put<std::int32_t>(g_law, 2 * 0xcce0 + 0x5f2c, 2);
    put<std::int32_t>(g_law, 2 * 0xcce0 + 0x5f28, 150);
    put<std::uint32_t>(c32, 1 * 0xb80 + 0x234, 1u << 28);
    put<std::uint32_t>(c0f, 0x15d4, 0);
    put<std::uint32_t>(rec, 0x14, kStaticN);
    put<void*>(rec, 0x20, g_static_index);
    put<void*>(rec, 0x30, g_static_columns);

    // generic record 3: selectors for types 0x15 (health) and 0x17 (proofs)
    std::uint8_t* g = arch() + 3 * 0x238;
    std::memset(g + 0x38, 0xff, 0x200);
    g[0x38 + 0x15] = 0x18;
    g[0x38 + 0x17] = 0x1a;
    auto* gidx = reinterpret_cast<std::uint16_t*>(alloc((kRow + 1) * kGenericN * 2));
    for (std::uint32_t i = 0; i < (kRow + 1) * kGenericN; ++i) gidx[i] = 0xffff;
    gidx[kRow * kGenericN + 0x18] = 1;
    gidx[kRow * kGenericN + 0x1a] = 0;
    std::uint8_t* gcols = alloc(kGenericN * 16);
    std::uint8_t* health = alloc(2 * 0x8b0);
    std::uint8_t* proofs = alloc(0x28);
    std::uint8_t* host = alloc(0x200);
    std::uint8_t* data = alloc(0x48);
    set_column(gcols, 0x18, health, 0x8b0);
    set_column(gcols, 0x1a, proofs, 0x28);
    put<void*>(health, 0x8b0 + 0x20, host);
    put<void*>(host, 0x68, data);
    put<float>(data, 0x40, 180.0f);
    put<float>(data, 0x38, 200.0f);
    put<std::uint32_t>(proofs, 0x10, 1u << 4);
    put<std::uint32_t>(g, 0x14, kGenericN);
    put<void*>(g, 0x20, gidx);
    put<void*>(g, 0x30, gcols);
}

void enter_vehicle(bool in) {
    g_w.in_vehicle = in;
    g_static_index[kRow * kStaticN + 0x20] = in ? 7 : 0xffff;   // a component row appears while seated
    if (in) g_w.had_vehicle = true;
    // bit 5 of +0x10 clear, so GET_PLAYERS_LAST_VEHICLE's core takes +0x8a8 (probe_layout.hpp ecs::kPedCol47)
    put<std::uint8_t>(g_col47, 0x10, 0);
    put<void*>(g_col47, 0x898, nullptr);
    put<void*>(g_col47, 0x8a8, g_w.had_vehicle ? g_vehicle_obj : nullptr);
}

// ------------------------------------------------------------------ host table with fake anchors and a log capture
std::vector<std::string> g_log;
g6_host_api g_api;

void G6_CALL cap_log(g6_log_level level, const char* plugin, const char* message) {
    char b[2200];
    std::snprintf(b, sizeof b, "[%d] %s: %s", static_cast<int>(level), plugin ? plugin : "?", message ? message : "");
    g_log.push_back(b);
    std::printf("    log %s\n", b);
}
g6_status G6_CALL fake_anchor_address(const char* name, uintptr_t* out) {
    if (!out) return G6_E_INVALID_ARG;
    void* p = g_fake_anchor ? g_fake_anchor(name) : nullptr;
    *out = reinterpret_cast<uintptr_t>(p);
    return p ? G6_OK : G6_E_NO_SUCH_ANCHOR;
}

bool log_has(const char* needle) {
    for (const std::string& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

// ------------------------------------------------------------------ scenario plumbing
using RegFn = g6_status (G6_CALL*)(uint64_t, g6_mock_native_fn, void*);
using StepFn = g6_status (G6_CALL*)(uint32_t);
StepFn g_step = nullptr;
std::wstring g_probe_dll, g_work;

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::vector<std::string> read_lines(const std::wstring& path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l)) if (!l.empty()) out.push_back(l);
    return out;
}
int count_ev(const std::vector<std::string>& lines, const char* ev) {
    const std::string k = std::string("\"ev\":\"") + ev + "\"";
    int n = 0;
    for (const std::string& l : lines) n += l.find(k) != std::string::npos ? 1 : 0;
    return n;
}
const std::string* find_line(const std::vector<std::string>& lines, const char* a, const char* b = nullptr) {
    for (const std::string& l : lines)
        if (l.find(a) != std::string::npos && (!b || l.find(b) != std::string::npos)) return &l;
    return nullptr;
}

// 4 hex digits of slot `i` in a record's static ECS row.
std::string static_slot(const std::string& rec, std::size_t i) {
    const std::string k = "\"static\":{\"ok\":true,\"n\":77,\"row\":\"";
    const std::size_t p = rec.find(k);
    if (p == std::string::npos || p + k.size() + i * 4 + 4 > rec.size()) return "";
    return rec.substr(p + k.size() + i * 4, 4);
}

struct Plugin {
    HMODULE mod = nullptr;
    g6_plugin_unload_fn unload = nullptr;
    g6_status load_rc = G6_E_INTERNAL;
    std::wstring dir;
};

Plugin start(const char* name, const std::string& ini) {
    Plugin p;
    p.dir = g_work + L"\\" + widen(name);
    CreateDirectoryW(p.dir.c_str(), nullptr);
    DeleteFileW((p.dir + L"\\g6probe.jsonl").c_str());
    DeleteFileW((p.dir + L"\\g6probe.fire").c_str());
    const std::wstring dll = p.dir + L"\\g6probe.dll";
    if (!CopyFileW(g_probe_dll.c_str(), dll.c_str(), FALSE)) { std::printf("cannot copy the probe DLL (%lu)\n", GetLastError()); return p; }
    { std::ofstream f(p.dir + L"\\g6probe.ini"); f << ini; }
    g_log.clear();
    p.mod = LoadLibraryW(dll.c_str());
    if (!p.mod) { std::printf("LoadLibrary failed (%lu)\n", GetLastError()); return p; }
    auto query = reinterpret_cast<g6_plugin_query_fn>(reinterpret_cast<void*>(GetProcAddress(p.mod, "g6_plugin_query")));
    auto load = reinterpret_cast<g6_plugin_load_fn>(reinterpret_cast<void*>(GetProcAddress(p.mod, "g6_plugin_load")));
    p.unload = reinterpret_cast<g6_plugin_unload_fn>(reinterpret_cast<void*>(GetProcAddress(p.mod, "g6_plugin_unload")));
    const g6_plugin_info* info = query ? query() : nullptr;
    CHECK(info && info->size >= sizeof(g6_plugin_info) && info->api_major == G6_API_VERSION_MAJOR && std::strcmp(info->name, "g6probe") == 0,
          "%s: plugin info", name);
    p.load_rc = load ? load(&g_api) : G6_E_INTERNAL;
    return p;
}

void stop(Plugin& p) {
    if (p.unload) p.unload();
    if (p.mod) FreeLibrary(p.mod);
    p.mod = nullptr;
}

// The fake timer block mirrors the mock's game clock as the script tick of this step sees it (the mock runs
// the tick before it advances the clock), so the raw read and GET_GAME_TIMER agree.
void steps(int n, std::uint32_t ms) {
    for (int i = 0; i < n; ++i) {
        ++g_w.frames;
        g_timer_block[0] = static_cast<std::uint32_t>(g_mock->game_time_ms());
        g_timer_block[1] = g_w.frames;
        g_step(ms);
    }
}

void fire(const Plugin& p) {
    { std::ofstream f(p.dir + L"\\g6probe.fire"); f << "x"; }
    Sleep(260);        // the probe polls the trigger file at most every 200 ms of wall time
    steps(1, 10);
}

int setters() { return g_w.set_coords + g_w.set_health + g_w.clear_wanted + g_w.add_clock; }

const char* kBase = "[probe]\nrate_hz=2\nrate_clock=game\nanchor_rva_check=0\n";

// ------------------------------------------------------------------ scenarios
void scenario_stage3() {
    std::printf("scenario stage3_read\n");
    g_w = World{};
    enter_vehicle(false);
    Plugin p = start("stage3_read", std::string(kBase) + "stage=3\n[writes]\nallow=coords_up,health\nup_axis=z\n");
    CHECK(p.load_rc == G6_OK, "stage 3 loads (rc %d)", static_cast<int>(p.load_rc));
    CHECK(log_has("allow is ignored: stage 3 is read-only"), "stage 3 warns that the allow list is ignored");
    CHECK(log_has("anchor ok: world.game_timer"), "game timer anchor resolved");
    CHECK(log_has("anchor ok: world.physics.gravity"), "gravity anchor resolved");
    steps(20, 100);                                   // 2 s standing still
    { std::ofstream f(p.dir + L"\\g6probe.fire"); f << "x"; }
    Sleep(260);
    for (int i = 0; i < 20; ++i) { g_w.x += 0.6; steps(1, 100); }   // walk 12 m along x
    steps(5, 100);
    enter_vehicle(true);
    steps(15, 100);
    stop(p);
    CHECK(GetFileAttributesW((p.dir + L"\\g6probe.fire").c_str()) != INVALID_FILE_ATTRIBUTES, "stage 3 never consumes the trigger file");
    CHECK(setters() == 0, "stage 3 called no setter (%d)", setters());
    CHECK(g_w.bad_args == 0, "every native got its signature's slot count (%d bad)", g_w.bad_args);
    CHECK(log_has("first tick on the script thread"), "first tick logged");
    const auto lines = read_lines(p.dir + L"\\g6probe.jsonl");
    CHECK(count_ev(lines, "meta") == 1, "one meta record");
    const int samples = count_ev(lines, "sample");
    CHECK(samples >= 11 && samples <= 13, "2 Hz over 6 s of game time: %d samples", samples);
    const std::string* m = find_line(lines, "\"ev\":\"meta\"");
    CHECK(m && m->find("\"stage\":3") != std::string::npos && m->find("\"allow\":[]") != std::string::npos, "meta: stage 3, empty allow");
    CHECK(m && m->find("\"bind\":\"ok\",\"status\":\"observed\"") != std::string::npos && m->find("not in the generated") == std::string::npos,
          "meta: natives bound against natives.h");
    const std::string* s = find_line(lines, "\"ev\":\"sample\"");
    CHECK(s && s->find("\"ped\":{\"lo\":\"0x00000001000000a5\",\"hi\":\"0x7f3c00000000beef\",\"valid\":true}") != std::string::npos, "ped handle, both qwords raw");
    CHECK(s && s->find("\"coords\":{\"ok\":true,\"v\":[100,200,30]}") != std::string::npos, "coords");
    CHECK(s && s->find("\"heading\":90") != std::string::npos && s->find("\"health\":180") != std::string::npos, "heading, health");
    CHECK(s && s->find("\"gravity\":{\"ok\":true") != std::string::npos && s->find("\"v\":[0,-9.80000019,0,0]") != std::string::npos, "gravity read");
    CHECK(s && s->find("\"arch\":3,\"row\":5") != std::string::npos && s->find("\"ecs_id\":\"0x00060005\"") != std::string::npos, "ped ECS id");
    CHECK(s && s->find("\"wanted_level\":2,\"wanted_score\":150") != std::string::npos, "wanted level from the law component");
    CHECK(s && s->find("\"wanted\":2") != std::string::npos, "GET_PLAYER_WANTED_LEVEL gets the player ped (G-090)");
    CHECK(s && s->find("\"ped_gravity_disabled\":true") != std::string::npos && s->find("\"invincible\":true") != std::string::npos,
          "col32 and proof bits");
    CHECK(s && s->find("\"health_f\":180,\"max_health_f\":200") != std::string::npos, "health through sel4d -> host -> data");
    CHECK(s && s->find("\"gated\":{}") != std::string::npos, "nothing gated in the fake image");
    CHECK(s && s->find("\"clock_raw\":{\"ok\":true,\"addr\":") != std::string::npos && s->find("\"minus_native_ms\":0}") != std::string::npos,
          "raw game timer read through the anchor, equal to GET_GAME_TIMER");
    const std::string* s500 = find_line(lines, "\"game_timer\":500,");
    CHECK(s500 && s500->find("\"time_ms\":500,\"frame_count\":6,\"minus_native_ms\":0}") != std::string::npos,
          "the sample at game time 500 ms reads 500 ms and frame 6 from the block");
    CHECK(m && m->find("\"name\":\"world.game_timer\",\"ok\":true") != std::string::npos && m->find("game timer block 0x14abbef40") == std::string::npos,
          "meta: timer anchor ok, no timer gate");
    for (const char* n : { "PLAYER_PED_ID_0EE0", "GET_PLAYER_INDEX", "GET_ENTITY_COORDS_F44D", "GET_ENTITY_HEADING_5507", "GET_ENTITY_HEALTH_C0F7",
                           "GET_ENTITY_MAX_HEALTH", "IS_PED_IN_ANY_VEHICLE", "GET_PLAYER_WANTED_LEVEL", "GET_GAME_TIMER_F667", "GET_FRAME_COUNT",
                           "GET_CLOCK_HOURS", "GET_CLOCK_MINUTES" })
        CHECK(s && s->find(std::string("\"n\":\"") + n + "\"") != std::string::npos, "sample logs the call to %s", n);
    // second entities (G-003): on foot, before the vehicle was ever entered
    CHECK(s && s->find("\"last_vehicle\":{\"rc\":0,\"lo\":\"0x0000000000000000\",\"hi\":\"0x0000000000000000\",\"valid\":false}") != std::string::npos,
          "no last vehicle yet");
    CHECK(s && s->find("\"last_vehicle_mem\":{\"ok\":true,") != std::string::npos && s->find("\"field\":null,\"ptr\":\"0x0000000000000000\"") != std::string::npos,
          "column 0x47 read: no last-vehicle pointer yet");
    CHECK(s && s->find("\"closest_ped\":{\"rc\":0,\"found\":true,\"out_written\":true,\"out_tail_intact\":true,\"lo\":\"0x00000003000000b1\","
                       "\"hi\":\"0x7f3c00000000d00d\",\"valid\":true,\"same_as_player\":false,\"coords\":{\"ok\":true,\"v\":[103,200,30]},\"dist_m\":3,") != std::string::npos,
          "closest ped: the second ped's handle, 3 m away, the 16 bytes after the handle untouched");
    CHECK(s && s->find("\"closest_vehicle\":{\"rc\":0,\"lo\":\"0x0000000200000077\",\"hi\":\"0x7f3c00000000cafe\",\"valid\":true,"
                       "\"same_as_player\":false,\"coords\":{\"ok\":true,\"v\":[105,200,30]},\"dist_m\":5,\"ptr_why\":\"gated: no anchor") != std::string::npos,
          "closest vehicle: handle, 5 m away, object pointer gated");
    CHECK(s && s->find("\"radius_m\":50") != std::string::npos, "search radius logged");
    for (const char* n : { "GET_PLAYERS_LAST_VEHICLE", "GET_CLOSEST_VEHICLE", "GET_CLOSEST_PED" })
        CHECK(s && s->find(std::string("\"n\":\"") + n + "\"") != std::string::npos, "sample logs the call to %s", n);
    const std::string* sv = find_line(lines, "\"in_vehicle\":true");
    CHECK(sv && sv->find("\"last_vehicle\":{\"rc\":0,\"lo\":\"0x0000000200000077\",\"hi\":\"0x7f3c00000000cafe\",\"valid\":true,"
                         "\"same_as_player\":false,\"same_as_vehicle_using\":true,") != std::string::npos,
          "seated: the last vehicle is the vehicle in use");
    CHECK(sv && sv->find("\"field\":\"0x000008a8\",\"ptr\":") != std::string::npos &&
              sv->find("\"type\":2,\"ecs_id\":\"0x00040009\",\"arch\":2,\"row\":9}") != std::string::npos,
          "seated: column 0x47 +0x8a8 names the vehicle entity: kind 2, ECS id arch 2 row 9");
    CHECK(sv && sv->find("\"vehicle\":{\"lo\":\"0x0000000200000077\"") != std::string::npos && sv->find("GET_VEHICLE_PED_IS_USING") != std::string::npos,
          "vehicle handle once seated");
    CHECK(sv && static_slot(*sv, 0x20) == "0007", "static ECS row slot 0x20 is 0007 once seated (%s)", sv ? static_slot(*sv, 0x20).c_str() : "-");
    CHECK(s && static_slot(*s, 0x20) == "ffff", "and ffff before");
    for (const std::string& l : lines)
        CHECK((l.find("SET_") == std::string::npos && l.find("CLEAR_PLAYER") == std::string::npos && l.find("ADD_TO_CLOCK") == std::string::npos) ||
                  l.find("\"ev\":\"meta\"") != std::string::npos,
              "no setter in any stage-3 record");
}

void scenario_rva_check() {
    std::printf("scenario stage3_rva_check\n");
    g_w = World{};
    Plugin p = start("stage3_rva_check", "[probe]\nstage=3\nrate_hz=2\nrate_clock=game\n");
    CHECK(p.load_rc == G6_OK, "loads with anchor_rva_check=1");
    CHECK(log_has("anchor GATED: world.physics.gravity: rva 0x") && log_has("differs from re-db 0x6dd5290"), "RVA mismatch gates the anchor");
    steps(6, 100);
    stop(p);
    const auto lines = read_lines(p.dir + L"\\g6probe.jsonl");
    const std::string* s = find_line(lines, "\"ev\":\"sample\"");
    CHECK(s && s->find("\"gravity\":{\"ok\":false,\"why\":\"gated: rva") != std::string::npos, "gravity gated in the record");
    CHECK(s && s->find("\"ped_mem\":{\"ok\":false,\"why\":\"gated:") != std::string::npos, "ped memory gated");
    CHECK(s && s->find("\"coords\":{\"ok\":true") != std::string::npos, "natives still logged");
    CHECK(s && s->find("\"clock_raw\":{\"ok\":false,\"why\":\"gated: rva") != std::string::npos &&
              s->find("\"fallback\":\"game_timer (GET_GAME_TIMER_F667)\"") != std::string::npos && s->find("\"game_timer\":") != std::string::npos &&
              s->find("\"game_timer\":null") == std::string::npos,
          "timer anchor gated: native GET_GAME_TIMER fallback still logged");
    CHECK(setters() == 0, "no setter");
}

void scenario_bad_stride() {
    std::printf("scenario stage3_bad_stride\n");
    g_w = World{};
    set_column(g_static_columns, 0x4a, g_law, 0xcce8);
    Plugin p = start("stage3_bad_stride", std::string(kBase) + "stage=3\n");
    steps(3, 100);
    stop(p);
    set_column(g_static_columns, 0x4a, g_law, 0xcce0);
    const auto lines = read_lines(p.dir + L"\\g6probe.jsonl");
    const std::string* s = find_line(lines, "\"ev\":\"sample\"");
    CHECK(s && s->find("\"law\":\"column stride 0xcce8 != re-db size 0xcce0 of CPedComp_col4a\"") != std::string::npos,
          "a stride that differs from re-db gates the component");
    CHECK(s && s->find("\"wanted_level\"") == std::string::npos, "and its fields are not read");
}

void scenario_no_axis() {
    std::printf("scenario stage4_no_axis\n");
    g_w = World{};
    enter_vehicle(false);
    Plugin p = start("stage4_no_axis", std::string(kBase) + "stage=4\n[writes]\nallow=coords_up,health\nhotkey=0\n");
    CHECK(p.load_rc == G6_OK, "stage 4 without up_axis still loads");
    CHECK(log_has("coords_up refused: [writes] up_axis is unset"), "coords_up refused at load with the cause");
    steps(3, 100);
    fire(p);
    fire(p);
    steps(12, 100);
    stop(p);
    CHECK(g_w.set_coords == 0, "SET_ENTITY_COORDS never called without an axis");
    CHECK(g_w.set_health == 1 && g_w.last_health_arg == 170, "health probe ran once: 180 + (-10) (arg %d)", g_w.last_health_arg);
    CHECK(log_has("no write probe left"), "second trigger finds nothing left");
    const auto lines = read_lines(p.dir + L"\\g6probe.jsonl");
    const std::string* m = find_line(lines, "\"ev\":\"meta\"");
    CHECK(m && m->find("\"up_axis\":null") != std::string::npos && m->find("\"refused_writes\":[\"coords_up refused") != std::string::npos, "meta records the refusal");
    CHECK(find_line(lines, "\"probe\":\"health\",\"phase\":\"immediate\"", "\"verdict\":{\"ok\":true,\"expect\":170}") != nullptr, "health verdict ok");
}

void scenario_writes() {
    std::printf("scenario stage4_writes\n");
    g_w = World{};
    enter_vehicle(false);
    Plugin p = start("stage4_writes", std::string(kBase) +
                     "stage=4\n[writes]\nallow=coords_up,health,wanted_clear,clock_add\nup_axis=+z\nhotkey=0\n[wait]\nprobe_ms=500\ncount=3\n");
    CHECK(p.load_rc == G6_OK, "stage 4 loads");
    steps(3, 100);
    CHECK(setters() == 0, "no setter before a trigger");
    fire(p);
    CHECK(g_w.set_coords == 1 && std::fabs(g_w.z - 31.0) < 1e-6 && g_w.x == 100.0 && g_w.y == 200.0, "coords_up: z 30 -> 31 (packed vector)");
    steps(12, 100);    // readbacks: next tick and after 1000 ms
    fire(p);
    fire(p);
    fire(p);
    steps(12, 100);
    fire(p);           // nothing left
    stop(p);
    CHECK(g_w.set_coords == 1 && g_w.set_health == 1 && g_w.clear_wanted == 1 && g_w.add_clock == 1, "each setter exactly once (%d %d %d %d)",
          g_w.set_coords, g_w.set_health, g_w.clear_wanted, g_w.add_clock);
    CHECK(g_w.bad_args == 0, "slot counts (%d bad)", g_w.bad_args);
    CHECK(g_w.wanted == 0, "wanted_clear passed the player ped, so the level was cleared (G-090)");
    const auto lines = read_lines(p.dir + L"\\g6probe.jsonl");
    const std::string* c = find_line(lines, "\"probe\":\"coords_up\",\"phase\":\"immediate\"");
    CHECK(c && c->find("\"verdict\":{\"ok\":true,\"along_up\":1,\"off_axis\":0") != std::string::npos && c->find("\"up_axis\":\"+z\"") != std::string::npos,
          "coords_up verdict: +1 along +z");
    CHECK(c && c->find("\"n\":\"SET_ENTITY_COORDS\"") != std::string::npos && c->find("\"0x4348000042c80000\",\"0x0000000041f80000\"") != std::string::npos,
          "SET_ENTITY_COORDS slots 2-3 hold the packed target (100, 200, 31)");
    CHECK(find_line(lines, "\"probe\":\"coords_up\",\"phase\":\"next_tick\"") && find_line(lines, "\"probe\":\"coords_up\",\"phase\":\"after_1000ms\""),
          "coords_up readbacks");
    CHECK(find_line(lines, "\"probe\":\"wanted_clear\",\"phase\":\"immediate\"", "\"verdict\":{\"ok\":true,\"expect\":0}"), "wanted verdict");
    CHECK(find_line(lines, "\"probe\":\"clock_add\",\"phase\":\"immediate\"", "\"verdict\":{\"ok\":true,\"expect_hours\":13}"), "clock verdict");
    CHECK(count_ev(lines, "wait") == 3, "wait probe: 3 rounds (%d)", count_ev(lines, "wait"));
    CHECK(find_line(lines, "\"ev\":\"wait\"", "\"rc\":8"), "mock script_wait reports not implemented (rc 8)");
    CHECK(log_has("no write probe left"), "fifth trigger: nothing left");
}

void scenario_in_vehicle() {
    std::printf("scenario stage4_in_vehicle\n");
    g_w = World{};
    enter_vehicle(true);
    Plugin p = start("stage4_in_vehicle", std::string(kBase) + "stage=4\n[writes]\nallow=coords_up\nup_axis=z\nhotkey=0\n");
    steps(2, 100);
    fire(p);
    stop(p);
    enter_vehicle(false);
    CHECK(g_w.set_coords == 0, "coords_up refused while seated");
    const auto lines = read_lines(p.dir + L"\\g6probe.jsonl");
    CHECK(find_line(lines, "\"probe\":\"coords_up\",\"phase\":\"refused\"", "on foot"), "refusal record names the cause");
}

void scenario_bad_ini() {
    std::printf("scenario bad_ini\n");
    g_w = World{};
    Plugin p = start("bad_ini", "[probe]\nrate_hz=2\n");
    CHECK(p.load_rc == G6_E_INVALID_ARG, "missing stage refuses the load (rc %d)", static_cast<int>(p.load_rc));
    CHECK(log_has("[probe] stage must be 3"), "the refusal names the key");
    steps(3, 100);
    CHECK(setters() == 0 && g_w.frames == 3, "nothing ticks");
    stop(p);
    Plugin q = start("bad_ini_allow", "[probe]\nstage=4\n[writes]\nallow=coords_up,teleport\nup_axis=z\n");
    CHECK(q.load_rc == G6_E_INVALID_ARG && log_has("unknown write probe 'teleport'"), "unknown write probe refuses the load");
    stop(q);
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5) { std::printf("usage: probe_mock_test <gta6mod_mock.dll> <g6probe.dll> <fake_title.dll> <work dir>\n"); return 2; }
    HMODULE mock = LoadLibraryA(argv[1]);
    HMODULE fake = LoadLibraryA(argv[3]);
    if (!mock || !fake) { std::printf("cannot load the mock or the fake title (%lu)\n", GetLastError()); return 2; }
    auto host = reinterpret_cast<const g6_host_api* (G6_CALL*)(void)>(reinterpret_cast<void*>(GetProcAddress(mock, "g6_get_host_api")));
    auto reg = reinterpret_cast<RegFn>(reinterpret_cast<void*>(GetProcAddress(mock, "g6_mock_register_native")));
    g_step = reinterpret_cast<StepFn>(reinterpret_cast<void*>(GetProcAddress(mock, "g6_mock_step")));
    if (!host || !reg || !g_step) { std::printf("mock exports missing\n"); return 2; }
    g_mock = host();
    g_api = *g_mock;
    g_api.log = cap_log;
    g_api.anchor_address = fake_anchor_address;
    char full[MAX_PATH];
    GetFullPathNameA(argv[2], MAX_PATH, full, nullptr);
    g_probe_dll = widen(full);
    GetFullPathNameA(argv[4], MAX_PATH, full, nullptr);
    g_work = widen(full);
    CreateDirectoryW(g_work.c_str(), nullptr);

    const struct { std::uint64_t h; g6_mock_native_fn fn; } natives[] = {
        { H_PLAYER_PED_ID, n_ped }, { H_GET_PLAYER_INDEX, n_player }, { H_GET_ENTITY_COORDS, n_coords },
        { H_GET_ENTITY_HEADING, n_heading }, { H_GET_ENTITY_HEALTH, n_health }, { H_GET_ENTITY_MAX_HEALTH, n_max_health },
        { H_IS_PED_IN_ANY_VEHICLE, n_in_vehicle }, { H_GET_VEHICLE_PED_IS_USING, n_vehicle }, { H_GET_PLAYER_WANTED_LEVEL, n_wanted },
        { H_GET_GAME_TIMER, n_timer }, { H_GET_FRAME_COUNT, n_frames }, { H_GET_CLOCK_HOURS, n_hours }, { H_GET_CLOCK_MINUTES, n_minutes },
        { H_SET_ENTITY_COORDS, n_set_coords }, { H_SET_ENTITY_HEALTH, n_set_health }, { H_CLEAR_PLAYER_WANTED_LEVEL, n_clear_wanted },
        { H_ADD_TO_CLOCK_TIME, n_add_clock },
        { H_GET_PLAYERS_LAST_VEHICLE, n_last_vehicle }, { H_GET_CLOSEST_VEHICLE, n_closest_vehicle }, { H_GET_CLOSEST_PED, n_closest_ped },
    };
    for (const auto& n : natives) if (reg(n.h, n.fn, nullptr) != G6_OK) { std::printf("register failed\n"); return 2; }
    build_memory(fake);

    scenario_stage3();
    scenario_rva_check();
    scenario_bad_stride();
    scenario_no_axis();
    scenario_writes();
    scenario_in_vehicle();
    scenario_bad_ini();
    std::printf("\ng6probe mock world: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
