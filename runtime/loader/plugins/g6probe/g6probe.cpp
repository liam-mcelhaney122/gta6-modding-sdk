// g6probe.cpp - live probe plugin for gta6mod_runtime ([runtime] mode=full), GTA VI 1.0.2.1.
//
// Per sampled tick (rate-limited, [probe] rate_hz, default 2 Hz) it appends one JSONL record: the player
// ped handle (both qwords raw), coordinates, heading, health, vehicle state, wanted level, the game
// timer, the gravity vector, the player ped's ECS rows and fields, and every native call it made with
// its argument slots and return slots. Stage 3 never calls a setter. Stage 4 runs each allow-listed
// write probe once, one per hotkey press or trigger file, with readbacks before and after.
// Layout and provenance: probe_layout.hpp. Config: g6probe.ini next to this DLL.
#include "probe_core.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#define G6PROBE_EXPORT extern "C" __declspec(dllexport)

namespace g6probe {
namespace {

namespace L = layout;
namespace E = layout::ecs;

const g6_host_api* g_api = nullptr;
Config g_cfg;
wchar_t g_dir[1024] = {};
wchar_t g_ini[1100] = {};
HANDLE g_out = INVALID_HANDLE_VALUE;
std::uint64_t g_bytes = 0;
bool g_full_logged = false;
Binding g_bind[static_cast<int>(N::count)];
unsigned g_fail_logs[static_cast<int>(N::count)] = {};
RateLimiter g_rate;
WriteQueue g_queue;
Edge g_key_edge;
std::uint64_t g_seq = 0;
bool g_first_tick = true;
g6_script_id g_sid = 0;
g6_script_id g_wait_sid = 0;
std::uint32_t g_wait_done = 0;
WriteProbe g_armed = WriteProbe::count;
std::uint64_t g_last_trigger_poll = 0;
bool g_trigger_delete_logged = false;
std::uint64_t g_session_unix_ms = 0;
std::uintptr_t g_module_base = 0;

struct AnchorState {
    const L::DataAnchor* a = nullptr;
    bool ok = false;
    std::uintptr_t addr = 0;
    std::uint64_t rva = 0;
    std::string why;
};
AnchorState g_anchors[sizeof(L::kAllAnchors) / sizeof(L::kAllAnchors[0])];

// ------------------------------------------------------------------ logging and output
void logf(g6_log_level lvl, const char* fmt, ...) {
    char buf[1600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_api && g_api->log) g_api->log(lvl, "g6probe", buf);
}

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Relative paths resolve against the folder of g6probe.dll.
std::wstring resolve_path(const std::string& p) {
    std::wstring w = wide(p);
    const bool absolute = (w.size() >= 2 && w[1] == L':') || (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\');
    return absolute ? w : std::wstring(g_dir) + L"\\" + w;
}

void emit(const Json& j) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    std::string line = j.str();
    line += '\n';
    if (g_bytes + line.size() > g_cfg.max_bytes) {
        if (!g_full_logged) {
            g_full_logged = true;
            logf(G6_LOG_WARN, "JSONL size limit [probe] max_mb reached (%llu bytes); no further records this session",
                 static_cast<unsigned long long>(g_bytes));
        }
        return;
    }
    DWORD wr = 0;
    if (WriteFile(g_out, line.data(), static_cast<DWORD>(line.size()), &wr, nullptr)) g_bytes += wr;
}

std::uint64_t unix_ms_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t / 10000ull - 11644473600000ull;
}

// ------------------------------------------------------------------ guarded memory reads
bool readable(std::uintptr_t p, std::size_t n) noexcept {
    if (!p || !n || p + n < p) return false;
    std::uintptr_t cur = p;
    const std::uintptr_t end = p + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & rd)) return false;
        cur = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// SEH only, no C++ objects in this frame: a page can be released between VirtualQuery and the copy.
int seh_copy(void* dst, const void* src, std::size_t n) noexcept {
    __try {
        std::memcpy(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

bool copy_from(std::uintptr_t src, void* dst, std::size_t n) noexcept {
    return readable(src, n) && seh_copy(dst, reinterpret_cast<const void*>(src), n) == 1;
}
template <class T> bool rd(std::uintptr_t src, T& out) noexcept { return copy_from(src, &out, sizeof(T)); }

// ------------------------------------------------------------------ ini
struct WinIni final : IniSource {
    std::string get(const char* section, const char* key, const char* def) const override {
        wchar_t buf[1024];
        GetPrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(def).c_str(), buf, 1024, g_ini);
        return utf8(buf);
    }
};

// ------------------------------------------------------------------ natives
struct CallOut {
    bool called = false;
    int rc = -1;
    std::uint64_t ret[G6_MAX_NATIVE_RETS] = {};
    bool ok() const noexcept { return called && rc == G6_OK; }
};

CallOut call(Json& calls, N id, const std::uint64_t* args, std::size_t argc) {
    const NativeSpec& s = spec(id);
    const int i = static_cast<int>(id);
    CallOut out;
    const Refusal r = gate(s, g_bind[i].state, g_cfg.stage, g_armed);
    const char* why = refusal_str(r);
    if (r == Refusal::none && argc != arg_slots(s.sig)) why = "internal: argument count differs from the signature's slots";
    if (r != Refusal::none || argc != arg_slots(s.sig)) {
        json_call(calls, s, g_bind[i].status, nullptr, 0, -1, nullptr, why);
        if (g_fail_logs[i]++ < 3) logf(G6_LOG_WARN, "native %s not called: %s", s.name, why);
        return out;
    }
    g6_native_call c{};
    c.size = sizeof c;
    c.argc = static_cast<std::uint32_t>(argc);
    for (std::size_t k = 0; k < argc; ++k) c.args[k] = args[k];
    const g6_status st = g_api->native_invoke_hash(s.hash, &c);
    out.called = true;
    out.rc = static_cast<int>(st);
    for (int k = 0; k < G6_MAX_NATIVE_RETS; ++k) out.ret[k] = c.ret[k];
    json_call(calls, s, g_bind[i].status, args, argc, out.rc, c.ret, nullptr);
    if (st != G6_OK && g_fail_logs[i]++ < 3) logf(G6_LOG_WARN, "native %s returned status %d", s.name, static_cast<int>(st));
    return out;
}

// What one round of native reads saw.
struct Snap {
    bool ped_ok = false;
    std::uint64_t lo = 0, hi = 0;
    bool player_ok = false;
    int player = 0;
    bool coords_ok = false;
    Vec3 coords;
    const char* coords_why = "not read";
    bool heading_ok = false;
    double heading = 0;
    bool health_ok = false;
    int health = 0;
    bool max_health_ok = false;
    int max_health = 0;
    bool in_vehicle_known = false;
    bool in_vehicle = false;
    bool vehicle_ok = false;
    std::uint64_t veh_lo = 0, veh_hi = 0;
    bool wanted_ok = false;
    int wanted = 0;
    bool timer_ok = false;
    std::uint32_t timer = 0;
    bool frames_ok = false;
    std::uint32_t frames = 0;
    bool clock_ok = false;
    int hours = 0, minutes = 0;
};

int ret_i32(const CallOut& c) noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(c.ret[0])); }

