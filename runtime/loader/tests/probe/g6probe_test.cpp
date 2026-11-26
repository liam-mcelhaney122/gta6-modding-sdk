// g6probe_test.cpp - offline unit test of g6probe's pure logic (probe_core.hpp, probe_natives.hpp,
// probe_layout.hpp). No game, no DLL. Exit code 0 = every check passed.
#include "probe_core.hpp"

#include "gta6mod/patterns.hpp"
#include "manifest_1_0_2_1.inc"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>

namespace {
int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) { ++g_pass; }                                       \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

using namespace g6probe;

struct MapIni final : IniSource {
    std::map<std::string, std::string> kv;
    MapIni& set(const char* sec, const char* key, const char* v) { kv[std::string(sec) + "." + key] = v; return *this; }
    std::string get(const char* sec, const char* key, const char* def) const override {
        const auto it = kv.find(std::string(sec) + "." + key);
        return it == kv.end() ? std::string(def) : it->second;
    }
};

void test_axis() {
    UpAxis a;
    CHECK(parse_up_axis("z", a) && a.axis == Axis::z && a.sign == 1, "z");
    CHECK(parse_up_axis(" +Y ", a) && a.axis == Axis::y && a.sign == 1, "+Y");
    CHECK(parse_up_axis("-x", a) && a.axis == Axis::x && a.sign == -1, "-x");
    CHECK(!parse_up_axis("", a), "empty refused");
    CHECK(!parse_up_axis("up", a), "word refused");
    CHECK(!parse_up_axis("zz", a), "zz refused");
    CHECK(!parse_up_axis("+", a), "sign only refused");
    CHECK(std::string(axis_name(UpAxis{ Axis::z, -1 })) == "-z", "axis_name -z");
    CHECK(std::string(axis_name(UpAxis{})) == "unset", "axis_name unset");
}

void test_allow_list() {
    std::vector<WriteProbe> v;
    std::string why;
    CHECK(parse_allow_list("", v, why) && v.empty(), "empty list");
    CHECK(parse_allow_list(" health , coords_up,CLOCK_ADD", v, why) && v.size() == 3 && v[0] == WriteProbe::health &&
          v[1] == WriteProbe::coords_up && v[2] == WriteProbe::clock_add, "order kept, case-insensitive");
    CHECK(!parse_allow_list("health,teleport", v, why) && why.find("teleport") != std::string::npos, "unknown refused: %s", why.c_str());
    CHECK(!parse_allow_list("health,health", v, why) && why.find("twice") != std::string::npos, "duplicate refused");
    CHECK(!parse_allow_list("health,", v, why), "trailing comma refused");
}

