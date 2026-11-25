#include "hook.hpp"

#include "x64_decode.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace g6rt::hook {
namespace {

void set_err(char* err, std::size_t cap, const char* fmt, ...) noexcept {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

bool fits_i32(std::int64_t v) noexcept { return v >= INT32_MIN && v <= INT32_MAX; }

struct Emitter {
    std::uint8_t* out;
    std::size_t cap;
    std::size_t len = 0;
    bool overflow = false;
    void put(const void* p, std::size_t n) noexcept {
        if (len + n > cap) { overflow = true; return; }
        std::memcpy(out + len, p, n);
        len += n;
    }
    void u8(std::uint8_t v) noexcept { put(&v, 1); }
    void i32(std::int32_t v) noexcept { put(&v, 4); }
    void u64(std::uint64_t v) noexcept { put(&v, 8); }
    void abs_jmp(std::uint64_t to) noexcept {  // jmp qword [rip+0]; dq to
        const std::uint8_t op[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
        put(op, 6);
        u64(to);
    }
};

// Suspends every other thread of the process; reports whether any is executing in [lo, lo+n).
class ThreadFreezer {
public:
    ~ThreadFreezer() { thaw(); }
    bool freeze(const std::uint8_t* lo, std::size_t n, bool& inside) noexcept {
        inside = false;
        std::vector<DWORD> tids;
        try {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return false;
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
                    te.dwSize = sizeof te;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            handles_.clear();
            handles_.reserve(tids.size());  // no allocation while threads are suspended
        } catch (...) {
            return false;
        }
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(lo);
        for (DWORD tid : tids) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!h) continue;  // exited meanwhile
            if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); continue; }
            handles_.push_back(h);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(h, &ctx)) { inside = true; continue; }  // unknown position: treat as unsafe
            if (ctx.Rip >= a && ctx.Rip < a + n) inside = true;
        }
        return true;
    }
    void thaw() noexcept {
        for (HANDLE h : handles_) { ResumeThread(h); CloseHandle(h); }
        handles_.clear();
    }
private:
    std::vector<HANDLE> handles_;
};

std::uint8_t* alloc_near(std::uintptr_t target, std::size_t size) noexcept {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const std::uintptr_t min_app = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t max_app = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    const std::uintptr_t reach = 0x7FF00000;  // keep margin below 2 GiB for the trampoline body
    const std::uintptr_t lo = target > reach + min_app ? target - reach : min_app;
    const std::uintptr_t hi = target + reach < max_app ? target + reach : max_app;

    // downward
    std::uintptr_t a = (target & ~(gran - 1));
    for (int guard = 0; guard < 100000 && a > lo + gran; ++guard) {
        a -= gran;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return static_cast<std::uint8_t*>(p);
        } else {
            const std::uintptr_t ab = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase) & ~(gran - 1);
            if (ab < a) a = ab;  // skip the whole allocation
        }
    }
    // upward
    a = (target & ~(gran - 1));
    for (int guard = 0; guard < 100000 && a + gran < hi; ++guard) {
        a += gran;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return static_cast<std::uint8_t*>(p);
        } else {
            const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize + gran - 1) & ~(gran - 1);
            if (end > a + gran) a = end - gran;
        }
    }
    return nullptr;
}

bool is_executable(const void* p) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD x = mbi.Protect & 0xFF;
    return x == PAGE_EXECUTE || x == PAGE_EXECUTE_READ || x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY;
}

} // namespace