void read_natives(Snap& s, Json& calls, bool full) {
    const CallOut ped = call(calls, N::player_ped_id, nullptr, 0);
    if (ped.ok()) {
        s.lo = ped.ret[0];
        s.hi = ped.ret[1];
        s.ped_ok = !handle_null(s.lo, s.hi);
    }
    const CallOut pl = call(calls, N::get_player_index, nullptr, 0);
    if (pl.ok()) { s.player_ok = true; s.player = ret_i32(pl); }
    if (s.ped_ok) {
        const std::uint64_t a_coords[4] = { s.lo, s.hi, 1, 0 };   // alive=TRUE, realCoords=FALSE (entities.py)
        const CallOut c = call(calls, N::get_entity_coords, a_coords, 4);
        if (c.ok()) {
            s.coords = slots_vec3(c.ret);
            const Check ck = check_coords(s.coords, g_cfg.max_abs_coord);
            s.coords_ok = ck.ok;
            s.coords_why = ck.why;
        } else {
            s.coords_why = "call failed";
        }
        const std::uint64_t a_e[2] = { s.lo, s.hi };
        const CallOut h = call(calls, N::get_entity_heading, a_e, 2);
        if (h.ok()) { s.heading = slot_float(h.ret[0]); s.heading_ok = check_heading(s.heading).ok; }
        const CallOut hp = call(calls, N::get_entity_health, a_e, 2);
        if (hp.ok()) { s.health_ok = true; s.health = ret_i32(hp); }
        const std::uint64_t a_mh[3] = { s.lo, s.hi, 0 };
        const CallOut mh = call(calls, N::get_entity_max_health, a_mh, 3);
        if (mh.ok()) { s.max_health_ok = true; s.max_health = ret_i32(mh); }
        const std::uint64_t a_iv[5] = { s.lo, s.hi, 0, 0, 0 };
        const CallOut iv = call(calls, N::is_ped_in_any_vehicle, a_iv, 5);
        if (iv.ok()) { s.in_vehicle_known = true; s.in_vehicle = (iv.ret[0] & 0xFF) != 0; }
        if (full && s.in_vehicle_known && s.in_vehicle) {
            const std::uint64_t a_v[3] = { s.lo, s.hi, 0 };
            const CallOut v = call(calls, N::get_vehicle_ped_is_using, a_v, 3);
            if (v.ok()) { s.vehicle_ok = true; s.veh_lo = v.ret[0]; s.veh_hi = v.ret[1]; }
        }
    }
    if (s.player_ok) {
        const std::uint64_t a_w[2] = { static_cast<std::uint64_t>(static_cast<std::int64_t>(s.player)), 0 };
        const CallOut w = call(calls, N::get_player_wanted_level, a_w, 2);
        if (w.ok()) { s.wanted_ok = true; s.wanted = ret_i32(w); }
    }
    if (full) {
        const CallOut t = call(calls, N::get_game_timer, nullptr, 0);
        if (t.ok()) { s.timer_ok = true; s.timer = static_cast<std::uint32_t>(t.ret[0]); }
        const CallOut f = call(calls, N::get_frame_count, nullptr, 0);
        if (f.ok()) { s.frames_ok = true; s.frames = static_cast<std::uint32_t>(f.ret[0]); }
    }
    const CallOut hh = call(calls, N::get_clock_hours, nullptr, 0);
    const CallOut mm = call(calls, N::get_clock_minutes, nullptr, 0);
    if (hh.ok() && mm.ok()) { s.clock_ok = true; s.hours = ret_i32(hh); s.minutes = ret_i32(mm); }
}

void json_vec(Json& j, const Vec3& v) { j.arr().f64(v.x).f64(v.y).f64(v.z).end_arr(); }

void json_snap(Json& j, const Snap& s) {
    j.key("ped").obj().key("lo").hex(s.lo).key("hi").hex(s.hi).key("valid").boolean(s.ped_ok).end_obj();
    j.key("player_index");
    if (s.player_ok) j.i64(s.player); else j.null();
    j.key("coords").obj().key("ok").boolean(s.coords_ok);
    if (s.ped_ok) { j.key("v"); json_vec(j, s.coords); }
    if (!s.coords_ok) j.key("why").str(s.ped_ok ? s.coords_why : "no valid player ped handle");
    j.end_obj();
    j.key("heading");
    if (s.heading_ok) j.f64(s.heading); else j.null();
    j.key("health");
    if (s.health_ok) j.i64(s.health); else j.null();
    j.key("max_health");
    if (s.max_health_ok) j.i64(s.max_health); else j.null();
    j.key("in_vehicle");
    if (s.in_vehicle_known) j.boolean(s.in_vehicle); else j.null();
    if (s.vehicle_ok) j.key("vehicle").obj().key("lo").hex(s.veh_lo).key("hi").hex(s.veh_hi).end_obj();
    j.key("wanted");
    if (s.wanted_ok) j.i64(s.wanted); else j.null();
    j.key("game_timer");
    if (s.timer_ok) j.u64(s.timer); else j.null();
    j.key("frame_count");
    if (s.frames_ok) j.u64(s.frames); else j.null();
    j.key("clock");
    if (s.clock_ok) j.obj().key("h").i64(s.hours).key("m").i64(s.minutes).end_obj(); else j.null();
}