void test_config() {
    {
        MapIni ini;
        Config c;
        const ConfigResult r = parse_config(ini, c);
        CHECK(!r.ok && !r.errors.empty() && r.errors[0].find("stage") != std::string::npos, "stage has no default");
    }
    {
        MapIni ini;
        ini.set("probe", "stage", "5");
        Config c;
        CHECK(!parse_config(ini, c).ok, "stage 5 refused");
    }
    {
        MapIni ini;
        ini.set("probe", "stage", "3").set("writes", "allow", "coords_up,health").set("writes", "up_axis", "z");
        Config c;
        const ConfigResult r = parse_config(ini, c);
        CHECK(r.ok && c.stage == 3 && c.allow.empty() && !r.warnings.empty(), "stage 3 ignores the allow list (warning)");
        CHECK(c.rate_hz == 2.0 && !c.rate_clock_game && c.anchor_rva_check && c.jsonl == "g6probe.jsonl", "defaults");
    }
    {
        MapIni ini;
        ini.set("probe", "stage", "4").set("writes", "allow", "coords_up,health");
        Config c;
        const ConfigResult r = parse_config(ini, c);
        CHECK(r.ok && c.stage == 4, "stage 4 loads without an axis");
        CHECK(c.allow.size() == 1 && c.allow[0] == WriteProbe::health, "coords_up removed when up_axis is unset");
        CHECK(r.refused_writes.size() == 1 && r.refused_writes[0].find("up_axis is unset") != std::string::npos, "refusal names the cause");
        CHECK(c.up.axis == Axis::unset, "no default axis");
    }
    {
        MapIni ini;
        ini.set("probe", "stage", "4").set("writes", "allow", "coords_up").set("writes", "up_axis", "q");
        Config c;
        const ConfigResult r = parse_config(ini, c);
        CHECK(r.ok && c.allow.empty() && r.refused_writes.size() == 1 && r.refused_writes[0].find("'q'") != std::string::npos,
              "invalid axis refuses coords_up");
    }
    {
        MapIni ini;
        ini.set("probe", "stage", "4").set("writes", "allow", "coords_up,wanted_clear").set("writes", "up_axis", "-y")
            .set("writes", "up_delta_m", "0.5");
        Config c;
        const ConfigResult r = parse_config(ini, c);
        CHECK(r.ok && c.allow.size() == 2 && c.up.axis == Axis::y && c.up.sign == -1 && c.up_delta == 0.5 && r.refused_writes.empty(),
              "axis set: both probes kept");
    }
    auto bad = [](const char* sec, const char* key, const char* v) {
        MapIni ini;
        ini.set("probe", "stage", "4").set("writes", "allow", "health").set(sec, key, v);
        Config c;
        return !parse_config(ini, c).ok;
    };
    CHECK(bad("probe", "rate_hz", "0"), "rate 0 refused");
    CHECK(bad("probe", "rate_hz", "25"), "rate 25 refused");
    CHECK(bad("probe", "rate_hz", "2x"), "rate junk refused");
    CHECK(bad("probe", "rate_clock", "cpu"), "rate_clock refused");
    CHECK(bad("probe", "max_mb", "0"), "max_mb 0 refused");
    CHECK(bad("probe", "anchor_rva_check", "2"), "anchor_rva_check 2 refused");
    CHECK(bad("probe", "max_abs_coord", "10"), "max_abs_coord 10 refused");
    CHECK(bad("writes", "up_delta_m", "0"), "up_delta 0 refused");
    CHECK(bad("writes", "up_delta_m", "6"), "up_delta 6 refused");
    CHECK(bad("writes", "health_delta", "0"), "health_delta 0 refused");
    CHECK(bad("writes", "hotkey", "0x100"), "hotkey 0x100 refused");
    CHECK(bad("writes", "allow", "health,nope"), "unknown probe refuses the load");
    CHECK(bad("wait", "probe_ms", "-1"), "negative wait refused");
    CHECK(bad("probe", "near_radius_m", "0.5"), "near radius below 1 m refused");
    CHECK(bad("probe", "near_radius_m", "2000"), "near radius above 1000 m refused");
    {
        MapIni ini;
        ini.set("probe", "stage", "3");
        Config c;
        CHECK(parse_config(ini, c).ok && c.near_radius_m == 50.0, "near radius defaults to 50 m");
        ini.set("probe", "near_radius_m", "12.5");
        CHECK(parse_config(ini, c).ok && c.near_radius_m == 12.5, "near radius set");
    }
    {
        MapIni ini;
        ini.set("probe", "stage", "4").set("writes", "hotkey", "0").set("writes", "trigger_file", "");
        Config c;
        CHECK(!parse_config(ini, c).ok, "no trigger at all refused");
    }
}

void test_rate() {
    RateLimiter r(2.0);
    CHECK(r.interval_ms() == 500, "2 Hz = 500 ms");
    CHECK(r.due(1000), "first call fires");
    CHECK(!r.due(1200) && !r.due(1499), "not before 500 ms");
    CHECK(r.due(1500), "fires at 500 ms");
    CHECK(!r.due(1999), "next window");
    CHECK(r.due(100), "clock going backwards fires once");
    CHECK(!r.due(101), "and restarts from there");
    RateLimiter f(20.0);
    CHECK(f.interval_ms() == 50, "20 Hz = 50 ms");
    RateLimiter s(0.1);
    CHECK(s.interval_ms() == 10000, "0.1 Hz = 10 s");
    int fired = 0;
    RateLimiter t(2.0);
    for (std::uint64_t ms = 0; ms < 10000; ms += 16) fired += t.due(ms) ? 1 : 0;   // 60 fps ticks for 10 s
    CHECK(fired == 20, "60 fps for 10 s at 2 Hz fires 20 times (got %d)", fired);
}

