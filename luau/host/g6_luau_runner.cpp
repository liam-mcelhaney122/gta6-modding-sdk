// g6_luau_runner — run Luau mods offline against the mock runtime (gta6mod_mock.dll).
//   g6_luau_runner <lib_dir> <mod_init.luau> [ms=2000] [--press CTL@MS]... [--expect-clean] [--expect-log TEXT]
// Registers fake natives with the same semantics as the Python mock world
// (sdk/python/gta6mod/runtime/mock.py) so a Luau mod and its Python twin behave
// the same: player ped 0x100 at (-1000, -2000, 15), spawned handles from 0x101,
// models load 50 ms after REQUEST_MODEL, CREATE_VEHICLE returns 0 until then,
// a pressed control is "just pressed" for one tick only.
//   --expect-clean     exit 1 if anything was logged at ERROR (failed script, failed load)
//   --expect-log TEXT  exit 1 unless some log message contains TEXT
#include "g6_luau.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Ent { uint64_t h; float x, y, z, heading; int32_t health; };
struct Model { uint32_t hash; uint64_t loaded_at; };
struct Press { uint64_t at; int32_t control; };

constexpr uint64_t kModelLoadMs = 50;

const g6_host_api* g_mock = nullptr;   // the mock DLL's table
g6_host_api        g_api{};            // what the Luau host sees: mock + counting log/invoke
std::vector<Ent>   g_ents;
std::vector<Model> g_models;
std::vector<int32_t> g_pressed;        // controls "just pressed" on pad 0 this tick
uint64_t           g_next_handle = 0x100;
uint64_t           g_native_calls = 0;
int                g_errors = 0;
const char*        g_expect_log = nullptr;
bool               g_expect_seen = false;

uint64_t now_ms() { return g_mock->game_time_ms(); }

uint64_t spawn(float x, float y, float z, float heading) {
    g_ents.push_back({ g_next_handle, x, y, z, heading, 200 });
    return g_next_handle++;
}
// Entity = 2 slots (lo, hi), findings/phase5/23_handle_abi.md. Mock handles have hi == 0.
Ent* ent(const uint64_t* slots) {
    for (auto& e : g_ents) if (e.h == slots[0] && slots[1] == 0) return &e;
    return nullptr;
}
Model* model(uint32_t hash) {
    for (auto& m : g_models) if (m.hash == hash) return &m;
    return nullptr;
}

void G6_CALL n_wait(g6_native_call*, void*) {}
void G6_CALL n_get_hash_key(g6_native_call* c, void*) {
    const char* s = (const char*)(uintptr_t)c->args[0];
    c->ret[0] = g6_joaat(s ? s : "");
}
void G6_CALL n_game_timer(g6_native_call* c, void*) { c->ret[0] = now_ms() & 0xFFFFFFFFu; }
void G6_CALL n_player_ped(g6_native_call* c, void*) { c->ret[0] = 0x100; }
void G6_CALL n_player_id(g6_native_call* c, void*) { c->ret[0] = 0; }
void G6_CALL n_exists(g6_native_call* c, void*) { c->ret[0] = ent(c->args) ? 1 : 0; }
void G6_CALL n_coords(g6_native_call* c, void*) {
    const Ent* e = ent(c->args);
    g6_vec3 v = { e ? e->x : 0.f, e ? e->y : 0.f, e ? e->z : 0.f };
    g6_set_vec3_packed(c->ret, v);  // packed Vector3 return
}
void G6_CALL n_set_coords(g6_native_call* c, void*) {
    if (Ent* e = ent(c->args)) { e->x = g6_ret_float(c->args[2]); e->y = g6_ret_float(c->args[3]); e->z = g6_ret_float(c->args[4]); }
}
void G6_CALL n_heading(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = g6_arg_float(e ? e->heading : 0.f); }
void G6_CALL n_health(g6_native_call* c, void*) { const Ent* e = ent(c->args); c->ret[0] = e ? (uint64_t)(int64_t)e->health : 0; }
void G6_CALL n_request_model(g6_native_call* c, void*) {
    uint32_t m = (uint32_t)c->args[0];
    if (!model(m)) g_models.push_back({ m, now_ms() + kModelLoadMs });
}
void G6_CALL n_model_loaded(g6_native_call* c, void*) {
    const Model* m = model((uint32_t)c->args[0]);
    c->ret[0] = (m && now_ms() >= m->loaded_at) ? 1 : 0;
}
void G6_CALL n_release_model(g6_native_call*, void*) {}
void G6_CALL n_create_vehicle(g6_native_call* c, void*) {
    const Model* m = model((uint32_t)c->args[0]);
    if (!m || now_ms() < m->loaded_at) { c->ret[0] = 0; return; }  // game returns 0 when the model is not loaded
    c->ret[0] = spawn(g6_ret_float(c->args[1]), g6_ret_float(c->args[2]), g6_ret_float(c->args[3]), g6_ret_float(c->args[4]));
}
void G6_CALL n_just_pressed(g6_native_call* c, void*) {
    int32_t pad = (int32_t)c->args[0], ctl = (int32_t)c->args[1];
    c->ret[0] = 0;
    if (pad == 0) for (int32_t p : g_pressed) if (p == ctl) c->ret[0] = 1;
}