// ------------------------------------------------------------------ anchors
AnchorState* anchor(const L::DataAnchor& a) {
    for (AnchorState& s : g_anchors) if (s.a == &a) return &s;
    return nullptr;
}

void resolve_anchors() {
    std::size_t k = 0;
    for (const L::DataAnchor* a : L::kAllAnchors) {
        AnchorState& s = g_anchors[k++];
        s = AnchorState{};
        s.a = a;
        std::uintptr_t addr = 0;
        const g6_status st = g_api->anchor_address(a->name, &addr);
        char why[300];
        if (st != G6_OK || !addr) {
            std::snprintf(why, sizeof why, "anchor_address returned %d", static_cast<int>(st));
            s.why = why;
            continue;
        }
        s.addr = addr;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE || mbi.State != MEM_COMMIT) {
            s.why = "not inside a committed image mapping";
            continue;
        }
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
        if (!GetProcAddress(reinterpret_cast<HMODULE>(base), L::kTitleExport)) {
            s.why = std::string("its module does not export ") + L::kTitleExport + " (not the title image)";
            continue;
        }
        if (g_module_base && g_module_base != base) {
            s.why = "in a different module than the other anchors";
            continue;
        }
        g_module_base = base;
        s.rva = addr - base;
        if (g_cfg.anchor_rva_check && s.rva != a->rva) {
            std::snprintf(why, sizeof why, "rva 0x%llx differs from re-db 0x%x ([probe] anchor_rva_check=1)",
                          static_cast<unsigned long long>(s.rva), a->rva);
            s.why = why;
            continue;
        }
        s.ok = true;
    }
    // The three static ped store anchors are fields of one record (probe_layout.hpp).
    AnchorState* rs = anchor(L::kPedRowStride);
    AnchorState* ix = anchor(L::kPedIndexTable);
    AnchorState* co = anchor(L::kPedColumns);
    if (rs && ix && co && rs->ok && ix->ok && co->ok &&
        (ix->addr - rs->addr != E::kIndexMinusStride || co->addr - rs->addr != E::kColumnsMinusStride)) {
        for (AnchorState* s : { rs, ix, co }) {
            s->ok = false;
            s->why = "static ped store anchors are not one record (+0x14/+0x20/+0x30)";
        }
    }
    for (const AnchorState& s : g_anchors) {
        if (s.ok) logf(G6_LOG_INFO, "anchor ok: %s at 0x%llx (rva 0x%llx, re-db 0x%x): %s", s.a->name,
                       static_cast<unsigned long long>(s.addr), static_cast<unsigned long long>(s.rva), s.a->rva, s.a->what);
        else logf(G6_LOG_WARN, "anchor GATED: %s: %s; the values that need it are logged as gated", s.a->name, s.why.c_str());
    }
}

const AnchorState* ok_anchor(const L::DataAnchor& a) {
    const AnchorState* s = anchor(a);
    return s && s->ok ? s : nullptr;
}

// ------------------------------------------------------------------ memory probes
void gated(Json& why, const char* key, const std::string& reason) { why.key(key).str(reason); }

void read_gravity(Json& j) {
    j.key("gravity").obj();
    const AnchorState* a = ok_anchor(L::kGravity);
    if (!a) {
        const AnchorState* s = anchor(L::kGravity);
        j.key("ok").boolean(false).key("why").str("gated: " + (s ? s->why : std::string("no anchor"))).end_obj();
        return;
    }
    float v[4] = {};
    if (!copy_from(a->addr, v, sizeof v)) {
        j.key("ok").boolean(false).key("addr").hex(a->addr).key("why").str("unreadable").end_obj();
        return;
    }
    const bool fin = std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]);
    j.key("ok").boolean(fin).key("addr").hex(a->addr).key("v").arr().f64(v[0]).f64(v[1]).f64(v[2]).f64(v[3]).end_arr();
    if (!fin) j.key("why").str("non-finite component");
    j.end_obj();
}

// The game timer block (anchor world.game_timer). The native value ("game_timer", GET_GAME_TIMER_F667) is
// logged every sample regardless and is the fallback when the anchor is gated.
void read_game_timer(Json& j, const Snap& s) {
    static_assert(L::timer::kFrameCountOff == L::timer::kTimeMsOff + 4, "one 8-byte read covers both fields");
    j.key("clock_raw").obj();
    const AnchorState* a = ok_anchor(L::kGameTimer);
    if (!a) {
        const AnchorState* st = anchor(L::kGameTimer);
        j.key("ok").boolean(false).key("why").str("gated: " + (st ? st->why : std::string("no anchor")))
            .key("fallback").str("game_timer (GET_GAME_TIMER_F667)").end_obj();
        return;
    }
    std::uint32_t v[2] = {};
    if (!copy_from(a->addr + L::timer::kTimeMsOff, v, sizeof v)) {
        j.key("ok").boolean(false).key("addr").hex(a->addr).key("why").str("unreadable")
            .key("fallback").str("game_timer (GET_GAME_TIMER_F667)").end_obj();
        return;
    }
    j.key("ok").boolean(true).key("addr").hex(a->addr).key("time_ms").u64(v[0]).key("frame_count").u64(v[1]);
    // u32 difference to the native value read earlier in this tick (wrap-safe); 0 expected in game
    if (s.timer_ok) j.key("minus_native_ms").i64(static_cast<std::int32_t>(v[0] - s.timer));
    j.end_obj();
}