bool build_trampoline(const std::uint8_t* src, std::uint64_t src_addr, std::size_t src_avail,
                      std::size_t min_bytes, std::uint64_t tramp_addr,
                      std::uint8_t* out, std::size_t out_cap, BuildResult& res) noexcept {
    res = BuildResult{};
    if (!src || !out || min_bytes == 0 || min_bytes > 32) {
        set_err(res.error, sizeof res.error, "bad arguments");
        return false;
    }
    // Pass 1: decode the stolen range.
    struct Item { x64::Insn in; std::size_t off; };
    Item items[32];
    std::size_t n = 0, off = 0;
    bool ends_with_terminator = false;
    while (off < min_bytes) {
        if (n == 32) { set_err(res.error, sizeof res.error, "too many instructions"); return false; }
        const x64::Insn in = x64::decode(src + off, src_avail > off ? src_avail - off : 0);
        if (in.len == 0) {
            set_err(res.error, sizeof res.error, "undecodable instruction at +0x%zx (byte %02X)", off, src[off]);
            return false;
        }
        if (in.branch == x64::Branch::loop_rel8) {
            set_err(res.error, sizeof res.error, "loop/jrcxz at +0x%zx is not relocatable", off);
            return false;
        }
        items[n++] = { in, off };
        off += in.len;
        if (in.terminator) {
            if (off < min_bytes) {
                set_err(res.error, sizeof res.error, "function ends at +0x%zx, inside the %zu-byte patch", off, min_bytes);
                return false;
            }
            ends_with_terminator = true;
        }
    }
    const std::size_t stolen = off;
    const std::uint64_t lo = src_addr, hi = src_addr + stolen;
    for (std::size_t k = 0; k < n; ++k) {
        const Item& it = items[k];
        const std::uint64_t ip = src_addr + it.off;
        if (it.in.branch != x64::Branch::none) {
            const std::uint64_t t = x64::rel_target(it.in, src + it.off, ip);
            if (t > lo && t < hi) {
                set_err(res.error, sizeof res.error, "branch at +0x%zx targets the stolen range", it.off);
                return false;
            }
            if (it.in.op_off != 0 && !(it.in.op_off == 1 && (src[it.off] == 0x2E || src[it.off] == 0x3E || src[it.off] == 0xF2))) {
                set_err(res.error, sizeof res.error, "prefixed branch at +0x%zx", it.off);
                return false;
            }
        }
        if (it.in.rip_relative) {
            const std::uint64_t t = x64::rip_target(it.in, src + it.off, ip);
            if (t >= lo && t < hi) {
                set_err(res.error, sizeof res.error, "rip-relative operand at +0x%zx reads the stolen range", it.off);
                return false;
            }
        }
    }
    // Pass 2: emit.
    Emitter e{ out, out_cap };
    for (std::size_t k = 0; k < n; ++k) {
        const Item& it = items[k];
        const std::uint8_t* p = src + it.off;
        const std::uint64_t ip = src_addr + it.off;
        const std::uint64_t cur = tramp_addr + e.len;
        const x64::Insn& in = it.in;
        if (in.branch == x64::Branch::none) {
            std::uint8_t buf[16];
            std::memcpy(buf, p, in.len);
            if (in.rip_relative) {
                const std::uint64_t t = x64::rip_target(in, p, ip);
                const std::int64_t nd = static_cast<std::int64_t>(t - (cur + in.len));
                if (!fits_i32(nd)) {
                    set_err(res.error, sizeof res.error, "rip-relative operand at +0x%zx out of reach from the trampoline", it.off);
                    return false;
                }
                const std::int32_t d32 = static_cast<std::int32_t>(nd);
                std::memcpy(buf + in.disp_off, &d32, 4);
            }
            e.put(buf, in.len);
            continue;
        }
        const std::uint64_t t = x64::rel_target(in, p, ip);
        switch (in.branch) {
        case x64::Branch::call_rel32: {
            const std::int64_t r = static_cast<std::int64_t>(t - (cur + 5));
            if (fits_i32(r)) { e.u8(0xE8); e.i32(static_cast<std::int32_t>(r)); }
            else {  // call [rip+2]; jmp +8; dq t
                const std::uint8_t op[8] = { 0xFF, 0x15, 0x02, 0, 0, 0, 0xEB, 0x08 };
                e.put(op, 8); e.u64(t);
            }
            break;
        }
        case x64::Branch::jmp_rel8:
        case x64::Branch::jmp_rel32: {
            const std::int64_t r = static_cast<std::int64_t>(t - (cur + 5));
            if (fits_i32(r)) { e.u8(0xE9); e.i32(static_cast<std::int32_t>(r)); }
            else e.abs_jmp(t);
            break;
        }
        case x64::Branch::jcc_rel8:
        case x64::Branch::jcc_rel32: {
            const std::int64_t r = static_cast<std::int64_t>(t - (cur + 6));
            if (fits_i32(r)) { e.u8(0x0F); e.u8(static_cast<std::uint8_t>(0x80 | in.cond)); e.i32(static_cast<std::int32_t>(r)); }
            else {  // j!cc +14; jmp [rip+0]; dq t
                e.u8(static_cast<std::uint8_t>(0x70 | (in.cond ^ 1))); e.u8(14);
                e.abs_jmp(t);
            }
            break;
        }
        default:
            set_err(res.error, sizeof res.error, "unsupported branch at +0x%zx", it.off);
            return false;
        }
    }
    if (!ends_with_terminator) e.abs_jmp(src_addr + stolen);
    if (e.overflow) { set_err(res.error, sizeof res.error, "trampoline buffer too small"); return false; }
    res.code_len = e.len;
    res.stolen = stolen;
    return true;
}

