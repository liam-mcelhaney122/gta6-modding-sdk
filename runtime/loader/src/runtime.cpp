// gta6mod_runtime: in-process runtime for GTA VI 1.0.2.1 under a VIctory-style host.
// Contract: docs/runtime_design.md. Read-only by default (see README.md).
#include "gta6mod/gta6mod.h"
#include "gta6mod/patterns.hpp"

#include "build_consts.hpp"
#include "g6rt_test_api.h"
#include "game_clock.hpp"
#include "guard.hpp"
#include "hook.hpp"
#include "identity.hpp"
#include "log.hpp"
#include "manifest_1_0_2_1.inc"
#include "natives.hpp"
#include "pattern_scan.hpp"
#include "scheduler.hpp"
#include "vfs.hpp"
#include "x64_decode.hpp"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#define G6RT_VERSION "0.2.0"

namespace g6rt {
namespace {

namespace m = g6rt::manifest;
namespace bc = g6rt::build;

// ------------------------------------------------------------------ config
enum class Mode : int { readonly = 0, observe = 1, full = 2 };
// Which script thread plugin scripts tick on (design §4). observe = never tick, log only.
enum class Bind : int { first = 0, hash = 1, observe = 2 };

struct Config {
    Mode mode = Mode::readonly;
    bool strict_identity = false;
    bool strict_patterns = false;
    wchar_t exe_path[1024] = {};
    std::uint32_t wait_for_game_ms = 120000;
    std::uint32_t host_script_hash = 0;
    Bind bind = Bind::first;
    bool clock_system = false;                 // [clock] source=system: never use the game timer
    vfs::Config vfs;
    char vfs_root[520] = {};                   // [vfs] root: overlay queued by the runtime itself
    std::uint32_t min_tick_interval_ms = 0;
    std::uint32_t fiber_stack_kb = 1024;
    bool offline_attested = false;             // [guard] offline_attested=1 (design §7 input 1)
    std::uint32_t guard_interval_ms = 5000;    // [guard] scan_interval_ms (design §7 input 2)
    log::Level level = log::Level::info;
};

Config g_cfg;
wchar_t g_dir[1024] = {};

char g_ini_a[1200] = {};   // ANSI path of the ini, for the [vfs] strings the engine consumes as char*

void read_config() noexcept {
    wchar_t ini[1100];
    std::swprintf(ini, 1100, L"%ls\\gta6mod_runtime.ini", g_dir);
    WideCharToMultiByte(CP_ACP, 0, ini, -1, g_ini_a, static_cast<int>(sizeof g_ini_a), nullptr, nullptr);
    wchar_t v[1024];
    GetPrivateProfileStringW(L"runtime", L"mode", L"readonly", v, 1024, ini);
    if (_wcsicmp(v, L"observe") == 0) g_cfg.mode = Mode::observe;
    else if (_wcsicmp(v, L"full") == 0) g_cfg.mode = Mode::full;
    else g_cfg.mode = Mode::readonly;
    GetPrivateProfileStringW(L"runtime", L"identity", L"normal", v, 1024, ini);
    g_cfg.strict_identity = _wcsicmp(v, L"strict") == 0;
    g_cfg.strict_patterns = GetPrivateProfileIntW(L"runtime", L"strict_patterns", 0, ini) != 0;
    GetPrivateProfileStringW(L"runtime", L"exe_path", L"", g_cfg.exe_path, 1024, ini);
    g_cfg.wait_for_game_ms = GetPrivateProfileIntW(L"runtime", L"wait_for_game_ms", 120000, ini);
    GetPrivateProfileStringW(L"script", L"host_script_hash", L"0", v, 1024, ini);
    g_cfg.host_script_hash = static_cast<std::uint32_t>(std::wcstoull(v, nullptr, 0));
    GetPrivateProfileStringW(L"script", L"bind", L"", v, 1024, ini);
    if (_wcsicmp(v, L"observe") == 0) g_cfg.bind = Bind::observe;
    else if (_wcsicmp(v, L"hash") == 0) g_cfg.bind = Bind::hash;
    else if (_wcsicmp(v, L"first") == 0) g_cfg.bind = Bind::first;
    else g_cfg.bind = g_cfg.host_script_hash ? Bind::hash : Bind::first;   // key absent: round-1 behaviour
    GetPrivateProfileStringW(L"clock", L"source", L"auto", v, 1024, ini);
    g_cfg.clock_system = _wcsicmp(v, L"system") == 0;
    GetPrivateProfileStringW(L"vfs", L"mode", L"off", v, 1024, ini);
    g_cfg.vfs.enabled = _wcsicmp(v, L"overlay") == 0;
    GetPrivateProfileStringA("vfs", "path_map", "", g_cfg.vfs.path_map, sizeof g_cfg.vfs.path_map, g_ini_a);
    GetPrivateProfileStringA("vfs", "mount_points", "platform:/,common:/", g_cfg.vfs.mount_points, sizeof g_cfg.vfs.mount_points, g_ini_a);
    GetPrivateProfileStringA("vfs", "root", "", g_cfg.vfs_root, sizeof g_cfg.vfs_root, g_ini_a);
    g_cfg.min_tick_interval_ms = GetPrivateProfileIntW(L"script", L"min_tick_interval_ms", 0, ini);
    g_cfg.fiber_stack_kb = GetPrivateProfileIntW(L"script", L"fiber_stack_kb", 1024, ini);
    g_cfg.offline_attested = GetPrivateProfileIntW(L"guard", L"offline_attested", 0, ini) == 1;
    g_cfg.guard_interval_ms = GetPrivateProfileIntW(L"guard", L"scan_interval_ms", 5000, ini);
    if (g_cfg.guard_interval_ms < 1000) g_cfg.guard_interval_ms = 1000;
    if (g_cfg.guard_interval_ms > 10000) g_cfg.guard_interval_ms = 10000;   // must stay well under Gate::kStaleMs
    GetPrivateProfileStringW(L"log", L"level", L"info", v, 1024, ini);
    if (_wcsicmp(v, L"debug") == 0) g_cfg.level = log::Level::debug;
    else if (_wcsicmp(v, L"warn") == 0) g_cfg.level = log::Level::warn;
    else g_cfg.level = log::Level::info;
}

const char* mode_str(Mode md) noexcept {
    return md == Mode::full ? "full" : md == Mode::observe ? "observe" : "readonly";
}
const char* bind_str(Bind b) noexcept { return b == Bind::hash ? "hash" : b == Bind::observe ? "observe" : "first"; }

// ------------------------------------------------------------------ state
std::atomic<int> g_init_status{ G6_E_UNSUPPORTED_BUILD };   // result of init (G6_OK when identity+patterns passed)
std::atomic<bool> g_identity_ok{ false };
std::uint8_t* g_base = nullptr;
std::size_t g_size = 0;
const std::uint8_t* g_text_lo = nullptr;
const std::uint8_t* g_text_hi = nullptr;
std::uint8_t g_exe_sha[32] = {};

struct AnchorResult {
    const g6::patterns::anchor* a = nullptr;
    std::uint32_t count = 0;
    const std::uint8_t* addr = nullptr;   // after deref; valid when count == 1
    const std::uint8_t* site = nullptr;   // signature match (before deref); null for chain anchors
    std::int64_t manifest_rva = -1;
    bool bad_signature = false;
    bool chain = false;                   // resolved through an xref chain (empty signature)
};
std::vector<AnchorResult> g_anchors;
const std::uint8_t* g_run = nullptr;

// ------------------------------------------------------------------ anchors
std::int64_t manifest_anchor_rva(const char* name) noexcept {
    std::size_t lo = 0, hi = sizeof(m::kAnchorRvas) / sizeof(m::kAnchorRvas[0]);
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int c = std::strcmp(m::kAnchorRvas[mid].name, name);
        if (c == 0) return m::kAnchorRvas[mid].rva;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

const IMAGE_NT_HEADERS64* nt_of(const std::uint8_t* base) noexcept {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
}

struct AnchorSummary {
    std::uint32_t exact = 0, missing = 0, ambiguous = 0, unsigned_n = 0, bad_sig = 0, rva_mismatch = 0, no_manifest = 0;
    std::uint32_t via_chain = 0, chain_failed = 0;            // unsigned anchors resolved by g6::patterns::resolve_anchor
    std::uint32_t xcheck = 0, xcheck_bad = 0;                 // signed anchors re-resolved by resolve_anchor (sample)
    bool required_ok = false;
    char required_detail[512] = {};
    const std::uint8_t* text_lo = nullptr;
    const std::uint8_t* text_hi = nullptr;
    const std::uint8_t* run = nullptr;
};

void appendf(char* buf, std::size_t cap, const char* fmt, ...) noexcept {
    const std::size_t used = std::strlen(buf);
    if (used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
}

const AnchorResult* find_result(const std::vector<AnchorResult>& v, const char* name) noexcept {
    for (const auto& r : v) if (std::strcmp(r.a->name, name) == 0) return &r;
    return nullptr;
}

// Headers must already be validated (identity::check_headers) for `base`.
bool resolve_all(const std::uint8_t* base, std::size_t size, std::vector<AnchorResult>& out, AnchorSummary& sum,
                 bool log_each, std::size_t xcheck_stride) {
    out.clear();
    sum = AnchorSummary{};
    const auto* nt = nt_of(base);
    const auto* sh = IMAGE_FIRST_SECTION(nt);
    std::vector<scan::Compiled> pats(g6::patterns::count);
    out.resize(g6::patterns::count);
    for (std::size_t i = 0; i < g6::patterns::count; ++i) {
        out[i].a = &g6::patterns::table[i];
        out[i].manifest_rva = manifest_anchor_rva(out[i].a->name);
        if (!out[i].a->signature[0]) continue;
        if (!scan::parse(out[i].a->signature, pats[i])) out[i].bad_signature = true;
    }
    std::vector<const std::uint8_t*> first(g6::patterns::count, nullptr);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!(sh[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::size_t va = sh[s].VirtualAddress;
        std::size_t len = sh[s].Misc.VirtualSize;
        if (va >= size) continue;
        if (len > size - va) len = size - va;
        if (!sum.text_lo || base + va < sum.text_lo) sum.text_lo = base + va;
        if (base + va + len > sum.text_hi) sum.text_hi = base + va + len;
        if (!identity::readable_range(base + va, len)) {
            appendf(sum.required_detail, sizeof sum.required_detail, "section %u not fully readable; ", s);
            return false;
        }
        std::vector<scan::Hit> hits;
        scan::scan_all(base + va, len, pats, hits);
        for (std::size_t i = 0; i < hits.size(); ++i) {
            if (hits[i].count && !first[i]) first[i] = base + va + hits[i].first;
            out[i].count += hits[i].count;
        }
    }
    // The largest executable section is the `code` range for resolve_anchor (chains + cross-check).
    const std::uint8_t* code = nullptr;
    std::size_t code_size = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!(sh[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) || sh[s].VirtualAddress >= size) continue;
        std::size_t len = sh[s].Misc.VirtualSize;
        if (len > size - sh[s].VirtualAddress) len = size - sh[s].VirtualAddress;
        if (len > code_size) { code = base + sh[s].VirtualAddress; code_size = len; }
    }
    std::size_t signed_seen = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        AnchorResult& r = out[i];
        if (!r.a->signature[0]) {
            // Xref-chain anchors (patterns.hpp chains[]): resolved by the generated resolver itself.
            if (!g6::patterns::chain_by_name(r.a->name) || !code) {
                ++sum.unsigned_n;
                if (log_each) G6LOG_I("anchor %-48s unsigned by design (no chain; not resolved)", r.a->name);
                continue;
            }
            const g6::patterns::result cr = g6::patterns::resolve_anchor(base, size, code, code_size, *r.a);
            r.chain = true;
            if (cr.state != g6::patterns::status::ok || !cr.address) {
                ++sum.chain_failed;
                r.count = cr.state == g6::patterns::status::ambiguous ? 2u : 0u;
                if (log_each) G6LOG_W("anchor %-48s chain FAILED (state %d)", r.a->name, static_cast<int>(cr.state));
                continue;
            }
            r.count = 1;
            r.addr = cr.address;
            ++sum.via_chain;
            const std::int64_t rva = r.addr - base;
            if (r.manifest_rva >= 0 && rva != r.manifest_rva) {
                ++sum.rva_mismatch;
                if (log_each) G6LOG_W("anchor %-48s chain rva 0x%llx != manifest 0x%llx (chain wins)", r.a->name,
                                      static_cast<long long>(rva), static_cast<long long>(r.manifest_rva));
            } else if (log_each) {
                G6LOG_I("anchor %-48s rva 0x%llx via xref chain", r.a->name, static_cast<long long>(rva));
            }
            continue;
        }
        if (r.bad_signature) { ++sum.bad_sig; G6LOG_W("anchor %-48s signature does not parse", r.a->name); continue; }
        if (r.count == 0) { ++sum.missing; if (log_each) G6LOG_W("anchor %-48s NOT FOUND", r.a->name); continue; }
        if (r.count > 1) { ++sum.ambiguous; if (log_each) G6LOG_W("anchor %-48s AMBIGUOUS (%u matches)", r.a->name, r.count); continue; }
        r.site = first[i];
        r.addr = g6::patterns::deref(first[i], *r.a);
        ++sum.exact;
        if (xcheck_stride && code && (signed_seen++ % xcheck_stride) == 0) {
            // Same anchor through the generated resolve_anchor (find + deref): must agree with the one-pass scanner.
            const g6::patterns::result gr = g6::patterns::resolve_anchor(base, size, code, code_size, *r.a);
            ++sum.xcheck;
            if (gr.state != g6::patterns::status::ok || gr.address != r.addr) {
                ++sum.xcheck_bad;
                G6LOG_W("anchor %-48s scanner/resolve_anchor disagree (state %d)", r.a->name, static_cast<int>(gr.state));
            }
        }
        const std::int64_t rva = r.addr - base;
        if (r.manifest_rva < 0) {
            ++sum.no_manifest;
            if (log_each) G6LOG_I("anchor %-48s rva 0x%llx (no manifest RVA)", r.a->name, static_cast<long long>(rva));
        } else if (rva != r.manifest_rva) {
            ++sum.rva_mismatch;
            if (log_each) G6LOG_W("anchor %-48s rva 0x%llx != manifest 0x%llx (pattern wins)", r.a->name,
                                  static_cast<long long>(rva), static_cast<long long>(r.manifest_rva));
        } else if (log_each) {
            G6LOG_D("anchor %-48s rva 0x%llx ok", r.a->name, static_cast<long long>(rva));
        }
    }

    // Required anchors.
    bool ok = true;
    auto need = [&](const char* name) -> const AnchorResult* {
        const AnchorResult* r = find_result(out, name);
        if (!r) { ok = false; appendf(sum.required_detail, sizeof sum.required_detail, "%s absent from patterns.hpp; ", name); return nullptr; }
        if (r->count != 1) { ok = false; appendf(sum.required_detail, sizeof sum.required_detail, "%s matched %u times; ", name, r->count); return nullptr; }
        return r;
    };
    const AnchorResult* run = need("script.exec.run");
    const AnchorResult* vt = need("script.exec.vtable");
    const AnchorResult* sec = need("script.native.handler.set_entity_coords");
    need("script.native.lock_fn");
    if (run && vt) {
        const std::uint8_t* slot = vt->addr + 8 * bc::kExecVtableRunSlot;
        if (vt->addr < base || slot + 8 > base + size || !identity::readable_range(slot, 8)) {
            ok = false;
            appendf(sum.required_detail, sizeof sum.required_detail, "exec vtable outside the image; ");
        } else {
            std::uint64_t v;
            std::memcpy(&v, slot, 8);
            const std::uint64_t run_rva = static_cast<std::uint64_t>(run->addr - base);
            if (v != reinterpret_cast<std::uint64_t>(run->addr) && v - nt->OptionalHeader.ImageBase != run_rva) {
                ok = false;
                appendf(sum.required_detail, sizeof sum.required_detail, "exec vtable slot %u = 0x%llx is not Run; ",
                        bc::kExecVtableRunSlot, static_cast<unsigned long long>(v));
            }
        }
        sum.run = run->addr;
    }
    if (sec) {
        const std::ptrdiff_t ni = natives::find(bc::kCheckNativeHash);
        if (ni < 0 || sec->addr != base + natives::rva_at(static_cast<std::size_t>(ni))) {
            ok = false;
            appendf(sum.required_detail, sizeof sum.required_detail, "SET_ENTITY_COORDS anchor != manifest handler RVA; ");
        } else {
            std::uint64_t id = 0;
            const natives::Verify v = natives::verify_selfid(sec->addr, sum.text_lo, sum.text_hi, bc::kCheckNativeHash, &id);
            if (v != natives::Verify::ok) {
                ok = false;
                appendf(sum.required_detail, sizeof sum.required_detail, "SET_ENTITY_COORDS self-id: %s; ", natives::verify_str(v));
            }
        }
    }
    sum.required_ok = ok;
    return ok;
}

vfs::Anchors vfs_anchors(const std::vector<AnchorResult>& v) noexcept {
    auto get = [&](const char* n) -> const std::uint8_t* {
        const AnchorResult* r = find_result(v, n);
        return r && r->count == 1 ? r->addr : nullptr;
    };
    vfs::Anchors a;
    a.mount_global = get("fs.mount.mount_global");
    a.rel_init = get("fs.device.relative.init");
    a.rel_mount_as = get("fs.device.relative.mount_as");
    a.rel_vtable = get("fs.device.relative.vtable");
    if (const AnchorResult* r = find_result(v, "fs.device.relative.vtable"); r && r->count == 1) a.rel_vtable_site = r->site;
    a.base_ctor = get("fs.device.base.ctor");
    a.mount_table = get("fs.mount.table");
    a.vt_base = get("fs.device.base.vtable");
    a.vt_local = get("fs.device.local.vtable");
    a.vt_packfile = get("fs.device.packfile.vtable");
    a.vt_memory = get("fs.device.memory.vtable");
    a.vt_growbuffer = get("fs.device.growbuffer.vtable");
    a.vt_count = get("fs.device.count.vtable");
    a.vt_dstorage = get("fs.device.dstorage.vtable");
    a.vt_rawstreamer = get("fs.device.rawstreamer.vtable");
    return a;
}

// ------------------------------------------------------------------ TLS publish
enum class TlsMode : int { unavailable = 0, native_gs = 1, host_thunk = 2 };
TlsMode g_tls_mode = TlsMode::unavailable;
using TlsThunk = void** (*)();
TlsThunk g_tls_thunk = nullptr;

void classify_tls_site(const std::uint8_t* run) noexcept {
    g_tls_mode = TlsMode::unavailable;
    const std::uint8_t* site = run + bc::kRunTlsSiteOff;
    if (!identity::readable_range(site, 9 + sizeof bc::kRunTlsSiteTail)) { G6LOG_W("TLS site unreadable"); return; }
    G6LOG_I("TLS site Run+0x%x: %02X %02X %02X %02X %02X %02X %02X %02X %02X", bc::kRunTlsSiteOff, site[0], site[1], site[2],
            site[3], site[4], site[5], site[6], site[7], site[8]);
    if (std::memcmp(site + 9, bc::kRunTlsSiteTail, sizeof bc::kRunTlsSiteTail) != 0) {
        G6LOG_W("TLS site tail differs from 1.0.2.1 (mov r14,[rax]; mov r12,[r14+0x1478]; mov [r14+0x1478],rsi): ticks disabled");
        return;
    }
    if (std::memcmp(site, bc::kRunTlsSiteGs, 9) == 0) {
        g_tls_mode = TlsMode::native_gs;
        G6LOG_I("TLS access: console-faithful gs:[0x58] load");
        return;
    }
    if (site[0] == 0xE8) {
        std::int32_t rel;
        std::memcpy(&rel, site + 1, 4);
        const std::uint8_t* thunk = site + 5 + rel;
        MEMORY_BASIC_INFORMATION mbi{};
        const bool exec = VirtualQuery(thunk, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                          ((mbi.Protect & 0xFF) == PAGE_EXECUTE_READ || (mbi.Protect & 0xFF) == PAGE_EXECUTE ||
                           (mbi.Protect & 0xFF) == PAGE_EXECUTE_READWRITE);
        const bool in_image = thunk >= g_base && thunk < g_base + g_size;
        if (!exec || in_image) {
            G6LOG_W("TLS site calls %p (exec=%d in_image=%d): not a host TLS thunk, ticks disabled", thunk, exec, in_image);
            return;
        }
        g_tls_thunk = reinterpret_cast<TlsThunk>(const_cast<std::uint8_t*>(thunk));
        g_tls_mode = TlsMode::host_thunk;
        G6LOG_I("TLS access: host thunk %p (replaces mov rax,gs:[0x58]; research/03 §2.1) [unverified live]", thunk);
        return;
    }
    G6LOG_W("TLS site has an unknown form: ticks disabled");
}

// Address of this thread's active-exec-context TLS slot, or null.
void** tls_active_ctx_slot() noexcept {
    void** arr = nullptr;
    if (g_tls_mode == TlsMode::native_gs) arr = reinterpret_cast<void**>(__readgsqword(0x58));
    else if (g_tls_mode == TlsMode::host_thunk) arr = g_tls_thunk();
    else return nullptr;
    if (!arr) return nullptr;
    std::uint8_t* block = static_cast<std::uint8_t*>(arr[0]);   // the game reads [rax] (its _tls_index is 0)
    if (!block) return nullptr;
    static std::uint8_t* s_checked = nullptr;   // only the bound script thread gets here
    if (s_checked != block) {
        if (!identity::readable_range(block, bc::kTlsBlockSize)) return nullptr;
        s_checked = block;
    }
    return reinterpret_cast<void**>(block + bc::kTlsActiveExecCtx);
}

// ------------------------------------------------------------------ single-player guard
// design §7: attestation + own-process TCP check (guard.cpp). No native is called for it.
enum class Session : int { unknown = 0, single_player = 1, online = 2 };
std::atomic<int> g_session{ static_cast<int>(Session::unknown) };
std::atomic<DWORD> g_tick_tid{ 0 };   // OS thread currently inside our tick window
guard::Gate g_gate;

void set_session(Session s, const char* why) noexcept {
    const int old = g_session.exchange(static_cast<int>(s));
    if (old != static_cast<int>(s))
        G6LOG_I("guard -> %s (%s)", s == Session::single_player ? "OPEN (offline attested, no remote connection)" :
                s == Session::online ? "CLOSED" : "closed", why);
}

// Called each tick on the script thread, and by the native/overlay paths before they act.
bool guard_open() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (g_gate.open(now)) { set_session(Session::single_player, "both checks pass"); return true; }
    set_session(g_gate.tripped() ? Session::online : Session::unknown, g_gate.why(now));
    return false;
}
void guard_evaluate() noexcept { (void)guard_open(); }

// ------------------------------------------------------------------ game clock
// game_time_ms = clock::Mixer output: the game timer block once validated, the system timer before
// that and as the fallback. Updated from the Run hook (every tick, and every >= 50 ms in observe).
std::atomic<std::uint64_t> g_game_ms{ 0 };
LARGE_INTEGER g_qpf{};
const volatile std::uint32_t* g_clock_ptr = nullptr;   // game ms (block +0), null = system timer only
clock::Mixer g_mixer;
std::atomic<bool> g_clock_busy{ false };
ULONGLONG g_last_clock_sample = 0;

std::uint64_t wall_ms() noexcept {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return g_qpf.QuadPart ? static_cast<std::uint64_t>(now.QuadPart / g_qpf.QuadPart * 1000 +
                                                       (now.QuadPart % g_qpf.QuadPart) * 1000 / g_qpf.QuadPart)
                          : GetTickCount64();
}

const char* clock_state_str() noexcept {
    switch (g_mixer.state()) {
    case clock::Mixer::State::trusted: return "game timer";
    case clock::Mixer::State::candidate: return "system timer (game timer being validated)";
    default: return "system timer";
    }
}

void clock_update() noexcept {
    if (g_clock_busy.exchange(true, std::memory_order_acquire)) return;   // another thread is sampling
    const bool has = g_clock_ptr != nullptr;
    const std::uint32_t v = has ? *g_clock_ptr : 0;
    const clock::Mixer::Event ev = g_mixer.feed(has, v, wall_ms());
    g_game_ms.store(g_mixer.now());
    switch (ev) {
    case clock::Mixer::Event::trusted:
        G6LOG_I("game clock: validated (%u consecutive plausible samples, game timer %u ms); game_time_ms now follows the game timer",
                clock::Mixer::kNeedGood, v);
        break;
    case clock::Mixer::Event::anomaly:
        if (g_mixer.anomalies() <= 4)
            G6LOG_W("game clock: %s (sample %u ms); %u of %u anomalies", g_mixer.last_reason(), v, g_mixer.anomalies(), clock::Mixer::kMaxAnomalies);
        break;
    case clock::Mixer::Event::rejected:
        G6LOG_W("game clock: %s; game_time_ms falls back to the system timer for this session", g_mixer.last_reason());
        break;
    default: break;
    }
    g_clock_busy.store(false, std::memory_order_release);
}

// GET_GAME_TIMER / GET_FRAME_COUNT / GET_FRAME_TIME handler bodies -> one data block (build_consts.hpp).
// Returns the block or null, with the reason in `why`.
const std::uint8_t* resolve_clock(const std::uint8_t* base, std::size_t size, const std::uint8_t* lo, const std::uint8_t* hi, bool live,
                                  char* why, std::size_t cap) noexcept {
    struct Want { std::uint64_t hash; clock::LoadKind kind; std::uint32_t off; const char* name; };
    const Want want[3] = {
        { bc::kNativeGetGameTimer, clock::LoadKind::mov_eax_rip32, 0, "GET_GAME_TIMER" },
        { bc::kNativeGetFrameCount, clock::LoadKind::mov_eax_rip32, bc::kClockFrameCountOff, "GET_FRAME_COUNT" },
        { bc::kNativeGetFrameTime, clock::LoadKind::vmovss_xmm0_rip, bc::kClockTimeStepOff, "GET_FRAME_TIME" },
    };
    const std::uint8_t* block = nullptr;
    why[0] = 0;
    for (const Want& w : want) {
        const std::ptrdiff_t i = natives::find(w.hash);
        if (i < 0) { std::snprintf(why, cap, "%s 0x%016llX not in the manifest", w.name, static_cast<unsigned long long>(w.hash)); return nullptr; }
        const std::uint8_t* fn = base + natives::rva_at(static_cast<std::size_t>(i));
        std::uint64_t id = 0;
        const natives::Verify sv = natives::verify_selfid(fn, lo, hi, w.hash, &id);
        if (sv != natives::Verify::ok) { std::snprintf(why, cap, "%s handler self-id: %s", w.name, natives::verify_str(sv)); return nullptr; }
        const clock::LoadSite ls = clock::find_handler_load(fn, lo, hi, w.kind);
        if (!ls.target) { std::snprintf(why, cap, "%s handler rva 0x%llx: %s", w.name, static_cast<long long>(fn - base), ls.why); return nullptr; }
        if (!block) block = ls.target - w.off;
        if (ls.target != block + w.off) {
            std::snprintf(why, cap, "%s reads rva 0x%llx, expected block+0x%x = rva 0x%llx", w.name, static_cast<long long>(ls.target - base),
                          w.off, static_cast<long long>(block + w.off - base));
            return nullptr;
        }
        appendf(why, cap, "%s rva 0x%llx -> %s0x%llx; ", w.name, static_cast<long long>(fn - base), ls.core ? "core " : "inline ",
                static_cast<long long>((ls.core ? ls.core : ls.insn) - base));
    }
    char sec[16] = {};
    const char* bad = clock::check_block(base, size, block, bc::kClockBlockLen, live, sec, sizeof sec);
    if (bad) { std::snprintf(why, cap, "clock block rva 0x%llx: %s", static_cast<long long>(block - base), bad); return nullptr; }
    appendf(why, cap, "block rva 0x%llx in %s (writable, not executable)", static_cast<long long>(block - base), sec);
    return block;
}

// ------------------------------------------------------------------ hook
hook::InlineHook g_run_hook;
using RunFn = std::uint64_t (*)(void*, void*, std::uint64_t, void*, void*, std::uint64_t);
RunFn g_orig_run = nullptr;
std::atomic<bool> g_hook_active{ false };
std::atomic<bool> g_ticks_enabled{ false };
std::atomic<std::uint32_t> g_hook_faults{ 0 };

struct SeenThread { std::uint32_t id, name; };
SeenThread g_seen[256];
std::uint32_t g_nseen = 0;
// Distinct scrThread name hashes (thread+0x2b8) and the states (thread+0x344, bit per value < 32) seen with each.
struct SeenName { std::uint32_t name, states, other_states; };
SeenName g_names[512];
std::uint32_t g_nnames = 0, g_name_logs = 0;
SRWLOCK g_seen_lock = SRWLOCK_INIT;
std::atomic<std::uint64_t> g_run_calls{ 0 };
ULONGLONG g_last_stats = 0;
std::uint32_t g_bound_id = 0;
ULONGLONG g_bound_seen = 0;
ULONGLONG g_last_tick_ms = 0;

void note_name(std::uint32_t id, std::uint32_t name, std::uint32_t state) noexcept {
    SeenName* e = nullptr;
    for (std::uint32_t i = 0; i < g_nnames; ++i) if (g_names[i].name == name) { e = &g_names[i]; break; }
    const std::uint32_t bit = state < 32 ? (1u << state) : 0u;
    if (!e) {
        if (g_nnames >= 512) return;
        e = &g_names[g_nnames++];
        *e = { name, bit, bit ? 0u : 1u };
        if (g_name_logs++ < 2000)
            G6LOG_I("script name hash first seen: 0x%08X state=%u (thread id=%u); distinct names so far: %u", name, state, id, g_nnames);
        return;
    }
    if (bit ? !(e->states & bit) : !e->other_states) {
        if (bit) e->states |= bit; else e->other_states = 1;
        if (g_name_logs++ < 2000) G6LOG_I("script name hash 0x%08X: new state %u (thread id=%u)", name, state, id);
    }
}

void observe(std::uint32_t id, std::uint32_t name, std::uint32_t state, void* ctx, void* thread) noexcept {
    g_run_calls.fetch_add(1, std::memory_order_relaxed);
    if (!TryAcquireSRWLockExclusive(&g_seen_lock)) return;
    bool seen = false;
    for (std::uint32_t i = 0; i < g_nseen; ++i) if (g_seen[i].id == id) { seen = true; break; }
    if (!seen && g_nseen < 256) {
        g_seen[g_nseen++] = { id, name };
        G6LOG_I("script thread first seen: id=%u name_hash=0x%08X state=%u thread=%p ctx=%p", id, name, state, thread, ctx);
    }
    note_name(id, name, state);
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_stats >= 10000) {
        G6LOG_I("Run calls so far: %llu; distinct script threads: %u, names: %u; bind=%s bound thread id: %u; game_time_ms=%llu "
                "(%s; game timer reads %u)", static_cast<unsigned long long>(g_run_calls.load()), g_nseen, g_nnames,
                bind_str(g_cfg.bind), g_bound_id, static_cast<unsigned long long>(g_game_ms.load()), clock_state_str(),
                g_clock_ptr ? *g_clock_ptr : 0u);
        g_last_stats = now;
    }
    ReleaseSRWLockExclusive(&g_seen_lock);
}

// bind=first: the first running thread, rebound after 5 s of absence. bind=hash: the same, among
// threads whose name hash is host_script_hash. bind=observe: never.
bool bind_matches(std::uint32_t id, std::uint32_t name) noexcept {
    if (g_cfg.bind == Bind::observe) return false;
    if (g_cfg.bind == Bind::hash && name != g_cfg.host_script_hash) return false;
    const ULONGLONG now = GetTickCount64();
    if (g_bound_id == 0 || (g_bound_id != id && now - g_bound_seen > 5000)) {
        G6LOG_I("binding plugin scripts to script thread id=%u name_hash=0x%08X (bind=%s)", id, name, bind_str(g_cfg.bind));
        g_bound_id = id;
    }
    if (id != g_bound_id) return false;
    g_bound_seen = now;
    return true;
}

void** g_pending_slot = nullptr;
void* g_pending_prev = nullptr;

void after_run(void* ctx) {
    const DWORD tid = GetCurrentThreadId();
    if (g_tick_tid.load(std::memory_order_relaxed) == tid) return;   // Run re-entered from our own tick
    std::uint8_t* thread = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(ctx) + bc::kCtxThread);
    if (!thread) return;
    std::uint32_t name, state, id;
    std::memcpy(&name, thread + bc::kThreadNameHash, 4);
    std::memcpy(&state, thread + bc::kThreadState, 4);
    std::memcpy(&id, thread + bc::kThreadId, 4);
    observe(id, name, state, ctx, thread);
    vfs::dump_once("first Run call");   // read-only; scripts run, so the game's own mounts exist
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_clock_sample >= 50) {
        g_last_clock_sample = now;
        clock_update();                  // observe mode validates the game clock too
    }
    if (g_cfg.mode != Mode::full || !g_ticks_enabled.load(std::memory_order_relaxed)) return;
    if (state != 1 || !bind_matches(id, name)) return;
    if (g_cfg.min_tick_interval_ms && now - g_last_tick_ms < g_cfg.min_tick_interval_ms) return;
    g_last_tick_ms = now;
    void** slot = tls_active_ctx_slot();
    if (!slot) {
        g_ticks_enabled.store(false);
        G6LOG_E("TLS active-context slot unavailable on the script thread; plugin ticks disabled");
        return;
    }
    DWORD idle = 0;
    if (!g_tick_tid.compare_exchange_strong(idle, tid)) return;   // another OS thread is ticking
    clock_update();
    g_pending_prev = *slot;
    g_pending_slot = slot;
    *slot = ctx;                       // natives see this script's exec context, as inside Run
    guard_evaluate();
    vfs::process_pending(guard_open());
    sched::tick_all(g_game_ms.load());
    *slot = g_pending_prev;
    g_pending_slot = nullptr;
    g_tick_tid.store(0);
}