void test_edge_queue() {
    Edge e;
    CHECK(!e.rising(false) && e.rising(true) && !e.rising(true) && !e.rising(false) && e.rising(true), "edge");
    WriteQueue q;
    q.reset({ WriteProbe::coords_up, WriteProbe::health });
    WriteProbe w{};
    CHECK(q.next(w) && w == WriteProbe::coords_up && q.remaining() == 2, "queue first");
    q.mark_done(w);
    CHECK(q.next(w) && w == WriteProbe::health && q.remaining() == 1, "queue second");
    q.mark_done(w);
    CHECK(!q.next(w) && q.remaining() == 0, "queue empty: each runs once");
}

void test_gate() {
    // Reads pass in both stages when bound; anything unbound is refused.
    for (const NativeSpec& s : kNatives) {
        if (s.write) continue;
        CHECK(gate(s, BindState::ok, 3, WriteProbe::count) == Refusal::none, "read %s stage 3", s.name);
        CHECK(gate(s, BindState::ok, 4, WriteProbe::health) == Refusal::none, "read %s stage 4 while armed", s.name);
        CHECK(gate(s, BindState::below_observed, 3, WriteProbe::count) == Refusal::unbound, "unbound read %s refused", s.name);
    }
    // Stage 3 never calls a setter, whatever is armed.
    int setters = 0;
    for (const NativeSpec& s : kNatives) {
        if (!s.write) continue;
        ++setters;
        for (int a = 0; a <= static_cast<int>(WriteProbe::count); ++a)
            CHECK(gate(s, BindState::ok, 3, static_cast<WriteProbe>(a)) == Refusal::write_stage_readonly, "stage 3 refuses %s (armed %d)", s.name, a);
        CHECK(gate(s, BindState::ok, 4, WriteProbe::count) == Refusal::write_not_armed, "stage 4 refuses %s when not armed", s.name);
    }
    CHECK(setters == 4, "four setters in the table");
    // Stage 4 allows exactly the armed probe's setter.
    for (int a = 0; a < static_cast<int>(WriteProbe::count); ++a) {
        const WriteProbe w = static_cast<WriteProbe>(a);
        for (const NativeSpec& s : kNatives) {
            if (!s.write) continue;
            const Refusal r = gate(s, BindState::ok, 4, w);
            if (write_probe_native(w) == s.id) CHECK(r == Refusal::none, "%s allowed for %s", s.name, write_probe_name(w));
            else CHECK(r == Refusal::write_wrong_probe, "%s refused for %s", s.name, write_probe_name(w));
        }
    }
    CHECK(gate(spec(N::set_entity_coords), BindState::sig_mismatch, 4, WriteProbe::coords_up) == Refusal::unbound, "unbound setter refused");
}

void test_validation() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(!check_coords({ nan, 0, 1 }, 1e5).ok, "NaN refused");
    CHECK(!check_coords({ 1, inf, 1 }, 1e5).ok, "inf refused");
    CHECK(!check_coords({ 1, 2, 200001 }, 1e5).ok, "out of range refused");
    CHECK(!check_coords({ 0, 0, 0 }, 1e5).ok, "all zero refused");
    CHECK(check_coords({ -1234.5, 88.25, 30.0 }, 1e5).ok, "plausible accepted");
    CHECK(check_heading(359.9).ok && !check_heading(nan).ok && !check_heading(1e6).ok, "heading");
    CHECK(handle_null(0, 5) && !handle_null(1, 0), "lo == 0 is null");
}

