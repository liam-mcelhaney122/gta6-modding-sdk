#include "vfs.hpp"

#include "build_consts.hpp"
#include "identity.hpp"
#include "log.hpp"
#include "x64_decode.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace g6rt::vfs {
namespace {

namespace bc = g6rt::build;

constexpr std::size_t kDevSlot = 0x200;        // per-device storage; must hold the engine's sizeof
constexpr std::size_t kMaxDevices = 32;
constexpr std::size_t kMaxPending = 8;
constexpr std::size_t kMaxMountPoints = 4;
constexpr std::size_t kMaxRootLen = 0xa0;      // the engine builds root + path in 0x100 bytes

int seh_filter(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_BREAKPOINT:          // the engine asserts with int3 (e.g. MountGlobal on a full table)
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

// --- guarded primitives (no C++ objects with destructors in these frames)
DWORD call_ctor(CtorFn f, void* d) noexcept {
    __try { f(d); } __except (seh_filter(GetExceptionCode())) { return GetExceptionCode(); }
    return 0;
}
DWORD call_init(InitFn f, void* d, const char* root, void* parent) noexcept {
    __try { f(d, root, true, parent); } __except (seh_filter(GetExceptionCode())) { return GetExceptionCode(); }
    return 0;
}
DWORD call_mount(MountAsFn f, void* d, const char* mp, bool* ok) noexcept {
    __try { *ok = f(d, mp); } __except (seh_filter(GetExceptionCode())) { return GetExceptionCode(); }
    return 0;
}
DWORD call_attr(GetAttrFn f, void* d, const char* p, std::uint32_t* out) noexcept {
    __try { *out = f(d, p); } __except (seh_filter(GetExceptionCode())) { return GetExceptionCode(); }
    return 0;
}
bool rd(const void* p, void* out, std::size_t n) noexcept {
    if (!p || !identity::readable_range(p, n)) return false;
    __try { std::memcpy(out, p, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
template <class T> bool rdv(const void* p, T& v) noexcept { return rd(p, &v, sizeof v); }

// Printable ASCII string of at most cap-1 chars; false if unreadable, unterminated or not printable.
bool read_cstr(const void* p, char* out, std::size_t cap) noexcept {
    if (!out || cap < 2) return false;
    out[0] = 0;
    const auto* s = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i + 1 < cap; ++i) {
        std::uint8_t c;
        if (!rd(s + i, &c, 1)) return false;
        if (c == 0) { out[i] = 0; return true; }
        if (c < 0x20 || c > 0x7e) { out[i] = 0; return false; }
        out[i] = static_cast<char>(c);
    }
    out[cap - 1] = 0;
    return false;
}

bool in_text(const std::uint8_t* p, std::size_t n, const std::uint8_t* lo, const std::uint8_t* hi) noexcept {
    return p && p >= lo && p < hi && static_cast<std::size_t>(hi - p) >= n;
}

// Instruction-aligned search: true if an instruction in the first `span` bytes is exactly `want`.
bool has_insn(const std::uint8_t* fn, std::size_t span, const std::uint8_t* lo, const std::uint8_t* hi,
              const std::uint8_t* want, std::size_t n) noexcept {
    std::size_t off = 0;
    while (off < span && in_text(fn + off, 1, lo, hi)) {
        const x64::Insn in = x64::decode(fn + off, static_cast<std::size_t>(hi - (fn + off)));
        if (in.len == 0) return false;
        if (in.len == n && std::memcmp(fn + off, want, n) == 0) return true;
        off += in.len;
    }
    return false;
}

const std::uint8_t* first_call(const std::uint8_t* fn, std::size_t span, const std::uint8_t* lo, const std::uint8_t* hi) noexcept {
    std::size_t off = 0;
    while (off < span && in_text(fn + off, 1, lo, hi)) {
        const x64::Insn in = x64::decode(fn + off, static_cast<std::size_t>(hi - (fn + off)));
        if (in.len == 0) return nullptr;
        if (in.branch == x64::Branch::call_rel32)
            return reinterpret_cast<const std::uint8_t*>(x64::rel_target(in, fn + off, reinterpret_cast<std::uint64_t>(fn + off)));
        off += in.len;
    }
    return nullptr;
}

// Linear sweep (not stopping at jumps): does any rip-relative operand point at `target`?
bool refs_rip(const std::uint8_t* fn, std::size_t span, const std::uint8_t* lo, const std::uint8_t* hi,
              const std::uint8_t* target) noexcept {
    std::size_t off = 0;
    while (off < span && in_text(fn + off, 1, lo, hi)) {
        const x64::Insn in = x64::decode(fn + off, static_cast<std::size_t>(hi - (fn + off)));
        if (in.len == 0) return false;
        if (in.rip_relative &&
            reinterpret_cast<const std::uint8_t*>(x64::rip_target(in, fn + off, reinterpret_cast<std::uint64_t>(fn + off))) == target)
            return true;
        off += in.len;
    }
    return false;
}

// A pointer stored in the image: live (relocated) or, in the offline file image, a preferred-base VA.
const std::uint8_t* image_ptr(const std::uint8_t* base, std::size_t size, std::uint64_t v) noexcept {
    const auto b = reinterpret_cast<std::uint64_t>(base);
    if (v >= b && v - b < size) return reinterpret_cast<const std::uint8_t*>(v);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const std::uint64_t ib = nt->OptionalHeader.ImageBase;
    if (v >= ib && v - ib < size) return base + (v - ib);
    return nullptr;
}

void app(char* buf, std::size_t cap, std::size_t& used, const char* fmt, ...) noexcept {
    if (used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    if (n > 0) used = (used + static_cast<std::size_t>(n) >= cap) ? cap - 1 : used + static_cast<std::size_t>(n);
}

bool sep(char c) noexcept { return c == '/' || c == '\\'; }
char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// --- runtime state
Config g_cfg;
Engine g_eng;
Anchors g_anc;
char g_mps[kMaxMountPoints][64] = {};
std::size_t g_nmps = 0;

struct Pending { char host[520]; char game[kMaxRootLen + 8]; std::int32_t prio; bool done; };
Pending g_pending[kMaxPending];
std::size_t g_npending = 0;
std::atomic<std::uint32_t> g_undone{ 0 };
SRWLOCK g_lock = SRWLOCK_INIT;
alignas(16) std::uint8_t g_dev_mem[kMaxDevices][kDevSlot];
std::size_t g_ndev = 0;
std::atomic<bool> g_faulted{ false };
std::atomic<bool> g_logged_off{ false }, g_logged_deferred{ false }, g_dumped{ false };

const char* kind_of(const Anchors& a, const Engine& e, const std::uint8_t* vt) noexcept {
    if (!vt) return "null";
    if (vt == a.vt_local) return "Local";
    if (vt == e.rel_vtable) return "Relative";
    if (vt == a.vt_packfile) return "Packfile";
    if (vt == a.vt_memory) return "Memory";
    if (vt == a.vt_growbuffer) return "GrowBuffer";
    if (vt == a.vt_count) return "Count";
    if (vt == a.vt_dstorage) return "DStorage";
    if (vt == a.vt_rawstreamer) return "RawStreamer";
    if (vt == a.vt_base) return "base";
    std::uint64_t s = 0, r = 0;
    if (e.rel_vtable && rdv(vt + bc::kDevVtGetAttributes, s) && rdv(static_cast<const std::uint8_t*>(e.rel_vtable) + bc::kDevVtGetAttributes, r) && s == r)
        return "Relative-derived";
    return "other";
}

struct Table { std::uint8_t* entries = nullptr; std::int32_t count = 0, cap = 0; };

bool read_table(const Engine& e, Table& t) noexcept {
    if (!e.mount_table) return false;
    if (!rdv(e.mount_table, t.entries) || !rdv(e.mount_table + bc::kMountCountOff, t.count) ||
        !rdv(e.mount_table + bc::kMountCapOff, t.cap))
        return false;
    if (t.count < 0 || t.cap < 0 || t.count > t.cap || t.cap > 4096) return false;
    if (t.count && !identity::readable_range(t.entries, static_cast<std::size_t>(t.count) * bc::kMountEntrySize)) return false;
    return true;
}

// Exact (case-insensitive) mount-point entry; returns its device count and last device.
bool find_entry(const Engine& e, const char* mp, std::int32_t* ndev, void** last) noexcept {
    Table t;
    if (!read_table(e, t)) return false;
    for (std::int32_t i = 0; i < t.count; ++i) {
        const std::uint8_t* ent = t.entries + static_cast<std::size_t>(i) * bc::kMountEntrySize;
        const char* pfx = nullptr;
        char buf[260];
        if (!rdv(ent + bc::kMountEntPrefix, pfx) || !read_cstr(pfx, buf, sizeof buf)) continue;
        if (_stricmp(buf, mp) != 0) continue;
        std::int32_t n = 0;
        void** devs = nullptr;
        if (!rdv(ent + bc::kMountEntDevCount, n) || !rdv(ent + bc::kMountEntDevices, devs) || n < 0 || n > 4096) return false;
        *ndev = n;
        *last = nullptr;
        if (n > 0 && !rdv(devs + (n - 1), *last)) return false;
        return true;
    }
    return false;
}

} // namespace

bool derive(const Anchors& a, const std::uint8_t* base, std::size_t size, const std::uint8_t* lo, const std::uint8_t* hi,
            Engine& e) noexcept {
    e = Engine{};
    e.mount_table = const_cast<std::uint8_t*>(a.mount_table);   // usable for the read-only dump even if the rest fails
    e.rel_vtable = a.rel_vtable;
    const char* missing = !a.mount_global ? "fs.mount.mount_global" : !a.rel_init ? "fs.device.relative.init"
                        : !a.rel_mount_as ? "fs.device.relative.mount_as" : !a.rel_vtable ? "fs.device.relative.vtable"
                        : !a.rel_vtable_site ? "fs.device.relative.vtable (site)" : !a.base_ctor ? "fs.device.base.ctor"
                        : !a.mount_table ? "fs.mount.table" : nullptr;
    if (missing) { std::snprintf(e.why, sizeof e.why, "anchor %s not resolved exactly once", missing); return false; }

    // 1. Relative ctor: `push rsi; sub rsp,0x20; mov rsi,rcx; call base_ctor` directly before the vtable lea.
    const std::uint8_t* ctor = a.rel_vtable_site - bc::kRelCtorSiteDelta;
    if (!in_text(ctor, 0x20, lo, hi) || std::memcmp(ctor, bc::kRelCtorHead, sizeof bc::kRelCtorHead) != 0) {
        std::snprintf(e.why, sizeof e.why, "fiDeviceRelative ctor not at vtable site - 0x%x (bytes differ)", bc::kRelCtorSiteDelta);
        return false;
    }
    std::int32_t rel;
    std::memcpy(&rel, ctor + 9, 4);
    if (ctor + 13 + rel != a.base_ctor || ctor + 13 != a.rel_vtable_site) {
        std::snprintf(e.why, sizeof e.why, "fiDeviceRelative ctor does not call fs.device.base.ctor before the vtable store");
        return false;
    }
    // base ctor writes only the vtable: mov rax,rcx; lea rdx,[rip+vt]; mov [rax],rdx; ret
    static const std::uint8_t kBase0[] = { 0x48, 0x89, 0xC8, 0x48, 0x8D, 0x15 }, kBase1[] = { 0x48, 0x89, 0x10, 0xC3 };
    if (!in_text(a.base_ctor, 16, lo, hi) || std::memcmp(a.base_ctor, kBase0, 6) != 0 || std::memcmp(a.base_ctor + 10, kBase1, 4) != 0) {
        std::snprintf(e.why, sizeof e.why, "fs.device.base.ctor is not the 4-instruction vtable store");
        return false;
    }
    // ctor tail: mov [rsi],rax; mov qword [rsi+8],0; mov rax,rsi; ...; ret
    static const std::uint8_t kCtorTail[] = { 0x48, 0x89, 0x06, 0x48, 0xC7, 0x46, 0x08, 0, 0, 0, 0, 0x48, 0x89, 0xF0 };
    if (std::memcmp(a.rel_vtable_site + 7, kCtorTail, sizeof kCtorTail) != 0) {
        std::snprintf(e.why, sizeof e.why, "fiDeviceRelative ctor tail differs (vtable / parent store)");
        return false;
    }

    // 2. Init(this, root, bool readOnly, parent): field stores at the offsets the runtime reads.
    static const std::uint8_t kInitR9[] = { 0x4C, 0x89, 0xCF }, kInitR8[] = { 0x44, 0x89, 0xC3 };
    static const std::uint8_t kInitRo[] = { 0x88, 0x9E, 0x14, 0x01, 0x00, 0x00 };   // mov [rsi+0x114], bl
    static const std::uint8_t kInitPar[] = { 0x48, 0x89, 0x7E, 0x08 };             // mov [rsi+8], rdi
    static const std::uint8_t kInitRoot[] = { 0x48, 0x83, 0xC6, 0x10 };            // add rsi, 0x10 (root buffer)
    if (!in_text(a.rel_init, 0xa0, lo, hi) || !has_insn(a.rel_init, 0x20, lo, hi, kInitR9, 3) || !has_insn(a.rel_init, 0x20, lo, hi, kInitR8, 3) ||
        !has_insn(a.rel_init, 0xa0, lo, hi, kInitRo, 6) || !has_insn(a.rel_init, 0xa0, lo, hi, kInitPar, 4) ||
        !has_insn(a.rel_init, 0xa0, lo, hi, kInitRoot, 4)) {
        std::snprintf(e.why, sizeof e.why, "fs.device.relative.init does not show (r8b->[+0x114], r9->[+8], root at +0x10)");
        return false;
    }
    // 3. MountAs(this, mountPoint): MountGlobal(mp, this, [this+0x114]); [this+0x110] = strlen(mp)-1.
    static const std::uint8_t kMaRo[] = { 0x44, 0x8A, 0x81, 0x14, 0x01, 0x00, 0x00 };   // mov r8b, [rcx+0x114]
    static const std::uint8_t kMaLen[] = { 0x89, 0x86, 0x10, 0x01, 0x00, 0x00 };      // mov [rsi+0x110], eax
    if (!in_text(a.rel_mount_as, 0x50, lo, hi) || !has_insn(a.rel_mount_as, 0x20, lo, hi, kMaRo, 7) ||
        !has_insn(a.rel_mount_as, 0x50, lo, hi, kMaLen, 6) || first_call(a.rel_mount_as, 0x30, lo, hi) != a.mount_global) {
        std::snprintf(e.why, sizeof e.why, "fs.device.relative.mount_as is not MountGlobal(mp, this, [this+0x114]) + [+0x110] store");
        return false;
    }
    // 4. MountGlobal uses the table anchor and the count next to it.
    if (!refs_rip(a.mount_global, 0x140, lo, hi, a.mount_table) || !refs_rip(a.mount_global, 0x140, lo, hi, a.mount_table + bc::kMountCountOff)) {
        std::snprintf(e.why, sizeof e.why, "fs.mount.mount_global does not reference fs.mount.table and its count (table-4)");
        return false;
    }
    // 5. sizeof(fiDeviceRelative) from the deleting dtor (vtable slot 0): mov edx, imm32 before operator delete.
    std::uint64_t slot0 = 0;
    if (!rdv(a.rel_vtable, slot0)) { std::snprintf(e.why, sizeof e.why, "Relative vtable unreadable"); return false; }
    const std::uint8_t* dtor = image_ptr(base, size, slot0);
    std::uint32_t dsz = 0;
    if (in_text(dtor, 0x40, lo, hi)) {
        std::size_t off = 0;
        for (int k = 0; k < 16 && off < 0x40; ++k) {
            const x64::Insn in = x64::decode(dtor + off, static_cast<std::size_t>(hi - (dtor + off)));
            if (in.len == 0 || in.terminator) break;
            if (in.len == 5 && dtor[off] == 0xBA) { std::memcpy(&dsz, dtor + off + 1, 4); break; }
            off += in.len;
        }
    }
    if (dsz < bc::kRelDevReadOnly + 1 || dsz > kDevSlot) {
        std::snprintf(e.why, sizeof e.why, "sizeof(fiDeviceRelative) not established from the dtor (read 0x%x)", dsz);
        return false;
    }
    e.ctor = reinterpret_cast<CtorFn>(const_cast<std::uint8_t*>(ctor));
    e.init = reinterpret_cast<InitFn>(const_cast<std::uint8_t*>(a.rel_init));
    e.mount_as = reinterpret_cast<MountAsFn>(const_cast<std::uint8_t*>(a.rel_mount_as));
    e.rel_vtable = a.rel_vtable;
    e.mount_table = const_cast<std::uint8_t*>(a.mount_table);
    e.dev_size = dsz;
    e.ok = true;
    std::snprintf(e.why, sizeof e.why, "ok");
    return true;
}

bool map_path(const char* host, const char* map, char* out, std::size_t cap, const char** why) noexcept {
    const char* w = "";
    if (why) *why = w;
    if (!out || cap < 2) { if (why) *why = "no output buffer"; return false; }
    out[0] = 0;
    if (!host || !*host) { if (why) *why = "empty host path"; return false; }
    if (!map || !*map) { if (why) *why = "[vfs] path_map is empty"; return false; }
    const char* bar = std::strchr(map, '|');
    if (!bar || bar == map || !bar[1]) { if (why) *why = "[vfs] path_map must be <host prefix>|<game prefix>"; return false; }
    const char* hp = map;
    std::size_t hn = static_cast<std::size_t>(bar - map);
    while (hn && (hp[hn - 1] == ' ')) --hn;
    while (hn && *hp == ' ') { ++hp; --hn; }
    const char* gp = bar + 1;
    while (*gp == ' ') ++gp;
    std::size_t gn = std::strlen(gp);
    while (gn && gp[gn - 1] == ' ') --gn;
    // host prefix match: case-insensitive, '/' == '\', a trailing separator on the prefix is optional
    while (hn && sep(hp[hn - 1])) --hn;
    if (!hn || !gn) { if (why) *why = "[vfs] path_map has an empty side"; return false; }
    std::size_t i = 0;
    for (; i < hn; ++i) {
        const char a = host[i], b = hp[i];
        if (!a) break;
        if (!(sep(a) && sep(b)) && lower(a) != lower(b)) break;
    }
    if (i != hn || (host[hn] && !sep(host[hn]))) { if (why) *why = "host path is not under the path_map host prefix"; return false; }
    const char* rest = host + hn;
    while (gn && sep(gp[gn - 1])) --gn;
    std::size_t rn = std::strlen(rest);
    while (rn && sep(rest[rn - 1])) --rn;
    if (gn + rn + 1 > cap || gn + rn > kMaxRootLen) { if (why) *why = "mapped game path too long (max 160 chars)"; return false; }
    std::memcpy(out, gp, gn);
    std::memcpy(out + gn, rest, rn);
    out[gn + rn] = 0;
    for (std::size_t k = 0; out[k]; ++k) {
        const unsigned char c = static_cast<unsigned char>(out[k]);
        if (c < 0x20 || c > 0x7e || c == '|' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>') {
            out[0] = 0;
            if (why) *why = "mapped path has a non-printable, non-ASCII or wildcard character";
            return false;
        }
    }
    if (!out[0]) { if (why) *why = "mapped game path is empty"; return false; }
    return true;
}

void dump_mounts(const Engine& e, const Anchors& a, const char* when) noexcept {
    Table t;
    if (!read_table(e, t)) { G6LOG_W("mount table (%s): unreadable or implausible (count/capacity)", when); return; }
    G6LOG_I("mount table (%s): %d entries, capacity %d", when, t.count, t.cap);
    for (std::int32_t i = 0; i < t.count && i < 64; ++i) {
        const std::uint8_t* ent = t.entries + static_cast<std::size_t>(i) * bc::kMountEntrySize;
        const char* pfx = nullptr;
        char name[260] = "?";
        std::uint16_t flag = 0;
        std::int32_t n = 0;
        void** devs = nullptr;
        rdv(ent + bc::kMountEntPrefix, pfx);
        if (!read_cstr(pfx, name, sizeof name)) std::snprintf(name, sizeof name, "<unreadable %p>", static_cast<const void*>(pfx));
        rdv(ent + bc::kMountEntFlag, flag);
        if (!rdv(ent + bc::kMountEntDevCount, n) || !rdv(ent + bc::kMountEntDevices, devs) || n < 0 || n > 4096) {
            G6LOG_W("  mount[%d] '%s': device array unreadable", i, name);
            continue;
        }
        char line[1200] = {};
        std::size_t used = 0;
        for (std::int32_t k = 0; k < n && k < 12; ++k) {
            void* d = nullptr;
            const std::uint8_t* vt = nullptr;
            if (!rdv(devs + k, d) || !rdv(d, vt)) { app(line, sizeof line, used, " [%d]?", k); continue; }
            const char* kind = kind_of(a, e, vt);
            char root[bc::kRelDevRootCap + 1] = {};
            if (std::strncmp(kind, "Relative", 8) == 0 && read_cstr(static_cast<std::uint8_t*>(d) + bc::kRelDevRoot, root, sizeof root))
                app(line, sizeof line, used, " [%d]%s root='%s'", k, kind, root);
            else
                app(line, sizeof line, used, " [%d]%s", k, kind);
        }
        G6LOG_I("  mount[%d] '%s' read_only=%u devices=%d:%s%s", i, name, flag, n, line, n > 12 ? " ..." : "");
    }
}

Result mount_one(const Engine& e, const char* game_root, const char* mp, std::uint8_t* dev, char* why, std::size_t cap) noexcept {
    if (why && cap) why[0] = 0;
    if (!e.ok || !dev || !game_root || !mp) { std::snprintf(why, cap, "engine not resolved"); return Result::refused; }
    std::int32_t before = 0;
    void* last = nullptr;
    if (!find_entry(e, mp, &before, &last) || before < 1) {
        std::snprintf(why, cap, "mount point '%s' is not in the mount table (nothing to overlay)", mp);
        return Result::refused;
    }
    std::memset(dev, 0, kDevSlot);
    DWORD x = call_ctor(e.ctor, dev);
    if (x) { std::snprintf(why, cap, "ctor faulted 0x%08lX", x); return Result::faulted; }
    if (*reinterpret_cast<const void* const*>(dev) != e.rel_vtable) { std::snprintf(why, cap, "ctor did not install the Relative vtable"); return Result::refused; }
    x = call_init(e.init, dev, game_root, nullptr);
    if (x) { std::snprintf(why, cap, "Init faulted 0x%08lX", x); return Result::faulted; }
    void* parent = *reinterpret_cast<void**>(dev + bc::kRelDevParent);
    const char* root = reinterpret_cast<const char*>(dev + bc::kRelDevRoot);
    if (!std::memchr(root, 0, bc::kRelDevRootCap)) { std::snprintf(why, cap, "Init left an unterminated root"); return Result::refused; }
    if (!parent) { std::snprintf(why, cap, "GetDevice found no device for root '%s'", root); return Result::refused; }
    const std::uint8_t* pvt = nullptr;
    GetAttrFn ga = nullptr;
    if (!rdv(parent, pvt) || !rdv(pvt + bc::kDevVtGetAttributes, ga) || !ga) {
        std::snprintf(why, cap, "parent device %p has no readable vtable", parent);
        return Result::refused;
    }
    std::uint32_t attr = 0;
    x = call_attr(ga, parent, root, &attr);
    if (x) { std::snprintf(why, cap, "parent GetAttributes('%s') faulted 0x%08lX", root, x); return Result::faulted; }
    if (attr == 0xFFFFFFFFu) {
        std::snprintf(why, cap, "the game's device reports root '%s' as missing (GetAttributes == -1): path_map is wrong for this host", root);
        return Result::refused;
    }
    bool ok = false;
    x = call_mount(e.mount_as, dev, mp, &ok);
    if (x) { std::snprintf(why, cap, "MountAs faulted 0x%08lX", x); return Result::faulted; }
    if (!ok) { std::snprintf(why, cap, "MountAs/MountGlobal returned false for '%s'", mp); return Result::refused; }
    std::int32_t after = 0;
    void* top = nullptr;
    if (!find_entry(e, mp, &after, &top) || after != before + 1 || top != dev)
        std::snprintf(why, cap, "mounted, but the readback differs (devices %d -> %d, newest %p, ours %p)", before, after, top,
                      static_cast<void*>(dev));
    else
        std::snprintf(why, cap, "root '%s' (parent %p, attributes 0x%x), devices %d -> %d, newest = ours", root, parent, attr, before, after);
    return Result::mounted;
}

void configure(const Config& c, const Engine& e, const Anchors& a) noexcept {
    g_cfg = c;
    g_eng = e;
    g_anc = a;
    g_nmps = 0;
    const char* p = c.mount_points[0] ? c.mount_points : "platform:/,common:/";
    while (*p && g_nmps < kMaxMountPoints) {
        while (*p == ',' || *p == ' ') ++p;
        std::size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ') ++n;
        if (n && n < sizeof g_mps[0] && n <= bc::kMountPointMax && sep(p[n - 1])) {
            std::memcpy(g_mps[g_nmps], p, n);
            g_mps[g_nmps++][n] = 0;
        } else if (n) {
            G6LOG_W("vfs: mount point '%.*s' ignored (must end in '/' or '\\', max %u chars)", static_cast<int>(n), p, bc::kMountPointMax);
        }
        p += n;
    }
}

g6_status request(const char* host_root, std::int32_t priority) noexcept {
    if (!g_cfg.enabled) {
        if (!g_logged_off.exchange(true)) G6LOG_I("vfs_add_overlay: [vfs] mode=off (the default); nothing is mounted");
        return G6_E_NOT_IMPLEMENTED;
    }
    if (!g_eng.ok) { G6LOG_W("vfs_add_overlay refused: engine ABI not established on this build: %s", g_eng.why); return G6_E_NOT_IMPLEMENTED; }
    if (g_faulted.load()) { G6LOG_W("vfs_add_overlay refused: an earlier mount call faulted; overlay disabled"); return G6_E_INTERNAL; }
    if (!host_root || !*host_root) return G6_E_INVALID_ARG;
    const DWORD fa = GetFileAttributesA(host_root);
    if (fa == INVALID_FILE_ATTRIBUTES || !(fa & FILE_ATTRIBUTE_DIRECTORY)) {
        G6LOG_W("vfs_add_overlay: '%s' is not a host directory", host_root);
        return G6_E_INVALID_ARG;
    }
    if (!g_cfg.path_map[0]) {
        G6LOG_W("vfs_add_overlay refused: missing fact: the game-side name of the host folder '%s'. [vfs] path_map "
                "(<host prefix>|<game prefix>) is empty; it must be read from a live mount-table dump (README).", host_root);
        return G6_E_NOT_IMPLEMENTED;
    }
    char game[kMaxRootLen + 8];
    const char* why = "";
    if (!map_path(host_root, g_cfg.path_map, game, sizeof game, &why)) {
        G6LOG_W("vfs_add_overlay: '%s': %s", host_root, why);
        return G6_E_INVALID_ARG;
    }
    for (std::size_t i = 0; i < g_nmps; ++i) {
        if (_strnicmp(game, g_mps[i], std::strlen(g_mps[i]) - 1) == 0) {
            G6LOG_W("vfs_add_overlay: game path '%s' is inside overlaid mount point '%s' (would resolve through itself)", game, g_mps[i]);
            return G6_E_INVALID_ARG;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g6_status st = G6_OK;
    bool dup = false;
    for (std::size_t i = 0; i < g_npending; ++i) dup |= _stricmp(g_pending[i].game, game) == 0;
    if (dup) {
        st = G6_OK;
    } else if (g_npending >= kMaxPending) {
        st = G6_E_INVALID_ARG;
    } else {
        Pending& p = g_pending[g_npending++];
        std::snprintf(p.host, sizeof p.host, "%s", host_root);
        std::snprintf(p.game, sizeof p.game, "%s", game);
        p.prio = priority;
        p.done = false;
        g_undone.fetch_add(1);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (st == G6_OK)
        G6LOG_I("vfs_add_overlay: '%s' -> game path '%s' priority %d %s; mounted on the next script tick while the "
                "single-player guard is open", host_root, game, priority, dup ? "(already queued)" : "queued");
    else
        G6LOG_W("vfs_add_overlay: queue full (%zu)", kMaxPending);
    return st;
}

void process_pending(bool single_player) noexcept {
    if (g_undone.load(std::memory_order_relaxed) == 0 || g_faulted.load()) return;
    if (!single_player) {
        if (!g_logged_deferred.exchange(true)) G6LOG_I("vfs overlay deferred: single-player guard closed (see the guard lines)");
        return;
    }
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    for (;;) {
        // lowest priority first, so the highest priority is mounted last = newest = wins
        Pending* p = nullptr;
        for (std::size_t i = 0; i < g_npending; ++i)
            if (!g_pending[i].done && (!p || g_pending[i].prio < p->prio)) p = &g_pending[i];
        if (!p) break;
        p->done = true;
        g_undone.fetch_sub(1);
        std::size_t mounted = 0;
        for (std::size_t m = 0; m < g_nmps; ++m) {
            if (g_ndev >= kMaxDevices) { G6LOG_W("vfs: device limit (%zu) reached", kMaxDevices); break; }
            char why[400];
            const Result r = mount_one(g_eng, p->game, g_mps[m], g_dev_mem[g_ndev], why, sizeof why);
            if (r == Result::mounted) {
                ++g_ndev;   // the engine now references this slot: it is never reused or freed
                ++mounted;
                G6LOG_I("vfs overlay mounted on '%s': %s", g_mps[m], why);
            } else if (r == Result::refused) {
                G6LOG_W("vfs overlay on '%s' refused: %s", g_mps[m], why);
            } else {
                g_faulted.store(true);
                G6LOG_E("vfs overlay on '%s': %s. Overlay disabled for this session (engine state unknown).", g_mps[m], why);
                break;
            }
        }
        G6LOG_I("vfs overlay '%s' (game '%s'): mounted on %zu of %zu mount point(s)", p->host, p->game, mounted, g_nmps);
        if (g_faulted.load()) break;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void dump_once(const char* when) noexcept {
    if (!g_eng.mount_table || g_dumped.load(std::memory_order_relaxed)) return;   // called on every Run call
    if (g_dumped.exchange(true)) return;
    dump_mounts(g_eng, g_anc, when);
}

} // namespace g6rt::vfs