void after_run_seh(void* ctx) noexcept {
    __try {
        after_run(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_tick_tid.load() == GetCurrentThreadId()) {
            if (g_pending_slot) { *g_pending_slot = g_pending_prev; g_pending_slot = nullptr; }
            g_tick_tid.store(0);
        }
        g_ticks_enabled.store(false);
        g_hook_active.store(false);    // the detour stays installed as a pure pass-through
        if (g_hook_faults.fetch_add(1) == 0)
            log::write(log::Level::error, "runtime", "exception 0x%08lX in the Run hook; hook now pass-through only", GetExceptionCode());
    }
}

std::uint64_t detour_run(void* ctx, void* entry, std::uint64_t argc, void* args, void* ret, std::uint64_t retc) {
    const std::uint64_t r = g_orig_run(ctx, entry, argc, args, ret, retc);
    if (ctx && g_hook_active.load(std::memory_order_acquire)) after_run_seh(ctx);
    return r;
}

// ------------------------------------------------------------------ plugins
struct Plugin {
    HMODULE mod = nullptr;
    char name[64] = {};
    g6_plugin_unload_fn unload = nullptr;
    g6_plugin_stop_fn stop = nullptr;   // optional export (API 0.2): the stop event
    bool loaded = false;
    bool stop_called = false;
};
Plugin g_plugins[32];
std::size_t g_nplugins = 0;
void* g_loading_plugin = nullptr;
const g6_host_api* host_api() noexcept;