void test_move() {
    const Vec3 b{ 10, 20, 30 };
    const UpAxis z{ Axis::z, 1 };
    const Vec3 t = apply_up(b, z, 1.0);
    CHECK(t.x == 10 && t.y == 20 && t.z == 31, "apply +z");
    CHECK(evaluate_move(b, t, z, 1.0, 0.05).ok, "exact +1 on z");
    const UpAxis my{ Axis::y, -1 };
    const Vec3 t2 = apply_up(b, my, 1.0);
    CHECK(t2.y == 19 && evaluate_move(b, t2, my, 1.0, 0.05).ok, "apply -y");
    CHECK(!evaluate_move(b, { 10.2, 20, 31 }, z, 1.0, 0.05).ok, "off-axis drift fails");
    CHECK(!evaluate_move(b, { 10, 20, 30.5 }, z, 1.0, 0.05).ok, "half a metre fails");
    CHECK(!evaluate_move(b, { 10, 20, 29 }, z, 1.0, 0.05).ok, "wrong direction fails");
    const MoveVerdict m = evaluate_move(b, { 10, 21, 30 }, z, 1.0, 0.05);
    CHECK(!m.ok && m.along == 0.0 && m.off_axis == 1.0, "moved on y while z was configured");
    CHECK(!evaluate_move(b, t, UpAxis{}, 1.0, 0.05).ok, "unset axis never passes");
}

void test_json() {
    Json j;
    j.obj().key("a").i64(-1).key("s").str("q\"b\\c\n\x01").key("n").f64(std::nan("")).key("arr").arr().u64(1).boolean(true).null().end_arr()
        .key("o").obj().end_obj().key("h").hex(0xABCull).key("h32").hex32(0x10).end_obj();
    const std::string want = "{\"a\":-1,\"s\":\"q\\\"b\\\\c\\n\\u0001\",\"n\":null,\"arr\":[1,true,null],\"o\":{},"
                             "\"h\":\"0x0000000000000abc\",\"h32\":\"0x00000010\"}";
    CHECK(j.str() == want && j.balanced(), "json text: %s", j.str().c_str());
    Json f;
    f.arr().f64(1.5).f64(-0.25).f64(1e20).end_arr();
    CHECK(f.str() == "[1.5,-0.25,1e+20]", "floats: %s", f.str().c_str());
    Json r;
    r.obj().key("x").raw("[1,2]").key("y").i64(3).end_obj();
    CHECK(r.str() == "{\"x\":[1,2],\"y\":3}", "raw: %s", r.str().c_str());
}

void test_calls_json() {
    std::uint64_t ret[4] = { 0x1111, 0x2222, 0, 0 };
    Json a;
    a.arr();
    json_call(a, spec(N::player_ped_id), "observed", nullptr, 0, 0, ret, nullptr);
    json_call(a, spec(N::set_entity_coords), "observed", nullptr, 0, -1, nullptr, refusal_str(Refusal::write_stage_readonly));
    a.end_arr();
    CHECK(a.str() == "[{\"n\":\"PLAYER_PED_ID_0EE0\",\"h\":\"0x096275889b8e0ee0\",\"st\":\"observed\",\"a\":[],\"rc\":0,"
                     "\"r\":[\"0x0000000000001111\",\"0x0000000000002222\"],\"rv\":{\"lo\":\"0x0000000000001111\",\"hi\":\"0x0000000000002222\"}},"
                     "{\"n\":\"SET_ENTITY_COORDS\",\"h\":\"0x06843da7060a026b\",\"st\":\"observed\",\"refused\":\"setter refused: stage 3 is read-only\"}]",
          "call json: %s", a.str().c_str());
    std::uint64_t v[4];
    vec3_slots({ 1.5, -2.0, 30.25 }, v);
    std::uint64_t args[4] = { 0xAA, 0xBB, 1, 0 };
    Json b;
    json_call(b, spec(N::get_entity_coords), "observed", args, 4, 0, v, nullptr);
    CHECK(b.str().find("\"rv\":[1.5,-2,30.25]") != std::string::npos && b.str().find("\"a\":[\"0x00000000000000aa\",\"0x00000000000000bb\",\"0x0000000000000001\",\"0x0000000000000000\"]") != std::string::npos,
          "vector return decoded: %s", b.str().c_str());
    std::uint64_t fr[4] = { float_slot(90.5f), 0, 0, 0 };
    Json c;
    json_call(c, spec(N::get_entity_heading), "observed", args, 2, 7, fr, nullptr);
    CHECK(c.str().find("\"rc\":7") != std::string::npos && c.str().find("\"rv\":null") != std::string::npos, "failed call has no decoded value");
    Json d;
    json_ret(d, 'f', fr);
    CHECK(d.str() == "90.5", "float ret");
    std::uint64_t ir[4] = { 0xFFFFFFFFull, 0, 0, 0 };
    Json e;
    json_ret(e, 'i', ir);
    CHECK(e.str() == "-1", "int ret sign: %s", e.str().c_str());
    std::uint64_t br[4] = { 0x100, 0, 0, 0 };
    Json g;
    json_ret(g, 'b', br);
    CHECK(g.str() == "false", "BOOL uses the low byte");
}