InlineHook::~InlineHook() = default;

bool InlineHook::create(void* target, void* detour, const Options& opt, char* err, std::size_t errcap) noexcept {
    if (created_) { set_err(err, errcap, "already created"); return false; }
    if (!target || !detour) { set_err(err, errcap, "null target/detour"); return false; }
    if (!is_executable(target)) { set_err(err, errcap, "target %p is not committed executable memory", target); return false; }
    target_ = static_cast<std::uint8_t*>(target);
    detour_ = detour;
    const std::uint64_t taddr = reinterpret_cast<std::uint64_t>(target);
    constexpr std::size_t kBlock = 4096;
    constexpr std::size_t kRelay = 16;
    std::uint8_t code[kMaxTrampoline];
    BuildResult br;
    char why_near[200] = "near mode disabled";

    if (opt.allow_rel32) {
        std::uint8_t* blk = alloc_near(static_cast<std::uintptr_t>(taddr), kBlock);
        if (!blk) {
            std::snprintf(why_near, sizeof why_near, "no free block within +-2 GiB");
        } else {
            const std::int64_t rel = static_cast<std::int64_t>(reinterpret_cast<std::uint64_t>(blk) - (taddr + 5));
            if (!fits_i32(rel)) {
                std::snprintf(why_near, sizeof why_near, "near block out of rel32 reach");
                VirtualFree(blk, 0, MEM_RELEASE);
            } else if (!build_trampoline(target_, taddr, 64, kRel32PatchLen, reinterpret_cast<std::uint64_t>(blk) + kRelay,
                                         code, sizeof code, br)) {
                std::snprintf(why_near, sizeof why_near, "%s", br.error);
                VirtualFree(blk, 0, MEM_RELEASE);
                set_err(err, errcap, "trampoline (rel32 mode): %s", why_near);
                target_ = nullptr;
                return false;  // the abs14 mode would copy a superset of these instructions
            } else {
                Emitter relay{ blk, kRelay };
                relay.abs_jmp(reinterpret_cast<std::uint64_t>(detour));
                std::memset(blk + relay.len, 0xCC, kRelay - relay.len);
                std::memcpy(blk + kRelay, code, br.code_len);
                block_ = blk;
                trampoline_ = blk + kRelay;
                kind_ = PatchKind::rel32_via_relay;
                patch_len_ = kRel32PatchLen;
                patch_[0] = 0xE9;
                const std::int32_t r32 = static_cast<std::int32_t>(rel);
                std::memcpy(patch_ + 1, &r32, 4);
            }
        }
    }
    if (!block_) {
        std::uint8_t* blk = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kBlock, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!blk) { set_err(err, errcap, "VirtualAlloc failed (%lu)", GetLastError()); target_ = nullptr; return false; }
        if (!build_trampoline(target_, taddr, 64, kAbsPatchLen, reinterpret_cast<std::uint64_t>(blk), code, sizeof code, br)) {
            set_err(err, errcap, "trampoline (abs14 mode; near: %s): %s", why_near, br.error);
            VirtualFree(blk, 0, MEM_RELEASE);
            target_ = nullptr;
            return false;
        }
        std::memcpy(blk, code, br.code_len);
        block_ = blk;
        trampoline_ = blk;
        kind_ = PatchKind::abs14;
        patch_len_ = kAbsPatchLen;
        Emitter pe{ patch_, sizeof patch_ };
        pe.abs_jmp(reinterpret_cast<std::uint64_t>(detour));
    }
    stolen_ = br.stolen;
    std::memcpy(saved_, target_, patch_len_);
    DWORD old = 0;
    if (!VirtualProtect(block_, kBlock, PAGE_EXECUTE_READ, &old)) {
        set_err(err, errcap, "VirtualProtect(trampoline) failed (%lu)", GetLastError());
        VirtualFree(block_, 0, MEM_RELEASE);
        block_ = nullptr; trampoline_ = nullptr; target_ = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), block_, kBlock);
    created_ = true;
    return true;
}