struct Store {
    bool ok = false;
    std::uintptr_t rec = 0;          // generic: record address (type_to_column lives there)
    std::uint32_t n = 0;
    std::uintptr_t row_index = 0, columns = 0;
    std::vector<std::uint16_t> row;
    std::string why;
};

bool load_row(Store& s, std::uint32_t row) {
    std::uint64_t first = 0;
    if (!row_first_index(row, s.n, first)) {
        char b[96];
        std::snprintf(b, sizeof b, "slots_per_row %u outside 1..0x%x", s.n, E::kMaxSlotsPerRow);
        s.why = b;
        return false;
    }
    if (!s.row_index || !s.columns) { s.why = "null row_index or columns pointer"; return false; }
    s.row.assign(s.n, 0);
    if (!copy_from(s.row_index + first * 2, s.row.data(), s.n * 2u)) { s.why = "row_index slice unreadable"; return false; }
    s.ok = true;
    return true;
}

bool load_static(Store& s, std::uint32_t row) {
    const AnchorState* rs = ok_anchor(L::kPedRowStride);
    const AnchorState* ix = ok_anchor(L::kPedIndexTable);
    const AnchorState* co = ok_anchor(L::kPedColumns);
    if (!rs || !ix || !co) { s.why = "gated: a static ped store anchor is not available (see the anchor lines)"; return false; }
    if (!rd(rs->addr, s.n) || !rd(ix->addr, s.row_index) || !rd(co->addr, s.columns)) { s.why = "store header unreadable"; return false; }
    return load_row(s, row);
}

bool load_generic(Store& s, std::uint32_t arch, std::uint32_t row) {
    const AnchorState* t = ok_anchor(L::kArchetypeTable);
    if (!t) { s.why = "gated: ecs.archetype_table is not available"; return false; }
    s.rec = t->addr + static_cast<std::uintptr_t>(arch) * E::kRecordSize;
    if (!rd(s.rec + E::kRecSlotsPerRow, s.n) || !rd(s.rec + E::kRecRowIndex, s.row_index) || !rd(s.rec + E::kRecColumns, s.columns)) {
        s.why = "record header unreadable";
        return false;
    }
    return load_row(s, row);
}

bool comp_addr(const Store& s, const E::Component& c, std::uintptr_t& out, std::string& why) {
    char b[160];
    if (!s.ok) { why = "store not loaded: " + s.why; return false; }
    std::uint32_t col = c.column_or_type;
    if (c.generic) {
        std::uint8_t sel = 0;
        if (c.column_or_type >= E::kTypeToColumnLen || !rd(s.rec + E::kRecTypeToColumn + c.column_or_type, sel)) { why = "selector unreadable"; return false; }
        if (sel == E::kAbsentColumn) { why = "component type absent in this archetype (selector 0xff)"; return false; }
        col = sel;
    }
    if (col >= s.n) { std::snprintf(b, sizeof b, "column 0x%x outside slots_per_row %u", col, s.n); why = b; return false; }
    const std::uint16_t idx = s.row[col];
    if (idx == E::kAbsentIndex) { why = "absent for this entity (index 0xffff)"; return false; }
    std::uintptr_t base = 0;
    std::uint64_t stride = 0;
    if (!rd(s.columns + static_cast<std::uintptr_t>(col) * E::kColumnSize, base) ||
        !rd(s.columns + static_cast<std::uintptr_t>(col) * E::kColumnSize + 8, stride)) { why = "column entry unreadable"; return false; }
    if (stride != c.size) {
        std::snprintf(b, sizeof b, "column stride 0x%llx != re-db size 0x%x of %s", static_cast<unsigned long long>(stride), c.size, c.name);
        why = b;
        return false;
    }
    if (!base) { why = "null column base"; return false; }
    out = base + static_cast<std::uintptr_t>(idx) * c.size;
    return true;
}

struct EcsFields {
    bool wanted_ok = false;
    int wanted_level = 0;
};