void test_slots() {
    CHECK(arg_slots("ewbbbbf>n") == 9, "SET_ENTITY_COORDS 9 slots");
    CHECK(arg_slots("eieqq>n") == 7, "SET_ENTITY_HEALTH 7 slots");
    CHECK(arg_slots("ebqq>b") == 5 && arg_slots(">e") == 0 && arg_slots("iii>n") == 3, "slot counts");
    CHECK(ret_code("ebb>w") == 'w' && ret_code(">e") == 'e' && ret_code("iq>n") == 'n', "ret codes");
    // 'h' = 16-byte non-entity handle (ScenarioPoint, G-099): 2 argument slots, 2 return slots, decoded as lo/hi
    CHECK(arg_slots("hib>n") == 4 && arg_slots("ehsibbubfbq>n") == 13 && ret_slots('h') == 2 && ret_slots('n') == 0 &&
          ret_slots('i') == 1, "16-byte handle slot counts");
    {
        const std::uint64_t hr[2] = { 0x5C00000000000001ull, 0x5C5Cull };
        Json g;
        json_ret(g, 'h', hr);
        CHECK(g.str() == "{\"lo\":\"0x5c00000000000001\",\"hi\":\"0x0000000000005c5c\"}", "16-byte handle return decoded lo/hi");
    }
    std::uint64_t s[2];
    vec3_slots({ 1.0, 2.0, -3.5 }, s);
    const Vec3 v = slots_vec3(s);
    CHECK(v.x == 1.0 && v.y == 2.0 && v.z == -3.5 && (s[1] >> 32) == 0, "packed vector round trip, w half zero");
    float gf = 0;
    const std::uint64_t one = g6_arg_float(1.0f);
    std::memcpy(&gf, &one, 4);
    CHECK(float_slot(1.0f) == one && slot_float(one) == 1.0f, "float slot matches gta6mod.h");
    g6_vec3 gv{ 1.0f, 2.0f, -3.5f };
    CHECK(g6_arg_vec3_lo(gv) == s[0] && g6_arg_vec3_hi(gv) == s[1], "packed layout matches g6_arg_vec3_lo/hi");
    // every spec's slot count is what the plugin passes (g6probe.cpp read_natives/run_write)
    CHECK(arg_slots(spec(N::get_entity_coords).sig) == 4 && arg_slots(spec(N::get_entity_max_health).sig) == 3 &&
          arg_slots(spec(N::is_ped_in_any_vehicle).sig) == 5 && arg_slots(spec(N::get_vehicle_ped_is_using).sig) == 3 &&
          arg_slots(spec(N::get_player_wanted_level).sig) == 2 && arg_slots(spec(N::clear_player_wanted_level).sig) == 2 &&
          arg_slots(spec(N::add_to_clock_time).sig) == 3, "argument counts used by the plugin");
    // second-entity natives: slots as the handlers read them (probe_layout.hpp namespace nearby)
    CHECK(arg_slots(spec(N::get_players_last_vehicle).sig) == 0 && ret_code(spec(N::get_players_last_vehicle).sig) == 'e',
          "GET_PLAYERS_LAST_VEHICLE: no argument, 16-byte return");
    CHECK(arg_slots(spec(N::get_closest_vehicle).sig) == 5 && ret_code(spec(N::get_closest_vehicle).sig) == 'e',
          "GET_CLOSEST_VEHICLE: 5 slots (packed position 0-1, radius, model, flags), 16-byte return");
    CHECK(arg_slots(spec(N::get_closest_ped).sig) == 10 && ret_code(spec(N::get_closest_ped).sig) == 'b' &&
          spec(N::get_closest_ped).sig[4] == 'E', "GET_CLOSEST_PED: 10 slots, the out pointer in slot 5 (5th code: w covers slots 0-1), BOOL return");
    for (N n : { N::get_players_last_vehicle, N::get_closest_vehicle, N::get_closest_ped })
        CHECK(!spec(n).write, "%s is not a setter", spec(n).name);
    CHECK(distance({ 1, 2, 3 }, { 4, 6, 3 }) == 5.0, "distance");
}

