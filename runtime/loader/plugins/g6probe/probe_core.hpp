// probe_core.hpp - pure logic of g6probe: configuration, validation, rate limiting, the setter gate,
// JSON output and slot decoding. No Windows calls and no game memory: unit-tested offline
// (tests/probe/g6probe_test.cpp).
#pragma once
#include "probe_layout.hpp"
#include "probe_natives.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace g6probe {

inline constexpr const char* kVersion = "0.2.0";   // 0.2.0: sample records carry "near" (G-003)

// ------------------------------------------------------------------ small helpers
inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

inline bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

// Whole-string integer (decimal or 0x hex, optional sign). False on junk or overflow of int64.
inline bool parse_i64(std::string_view s, long long& out) noexcept {
    s = trim(s);
    if (s.empty() || s.size() > 40) return false;
    char buf[48];
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(buf, &end, 0);
    if (errno != 0 || !end || *end != 0) return false;
    out = v;
    return true;
}

inline bool parse_f64(std::string_view s, double& out) noexcept {
    s = trim(s);
    if (s.empty() || s.size() > 40) return false;
    char buf[48];
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(buf, &end);
    if (errno != 0 || !end || *end != 0 || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// ------------------------------------------------------------------ vectors and axes
struct Vec3 { double x = 0, y = 0, z = 0; };

enum class Axis : int { unset = 0, x = 1, y = 2, z = 3 };
struct UpAxis { Axis axis = Axis::unset; int sign = 1; };

inline const char* axis_name(UpAxis a) noexcept {
    switch (a.axis) {
    case Axis::x: return a.sign > 0 ? "+x" : "-x";
    case Axis::y: return a.sign > 0 ? "+y" : "-y";
    case Axis::z: return a.sign > 0 ? "+z" : "-z";
    default: return "unset";
    }
}

// "x", "+y", "-z" (case-insensitive). No default exists: which axis is up is open (G-011).
inline bool parse_up_axis(std::string_view s, UpAxis& out) noexcept {
    s = trim(s);
    UpAxis a;
    if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
        a.sign = s.front() == '-' ? -1 : 1;
        s.remove_prefix(1);
    }
    if (ieq(s, "x")) a.axis = Axis::x;
    else if (ieq(s, "y")) a.axis = Axis::y;
    else if (ieq(s, "z")) a.axis = Axis::z;
    else return false;
    out = a;
    return true;
}

inline double component(const Vec3& v, Axis a) noexcept {
    return a == Axis::x ? v.x : a == Axis::y ? v.y : a == Axis::z ? v.z : 0.0;
}

inline double distance(const Vec3& a, const Vec3& b) noexcept {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline Vec3 apply_up(Vec3 v, UpAxis up, double delta) noexcept {
    const double d = delta * up.sign;
    if (up.axis == Axis::x) v.x += d;
    else if (up.axis == Axis::y) v.y += d;
    else if (up.axis == Axis::z) v.z += d;
    return v;
}

struct MoveVerdict {
    double along = 0;        // signed movement along the configured up direction
    double off_axis = 0;     // largest movement on the other two axes
    bool ok = false;         // |along - delta| <= tol and off_axis <= tol
};

inline MoveVerdict evaluate_move(const Vec3& before, const Vec3& after, UpAxis up, double delta, double tol) noexcept {
    MoveVerdict m;
    if (up.axis == Axis::unset) return m;
    const double d[3] = { after.x - before.x, after.y - before.y, after.z - before.z };
    const int ai = static_cast<int>(up.axis) - 1;
    m.along = d[ai] * up.sign;
    for (int i = 0; i < 3; ++i)
        if (i != ai && std::fabs(d[i]) > m.off_axis) m.off_axis = std::fabs(d[i]);
    m.ok = std::isfinite(m.along) && std::fabs(m.along - delta) <= tol && m.off_axis <= tol;
    return m;
}

// ------------------------------------------------------------------ validation
struct Check { bool ok; const char* why; };

inline bool finite3(const Vec3& v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// max_abs is the probe's plausibility bound ([probe] max_abs_coord), not a game fact.
inline Check check_coords(const Vec3& v, double max_abs) noexcept {
    if (!finite3(v)) return { false, "non-finite component" };
    if (std::fabs(v.x) > max_abs || std::fabs(v.y) > max_abs || std::fabs(v.z) > max_abs)
        return { false, "outside [probe] max_abs_coord" };
    if (v.x == 0.0 && v.y == 0.0 && v.z == 0.0) return { false, "all zero (no entity or the call failed)" };
    return { true, "" };
}

// Heading is logged as returned; only finiteness is required (its unit/range is unconfirmed live).
inline Check check_heading(double h) noexcept {
    if (!std::isfinite(h)) return { false, "non-finite" };
    if (h < -720.0 || h > 720.0) return { false, "outside [-720, 720]" };
    return { true, "" };
}

// GTA VI handles are 128-bit opaque keys; lo == 0 means "no entity" (gta6mod.h g6_entity_valid).
inline bool handle_null(std::uint64_t lo, std::uint64_t /*hi*/) noexcept { return lo == 0; }

// ------------------------------------------------------------------ rate limiting
// Fires on the first call and then whenever at least interval_ms passed. A clock that goes
// backwards fires once and restarts from the new value (unsigned difference wraps high).
class RateLimiter {
public:
    explicit RateLimiter(double hz = 2.0) noexcept { set_hz(hz); }
    void set_hz(double hz) noexcept {
        if (!(hz > 0.0)) hz = 2.0;
        interval_ = static_cast<std::uint64_t>(std::llround(1000.0 / hz));
        if (interval_ == 0) interval_ = 1;
    }
    std::uint64_t interval_ms() const noexcept { return interval_; }
    bool due(std::uint64_t now) noexcept {
        if (!have_ || now - last_ >= interval_) {
            have_ = true;
            last_ = now;
            return true;
        }
        return false;
    }
private:
    std::uint64_t interval_ = 500;
    std::uint64_t last_ = 0;
    bool have_ = false;
};

struct Edge {
    bool prev = false;
    bool rising(bool now) noexcept { const bool r = now && !prev; prev = now; return r; }
};

// ------------------------------------------------------------------ write probes
enum class WriteProbe : int { coords_up = 0, health = 1, wanted_clear = 2, clock_add = 3, count = 4 };

inline const char* write_probe_name(WriteProbe w) noexcept {
    switch (w) {
    case WriteProbe::coords_up: return "coords_up";
    case WriteProbe::health: return "health";
    case WriteProbe::wanted_clear: return "wanted_clear";
    case WriteProbe::clock_add: return "clock_add";
    default: return "?";
    }
}

// The one setter each write probe may call.
inline N write_probe_native(WriteProbe w) noexcept {
    switch (w) {
    case WriteProbe::coords_up: return N::set_entity_coords;
    case WriteProbe::health: return N::set_entity_health;
    case WriteProbe::wanted_clear: return N::clear_player_wanted_level;
    case WriteProbe::clock_add: return N::add_to_clock_time;
    default: return N::count;
    }
}

inline bool parse_write_probe(std::string_view s, WriteProbe& out) noexcept {
    s = trim(s);
    for (int i = 0; i < static_cast<int>(WriteProbe::count); ++i)
        if (ieq(s, write_probe_name(static_cast<WriteProbe>(i)))) { out = static_cast<WriteProbe>(i); return true; }
    return false;
}

// Comma-separated, order kept (it is the execution order). Unknown names and duplicates are errors.
inline bool parse_allow_list(std::string_view s, std::vector<WriteProbe>& out, std::string& why) {
    out.clear();
    s = trim(s);
    if (s.empty()) return true;
    while (true) {
        const std::size_t comma = s.find(',');
        const std::string_view item = trim(s.substr(0, comma));
        WriteProbe w{};
        if (!parse_write_probe(item, w)) { why = "unknown write probe '" + std::string(item) + "' (known: coords_up, health, wanted_clear, clock_add)"; return false; }
        for (WriteProbe e : out)
            if (e == w) { why = "write probe '" + std::string(item) + "' listed twice"; return false; }
        out.push_back(w);
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return true;
}

// Order of execution; each probe runs once.
class WriteQueue {
public:
    void reset(const std::vector<WriteProbe>& order) { order_ = order; done_.assign(order.size(), false); }
    bool next(WriteProbe& out) const noexcept {
        for (std::size_t i = 0; i < order_.size(); ++i) if (!done_[i]) { out = order_[i]; return true; }
        return false;
    }
    void mark_done(WriteProbe w) noexcept {
        for (std::size_t i = 0; i < order_.size(); ++i) if (order_[i] == w) done_[i] = true;
    }
    std::size_t remaining() const noexcept {
        std::size_t n = 0;
        for (bool d : done_) n += d ? 0 : 1;
        return n;
    }
private:
    std::vector<WriteProbe> order_;
    std::vector<bool> done_;
};

// ------------------------------------------------------------------ configuration
struct Config {
    int stage = 0;                         // 3 (reads only) or 4 (allow-listed writes on a hotkey)
    std::string jsonl = "g6probe.jsonl";   // relative = next to g6probe.dll
    double rate_hz = 2.0;
    bool rate_clock_game = false;          // [probe] rate_clock=game uses game_time_ms (tests); default wall
    std::uint64_t max_bytes = 256ull << 20;
    double max_abs_coord = 100000.0;
    bool anchor_rva_check = true;
    double near_radius_m = 50.0;           // GET_CLOSEST_PED / GET_CLOSEST_VEHICLE search radius (probe choice)
    std::vector<WriteProbe> allow;
    std::uint32_t hotkey_vk = 0x79;        // F10; 0 = no hotkey
    std::string trigger_file = "g6probe.fire";
    UpAxis up;                             // no default (G-011)
    std::string up_axis_raw;
    double up_delta = 1.0;
    long long health_delta = -10;
    std::uint32_t wait_probe_ms = 0;       // 0 = off
    std::uint32_t wait_probe_count = 20;
};

struct IniSource {
    virtual ~IniSource() = default;
    virtual std::string get(const char* section, const char* key, const char* def) const = 0;
};

struct ConfigResult {
    bool ok = true;
    std::vector<std::string> errors;           // load is refused
    std::vector<std::string> warnings;
    std::vector<std::string> refused_writes;   // probes removed from the allow list, with the reason
};

inline ConfigResult parse_config(const IniSource& ini, Config& c) {
    ConfigResult r;
    auto err = [&](std::string m) { r.ok = false; r.errors.push_back(std::move(m)); };
    long long iv = 0;
    double dv = 0;

    const std::string stage = std::string(trim(ini.get("probe", "stage", "")));
    if (stage == "3") c.stage = 3;
    else if (stage == "4") c.stage = 4;
    else err("[probe] stage must be 3 (read-only) or 4 (allow-listed writes); got '" + stage + "'");

    c.jsonl = std::string(trim(ini.get("probe", "jsonl", "g6probe.jsonl")));
    if (c.jsonl.empty()) err("[probe] jsonl is empty");

    if (!parse_f64(ini.get("probe", "rate_hz", "2"), dv) || dv < 0.1 || dv > 20.0) err("[probe] rate_hz must be 0.1..20");
    else c.rate_hz = dv;
    const std::string clk = std::string(trim(ini.get("probe", "rate_clock", "wall")));
    if (ieq(clk, "wall")) c.rate_clock_game = false;
    else if (ieq(clk, "game")) c.rate_clock_game = true;
    else err("[probe] rate_clock must be wall or game");
    if (!parse_i64(ini.get("probe", "max_mb", "256"), iv) || iv < 1 || iv > 4096) err("[probe] max_mb must be 1..4096");
    else c.max_bytes = static_cast<std::uint64_t>(iv) << 20;
    if (!parse_f64(ini.get("probe", "max_abs_coord", "100000"), dv) || dv < 100.0 || dv > 1e7) err("[probe] max_abs_coord must be 100..1e7");
    else c.max_abs_coord = dv;
    if (!parse_i64(ini.get("probe", "anchor_rva_check", "1"), iv) || (iv != 0 && iv != 1)) err("[probe] anchor_rva_check must be 0 or 1");
    else c.anchor_rva_check = iv == 1;
    if (!parse_f64(ini.get("probe", "near_radius_m", "50"), dv) || dv < 1.0 || dv > 1000.0) err("[probe] near_radius_m must be 1..1000");
    else c.near_radius_m = dv;
    if (!parse_i64(ini.get("wait", "probe_ms", "0"), iv) || iv < 0 || iv > 60000) err("[wait] probe_ms must be 0..60000");
    else c.wait_probe_ms = static_cast<std::uint32_t>(iv);
    if (!parse_i64(ini.get("wait", "count", "20"), iv) || iv < 1 || iv > 1000) err("[wait] count must be 1..1000");
    else c.wait_probe_count = static_cast<std::uint32_t>(iv);

    const std::string allow = ini.get("writes", "allow", "");
    std::string why;
    std::vector<WriteProbe> list;
    if (!parse_allow_list(allow, list, why)) { err("[writes] allow: " + why); list.clear(); }
    if (c.stage != 4) {
        if (!list.empty()) r.warnings.push_back("[writes] allow is ignored: stage " + stage + " is read-only and never calls a setter");
        c.allow.clear();
        return r;
    }

    if (!parse_i64(ini.get("writes", "hotkey", "0x79"), iv) || iv < 0 || iv > 0xFE) err("[writes] hotkey must be a virtual-key code 0..0xFE (0 = none)");
    else c.hotkey_vk = static_cast<std::uint32_t>(iv);
    c.trigger_file = std::string(trim(ini.get("writes", "trigger_file", "g6probe.fire")));
    if (c.hotkey_vk == 0 && c.trigger_file.empty()) err("[writes] neither hotkey nor trigger_file is set: write probes could never run");

    c.up_axis_raw = std::string(trim(ini.get("writes", "up_axis", "")));
    const bool axis_ok = parse_up_axis(c.up_axis_raw, c.up);
    if (!parse_f64(ini.get("writes", "up_delta_m", "1.0"), dv) || dv <= 0.0 || dv > 5.0) err("[writes] up_delta_m must be in (0, 5]");
    else c.up_delta = dv;
    if (!parse_i64(ini.get("writes", "health_delta", "-10"), iv) || iv == 0 || iv < -1000 || iv > 1000) err("[writes] health_delta must be a non-zero integer in -1000..1000");
    else c.health_delta = iv;

    for (WriteProbe w : list) {
        if (w == WriteProbe::coords_up && !axis_ok) {
            r.refused_writes.push_back(std::string("coords_up refused: [writes] up_axis is ") +
                                       (c.up_axis_raw.empty() ? "unset" : "'" + c.up_axis_raw + "' (not x|y|z with optional sign)") +
                                       "; it has no default because the up axis is unconfirmed (G-011)");
            continue;
        }
        c.allow.push_back(w);
    }
    if (c.allow.empty() && r.ok) r.warnings.push_back("stage 4 with no runnable write probe: only reads will happen");
    return r;
}

// ------------------------------------------------------------------ the setter gate
enum class Refusal : int { none = 0, unbound = 1, write_stage_readonly = 2, write_not_armed = 3, write_wrong_probe = 4 };

inline const char* refusal_str(Refusal r) noexcept {
    switch (r) {
    case Refusal::none: return "";
    case Refusal::unbound: return "native not usable (see the binding check at load)";
    case Refusal::write_stage_readonly: return "setter refused: stage 3 is read-only";
    case Refusal::write_not_armed: return "setter refused: no write probe is executing";
    case Refusal::write_wrong_probe: return "setter refused: not the setter of the executing write probe";
    }
    return "?";
}

// `armed` is the write probe currently executing (count = none). Every native call goes through this.
inline Refusal gate(const NativeSpec& s, BindState bound, int stage, WriteProbe armed) noexcept {
    if (bound != BindState::ok) return Refusal::unbound;
    if (!s.write) return Refusal::none;
    if (stage != 4) return Refusal::write_stage_readonly;
    if (armed == WriteProbe::count) return Refusal::write_not_armed;
    if (write_probe_native(armed) != s.id) return Refusal::write_wrong_probe;
    return Refusal::none;
}

// ------------------------------------------------------------------ slots
// 'e' entity, 'h' 16-byte non-entity handle (ScenarioPoint), 'w' packed vector: 2 slots; 'v' legacy vector: 3.
inline std::size_t slot_width(char code) noexcept {
    return code == 'e' || code == 'h' || code == 'w' ? 2 : code == 'v' ? 3 : 1;
}
inline std::size_t ret_slots(char code) noexcept { return code == 'n' ? 0 : code == 'e' || code == 'h' || code == 'w' ? 2 : 1; }

// Number of argument slots a signature fills ("ewbbbbf>n" -> 9).
inline std::size_t arg_slots(const char* sig) noexcept {
    std::size_t n = 0;
    for (const char* p = sig; *p && *p != '>'; ++p) n += slot_width(*p);
    return n;
}

inline char ret_code(const char* sig) noexcept {
    const char* p = std::strchr(sig, '>');
    return p && p[1] ? p[1] : 'n';
}

inline float slot_float(std::uint64_t s) noexcept {
    const std::uint32_t u = static_cast<std::uint32_t>(s);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline std::uint64_t float_slot(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline Vec3 slots_vec3(const std::uint64_t* r) noexcept {
    return { slot_float(r[0]), slot_float(r[0] >> 32), slot_float(r[1]) };
}
inline void vec3_slots(const Vec3& v, std::uint64_t* out) noexcept {
    out[0] = float_slot(static_cast<float>(v.x)) | (float_slot(static_cast<float>(v.y)) << 32);
    out[1] = float_slot(static_cast<float>(v.z));
}

// ------------------------------------------------------------------ JSON
class Json {
public:
    Json& obj() { value_prefix(); s_ += '{'; first_.push_back(true); return *this; }
    Json& end_obj() { s_ += '}'; first_.pop_back(); return *this; }
    Json& arr() { value_prefix(); s_ += '['; first_.push_back(true); return *this; }
    Json& end_arr() { s_ += ']'; first_.pop_back(); return *this; }
    Json& key(std::string_view k) {
        comma();
        quote(k);
        s_ += ':';
        after_key_ = true;
        return *this;
    }
    Json& str(std::string_view v) { value_prefix(); quote(v); return *this; }
    Json& i64(long long v) { value_prefix(); char b[32]; std::snprintf(b, sizeof b, "%lld", v); s_ += b; return *this; }
    Json& u64(unsigned long long v) { value_prefix(); char b[32]; std::snprintf(b, sizeof b, "%llu", v); s_ += b; return *this; }
    Json& hex(unsigned long long v) { value_prefix(); char b[32]; std::snprintf(b, sizeof b, "\"0x%016llx\"", v); s_ += b; return *this; }
    Json& hex32(unsigned long v) { value_prefix(); char b[24]; std::snprintf(b, sizeof b, "\"0x%08lx\"", v); s_ += b; return *this; }
    Json& f64(double v) {
        value_prefix();
        if (!std::isfinite(v)) { s_ += "null"; return *this; }
        char b[40];
        std::snprintf(b, sizeof b, "%.9g", v);
        s_ += b;
        return *this;
    }
    Json& boolean(bool v) { value_prefix(); s_ += v ? "true" : "false"; return *this; }
    Json& null() { value_prefix(); s_ += "null"; return *this; }
    Json& raw(std::string_view v) { value_prefix(); s_ += v; return *this; }   // caller-made valid JSON

    const std::string& str() const noexcept { return s_; }
    bool balanced() const noexcept { return first_.empty(); }

private:
    void comma() {
        if (!first_.empty()) {
            if (!first_.back()) s_ += ',';
            first_.back() = false;
        }
    }
    void value_prefix() {
        if (after_key_) { after_key_ = false; return; }
        comma();
    }
    void quote(std::string_view v) {
        s_ += '"';
        for (unsigned char c : v) {
            switch (c) {
            case '"': s_ += "\\\""; break;
            case '\\': s_ += "\\\\"; break;
            case '\n': s_ += "\\n"; break;
            case '\r': s_ += "\\r"; break;
            case '\t': s_ += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    s_ += b;
                } else {
                    s_ += static_cast<char>(c);
                }
            }
        }
        s_ += '"';
    }
    std::string s_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

// The decoded return of a native, by its signature's return code.
inline void json_ret(Json& j, char code, const std::uint64_t* ret) {
    switch (code) {
    case 'e': case 'h': j.obj().key("lo").hex(ret[0]).key("hi").hex(ret[1]).end_obj(); break;
    case 'w': { const Vec3 v = slots_vec3(ret); j.arr().f64(v.x).f64(v.y).f64(v.z).end_arr(); break; }
    case 'f': j.f64(slot_float(ret[0])); break;
    case 'b': j.boolean((ret[0] & 0xFF) != 0); break;
    case 'i': j.i64(static_cast<std::int32_t>(static_cast<std::uint32_t>(ret[0]))); break;
    case 'u': j.u64(static_cast<std::uint32_t>(ret[0])); break;
    case 'n': j.null(); break;
    default: j.hex(ret[0]); break;
    }
}

// One native call: {"n":name,"h":hash,"st":status,"a":[slots],"rc":status,"r":[ret slots],"rv":decoded}
// or, when refused before the call, {"n":..,"refused":reason}.
inline void json_call(Json& j, const NativeSpec& s, const char* status, const std::uint64_t* args, std::size_t argc,
                      int rc, const std::uint64_t* ret, const char* refused) {
    j.obj().key("n").str(s.name).key("h").hex(s.hash).key("st").str(status ? status : "");
    if (refused && *refused) { j.key("refused").str(refused).end_obj(); return; }
    j.key("a").arr();
    for (std::size_t i = 0; i < argc; ++i) j.hex(args[i]);
    j.end_arr().key("rc").i64(rc);
    const char rc_code = ret_code(s.sig);
    const std::size_t nret = ret_slots(rc_code);
    j.key("r").arr();
    for (std::size_t i = 0; i < nret; ++i) j.hex(ret[i]);
    j.end_arr().key("rv");
    if (rc == 0) json_ret(j, rc_code, ret);
    else j.null();
    j.end_obj();
}

// ------------------------------------------------------------------ ECS (findings/phase5/24)
struct EcsId { std::uint32_t raw = 0, arch = 0, row = 0; };

inline EcsId decode_ecs_id(std::uint32_t id) noexcept {
    namespace e = layout::ecs;
    return { id, (id >> e::kArchShift) & e::kArchMask, id & e::kRowMask };
}

// First index of `row` in a row_index table with `n` slots per row; false if n is out of bounds.
inline bool row_first_index(std::uint32_t row, std::uint32_t n, std::uint64_t& first) noexcept {
    if (n == 0 || n > layout::ecs::kMaxSlotsPerRow || row > layout::ecs::kRowMask) return false;
    first = static_cast<std::uint64_t>(row) * n;
    return true;
}

// Which last-vehicle pointer GET_PLAYERS_LAST_VEHICLE's core 0x1420f4b40 uses (probe_layout.hpp ecs::kPedCol47).
struct LastVehiclePick {
    std::uintptr_t ptr = 0;
    std::uint32_t field = 0;   // 0x898, 0x8a8, or 0 = no vehicle
};

inline LastVehiclePick pick_last_vehicle(std::uint8_t flags, std::uintptr_t at_a, std::uintptr_t at_b) noexcept {
    namespace e = layout::ecs;
    if (((flags >> e::kCol47FlagsBit) & 1u) && at_a) return { at_a, e::kCol47VehicleA };
    if (at_b) return { at_b, e::kCol47VehicleB };
    return {};
}

// u16 row as 4 hex digits per slot, no separators ("ffff0002...").
inline std::string hex_row(const std::uint16_t* v, std::size_t n) {
    std::string s;
    s.reserve(n * 4);
    char b[8];
    for (std::size_t i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%04x", v[i]); s += b; }
    return s;
}

} // namespace g6probe