void read_ped_memory(Json& j, EcsFields& ef) {
    j.key("ped_mem").obj();
    const AnchorState* pa = ok_anchor(L::kPlayerPedLocal);
    std::uintptr_t ped = 0;
    std::uint8_t type = 0;
    std::uint32_t raw_id = 0;
    bool have_id = false;
    if (!pa) {
        const AnchorState* s = anchor(L::kPlayerPedLocal);
        j.key("ok").boolean(false).key("why").str("gated: " + (s ? s->why : std::string("no anchor")));
    } else if (!rd(pa->addr, ped)) {
        j.key("ok").boolean(false).key("why").str("player.ped.local unreadable");
    } else if (!ped) {
        j.key("ok").boolean(false).key("ptr").hex(0).key("why").str("null ped pointer");
    } else if (!rd(ped + L::entity::kTypeOff, type) || !rd(ped + L::entity::kEcsIdOff, raw_id)) {
        j.key("ok").boolean(false).key("ptr").hex(ped).key("why").str("entity unreadable");
    } else {
        have_id = true;
        const EcsId id = decode_ecs_id(raw_id);
        j.key("ok").boolean(true).key("ptr").hex(ped).key("type").u64(type).key("ecs_id").hex32(raw_id)
            .key("arch").u64(id.arch).key("row").u64(id.row);
        if (type != L::entity::kTypePed) j.key("note").str("entity_type is not 3 (ped)");
    }
    j.end_obj();

    j.key("ecs").obj();
    if (!have_id) { j.key("ok").boolean(false).key("why").str("no ped ECS id").end_obj(); return; }
    const EcsId id = decode_ecs_id(raw_id);
    if (id.arch != E::kPedArch) {
        j.key("ok").boolean(false).key("why").str("ECS archetype is not 3 (ped); stores not read").end_obj();
        return;
    }
    Store st, gen;
    load_static(st, id.row);
    load_generic(gen, id.arch, id.row);
    j.key("ok").boolean(st.ok || gen.ok);
    j.key("static").obj().key("ok").boolean(st.ok).key("n").u64(st.n);
    if (st.ok) j.key("row").str(hex_row(st.row.data(), st.row.size())); else j.key("why").str(st.why);
    j.end_obj();
    j.key("generic").obj().key("ok").boolean(gen.ok).key("n").u64(gen.n);
    if (gen.ok) j.key("row").str(hex_row(gen.row.data(), gen.row.size())); else j.key("why").str(gen.why);
    j.end_obj();

    Json why;
    why.obj();
    j.key("fields").obj();
    std::uintptr_t c = 0;
    std::string w;
    if (comp_addr(st, E::kPedLaw, c, w)) {
        std::int32_t lvl = 0, score = 0;
        std::uint32_t f14 = 0;
        if (rd(c + E::kWantedLevel, lvl) && rd(c + E::kWantedScore, score) && rd(c + E::kLawFlags14, f14)) {
            j.key("law_comp").hex(c).key("wanted_level").i64(lvl).key("wanted_score").i64(score).key("law_flags14").hex32(f14)
                .key("wanted_disabled").boolean((f14 >> 5) & 1u);
            ef.wanted_ok = true;
            ef.wanted_level = lvl;
        } else gated(why, "law", "fields unreadable");
    } else gated(why, "law", w);
    if (comp_addr(st, E::kPedCol32, c, w)) {
        std::uint32_t f = 0;
        if (rd(c + E::kCol32Flags, f))
            j.key("col32_flags234").hex32(f).key("ped_gravity_disabled").boolean((f >> E::kCol32GravityBit) & 1u);
        else gated(why, "col32", "field unreadable");
    } else gated(why, "col32", w);
    if (comp_addr(st, E::kPedCol0f, c, w)) {
        std::uint32_t f = 0;
        if (rd(c + E::kCol0fFlags, f)) j.key("col0f_flags15d4").hex32(f).key("infinite_ammo_clip").boolean((f >> 11) & 1u);
        else gated(why, "col0f", "field unreadable");
    } else gated(why, "col0f", w);
    if (comp_addr(gen, E::kGenHealth, c, w)) {
        std::uintptr_t host = 0, data = 0;
        float hp = 0, mx = 0;
        if (rd(c + E::kHealthHost, host) && host && rd(host + E::kHostHealthData, data) && data && rd(data + E::kDataHealth, hp) &&
            rd(data + E::kDataMaxHealth, mx))
            j.key("health_data").hex(data).key("health_f").f64(hp).key("max_health_f").f64(mx);
        else gated(why, "health", "health host/data chain unreadable or null");
    } else gated(why, "health", w);
    if (comp_addr(gen, E::kGenProofs, c, w)) {
        std::uint32_t f = 0;
        if (rd(c + E::kProofFlags, f)) j.key("proof_flags").hex32(f).key("invincible").boolean((f >> 4) & 1u);
        else gated(why, "proofs", "field unreadable");
    } else gated(why, "proofs", w);
    j.end_obj();
    why.end_obj();
    j.key("gated").raw(why.str());
    j.end_obj();
}

// ------------------------------------------------------------------ records
std::uint64_t clock_now() {
    return g_cfg.rate_clock_game ? g_api->game_time_ms() : static_cast<std::uint64_t>(GetTickCount64());
}

void begin_record(Json& j, const char* ev) {
    j.obj().key("ev").str(ev).key("seq").u64(++g_seq).key("unix_ms").u64(unix_ms_now()).key("wall_ms").u64(GetTickCount64())
        .key("game_time_ms").u64(g_api->game_time_ms())
        .key("stage").i64(g_cfg.stage);
}

void sample() {
    Json calls;
    calls.arr();
    Snap s;
    read_natives(s, calls, true);
    Json j;
    begin_record(j, "sample");
    json_snap(j, s);
    read_game_timer(j, s);
    read_gravity(j);
    EcsFields ef;
    read_ped_memory(j, ef);
    calls.end_arr();
    j.key("calls").raw(calls.str());
    j.end_obj();
    emit(j);
}

// ------------------------------------------------------------------ write probes (stage 4)
struct Pending {
    WriteProbe w = WriteProbe::count;
    std::uint64_t due = 0;           // clock_now() value; 0 = next tick
    const char* phase = "";
    Snap before;
    Vec3 target;
    int expect = 0;
};
std::vector<Pending> g_pending;

void json_write_head(Json& j, WriteProbe w, const char* phase) {
    begin_record(j, "write");
    j.key("probe").str(write_probe_name(w)).key("phase").str(phase);
}

// Verdict of one readback, also used for the immediate read in the same tick.
void json_verdict(Json& j, WriteProbe w, const Snap& before, const Snap& after, const Vec3& target, int expect) {
    j.key("verdict").obj();
    switch (w) {
    case WriteProbe::coords_up: {
        const MoveVerdict m = after.coords_ok && before.coords_ok ? evaluate_move(before.coords, after.coords, g_cfg.up, g_cfg.up_delta, 0.05)
                                                                   : MoveVerdict{};
        j.key("ok").boolean(m.ok).key("along_up").f64(m.along).key("off_axis").f64(m.off_axis).key("tol").f64(0.05)
            .key("up_axis").str(axis_name(g_cfg.up)).key("target");
        json_vec(j, target);
        break;
    }
    case WriteProbe::health:
        j.key("ok").boolean(after.health_ok && after.health == expect).key("expect").i64(expect);
        break;
    case WriteProbe::wanted_clear:
        j.key("ok").boolean(after.wanted_ok && after.wanted == 0).key("expect").i64(0);
        break;
    case WriteProbe::clock_add:
        j.key("ok").boolean(after.clock_ok && after.hours == expect).key("expect_hours").i64(expect);
        break;
    default:
        j.key("ok").boolean(false);
    }
    j.end_obj();
}