bool InlineHook::write_patch(const std::uint8_t* bytes, const std::uint8_t* expect, char* err, std::size_t errcap) noexcept {
    ThreadFreezer fz;
    bool inside = false;
    int attempt = 0;
    for (;; ++attempt) {
        if (!fz.freeze(target_, patch_len_, inside)) { set_err(err, errcap, "could not enumerate/suspend threads"); return false; }
        if (!inside) break;
        fz.thaw();
        if (attempt >= 200) { set_err(err, errcap, "a thread stayed inside the patch range"); return false; }
        Sleep(1);
    }
    if (std::memcmp(target_, expect, patch_len_) != 0) {
        fz.thaw();
        set_err(err, errcap, "target bytes changed since create (someone else patched it?)");
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(target_, patch_len_, PAGE_EXECUTE_READWRITE, &old)) {
        const DWORD e = GetLastError();
        fz.thaw();
        set_err(err, errcap, "VirtualProtect(target) failed (%lu)", e);
        return false;
    }
    const std::uintptr_t t = reinterpret_cast<std::uintptr_t>(target_);
    if (patch_len_ <= 8 && (t & 7) + patch_len_ <= 8) {
        volatile LONG64* q = reinterpret_cast<volatile LONG64*>(t & ~static_cast<std::uintptr_t>(7));
        LONG64 cur = *q, want = cur;
        std::memcpy(reinterpret_cast<std::uint8_t*>(&want) + (t & 7), bytes, patch_len_);
        InterlockedCompareExchange64(q, want, cur);
    } else {
        std::memcpy(target_, bytes, patch_len_);
    }
    DWORD dummy = 0;
    VirtualProtect(target_, patch_len_, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), target_, patch_len_);
    fz.thaw();
    if (std::memcmp(target_, bytes, patch_len_) != 0) { set_err(err, errcap, "patch readback mismatch"); return false; }
    return true;
}

bool InlineHook::enable(char* err, std::size_t errcap) noexcept {
    if (!created_) { set_err(err, errcap, "not created"); return false; }
    if (enabled_) return true;
    if (!write_patch(patch_, saved_, err, errcap)) return false;
    enabled_ = true;
    return true;
}

bool InlineHook::disable(char* err, std::size_t errcap) noexcept {
    if (!created_) { set_err(err, errcap, "not created"); return false; }
    if (!enabled_) return true;
    if (!write_patch(saved_, patch_, err, errcap)) return false;
    enabled_ = false;
    return true;
}

void InlineHook::release_unsafe() noexcept {
    if (enabled_) return;
    if (block_) VirtualFree(block_, 0, MEM_RELEASE);
    block_ = nullptr;
    trampoline_ = nullptr;
    target_ = nullptr;
    created_ = false;
}

} // namespace g6rt::hook