void G6_CALL counting_log(g6_log_level lvl, const char* who, const char* msg) {
    if (lvl >= G6_LOG_ERROR) ++g_errors;
    if (g_expect_log && msg && std::strstr(msg, g_expect_log)) g_expect_seen = true;
    g_mock->log(lvl, who, msg);
}
g6_status G6_CALL counting_invoke(g6_native_handle h, g6_native_call* c) {
    ++g_native_calls;
    return g_mock->native_invoke(h, c);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <lib_dir> <init.luau> [ms] [--press CTL@MS]... [--expect-clean] [--expect-log TEXT]\n", argv[0]);
        return 2;
    }
    uint64_t run_ms = 2000;
    bool expect_clean = false;
    std::vector<Press> presses;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--press") && i + 1 < argc) {
            const char* spec = argv[++i];
            const char* at = std::strchr(spec, '@');
            presses.push_back({ at ? std::strtoull(at + 1, nullptr, 10) : 0, (int32_t)std::atoi(spec) });
        } else if (!std::strcmp(argv[i], "--press-ms") && i + 1 < argc) {  // older form: control 51 at N ms
            presses.push_back({ std::strtoull(argv[++i], nullptr, 10), 51 });
        } else if (!std::strcmp(argv[i], "--expect-clean")) {
            expect_clean = true;
        } else if (!std::strcmp(argv[i], "--expect-log") && i + 1 < argc) {
            g_expect_log = argv[++i];
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

    spawn(-1000.f, -2000.f, 15.f, 0.f);  // player ped, handle 0x100
    const struct { uint64_t hash; g6_mock_native_fn fn; } fakes[] = {
        { 0x4EDE34FBADD967A6ull, n_wait },           { 0xD24D37CC275948CCull, n_get_hash_key },
        { 0x9CD27B0045628463ull, n_game_timer },     { 0xD80958FC74E988A6ull, n_player_ped },
        { 0x4F8644AF03D0E0D6ull, n_player_id },      { 0x7239B21A38F536BAull, n_exists },
        { 0x3FEF770D40960D5Aull, n_coords },         { 0x06843DA7060A026Bull, n_set_coords },
        { 0xE83D4F9BA2A38914ull, n_heading },        { 0xEEF059FAD016D209ull, n_health },
        { 0x963D27A58DF860ACull, n_request_model },  { 0x98A4EB5D89A0C952ull, n_model_loaded },
        { 0xE532F5D78798DAABull, n_release_model },  { 0xAF35D0D2583051B0ull, n_create_vehicle },
        { 0x580417101DDB492Full, n_just_pressed },
    };
    for (const auto& f : fakes) g6_mock_register_native(f.hash, f.fn, nullptr);

    g6_luau_config cfg = { sizeof(g6_luau_config), argv[1], ".", 1 /* mock: allow unverified */ };
    g6_luau_host* host = g6_luau_create(&g_api, &cfg);
    if (!host) { std::fprintf(stderr, "g6_luau_create failed\n"); return 1; }
    if (g6_luau_run_file(host, argv[2]) != 0) { g6_luau_destroy(host); return 1; }

    while (now_ms() < run_ms) {
        for (const Press& p : presses) if (p.at == now_ms()) g_pressed.push_back(p.control);
        g6_luau_tick(host);
        g6_mock_step(16);
        g_pressed.clear();
    }
    std::printf("ran %llu ms; %llu native calls; %d live scripts; %d errors; %zu entities\n",
                (unsigned long long)now_ms(), (unsigned long long)g_native_calls, g6_luau_script_count(host), g_errors, g_ents.size());
    g6_luau_destroy(host);
    if (expect_clean && g_errors) { std::fprintf(stderr, "FAIL: %d error(s) logged\n", g_errors); return 1; }
    if (g_expect_log && !g_expect_seen) { std::fprintf(stderr, "FAIL: no log message containing \"%s\"\n", g_expect_log); return 1; }
    return 0;
}