void test_ecs() {
    const EcsId id = decode_ecs_id((3u << 17) | 5u);
    CHECK(id.arch == 3 && id.row == 5, "decode arch 3 row 5");
    const EcsId hi = decode_ecs_id(0xFFFFFFFFu);
    CHECK(hi.arch == 0x1f && hi.row == 0x1ffff, "masks");
    std::uint64_t first = 0;
    CHECK(row_first_index(5, 0x4d, first) && first == 5u * 0x4d, "first index");
    CHECK(!row_first_index(5, 0, first) && !row_first_index(5, 0x201, first) && !row_first_index(0x20000, 4, first), "bounds");
    const std::uint16_t row[3] = { 0xffff, 0x0002, 0x1234 };
    CHECK(hex_row(row, 3) == "ffff00021234", "hex row");
    namespace E = layout::ecs;
    CHECK(E::kIndexMinusStride == 0xc && E::kColumnsMinusStride == 0x1c, "static record field distances");
    CHECK(E::kRecTypeToColumn + E::kGenHealth.column_or_type == 0x4d && E::kRecTypeToColumn + E::kGenProofs.column_or_type == 0x4f,
          "generic selectors land on the +0x4d / +0x4f bytes of findings/phase5/24");
    // GET_PLAYERS_LAST_VEHICLE core 0x1420f4b40: +0x898 only with bit 5 of +0x10, else +0x8a8 when non-null
    CHECK(!E::kPedCol47.generic && E::kPedCol47.column_or_type == 0x47 && E::kCol47VehicleB + 8 <= E::kPedCol47.size,
          "column 0x47 fields lie inside its 0xb30-byte element");
    const LastVehiclePick p0 = pick_last_vehicle(0x00, 0x1000, 0x2000);
    CHECK(p0.ptr == 0x2000 && p0.field == 0x8a8, "bit 5 clear: +0x8a8");
    const LastVehiclePick p1 = pick_last_vehicle(0x20, 0x1000, 0x2000);
    CHECK(p1.ptr == 0x1000 && p1.field == 0x898, "bit 5 set and +0x898 non-null: +0x898");
    const LastVehiclePick p2 = pick_last_vehicle(0x20, 0, 0x2000);
    CHECK(p2.ptr == 0x2000 && p2.field == 0x8a8, "bit 5 set but +0x898 null: +0x8a8");
    const LastVehiclePick p3 = pick_last_vehicle(0xdf, 0x1000, 0);
    CHECK(p3.ptr == 0 && p3.field == 0, "only bit 5 counts; nothing when +0x8a8 is null");
    const LastVehiclePick p4 = pick_last_vehicle(0xff, 0, 0);
    CHECK(p4.ptr == 0 && p4.field == 0, "no vehicle");
}