template <class Fn> Fn proc(HMODULE mod, const char* name) noexcept {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
}

void load_plugins() noexcept {
    wchar_t pat[1100];
    std::swprintf(pat, 1100, L"%ls\\plugins\\*.dll", g_dir);
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) { G6LOG_I("no plugins in %ls\\plugins", g_dir); return; }
    do {
        if (g_nplugins >= 32) { G6LOG_W("plugin limit (32) reached"); break; }
        wchar_t path[1400];
        std::swprintf(path, 1400, L"%ls\\plugins\\%ls", g_dir, fd.cFileName);
        HMODULE mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) { G6LOG_W("plugin %ls: LoadLibrary failed (%lu)", fd.cFileName, GetLastError()); continue; }
        auto query = proc<g6_plugin_query_fn>(mod, "g6_plugin_query");
        auto load = proc<g6_plugin_load_fn>(mod, "g6_plugin_load");
        auto unload = proc<g6_plugin_unload_fn>(mod, "g6_plugin_unload");
        const g6_plugin_info* info = query ? query() : nullptr;
        if (!load || !info || info->size < sizeof(g6_plugin_info) || info->api_major != G6_API_VERSION_MAJOR) {
            G6LOG_W("plugin %ls: missing exports or API %u.x (runtime %u.%u); not loaded", fd.cFileName,
                    info ? info->api_major : 0xFFFFu, G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR);
            FreeLibrary(mod);
            continue;
        }
        Plugin& p = g_plugins[g_nplugins++];
        p.mod = mod;
        std::snprintf(p.name, sizeof p.name, "%s", info->name ? info->name : "?");
        p.unload = unload;
        p.stop = proc<g6_plugin_stop_fn>(mod, "g6_plugin_stop");
        p.stop_called = false;
        g_loading_plugin = &p;
        const g6_status st = load(host_api());
        g_loading_plugin = nullptr;
        if (st != G6_OK) {
            sched::unregister_owner(&p);
            G6LOG_W("plugin '%s' (%ls) g6_plugin_load returned %d; its scripts were removed, module left loaded", p.name,
                    fd.cFileName, static_cast<int>(st));
            continue;
        }
        p.loaded = true;
        G6LOG_I("plugin '%s' %s loaded from %ls", p.name, info->version ? info->version : "", fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// ------------------------------------------------------------------ host API
std::uint8_t g_handle_tokens[sizeof(m::kNatives) / sizeof(m::kNatives[0])];

g6_native_handle handle_of(std::size_t i) noexcept { return reinterpret_cast<g6_native_handle>(&g_handle_tokens[i]); }
std::ptrdiff_t index_of(g6_native_handle h) noexcept {
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(h);
    if (p < g_handle_tokens || p >= g_handle_tokens + sizeof g_handle_tokens) return -1;
    return p - g_handle_tokens;
}

g6_status G6_CALL api_get_build_info(g6_build_info* out) {
    if (!out || out->size < sizeof(g6_build_info)) return G6_E_INVALID_ARG;
    out->game_version = m::kGameBuild;
    out->platform = "xbox-scarlett-host";
    std::memcpy(out->exe_sha256, g_exe_sha, 32);
    out->natives_known = static_cast<std::uint32_t>(natives::table_size());
    out->natives_verified = natives::verified_count();
    return g_identity_ok.load() ? G6_OK : G6_E_UNSUPPORTED_BUILD;
}

void G6_CALL api_log(g6_log_level level, const char* plugin, const char* message) {
    const int l = static_cast<int>(level);
    log::write(static_cast<log::Level>(l < 0 ? 0 : l > 3 ? 3 : l), plugin ? plugin : "plugin", "%s", message ? message : "");
}

g6_status G6_CALL api_native_resolve(std::uint64_t hash, g6_native_handle* out) {
    if (!out) return G6_E_INVALID_ARG;
    *out = nullptr;
    const std::ptrdiff_t i = natives::find(hash);
    if (i < 0) return G6_E_UNKNOWN_NATIVE;
    const natives::Verify v = natives::state_at(static_cast<std::size_t>(i));
    if (v != natives::Verify::unchecked && v != natives::Verify::ok) return G6_E_UNVERIFIED_NATIVE;
    *out = handle_of(static_cast<std::size_t>(i));
    return G6_OK;
}

std::atomic<std::uint32_t> g_refused_online_logs{ 0 };

g6_status G6_CALL api_native_invoke(g6_native_handle h, g6_native_call* call) {
    if (!h || !call || call->size < sizeof(g6_native_call)) return G6_E_INVALID_ARG;
    const std::ptrdiff_t i = index_of(h);
    if (i < 0) return G6_E_INVALID_ARG;
    if (g_tick_tid.load(std::memory_order_relaxed) != GetCurrentThreadId() || !sched::in_script_fiber())
        return G6_E_NOT_ON_SCRIPT_THREAD;
    if (!g_identity_ok.load()) return G6_E_UNSUPPORTED_BUILD;
    if (call->argc > G6_MAX_NATIVE_ARGS) return G6_E_TOO_MANY_ARGS;
    const std::size_t idx = static_cast<std::size_t>(i);
    if (natives::is_network(idx) || !guard_open()) {
        if (g_refused_online_logs.fetch_add(1) < 20)
            G6LOG_W("refused native %s: %s", natives::name_at(idx),
                    natives::is_network(idx) ? "NETWORK native (always refused)" : g_gate.why(GetTickCount64()));
        return G6_E_ONLINE_SESSION;
    }
    return natives::call_handler(idx, call);
}

g6_status G6_CALL api_native_invoke_hash(std::uint64_t hash, g6_native_call* call) {
    g6_native_handle h = nullptr;
    const g6_status s = api_native_resolve(hash, &h);
    return s == G6_OK ? api_native_invoke(h, call) : s;
}

g6_status G6_CALL api_script_register(const g6_script_desc* desc, g6_script_id* out) {
    void* owner = sched::current_owner();
    if (!owner) owner = g_loading_plugin;
    return sched::register_script(desc, owner, out);
}
g6_status G6_CALL api_script_unregister(g6_script_id id) { return sched::unregister_script(id); }
g6_status G6_CALL api_script_wait(std::uint32_t ms) { return sched::wait(ms); }

g6_status G6_CALL api_anchor_address(const char* name, std::uintptr_t* out) {
    if (!name || !out) return G6_E_INVALID_ARG;
    *out = 0;
    if (!g_identity_ok.load()) return G6_E_UNSUPPORTED_BUILD;
    const AnchorResult* r = find_result(g_anchors, name);
    if (!r || r->count != 1 || !r->addr) return G6_E_NO_SUCH_ANCHOR;
    *out = reinterpret_cast<std::uintptr_t>(r->addr);
    return G6_OK;
}

// Queues the overlay (vfs.cpp); the mount itself runs on the script thread. G6_OK = accepted.
g6_status G6_CALL api_vfs_add_overlay(const char* mods_root, std::int32_t priority) {
    if (!g_identity_ok.load()) return G6_E_UNSUPPORTED_BUILD;
    return vfs::request(mods_root, priority);
}

std::uint64_t G6_CALL api_game_time_ms(void) { return g_game_ms.load(); }

const g6_host_api g_api = {
    sizeof(g6_host_api), G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR,
    api_get_build_info, api_log,
    api_native_resolve, api_native_invoke, api_native_invoke_hash,
    api_script_register, api_script_unregister, api_script_wait,
    api_anchor_address, api_vfs_add_overlay,
    api_game_time_ms,
};
const g6_host_api* host_api() noexcept { return &g_api; }

// ------------------------------------------------------------------ stop event
// gta6mod.h g6_plugin_stop: before unloading, each loaded plugin gets its stop call once, preferably from a
// script fiber on the game script thread (natives work there), else from the shutdown thread.
constexpr DWORD kStopWaitMs = 1000;
std::atomic<bool> g_stop_done{ false };
g6_script_id g_stop_script = 0;

void call_plugin_stops(std::size_t first, const char* where) noexcept {
    for (std::size_t i = first; i < g_nplugins; ++i) {
        Plugin& p = g_plugins[i];
        if (!p.loaded || !p.stop || p.stop_called) continue;
        p.stop_called = true;
        G6LOG_I("stop event -> plugin '%s' (%s)", p.name, where);
        p.stop();
    }
}

void G6_CALL stop_event_tick(void*) {   // runtime-owned script: one fiber on the script thread
    call_plugin_stops(0, "script thread");
    sched::unregister_script(g_stop_script);
    g_stop_done.store(true, std::memory_order_release);
}

bool stop_pending(std::size_t first) noexcept {
    for (std::size_t i = first; i < g_nplugins; ++i)
        if (g_plugins[i].loaded && g_plugins[i].stop && !g_plugins[i].stop_called) return true;
    return false;
}

// Queue the stop event as a script; the next scheduler tick runs it. False when nothing was queued.
bool request_stop_event() noexcept {
    g_stop_done.store(false);
    g6_script_desc d{ sizeof(g6_script_desc), "runtime.stop", stop_event_tick, nullptr };
    if (sched::register_script(&d, nullptr, &g_stop_script) != G6_OK) { g_stop_script = 0; return false; }
    return true;
}

// Plugins from `first` on: the stop call for every plugin that has not had it (this thread), then unload.
void stop_and_unload_plugins(std::size_t first) noexcept {
    if (g_stop_script && !g_stop_done.load()) sched::unregister_script(g_stop_script);  // never ran
    g_stop_script = 0;
    call_plugin_stops(first, "shutdown thread; natives are refused here");
    for (std::size_t i = first; i < g_nplugins; ++i) {
        if (g_plugins[i].loaded && g_plugins[i].unload) g_plugins[i].unload();
        g_plugins[i].loaded = false;
    }
}

// ------------------------------------------------------------------ init / teardown
void teardown(const char* why) noexcept {
    G6LOG_W("teardown: %s", why);
    if (stop_pending(0)) {
        // Ticks still run: let the script thread raise the stop event, so plugin cleanup can call natives.
        const bool ticking = g_cfg.mode == Mode::full && g_ticks_enabled.load() &&
                             g_tick_tid.load() != GetCurrentThreadId();
        if (ticking && request_stop_event()) {
            const ULONGLONG t0 = GetTickCount64();
            while (!g_stop_done.load(std::memory_order_acquire) && GetTickCount64() - t0 < kStopWaitMs) Sleep(1);
        }
        if (!g_stop_done.load()) G6LOG_W("stop event: no script tick within %lu ms; stopping plugins on this thread", kStopWaitMs);
    }
    g_ticks_enabled.store(false);
    g_hook_active.store(false, std::memory_order_release);
    for (int k = 0; k < 2000 && g_tick_tid.load() != 0; ++k) Sleep(1);
    char err[256] = {};
    if (g_run_hook.enabled()) {
        if (g_run_hook.disable(err, sizeof err)) G6LOG_I("Run hook removed (original bytes restored; trampoline kept allocated)");
        else G6LOG_E("Run hook removal failed: %s (left installed, inert)", err);
    }
    stop_and_unload_plugins(0);
    if (g_tick_tid.load() == 0) sched::delete_all();
}

int hex_nibble(char c) noexcept { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

g6_status run_init() {
    G6LOG_I("gta6mod_runtime %s for GTA VI %s; mode=%s identity=%s strict_patterns=%d", G6RT_VERSION, m::kGameBuild,
            mode_str(g_cfg.mode), g_cfg.strict_identity ? "strict" : "normal", g_cfg.strict_patterns ? 1 : 0);
    for (int i = 0; i < 32; ++i) g_exe_sha[i] = static_cast<std::uint8_t>((hex_nibble(m::kExeSha256Hex[2 * i]) << 4) | hex_nibble(m::kExeSha256Hex[2 * i + 1]));

    // 1. game module
    identity::GameModule gm;
    const ULONGLONG t0 = GetTickCount64();
    while (!identity::find_game_module(gm)) {
        if (GetTickCount64() - t0 >= g_cfg.wait_for_game_ms) {
            G6LOG_E("no module exporting __XMemAllocationHooks__ after %u ms; nothing to do", g_cfg.wait_for_game_ms);
            return G6_E_UNSUPPORTED_BUILD;
        }
        Sleep(250);
    }
    g_base = gm.base;
    g_size = gm.size;
    G6LOG_I("game module at %p size 0x%zx (%s) path '%ls'", gm.base, gm.size, gm.from_loader_list ? "loader list" : "memory scan", gm.path);

    // 2. identity
    if (!identity::readable_range(gm.base, 0x1000)) { G6LOG_E("game headers unreadable"); return G6_E_UNSUPPORTED_BUILD; }
    identity::Report ir;
    identity::check_headers(gm.base, gm.size, ir);
    G6LOG_I("identity: headers %s, exports %s%s%s", ir.headers_ok ? "match" : "MISMATCH", ir.exports_ok ? "match" : "MISMATCH",
            ir.detail[0] ? " - " : "", ir.detail);
    G6LOG_I("identity: host-patched fields (informational): Characteristics 0x%04x, entry RVA 0x%x (file 0x%x), import dir 0x%x "
            "(file 0x%x), TLS dir 0x%x, ImageBase 0x%llx", ir.characteristics, ir.entry_rva, m::kEntryRva, ir.import_dir_rva,
            m::kOrigImportDirRva, ir.tls_dir_rva, static_cast<unsigned long long>(ir.header_image_base));
    if (!ir.headers_ok || !ir.exports_ok) { G6LOG_E("refusing: unknown build (G6_E_UNSUPPORTED_BUILD)"); return G6_E_UNSUPPORTED_BUILD; }
    identity::check_imphash(gm.base, gm.size, ir);
    G6LOG_I("identity: imphash of original import descriptors (RVA 0x%x): %s (%s)", m::kOrigImportDirRva,
            ir.imphash == identity::Tri::match ? "match" : ir.imphash == identity::Tri::mismatch ? "MISMATCH" : "unavailable",
            ir.imphash_hex[0] ? ir.imphash_hex : "-");
    if (gm.path[0]) {
        if (identity::sha256_file(gm.path, ir.module_file_sha_hex))
            ir.module_file_sha = std::strcmp(ir.module_file_sha_hex, m::kExeSha256Hex) == 0 ? identity::Tri::match : identity::Tri::mismatch;
        G6LOG_I("identity: SHA-256 of mapped module file: %s (%s)%s", ir.module_file_sha_hex[0] ? ir.module_file_sha_hex : "unreadable",
                ir.module_file_sha == identity::Tri::match ? "match" : ir.module_file_sha == identity::Tri::mismatch ? "differs" : "n/a",
                ir.module_file_sha == identity::Tri::mismatch ? " - expected under VIctory, which maps a patched copy" : "");
    }
    if (g_cfg.exe_path[0]) {
        if (identity::sha256_file(g_cfg.exe_path, ir.exe_path_sha_hex))
            ir.exe_path_sha = std::strcmp(ir.exe_path_sha_hex, m::kExeSha256Hex) == 0 ? identity::Tri::match : identity::Tri::mismatch;
        G6LOG_I("identity: SHA-256 of exe_path '%ls': %s (%s)", g_cfg.exe_path, ir.exe_path_sha_hex[0] ? ir.exe_path_sha_hex : "unreadable",
                ir.exe_path_sha == identity::Tri::match ? "match" : "NO MATCH");
    }
    const bool file_sha = ir.module_file_sha == identity::Tri::match || ir.exe_path_sha == identity::Tri::match;
    const bool corroborated = file_sha || ir.imphash == identity::Tri::match;
    if (g_cfg.strict_identity ? !file_sha : !corroborated) {
        G6LOG_E("refusing: %s (set [runtime] exe_path to the original game_scarlett_master.exe to corroborate by SHA-256)",
                g_cfg.strict_identity ? "identity=strict needs a file SHA-256 match" : "no content corroboration (imphash or file SHA-256)");
        return G6_E_UNSUPPORTED_BUILD;
    }
    G6LOG_I("identity: build %s accepted (%s)", m::kGameBuild, file_sha ? "file SHA-256" : "headers + imphash");

    // 3. patterns
    AnchorSummary sum;
    const ULONGLONG ts = GetTickCount64();
    const bool req = resolve_all(gm.base, gm.size, g_anchors, sum, true, 0);
    G6LOG_I("patterns: %zu anchors in %llu ms: %u exact by signature, %u via xref chain (%u chain failures), %u missing, %u ambiguous, "
            "%u unsigned, %u bad signature; RVA cross-check: %u mismatches, %u without manifest RVA", g_anchors.size(),
            static_cast<unsigned long long>(GetTickCount64() - ts), sum.exact, sum.via_chain, sum.chain_failed, sum.missing,
            sum.ambiguous, sum.unsigned_n, sum.bad_sig, sum.rva_mismatch, sum.no_manifest);
    if (!req) { G6LOG_E("refusing: required anchors failed: %s", sum.required_detail); return G6_E_UNSUPPORTED_BUILD; }
    if (g_cfg.strict_patterns && (sum.missing || sum.ambiguous || sum.bad_sig || sum.chain_failed)) {
        G6LOG_E("refusing: strict_patterns=1 and not every signed anchor matched exactly once");
        return G6_E_UNSUPPORTED_BUILD;
    }
    // The required anchors are re-resolved through the generated g6::patterns::resolve_anchor: the
    // one-pass scanner is only a faster equivalent and must agree with it exactly.
    for (const char* name : { "script.exec.run", "script.exec.vtable", "script.native.handler.set_entity_coords", "script.native.lock_fn" }) {
        const AnchorResult* r = find_result(g_anchors, name);
        const g6::patterns::result gr = g6::patterns::resolve_anchor(gm.base, gm.size, sum.text_lo,
                                                                     static_cast<std::size_t>(sum.text_hi - sum.text_lo), *r->a);
        if (gr.state != g6::patterns::status::ok || gr.address != r->addr) {
            G6LOG_E("refusing: %s: scanner %p vs resolve_anchor %p (state %d)", name, r->addr, gr.address, static_cast<int>(gr.state));
            return G6_E_UNSUPPORTED_BUILD;
        }
    }
    G6LOG_I("required anchors ok: script.exec.run at %p (vtable slot %u agrees), SET_ENTITY_COORDS handler self-id ok, "
            "resolve_anchor agrees on all four", sum.run, bc::kExecVtableRunSlot);
    g_text_lo = sum.text_lo;
    g_text_hi = sum.text_hi;
    g_run = sum.run;

    // 4. natives (static, read-only)
    std::uint32_t bad = 0;
    const std::uint32_t okn = natives::bind_and_verify(gm.base, g_text_lo, g_text_hi, &bad);
    G6LOG_I("natives: %u of %zu handlers verified statically (cmp [rcx+0x10],0 / js / imm64 == rotl64(~hash,32)); %u failed",
            okn, natives::table_size(), bad);
    g_identity_ok.store(true);

    // 4b. game clock block (read-only: decodes three handler bodies)
    {
        char why[700];
        const std::uint8_t* blk = resolve_clock(gm.base, gm.size, g_text_lo, g_text_hi, true, why, sizeof why);
        if (!blk) {
            G6LOG_W("game clock: not resolved (%s); game_time_ms uses the system timer", why);
        } else if (g_cfg.clock_system) {
            G6LOG_I("game clock: resolved (%s) but [clock] source=system; game_time_ms uses the system timer", why);
        } else {
            g_clock_ptr = reinterpret_cast<const volatile std::uint32_t*>(blk);
            G6LOG_I("game clock: %s; current value %u ms; used after %u plausible samples in the Run hook", why, *g_clock_ptr,
                    clock::Mixer::kNeedGood);
        }
    }

    // 4c. mods/ overlay engine ABI (read-only checks) + mount table dump
    {
        const vfs::Anchors va = vfs_anchors(g_anchors);
        vfs::Engine eng;
        if (vfs::derive(va, gm.base, gm.size, g_text_lo, g_text_hi, eng))
            G6LOG_I("vfs: engine ABI checks passed (Relative ctor %p, Init %p, MountAs %p, MountGlobal %p, sizeof(Relative) 0x%x); "
                    "[vfs] mode=%s", reinterpret_cast<void*>(eng.ctor), reinterpret_cast<void*>(eng.init),
                    reinterpret_cast<void*>(eng.mount_as), va.mount_global, eng.dev_size, g_cfg.vfs.enabled ? "overlay" : "off");
        else
            G6LOG_W("vfs: engine ABI not established: %s; vfs_add_overlay will refuse", eng.why);
        vfs::configure(g_cfg.vfs, eng, va);
        vfs::dump_mounts(eng, va, "init");
    }

    // 5. TLS publish site (read-only inspection)
    classify_tls_site(g_run);
    // 5b. single-player guard (design §7): attestation + read-only TCP check of this process
    g_gate.set_attested(g_cfg.offline_attested);
    {
        const guard::ScanResult r0 = guard::scan_process(GetCurrentProcessId());
        G6LOG_I("guard: offline_attested=%d; initial connection scan: %s, %u own TCP row(s), %u to a remote%s%s",
                g_cfg.offline_attested ? 1 : 0, r0.ok ? "ok" : "FAILED", r0.own_rows, r0.tripping,
                r0.tripping ? ", first " : "", r0.first_trip);
        const bool started = guard::start_worker(&g_gate, g_cfg.guard_interval_ms);
        if (!started) G6LOG_E("guard: worker thread not started; the guard stays closed");
        if (!g_cfg.offline_attested)
            G6LOG_W("guard: [guard] offline_attested is not 1: every plugin native call and overlay mount is refused "
                    "(G6_E_ONLINE_SESSION)");
    }

    if (g_cfg.bind == Bind::hash && g_cfg.host_script_hash == 0) {
        G6LOG_W("[script] bind=hash without host_script_hash: treated as bind=observe (no plugin ticks)");
        g_cfg.bind = Bind::observe;
    }
    G6LOG_I("script binding: bind=%s host_script_hash=0x%08X", bind_str(g_cfg.bind), g_cfg.host_script_hash);

    if (g_cfg.mode == Mode::readonly) {
        G6LOG_I("mode=readonly: Run hook NOT installed, plugins NOT loaded. Set [runtime] mode=observe for the next step.");
        return G6_OK;
    }

    // 6. hook
    {
        char err[256] = {};
        hook::Options opt;
        if (!g_run_hook.create(const_cast<std::uint8_t*>(g_run), reinterpret_cast<void*>(&detour_run), opt, err, sizeof err)) {
            G6LOG_E("Run hook create failed: %s", err);
            teardown("hook create failed");
            return G6_E_INTERNAL;
        }
        g_orig_run = reinterpret_cast<RunFn>(g_run_hook.original());
        const std::uint8_t* sb = g_run_hook.saved_bytes();
        G6LOG_I("Run hook prepared: %s patch, %zu bytes relocated, original bytes %02X %02X %02X %02X %02X ..., trampoline %p",
                g_run_hook.kind() == hook::PatchKind::rel32_via_relay ? "5-byte rel32" : "14-byte abs", g_run_hook.stolen(),
                sb[0], sb[1], sb[2], sb[3], sb[4], g_run_hook.original());
        g_hook_active.store(true, std::memory_order_release);
        // The runtime may be injected after the title's threads are running: enable() suspends every
        // other thread and refuses while any RIP is inside the bytes it replaces (hook.cpp write_patch).
        if (!g_run_hook.enable(err, sizeof err)) {
            G6LOG_E("Run hook NOT installed: %s", err);
            teardown("hook enable failed");
            return G6_E_INTERNAL;
        }
        const hook::FreezeStats& fs = g_run_hook.last_freeze();
        G6LOG_I("Run hook installed at %p (%u other thread(s) suspended during the write, %u attempt(s) in %u ms, guarded range %zu bytes)",
                g_run, fs.threads, fs.attempts, fs.elapsed_ms, hook::guarded_len(g_run_hook.patch_len(), g_run_hook.stolen()));
    }
    if (g_cfg.mode == Mode::observe) {
        G6LOG_I("mode=observe: hook logs script threads only; plugins NOT loaded");
        return G6_OK;
    }

    // 7. plugins
    sched::set_fiber_stack(static_cast<std::size_t>(g_cfg.fiber_stack_kb) << 10);
    if (g_tls_mode != TlsMode::unavailable) g_ticks_enabled.store(true);
    else G6LOG_W("plugin ticks stay disabled: TLS publish site not understood");
    if (g_cfg.bind == Bind::observe) G6LOG_W("[script] bind=observe: plugins load but their scripts never tick");
    if (g_cfg.vfs.enabled && g_cfg.vfs_root[0]) vfs::request(g_cfg.vfs_root, 0);
    load_plugins();
    G6LOG_I("init complete: %zu plugin module(s), %zu active script(s)", g_nplugins, sched::active_count());
    return G6_OK;
}

DWORD WINAPI init_thread(void*) {
    wchar_t logp[1100];
    std::swprintf(logp, 1100, L"%ls\\gta6mod_runtime.log", g_dir);
    read_config();
    log::open(logp);
    log::set_level(g_cfg.level);
    QueryPerformanceFrequency(&g_qpf);
    g6_status st = G6_E_INTERNAL;
    try {
        st = run_init();
    } catch (...) {
        G6LOG_E("init: C++ exception (out of memory?)");
        teardown("init exception");
        st = G6_E_INTERNAL;
    }
    g_init_status.store(st);
    G6LOG_I("init finished: %d (%s)", static_cast<int>(st), st == G6_OK ? "ok" : "refused/failed");
    return 0;
}

// ------------------------------------------------------------------ test API
int t_decode(const std::uint8_t* p, std::size_t n, g6rt_insn_info* o) {
    const x64::Insn in = x64::decode(p, n);
    if (!o) return in.len;
    *o = g6rt_insn_info{ in.len, in.op_off, in.map, in.op, in.rip_relative ? 1u : 0u, in.disp_off,
                         static_cast<std::uint32_t>(in.branch), in.rel_off, in.rel_size, in.terminator ? 1u : 0u, in.imm_size };
    return in.len;
}
int t_scan(const std::uint8_t* base, std::size_t size, const char* const* sigs, std::size_t n, std::uint32_t* counts, std::size_t* first) {
    try {
        std::vector<scan::Compiled> pats(n);
        for (std::size_t i = 0; i < n; ++i) if (!scan::parse(sigs[i], pats[i])) return -1;
        std::vector<scan::Hit> hits;
        scan::scan_all(base, size, pats, hits);
        for (std::size_t i = 0; i < n; ++i) { counts[i] = hits[i].count; first[i] = hits[i].first; }
        return 0;
    } catch (...) { return -2; }
}
int t_build(const std::uint8_t* src, std::uint64_t src_addr, std::size_t avail, std::size_t min_bytes, std::uint64_t tramp,
            std::uint8_t* out, std::size_t cap, std::size_t* out_len, std::size_t* stolen, char* err, std::size_t errcap) {
    hook::BuildResult r;
    const bool ok = hook::build_trampoline(src, src_addr, avail, min_bytes, tramp, out, cap, r);
    if (out_len) *out_len = r.code_len;
    if (stolen) *stolen = r.stolen;
    if (err && errcap) std::snprintf(err, errcap, "%s", r.error);
    return ok ? 1 : 0;
}
void* t_hook_create(void* target, void* detour, int allow_rel32, void** original, char* err, std::size_t errcap) {
    auto* h = new (std::nothrow) hook::InlineHook();
    if (!h) return nullptr;
    hook::Options o;
    o.allow_rel32 = allow_rel32 != 0;
    if (!h->create(target, detour, o, err, errcap)) { delete h; return nullptr; }
    if (original) *original = h->original();
    return h;
}
int t_hook_enable(void* h, char* err, std::size_t cap) { return static_cast<hook::InlineHook*>(h)->enable(err, cap) ? 1 : 0; }
int t_hook_disable(void* h, char* err, std::size_t cap) { return static_cast<hook::InlineHook*>(h)->disable(err, cap) ? 1 : 0; }
void t_hook_destroy(void* h) {
    auto* p = static_cast<hook::InlineHook*>(h);
    p->release_unsafe();
    delete p;
}
int t_hook_info(void* h, int* kind, std::size_t* stolen, std::size_t* patch_len) {
    auto* p = static_cast<hook::InlineHook*>(h);
    if (kind) *kind = static_cast<int>(p->kind());
    if (stolen) *stolen = p->stolen();
    if (patch_len) *patch_len = p->patch_len();
    return 1;
}
int t_rip_in_range(std::uint64_t rip, std::uint64_t lo, std::size_t n) { return hook::rip_in_range(rip, lo, n) ? 1 : 0; }
std::size_t t_guarded_len(std::size_t patch_len, std::size_t stolen) { return hook::guarded_len(patch_len, stolen); }
int t_hook_freeze_info(void* h, std::uint32_t* threads, std::uint32_t* attempts, std::uint32_t* blocking_tid, std::uint64_t* blocking_rip) {
    const hook::FreezeStats& f = static_cast<hook::InlineHook*>(h)->last_freeze();
    if (threads) *threads = f.threads;
    if (attempts) *attempts = f.attempts;
    if (blocking_tid) *blocking_tid = f.blocking_tid;
    if (blocking_rip) *blocking_rip = f.blocking_rip;
    return 1;
}
int t_verify_selfid(const std::uint8_t* fn, const std::uint8_t* lo, const std::uint8_t* hi, std::uint64_t hash, std::uint64_t* id) {
    return static_cast<int>(natives::verify_selfid(fn, lo, hi, hash, id));
}
int t_check_image(const std::uint8_t* base, std::size_t size, char* report, std::size_t cap) {
    identity::Report r;
    identity::check_headers(base, size, r);
    if (r.headers_ok && r.exports_ok) identity::check_imphash(base, size, r);
    if (report && cap)
        std::snprintf(report, cap, "headers %s, exports %s, imphash %s (%s) %s", r.headers_ok ? "match" : "MISMATCH",
                      r.exports_ok ? "match" : "MISMATCH",
                      r.imphash == identity::Tri::match ? "match" : r.imphash == identity::Tri::mismatch ? "MISMATCH" : "unavailable",
                      r.imphash_hex, r.detail);
    return r.headers_ok && r.exports_ok && r.imphash == identity::Tri::match ? 1 : 0;
}
int t_resolve_anchors(const std::uint8_t* base, std::size_t size, char* report, std::size_t cap) {
    try {
        identity::Report r;
        identity::check_headers(base, size, r);
        if (!r.headers_ok) { if (report) std::snprintf(report, cap, "headers mismatch: %s", r.detail); return 0; }
        std::vector<AnchorResult> res;
        AnchorSummary s;
        const bool ok = resolve_all(base, size, res, s, false, 25);
        if (report && cap) {
            std::snprintf(report, cap, "%zu anchors: %u exact, %u via chain, %u chain failed, %u missing, %u ambiguous, %u unsigned, "
                          "%u bad sig, %u rva mismatch, %u no manifest rva; resolve_anchor cross-check %u/%u agree; required %s %s",
                          res.size(), s.exact, s.via_chain, s.chain_failed, s.missing, s.ambiguous, s.unsigned_n, s.bad_sig,
                          s.rva_mismatch, s.no_manifest, s.xcheck - s.xcheck_bad, s.xcheck, ok ? "ok" : "FAILED", s.required_detail);
            for (const auto& a : res) {
                if ((a.a->signature[0] || a.chain) && a.count != 1) appendf(report, cap, "\n    not exact: %s (%u)", a.a->name, a.count);
                else if (a.count == 1 && a.manifest_rva >= 0 && a.addr - base != a.manifest_rva)
                    appendf(report, cap, "\n    rva mismatch: %s", a.a->name);
            }
        }
        return ok && s.xcheck_bad == 0 && s.chain_failed == 0 && s.rva_mismatch == 0 ? 1 : 0;
    } catch (...) { return -2; }
}
int t_verify_handlers(const std::uint8_t* base, std::size_t size, std::uint32_t* ok, std::uint32_t* bad) {
    const auto* nt = nt_of(base);
    const auto* sh = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* lo = base + sh[0].VirtualAddress;
    const std::uint8_t* hi = lo + sh[0].Misc.VirtualSize;
    if (hi > base + size) return 0;
    std::uint32_t b = 0;
    const std::uint32_t o = natives::bind_and_verify(const_cast<std::uint8_t*>(base), lo, hi, &b);
    if (ok) *ok = o;
    if (bad) *bad = b;
    return 1;
}
int t_sched_tick(std::uint64_t now_ms) {
    g_tick_tid.store(GetCurrentThreadId());
    sched::tick_all(now_ms);
    g_tick_tid.store(0);
    return 1;
}

int t_clock_find_load(const std::uint8_t* fn, const std::uint8_t* lo, const std::uint8_t* hi, int kind, const std::uint8_t** target,
                      const std::uint8_t** core, char* why, std::size_t cap) {
    const clock::LoadSite s = clock::find_handler_load(fn, lo, hi, kind == 1 ? clock::LoadKind::vmovss_xmm0_rip : clock::LoadKind::mov_eax_rip32);
    if (target) *target = s.target;
    if (core) *core = s.core;
    if (why && cap) std::snprintf(why, cap, "%s", s.why);
    return s.target ? 1 : 0;
}
int t_clock_mix(int has_game, const std::uint32_t* game, const std::uint64_t* wall, std::size_t n, std::uint64_t* out_now, int* state,
                std::uint32_t* n_trusted, std::uint32_t* n_anomaly, std::uint32_t* n_rejected) {
    clock::Mixer mx;
    std::uint32_t t = 0, a = 0, r = 0;
    std::uint64_t prev = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const clock::Mixer::Event e = mx.feed(has_game != 0, game[i], wall[i]);
        t += e == clock::Mixer::Event::trusted;
        a += e == clock::Mixer::Event::anomaly;
        r += e == clock::Mixer::Event::rejected;
        if (mx.now() < prev) return -1;   // the output must never go backwards
        prev = mx.now();
    }
    if (out_now) *out_now = mx.now();
    if (state) *state = static_cast<int>(mx.state());
    if (n_trusted) *n_trusted = t;
    if (n_anomaly) *n_anomaly = a;
    if (n_rejected) *n_rejected = r;
    return 1;
}
int t_resolve_engine(const std::uint8_t* base, std::size_t size, char* report, std::size_t cap, std::uint32_t* clock_rva, int* vfs_ok) {
    try {
        if (clock_rva) *clock_rva = 0;
        if (vfs_ok) *vfs_ok = 0;
        identity::Report ir;
        identity::check_headers(base, size, ir);
        if (!ir.headers_ok) { std::snprintf(report, cap, "headers mismatch: %s", ir.detail); return 0; }
        std::vector<AnchorResult> res;
        AnchorSummary s;
        resolve_all(base, size, res, s, false, 0);
        char why[700];
        const std::uint8_t* blk = resolve_clock(base, size, s.text_lo, s.text_hi, true, why, sizeof why);
        if (blk && clock_rva) *clock_rva = static_cast<std::uint32_t>(blk - base);
        std::snprintf(report, cap, "clock: %s%s", blk ? "" : "NOT RESOLVED: ", why);
        vfs::Engine e;
        const bool ok = vfs::derive(vfs_anchors(res), base, size, s.text_lo, s.text_hi, e);
        if (vfs_ok) *vfs_ok = ok ? 1 : 0;
        appendf(report, cap, "\n    vfs: %s (sizeof(Relative) 0x%x, ctor rva 0x%llx)", e.why, e.dev_size,
                ok ? static_cast<long long>(reinterpret_cast<const std::uint8_t*>(e.ctor) - base) : -1ll);
        return blk && ok ? 1 : 0;
    } catch (...) { return -2; }
}
int t_vfs_map_path(const char* host, const char* map, char* out, std::size_t cap) {
    const char* why = "";
    return vfs::map_path(host, map, out, cap, &why) ? 1 : 0;
}
int t_vfs_mount_fake(void* ctor, void* init, void* mount_as, const void* rel_vtable, void* mount_table_slot, std::uint32_t dev_size,
                     const char* root, const char* mp, std::uint8_t* dev_mem, char* why, std::size_t cap) {
    vfs::Engine e;
    e.ctor = reinterpret_cast<vfs::CtorFn>(ctor);
    e.init = reinterpret_cast<vfs::InitFn>(init);
    e.mount_as = reinterpret_cast<vfs::MountAsFn>(mount_as);
    e.rel_vtable = rel_vtable;
    e.mount_table = static_cast<std::uint8_t*>(mount_table_slot);
    e.dev_size = dev_size;
    e.ok = true;
    return static_cast<int>(vfs::mount_one(e, root, mp, dev_mem, why, cap));
}

int t_guard_classify(int v6, const std::uint8_t* a) {
    return static_cast<int>(v6 ? guard::classify_v6(a) : guard::classify_v4(a));
}
int t_guard_trips(int c, std::uint32_t state) { return guard::trips(static_cast<guard::AddrClass>(c), state) ? 1 : 0; }
int t_guard_gate_run(const int* ops, const std::uint64_t* t, std::size_t n, int* out) {
    guard::Gate g;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = -1;
        switch (ops[i]) {
        case 0: g.set_attested(false); break;
        case 1: g.set_attested(true); break;
        case 2: g.scan_clean(t[i]); break;
        case 3: g.scan_failed(); break;
        case 4: out[i] = g.trip() ? 1 : 0; break;
        case 5: out[i] = g.open(t[i]) ? 1 : 0; break;
        default: return 0;
        }
    }
    return 1;
}
int t_guard_scan_self(std::uint32_t* own, std::uint32_t* trip, unsigned long* err) {
    const guard::ScanResult r = guard::scan_process(GetCurrentProcessId());
    if (own) *own = r.own_rows;
    if (trip) *trip = r.tripping;
    if (err) *err = r.error;
    return r.ok ? 1 : 0;
}