void refuse_write(WriteProbe w, const Snap& before, Json& calls, const std::string& why) {
    Json j;
    json_write_head(j, w, "refused");
    j.key("why").str(why).key("before").obj();
    json_snap(j, before);
    j.end_obj();
    calls.end_arr();
    j.key("calls").raw(calls.str()).end_obj();
    emit(j);
    logf(G6_LOG_WARN, "write probe %s refused: %s", write_probe_name(w), why.c_str());
}

void run_write(WriteProbe w, const char* source) {
    Json calls;
    calls.arr();
    Snap before;
    read_natives(before, calls, false);
    Vec3 target;
    int expect = 0;
    std::uint64_t args[G6_MAX_NATIVE_ARGS] = {};
    std::size_t argc = 0;
    switch (w) {
    case WriteProbe::coords_up: {
        if (!before.ped_ok) return refuse_write(w, before, calls, "no valid player ped handle");
        if (!before.coords_ok) return refuse_write(w, before, calls, std::string("coordinates before: ") + before.coords_why);
        if (!before.in_vehicle_known || before.in_vehicle) return refuse_write(w, before, calls, "the player must be on foot (IS_PED_IN_ANY_VEHICLE false)");
        target = apply_up(before.coords, g_cfg.up, g_cfg.up_delta);
        if (!check_coords(target, g_cfg.max_abs_coord).ok) return refuse_write(w, before, calls, "target coordinates implausible");
        args[0] = before.lo;
        args[1] = before.hi;
        vec3_slots(target, &args[2]);
        args[4] = args[5] = args[6] = args[7] = 0;   // the four BOOLs as entities.py passes them
        args[8] = float_slot(0.0f);                  // trailing float slot 8: not in nativedb (findings/phase5/23)
        argc = 9;
        break;
    }
    case WriteProbe::health: {
        if (!before.ped_ok || !before.health_ok) return refuse_write(w, before, calls, "health not readable before the write");
        if (!before.max_health_ok || before.max_health <= 0) return refuse_write(w, before, calls, "max health not readable before the write");
        const long long t = static_cast<long long>(before.health) + g_cfg.health_delta;
        if (t < 1 || t > before.max_health) return refuse_write(w, before, calls, "health + health_delta outside [1, max health]");
        expect = static_cast<int>(t);
        args[0] = before.lo;
        args[1] = before.hi;
        args[2] = static_cast<std::uint64_t>(static_cast<std::int64_t>(expect));
        args[3] = args[4] = 0;                       // instigator: no entity
        args[5] = args[6] = 0;
        argc = 7;
        break;
    }
    case WriteProbe::wanted_clear:
        if (!before.player_ok || !before.wanted_ok) return refuse_write(w, before, calls, "player index or wanted level not readable before the write");
        args[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(before.player));
        args[1] = 0;
        argc = 2;
        break;
    case WriteProbe::clock_add:
        if (!before.clock_ok || before.hours < 0 || before.hours > 23) return refuse_write(w, before, calls, "clock hours not readable or outside 0..23");
        expect = (before.hours + 1) % 24;
        args[0] = 1;
        args[1] = 0;
        args[2] = 0;
        argc = 3;
        break;
    default:
        return;
    }

    g_armed = w;
    const CallOut c = call(calls, write_probe_native(w), args, argc);
    g_armed = WriteProbe::count;
    Snap after;
    read_natives(after, calls, false);

    Json j;
    json_write_head(j, w, "immediate");
    j.key("source").str(source).key("called").boolean(c.called).key("rc").i64(c.rc);
    j.key("before").obj();
    json_snap(j, before);
    j.end_obj().key("after").obj();
    json_snap(j, after);
    j.end_obj();
    json_verdict(j, w, before, after, target, expect);
    calls.end_arr();
    j.key("calls").raw(calls.str()).end_obj();
    emit(j);
    logf(G6_LOG_INFO, "write probe %s executed (%s): native rc %d; readbacks follow on the next tick and after 1000 ms",
         write_probe_name(w), source, c.rc);
    if (c.called) {
        const std::uint64_t now = clock_now();
        g_pending.push_back({ w, 0, "next_tick", before, target, expect });
        g_pending.push_back({ w, now + 1000, "after_1000ms", before, target, expect });
    }
}