void test_bindings() {
    for (const NativeSpec& s : kNatives) {
        const Binding b = bind(s, g6_natives, G6_NATIVE_COUNT);
        CHECK(b.state == BindState::ok, "%s binds against natives.h: %s", s.name, bind_state_str(b.state));
        CHECK(status_rank(b.status) >= 1, "%s status '%s' at least observed", s.name, b.status);
        // the runtime resolves natives through its manifest table: the same hash must be there, same handler
        bool in_manifest = false;
        for (const auto& r : g6rt::manifest::kNatives)
            if (r.hash == s.hash) { in_manifest = r.handler_rva == b.handler_rva; break; }
        CHECK(in_manifest, "%s in the runtime manifest with handler rva 0x%x", s.name, b.handler_rva);
    }
    // the GTA V seed variants the probe avoids are unverified in natives.h
    const NativeSpec seed_ped{ N::player_ped_id, "PLAYER_PED_ID", 0xD80958FC74E988A6ull, ">e", false };
    CHECK(bind(seed_ped, g6_natives, G6_NATIVE_COUNT).state == BindState::below_observed, "seed PLAYER_PED_ID refused (unverified)");
    const g6_native_info rows[] = {
        { 0x1ull, "NS", "A", ">i", 0, 100u, "unverified" },
        { 0x2ull, "NS", "B", ">i", 0, 0u, "observed" },
        { 0x3ull, "NS", "C", ">i", 0, 100u, "observed" },
        { 0x4ull, "NS", "D", ">i", 1, 100u, "verified" },
    };
    CHECK(bind({ N::get_frame_count, "A", 0x1, ">i", false }, rows, 4).state == BindState::below_observed, "unverified refused");
    CHECK(bind({ N::get_frame_count, "B", 0x2, ">i", false }, rows, 4).state == BindState::no_handler, "no handler refused");
    CHECK(bind({ N::get_frame_count, "X", 0x3, ">i", false }, rows, 4).state == BindState::name_mismatch, "name mismatch refused");
    CHECK(bind({ N::get_frame_count, "C", 0x3, ">e", false }, rows, 4).state == BindState::sig_mismatch, "sig mismatch refused");
    CHECK(bind({ N::get_frame_count, "C", 0x3, ">i", false }, rows, 4).state == BindState::ok, "observed accepted");
    CHECK(bind({ N::get_frame_count, "D", 0x4, ">i", false }, rows, 4).state == BindState::ok, "verified accepted");
    CHECK(bind({ N::get_frame_count, "Z", 0x9, ">i", false }, rows, 4).state == BindState::missing, "missing refused");
}

void test_layout_against_redb() {
    for (const layout::DataAnchor* a : layout::kAllAnchors) {
        const g6::patterns::anchor* p = g6::patterns::by_name(a->name);
        CHECK(p != nullptr, "%s in patterns.hpp", a->name);
        // signed, or an xref chain with a chains[] row (world.game_timer)
        if (p) CHECK(p->target_kind == g6::patterns::kind::data && p->signature && (*p->signature || g6::patterns::chain_by_name(a->name)),
                     "%s is a signed or chained data anchor", a->name);
        bool found = false;
        for (const auto& r : g6rt::manifest::kAnchorRvas)
            if (std::strcmp(r.name, a->name) == 0) { found = true; CHECK(r.rva == a->rva, "%s rva 0x%x == manifest 0x%x", a->name, a->rva, r.rva); }
        CHECK(found, "%s in the runtime manifest", a->name);
    }
}
} // namespace

int main() {
    test_axis();
    test_allow_list();
    test_config();
    test_rate();
    test_edge_queue();
    test_gate();
    test_validation();
    test_move();
    test_json();
    test_calls_json();
    test_slots();
    test_ecs();
    test_bindings();
    test_layout_against_redb();
    std::printf("g6probe unit: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