// Teardown's plugin sequence with fake plugins appended after the real ones: queue the stop event, optionally
// run one scheduler tick on this thread (acting as the script thread), then stop the rest and unload.
int t_plugin_stop_run(const g6_plugin_stop_fn* stops, const g6_plugin_unload_fn* unloads, std::size_t n, int tick,
                      std::uint64_t now_ms) {
    const std::size_t first = g_nplugins;
    if (n == 0 || first + n > sizeof g_plugins / sizeof g_plugins[0]) return -1;
    for (std::size_t i = 0; i < n; ++i) {
        Plugin& p = g_plugins[first + i];
        p = Plugin{};
        std::snprintf(p.name, sizeof p.name, "fake%zu", i);
        p.stop = stops ? stops[i] : nullptr;
        p.unload = unloads ? unloads[i] : nullptr;
        p.loaded = true;
    }
    g_nplugins = first + n;
    const int queued = stop_pending(first) && request_stop_event() ? 1 : 0;
    if (tick) t_sched_tick(now_ms);
    stop_and_unload_plugins(first);
    t_sched_tick(now_ms);   // frees the stop script's slot (pending delete)
    for (std::size_t i = first; i < g_nplugins; ++i) g_plugins[i] = Plugin{};
    g_nplugins = first;
    return queued;
}