void run_pending(std::uint64_t now) {
    for (std::size_t i = 0; i < g_pending.size();) {
        Pending& p = g_pending[i];
        if (p.due != 0 && now < p.due) { ++i; continue; }
        Json calls;
        calls.arr();
        Snap after;
        read_natives(after, calls, false);
        Json j;
        json_write_head(j, p.w, p.phase);
        j.key("before").obj();
        json_snap(j, p.before);
        j.end_obj().key("after").obj();
        json_snap(j, after);
        j.end_obj();
        json_verdict(j, p.w, p.before, after, p.target, p.expect);
        calls.end_arr();
        j.key("calls").raw(calls.str()).end_obj();
        emit(j);
        g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void fire(const char* source) {
    WriteProbe w{};
    if (!g_queue.next(w)) {
        logf(G6_LOG_INFO, "trigger (%s): no write probe left", source);
        return;
    }
    g_queue.mark_done(w);   // once each, even when refused
    run_write(w, source);
}

bool foreground_is_this_process() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void poll_triggers() {
    if (g_cfg.hotkey_vk) {
        const bool down = (GetAsyncKeyState(static_cast<int>(g_cfg.hotkey_vk)) & 0x8000) != 0 && foreground_is_this_process();
        if (g_key_edge.rising(down)) fire("hotkey");
    }
    if (!g_cfg.trigger_file.empty()) {
        const std::uint64_t now = GetTickCount64();
        if (now - g_last_trigger_poll < 200) return;
        g_last_trigger_poll = now;
        const std::wstring p = resolve_path(g_cfg.trigger_file);
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        if (!DeleteFileW(p.c_str())) {
            if (!g_trigger_delete_logged) {
                g_trigger_delete_logged = true;
                logf(G6_LOG_WARN, "trigger file exists but cannot be deleted (error %lu); ignored so it cannot fire twice", GetLastError());
            }
            return;
        }
        fire("trigger_file");
    }
}

// ------------------------------------------------------------------ scripts
void tick_body() {
    if (g_first_tick) {
        g_first_tick = false;
        logf(G6_LOG_INFO, "first tick on the script thread (OS thread %lu); sampling every %llu ms (%s clock)", GetCurrentThreadId(),
             static_cast<unsigned long long>(g_rate.interval_ms()), g_cfg.rate_clock_game ? "game" : "wall");
    }
    const std::uint64_t now = clock_now();
    if (g_cfg.stage == 4) {
        run_pending(now);
        poll_triggers();
    }
    if (g_rate.due(now)) sample();
}

bool g_exception_logged = false;

// C++ exceptions (allocation failure) must not leave a script fiber: the runtime would stop the script.
void G6_CALL tick(void*) {
    try {
        tick_body();
    } catch (...) {
        g_armed = WriteProbe::count;
        if (!g_exception_logged) { g_exception_logged = true; logf(G6_LOG_ERROR, "C++ exception in the probe tick; tick skipped"); }
    }
}

void wait_body() {
    if (g_wait_done >= g_cfg.wait_probe_count) return;
    const std::uint64_t g0 = g_api->game_time_ms();
    const std::uint64_t w0 = GetTickCount64();
    const g6_status st = g_api->script_wait(g_cfg.wait_probe_ms);
    const std::uint64_t g1 = g_api->game_time_ms();
    const std::uint64_t w1 = GetTickCount64();
    ++g_wait_done;
    Json j;
    begin_record(j, "wait");
    j.key("req_ms").u64(g_cfg.wait_probe_ms).key("rc").i64(static_cast<int>(st)).key("game_elapsed_ms").u64(g1 - g0)
        .key("wall_elapsed_ms").u64(w1 - w0).key("n").u64(g_wait_done).end_obj();
    emit(j);
}

void G6_CALL wait_tick(void*) {
    try {
        wait_body();
    } catch (...) {
        if (!g_exception_logged) { g_exception_logged = true; logf(G6_LOG_ERROR, "C++ exception in the wait probe; skipped"); }
    }
}

// ------------------------------------------------------------------ load / unload
void reset_state() {
    g_cfg = Config{};
    g_out = INVALID_HANDLE_VALUE;
    g_bytes = 0;
    g_full_logged = false;
    for (Binding& b : g_bind) b = Binding{};
    for (unsigned& n : g_fail_logs) n = 0;
    g_rate = RateLimiter{};
    g_queue = WriteQueue{};
    g_key_edge = Edge{};
    g_seq = 0;
    g_first_tick = true;
    g_sid = g_wait_sid = 0;
    g_wait_done = 0;
    g_armed = WriteProbe::count;
    g_last_trigger_poll = 0;
    g_trigger_delete_logged = false;
    g_module_base = 0;
    g_pending.clear();
    g_exception_logged = false;
    for (AnchorState& a : g_anchors) a = AnchorState{};
}

void close_output() {
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    g_out = INVALID_HANDLE_VALUE;
}

g6_status load(const g6_host_api* api) {
    reset_state();
    g_api = api;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&reset_state), &self) ||
        !GetModuleFileNameW(self, g_dir, 1024)) {
        logf(G6_LOG_ERROR, "cannot find this DLL's folder; not loaded");
        return G6_E_INTERNAL;
    }
    if (wchar_t* slash = std::wcsrchr(g_dir, L'\\')) *slash = 0;
    std::swprintf(g_ini, 1100, L"%ls\\g6probe.ini", g_dir);
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {
        logf(G6_LOG_ERROR, "%s not found; not loaded ([probe] stage has no default)", utf8(g_ini).c_str());
        return G6_E_INVALID_ARG;
    }
    const ConfigResult cr = parse_config(WinIni{}, g_cfg);
    for (const std::string& e : cr.errors) logf(G6_LOG_ERROR, "config: %s", e.c_str());
    if (!cr.ok) { logf(G6_LOG_ERROR, "not loaded: fix %s", utf8(g_ini).c_str()); return G6_E_INVALID_ARG; }
    for (const std::string& w : cr.warnings) logf(G6_LOG_WARN, "config: %s", w.c_str());
    for (const std::string& r : cr.refused_writes) logf(G6_LOG_ERROR, "config: %s", r.c_str());

    g6_build_info bi{};
    bi.size = sizeof bi;
    const g6_status bst = api->get_build_info(&bi);
    if (bst != G6_OK || !bi.game_version || std::strcmp(bi.game_version, L::kGameBuild) != 0) {
        logf(G6_LOG_ERROR, "build info status %d, game version '%s': this probe's layout is for %s only; not loaded",
             static_cast<int>(bst), bi.game_version ? bi.game_version : "?", L::kGameBuild);
        return G6_E_UNSUPPORTED_BUILD;
    }

    const std::wstring out = resolve_path(g_cfg.jsonl);
    g_out = CreateFileW(out.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_out == INVALID_HANDLE_VALUE) {
        logf(G6_LOG_ERROR, "cannot open %s (error %lu); not loaded", utf8(out.c_str()).c_str(), GetLastError());
        return G6_E_INVALID_ARG;
    }
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(g_out, &sz)) g_bytes = static_cast<std::uint64_t>(sz.QuadPart);

    for (const NativeSpec& s : kNatives) {
        Binding& b = g_bind[static_cast<int>(s.id)];
        b = bind(s, g6_natives, G6_NATIVE_COUNT);
        if (b.state == BindState::ok)
            logf(G6_LOG_DEBUG, "native ok: %s 0x%016llX sig %s status %s handler rva 0x%x%s", s.name, static_cast<unsigned long long>(s.hash),
                 s.sig, b.status, b.handler_rva, s.write ? " (setter)" : "");
        else
            logf(G6_LOG_WARN, "native REFUSED: %s 0x%016llX: %s (status '%s')", s.name, static_cast<unsigned long long>(s.hash),
                 bind_state_str(b.state), b.status);
    }
    resolve_anchors();
    g_rate.set_hz(g_cfg.rate_hz);
    g_queue.reset(g_cfg.allow);
    g_session_unix_ms = unix_ms_now();

    Json m;
    m.obj().key("ev").str("meta").key("probe").str("g6probe").key("version").str(kVersion)
        .key("session").obj().key("pid").u64(GetCurrentProcessId()).key("unix_ms").u64(g_session_unix_ms).end_obj()
        .key("build").obj().key("game_version").str(bi.game_version).key("platform").str(bi.platform ? bi.platform : "")
        .key("natives_known").u64(bi.natives_known).key("natives_verified").u64(bi.natives_verified).end_obj()
        .key("stage").i64(g_cfg.stage)
        .key("config").obj().key("rate_hz").f64(g_cfg.rate_hz).key("rate_clock").str(g_cfg.rate_clock_game ? "game" : "wall")
        .key("max_abs_coord").f64(g_cfg.max_abs_coord).key("anchor_rva_check").boolean(g_cfg.anchor_rva_check)
        .key("up_axis");
    if (g_cfg.up.axis != Axis::unset) m.str(axis_name(g_cfg.up)); else m.null();
    m.key("up_delta_m").f64(g_cfg.up_delta).key("health_delta").i64(g_cfg.health_delta).key("hotkey_vk").u64(g_cfg.hotkey_vk)
        .key("trigger_file").str(g_cfg.trigger_file).key("wait_probe_ms").u64(g_cfg.wait_probe_ms).key("allow").arr();
    for (WriteProbe w : g_cfg.allow) m.str(write_probe_name(w));
    m.end_arr().key("refused_writes").arr();
    for (const std::string& r : cr.refused_writes) m.str(r);
    m.end_arr().end_obj().key("natives").arr();
    for (const NativeSpec& s : kNatives) {
        const Binding& b = g_bind[static_cast<int>(s.id)];
        m.obj().key("n").str(s.name).key("h").hex(s.hash).key("sig").str(s.sig).key("write").boolean(s.write)
            .key("bind").str(b.state == BindState::ok ? "ok" : bind_state_str(b.state)).key("status").str(b.status)
            .key("handler_rva").hex32(b.handler_rva).end_obj();
    }
    m.end_arr().key("module_base").hex(g_module_base).key("anchors").arr();
    for (const AnchorState& a : g_anchors) {
        m.obj().key("name").str(a.a->name).key("ok").boolean(a.ok).key("addr").hex(a.addr).key("rva").hex(a.rva)
            .key("rva_redb").hex32(a.a->rva);
        if (!a.ok) m.key("why").str(a.why);
        m.end_obj();
    }
    m.end_arr().key("gated").arr()
        .str("vehicle ECS rows: no anchor for the vehicle entity pointer; the vehicle handle from GET_VEHICLE_PED_IS_USING is logged instead")
        .end_arr().end_obj();
    emit(m);

    const g6_script_desc d{ sizeof(g6_script_desc), "g6probe", &tick, nullptr };
    const g6_status rs = api->script_register(&d, &g_sid);
    if (rs != G6_OK) {
        logf(G6_LOG_ERROR, "script_register failed (%d); not loaded", static_cast<int>(rs));
        close_output();
        return rs;
    }
    if (g_cfg.wait_probe_ms) {
        const g6_script_desc dw{ sizeof(g6_script_desc), "g6probe.wait", &wait_tick, nullptr };
        if (api->script_register(&dw, &g_wait_sid) != G6_OK) logf(G6_LOG_WARN, "wait probe script not registered");
    }
    std::string allow;
    for (WriteProbe w : g_cfg.allow) { if (!allow.empty()) allow += ','; allow += write_probe_name(w); }
    if (g_cfg.stage == 3)
        logf(G6_LOG_INFO, "g6probe %s: stage=3 (read-only: no setter is ever called) rate=%.2f Hz jsonl='%s'", kVersion, g_cfg.rate_hz,
             utf8(out.c_str()).c_str());
    else
        logf(G6_LOG_INFO, "g6probe %s: stage=4 rate=%.2f Hz jsonl='%s'; write probes in order: %s; up_axis=%s up_delta_m=%.2f; "
             "fire with hotkey vk 0x%02X (game window in front) or by creating '%s'", kVersion, g_cfg.rate_hz, utf8(out.c_str()).c_str(),
             allow.empty() ? "(none)" : allow.c_str(), axis_name(g_cfg.up), g_cfg.up_delta, g_cfg.hotkey_vk,
             utf8(resolve_path(g_cfg.trigger_file).c_str()).c_str());
    return G6_OK;
}

} // namespace
} // namespace g6probe

G6PROBE_EXPORT const g6_plugin_info* G6_CALL g6_plugin_query(void) {
    static const g6_plugin_info info = { sizeof(g6_plugin_info), "g6probe", g6probe::kVersion, G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR };
    return &info;
}

G6PROBE_EXPORT g6_status G6_CALL g6_plugin_load(const g6_host_api* api) {
    if (!api || api->size < sizeof(g6_host_api) || api->version_major != G6_API_VERSION_MAJOR) return G6_E_UNSUPPORTED_BUILD;
    try {
        const g6_status st = g6probe::load(api);
        if (st != G6_OK) g6probe::close_output();
        return st;
    } catch (...) {
        g6probe::close_output();
        return G6_E_INTERNAL;
    }
}

G6PROBE_EXPORT void G6_CALL g6_plugin_unload(void) {
    using namespace g6probe;
    if (g_api) {
        if (g_sid) g_api->script_unregister(g_sid);
        if (g_wait_sid) g_api->script_unregister(g_wait_sid);
        logf(G6_LOG_INFO, "unloaded after %llu record(s)", static_cast<unsigned long long>(g_seq));
    }
    close_output();
    g_sid = g_wait_sid = 0;
    g_api = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