const g6rt_test_api g_test_api = {
    sizeof(g6rt_test_api), t_decode, t_scan, t_build, t_hook_create, t_hook_enable, t_hook_disable, t_hook_destroy,
    t_hook_info, t_verify_selfid, t_check_image, t_resolve_anchors, t_verify_handlers, t_sched_tick,
    t_clock_find_load, t_clock_mix, t_resolve_engine, t_vfs_map_path, t_vfs_mount_fake,
    t_guard_classify, t_guard_trips, t_guard_gate_run, t_guard_scan_self,
    t_rip_in_range, t_guarded_len, t_hook_freeze_info, t_plugin_stop_run,
};

} // namespace
} // namespace g6rt

// ---------------------------------------------------------------------- exports
extern "C" {

G6_EXPORT const g6_host_api* G6_CALL g6_get_host_api(void) { return g6rt::host_api(); }

G6_EXPORT const char* G6_CALL g6_status_str(g6_status s) {
    switch (s) {
    case G6_OK: return "ok";
    case G6_E_INVALID_ARG: return "invalid argument";
    case G6_E_UNKNOWN_NATIVE: return "unknown native";
    case G6_E_UNVERIFIED_NATIVE: return "unverified native";
    case G6_E_TOO_MANY_ARGS: return "too many arguments";
    case G6_E_NOT_ON_SCRIPT_THREAD: return "not on script thread";
    case G6_E_UNSUPPORTED_BUILD: return "unsupported game build";
    case G6_E_ONLINE_SESSION: return "online session (refused)";
    case G6_E_NOT_IMPLEMENTED: return "not implemented";
    case G6_E_NO_SUCH_ANCHOR: return "no such anchor";
    default: return "internal error";
    }
}

// The mock-only entry points exist in the shared header; the real runtime rejects them.
G6_EXPORT g6_status G6_CALL g6_mock_register_native(uint64_t, g6_mock_native_fn, void*) { return G6_E_NOT_IMPLEMENTED; }
G6_EXPORT g6_status G6_CALL g6_mock_step(uint32_t) { return G6_E_NOT_IMPLEMENTED; }

// Result of the background init (G6_OK, G6_E_UNSUPPORTED_BUILD, ...).
__declspec(dllexport) g6_status G6_CALL g6rt_init_status(void) { return static_cast<g6_status>(g6rt::g_init_status.load()); }

// Orderly stop: plugin ticks off, Run bytes restored, plugins' g6_plugin_unload called.
// The runtime DLL and the trampoline stay mapped (a thread may still be returning through them).
__declspec(dllexport) void G6_CALL g6rt_runtime_shutdown(void) { g6rt::teardown("g6rt_runtime_shutdown"); }

__declspec(dllexport) const g6rt_test_api* G6_CALL g6rt_get_test_api(void) { return &g6rt::g_test_api; }

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    DWORD n = GetModuleFileNameW(inst, g6rt::g_dir, static_cast<DWORD>(sizeof g6rt::g_dir / sizeof g6rt::g_dir[0]));
    while (n > 0 && g6rt::g_dir[n - 1] != L'\\' && g6rt::g_dir[n - 1] != L'/') --n;
    if (n > 0) g6rt::g_dir[n - 1] = 0;
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"G6RT_NO_AUTOSTART", env, 8) > 0) return TRUE;  // offline self-test
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    HANDLE t = CreateThread(nullptr, 0, g6rt::init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
