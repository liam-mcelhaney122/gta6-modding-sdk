// Offline self-test for gta6mod_runtime.dll: loads the DLL with autostart disabled and
// exercises the parts that do not need the game. With G6_DUMP set (folder containing
// game_scarlett_master.exe) it also maps the exe FILE section by section (no code runs)
// and checks identity, every anchor and every native handler against the manifest.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "g6rt_test_api.h"
#include "gta6mod/patterns.hpp"

/* natives.json for 1.0.2.1: 6,813 .pdata-scanned handlers + 16 .pdata-less wrappers found as NATIVE
   operands in decoded bytecode (findings/phase3/09, rounds 2-3). Update with the manifest. */
static constexpr unsigned kExpectedNatives = 6829;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) { ++g_pass; }                                                 \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
               std::printf(__VA_ARGS__); std::printf("\n"); }                   \
    } while (0)

static const g6rt_test_api* T = nullptr;
static const g6_host_api* H = nullptr;

// ------------------------------------------------------------------ decoder
struct Vec { const char* name; std::vector<uint8_t> b; uint32_t len, rip, disp_off, branch, term; };

static void test_decoder() {
    const Vec v[] = {
        { "mov rax,[rip+d32]", { 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44 }, 7, 1, 3, 0, 0 },
        { "call rel32", { 0xE8, 0, 0, 0, 0 }, 5, 0, 0, 3, 0 },
        { "jz rel32", { 0x0F, 0x84, 0x10, 0, 0, 0 }, 6, 0, 0, 5, 0 },
        { "jmp rel8", { 0xEB, 0xF0 }, 2, 0, 0, 1, 1 },
        { "mov rax,gs:[0x58]", { 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0 }, 9, 0, 0, 0, 0 },
        { "vmovaps [rsp+20],xmm6", { 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x20 }, 6, 0, 0, 0, 0 },
        { "vmovaps [rsp+b0],xmm8", { 0xC5, 0x78, 0x29, 0x84, 0x24, 0xB0, 0, 0, 0 }, 9, 0, 0, 0, 0 },
        { "movabs rcx,imm64", { 0x48, 0xB9, 0x58, 0xC2, 0x7B, 0xF9, 0x94, 0xFD, 0xF5, 0xF9 }, 10, 0, 0, 0, 0 },
        { "test byte [rip+d],1", { 0xF6, 0x05, 0x78, 0x56, 0x34, 0x12, 0x01 }, 7, 1, 2, 0, 0 },
        { "push r15", { 0x41, 0x57 }, 2, 0, 0, 0, 0 },
        { "sub rsp,0x30", { 0x48, 0x83, 0xEC, 0x30 }, 4, 0, 0, 0, 0 },
        { "cmp dword [rcx+0x10],0", { 0x83, 0x79, 0x10, 0x00 }, 4, 0, 0, 0, 0 },
        { "nopw [rax+rax]", { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 }, 6, 0, 0, 0, 0 },
        { "nop4", { 0x0F, 0x1F, 0x40, 0x00 }, 4, 0, 0, 0, 0 },
        { "lea rcx,[rip]", { 0x48, 0x8D, 0x0D, 0, 0, 0, 0 }, 7, 1, 3, 0, 0 },
        { "vpmovzxbw xmm0,xmm0", { 0xC4, 0xE2, 0x79, 0x30, 0xC0 }, 5, 0, 0, 0, 0 },
        { "vpxor xmm0,xmm0,[rip+d]", { 0xC5, 0xF9, 0xEF, 0x05, 1, 2, 3, 4 }, 8, 1, 4, 0, 0 },
        { "lock cmpxchg [rip+d],rdx", { 0xF0, 0x48, 0x0F, 0xB1, 0x15, 0xDC, 0xF9, 0xAD, 0x05 }, 9, 1, 5, 0, 0 },
        { "extrq xmm0,5,16", { 0x66, 0x0F, 0x78, 0xC0, 0x05, 0x10 }, 6, 0, 0, 0, 0 },
        { "loop rel8", { 0xE2, 0xFE }, 2, 0, 0, 6, 0 },
        { "jmp [rip+0]", { 0xFF, 0x25, 0, 0, 0, 0 }, 6, 1, 2, 0, 1 },
        { "mov rax,[rsp+0x90]", { 0x48, 0x8B, 0x84, 0x24, 0x90, 0, 0, 0 }, 8, 0, 0, 0, 0 },
        { "EVEX vmovaps zmm0,[rip]", { 0x62, 0xF1, 0x7C, 0x48, 0x28, 0x05, 0, 0, 0, 0 }, 10, 1, 6, 0, 0 },
        { "ret", { 0xC3 }, 1, 0, 0, 0, 1 },
        { "int3", { 0xCC }, 1, 0, 0, 0, 1 },
        { "lea r11,[rip+d]", { 0x4C, 0x8D, 0x1D, 0x11, 0x22, 0x33, 0x44 }, 7, 1, 3, 0, 0 },
        { "mov r15d,[rax+0x348]", { 0x44, 0x8B, 0xB8, 0x48, 0x03, 0, 0 }, 7, 0, 0, 0, 0 },
        { "mov dword [rsp+20],-1", { 0xC7, 0x44, 0x24, 0x20, 0xFF, 0xFF, 0xFF, 0xFF }, 8, 0, 0, 0, 0 },
        { "mov ax,imm16", { 0x66, 0xB8, 0x34, 0x12 }, 4, 0, 0, 0, 0 },
        { "js rel8", { 0x78, 0x3B }, 2, 0, 0, 4, 0 },
    };
    for (const Vec& t : v) {
        g6rt_insn_info in{};
        const int n = T->decode(t.b.data(), t.b.size(), &in);
        CHECK(n == static_cast<int>(t.len), "%s: len %d, want %u", t.name, n, t.len);
        CHECK(in.rip_relative == t.rip, "%s: rip %u, want %u", t.name, in.rip_relative, t.rip);
        if (t.rip) CHECK(in.disp_off == t.disp_off, "%s: disp_off %u, want %u", t.name, in.disp_off, t.disp_off);
        CHECK(in.branch == t.branch, "%s: branch %u, want %u", t.name, in.branch, t.branch);
        CHECK(in.terminator == t.term, "%s: terminator %u, want %u", t.name, in.terminator, t.term);
    }
    const uint8_t refuse[][4] = { { 0x0F, 0x0F, 0xC1, 0xB4 }, { 0x8F, 0xE8, 0x78, 0xC2 }, { 0x0F, 0x20, 0xC0, 0 }, { 0x06, 0, 0, 0 } };
    for (const auto& r : refuse) CHECK(T->decode(r, 4, nullptr) == 0, "refusal expected for %02X %02X", r[0], r[1]);
    CHECK(T->decode((const uint8_t*)"\x48\x8B\x05\x11", 4, nullptr) == 0, "truncated instruction must be refused");

    // The real scrExecContext::Run prologue (1.0.2.1, 0x143de1f10) incl. the gs load at +0x15.
    const uint8_t run[] = { 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x30,
                            0x48, 0x8B, 0x41, 0x08, 0x48, 0x89, 0xCE, 0x65, 0x48, 0x8B, 0x0C, 0x25, 0x58, 0, 0, 0, 0x4C, 0x89, 0xCB };
    const int want[] = { 2, 2, 2, 1, 1, 1, 1, 4, 4, 3, 9, 3 };
    size_t off = 0;
    for (int w : want) {
        const int n = T->decode(run + off, sizeof run - off, nullptr);
        CHECK(n == w, "Run prologue at +0x%zx: len %d want %d", off, n, w);
        off += n > 0 ? static_cast<size_t>(n) : 1;
    }
    std::printf("decoder: done\n");
}

// ------------------------------------------------------------------ scanner
static uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

static uint32_t naive_count(const uint8_t* b, size_t n, const char* sig) {
    std::vector<int> pat;
    for (const char* s = sig; *s;) {
        if (*s == ' ') { ++s; continue; }
        if (*s == '?') { pat.push_back(-1); ++s; if (*s == '?') ++s; continue; }
        pat.push_back(static_cast<int>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16))); s += 2;
    }
    uint32_t c = 0;
    for (size_t i = 0; i + pat.size() <= n; ++i) {
        bool ok = true;
        for (size_t k = 0; k < pat.size() && ok; ++k) ok = pat[k] < 0 || b[i + k] == pat[k];
        c += ok;
    }
    return c;
}

static void test_scanner() {
    std::vector<uint8_t> buf(1 << 20);
    uint32_t s = 12345;
    for (auto& b : buf) b = static_cast<uint8_t>(lcg(s));
    const uint8_t a[] = { 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x41, 0x08 };
    std::memcpy(&buf[123457], a, sizeof a);
    const uint8_t bpat[] = { 0x4C, 0x8D, 0x1D, 0xAA, 0xBB, 0xCC, 0xDD, 0xC5, 0xF8, 0x57, 0xC0, 0x45 };
    std::memcpy(&buf[1000], bpat, sizeof bpat);
    std::memcpy(&buf[900000], bpat, sizeof bpat);
    buf[900003] = 0x11;  // differs only in a wildcard position
    const char* sigs[] = {
        "41 57 41 56 41 54 56 57 55 53 48 83 EC 30 48 8B 41 08",  // once
        "4C 8D 1D ? ? ? ? C5 F8 57 C0 45",                        // twice (single-? wildcards)
        "?? ?? 41 54 56 57 55 53 48 83 EC 30",                   // leading wildcards, once
        "DE AD BE EF DE AD BE EF 13 37",                          // absent
        "",                                                       // unsigned
        "41 57 41 56 41 54 56 57 55 53 48 83 EC 30 48 8B 41 08 FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF",  // runs past nothing
    };
    const size_t n = sizeof sigs / sizeof sigs[0];
    uint32_t counts[n] = {};
    size_t first[n] = {};
    CHECK(T->scan(buf.data(), buf.size(), sigs, n, counts, first) == 0, "scan failed");
    CHECK(counts[0] == 1 && first[0] == 123457, "sig0 count %u first %zu", counts[0], first[0]);
    CHECK(counts[1] == 2 && first[1] == 1000, "sig1 count %u first %zu", counts[1], first[1]);
    CHECK(counts[2] == 1 && first[2] == 123459, "sig2 count %u first %zu", counts[2], first[2]);
    CHECK(counts[3] == 0, "sig3 count %u", counts[3]);
    CHECK(counts[4] == 0, "unsigned sig count %u", counts[4]);
    for (size_t i = 0; i < n; ++i)
        if (sigs[i][0]) CHECK(counts[i] == naive_count(buf.data(), buf.size(), sigs[i]), "sig%zu disagrees with naive scan", i);
    // pattern at the very end of the range
    std::memcpy(&buf[buf.size() - sizeof a], a, sizeof a);
    CHECK(T->scan(buf.data(), buf.size(), sigs, 1, counts, first) == 0 && counts[0] == 2, "pattern at range end: %u", counts[0]);
    const char* bad[] = { "4G 00" };
    CHECK(T->scan(buf.data(), buf.size(), bad, 1, counts, first) == -1, "malformed signature must be rejected");
    std::printf("scanner: done\n");
}

// ------------------------------------------------------------------ trampoline (pure)
static void put32(uint8_t* p, int64_t v) { int32_t x = static_cast<int32_t>(v); std::memcpy(p, &x, 4); }

static void test_builder() {
    uint8_t out[160]; size_t len = 0, stolen = 0; char err[200];
    const uint64_t src = 0x7FF600001000ull;
    const uint64_t far_t = src + 0x100000000ull;   // 4 GiB away: rel32 cannot reach back
    // rip-relative + far_t trampoline -> refused
    uint8_t f1[32] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC0, 0x01, 0xC3 };
    put32(f1 + 3, 0x7F9);
    CHECK(!T->build_trampoline(f1, src, sizeof f1, 5, far_t, out, sizeof out, &len, &stolen, err, sizeof err),
          "far_t rip-relative must be refused");
    CHECK(std::strstr(err, "rip-relative") != nullptr, "error text: %s", err);
    // near trampoline: rip disp rewritten so the target is unchanged
    const uint64_t near_t = src + 0x10000;
    CHECK(T->build_trampoline(f1, src, sizeof f1, 5, near_t, out, sizeof out, &len, &stolen, err, sizeof err), "near: %s", err);
    CHECK(stolen == 7 && len == 7 + 14, "near: stolen %zu len %zu", stolen, len);
    int32_t d; std::memcpy(&d, out + 3, 4);
    CHECK(near_t + 7 + static_cast<int64_t>(d) == src + 7 + 0x7F9, "rip target moved");
    uint64_t back; std::memcpy(&back, out + 7 + 6, 8);
    CHECK(out[7] == 0xFF && out[8] == 0x25 && back == src + 7, "back jump");
    // call rel32 at the start + far_t trampoline -> absolute call form
    uint8_t f3[32] = { 0xE8, 0, 0, 0, 0, 0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3 };
    put32(f3 + 1, 0x2FB);  // target src + 0x300
    CHECK(T->build_trampoline(f3, src, sizeof f3, 5, far_t, out, sizeof out, &len, &stolen, err, sizeof err), "far_t call: %s", err);
    uint64_t abs; std::memcpy(&abs, out + 8, 8);
    CHECK(out[0] == 0xFF && out[1] == 0x15 && out[6] == 0xEB && abs == src + 0x300, "far_t call encoding");
    // f3 with a 14-byte patch: function ends (ret at +11) inside the patch -> refused
    CHECK(!T->build_trampoline(f3, src, sizeof f3, 14, far_t, out, sizeof out, &len, &stolen, err, sizeof err) &&
          std::strstr(err, "ends") != nullptr, "short function must be refused: %s", err);
    // jcc rel8 leaving the stolen range, far_t -> inverted jcc over an absolute jmp
    uint8_t fj[32] = { 0x74, 0x03, 0x90, 0x90, 0x90, 0xC3 };
    CHECK(T->build_trampoline(fj, src, sizeof fj, 5, far_t, out, sizeof out, &len, &stolen, err, sizeof err), "jcc: %s", err);
    std::memcpy(&abs, out + 2 + 6, 8);
    CHECK(out[0] == 0x75 && out[1] == 14 && out[2] == 0xFF && abs == src + 5, "far_t jcc encoding");
    // jcc into the stolen range -> refused
    uint8_t fi[32] = { 0x74, 0x01, 0x90, 0x90, 0x90, 0x90, 0xC3 };
    CHECK(!T->build_trampoline(fi, src, sizeof fi, 5, near_t, out, sizeof out, &len, &stolen, err, sizeof err) &&
          std::strstr(err, "stolen range") != nullptr, "internal branch must be refused: %s", err);
    uint8_t fl[32] = { 0xE2, 0xFE, 0x90, 0x90, 0x90, 0xC3 };
    CHECK(!T->build_trampoline(fl, src, sizeof fl, 5, near_t, out, sizeof out, &len, &stolen, err, sizeof err), "loop must be refused");
    uint8_t fu[32] = { 0x0F, 0x0F, 0xC1, 0xB4, 0x90, 0x90, 0xC3 };
    CHECK(!T->build_trampoline(fu, src, sizeof fu, 5, near_t, out, sizeof out, &len, &stolen, err, sizeof err) &&
          std::strstr(err, "undecodable") != nullptr, "undecodable must be refused: %s", err);
    // Run prologue: 5-byte patch copies the three pushes, 14-byte patch copies exactly 14 bytes
    const uint8_t run[64] = { 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x30,
                              0x48, 0x8B, 0x41, 0x08, 0x48, 0x89, 0xCE, 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x40, 0x00 };
    CHECK(T->build_trampoline(run, src, sizeof run, 5, near_t, out, sizeof out, &len, &stolen, err, sizeof err) && stolen == 6,
          "Run rel32 stolen %zu", stolen);
    CHECK(T->build_trampoline(run, src, sizeof run, 14, far_t, out, sizeof out, &len, &stolen, err, sizeof err) && stolen == 14,
          "Run abs14 stolen %zu", stolen);
    std::printf("trampoline builder: done\n");
}

// ------------------------------------------------------------------ live hooks on local code
typedef uint64_t (*F1)(uint64_t);
typedef uint64_t (*F2)(const uint64_t*);
typedef uint64_t (*F3)(uint64_t, uint64_t);
static F1 o1; static F2 o2; static F3 o3;
static int n1, n2, n3;
static uint64_t d1(uint64_t x) { ++n1; return o1(x) + 100000; }
static uint64_t d2(const uint64_t* c) { ++n2; return o2(c) + 1000; }
static uint64_t d3(uint64_t a, uint64_t b) { ++n3; return o3(a, b) + 500; }

static void test_live_hooks() {
    uint8_t* P = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    CHECK(P != nullptr, "VirtualAlloc");
    if (!P) return;
    std::memset(P, 0xCC, 0x1000);
    // f1 @0: mov rax,[rip->data]; call helper; add rax,1; ret
    uint8_t f1[] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC0, 0x01, 0xC3 };
    put32(f1 + 3, 0x800 - 7);
    put32(f1 + 8, 0x100 - 12);
    std::memcpy(P, f1, sizeof f1);
    const uint8_t helper[] = { 0x48, 0x01, 0xC8, 0xC3 };  // add rax,rcx; ret
    std::memcpy(P + 0x100, helper, sizeof helper);
    const uint64_t data = 1000;
    std::memcpy(P + 0x800, &data, 8);
    // f2 @0x200: Run-like prologue with a host-style "call thunk; nop4" at +0x15
    uint8_t f2[] = { 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x30,
                     0x48, 0x8B, 0x41, 0x08, 0x48, 0x89, 0xCE, 0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x40, 0x00,
                     0x48, 0x89, 0xC8, 0x48, 0x03, 0x46, 0x08, 0x48, 0x83, 0xC4, 0x30,
                     0x5B, 0x5D, 0x5F, 0x5E, 0x41, 0x5C, 0x41, 0x5E, 0x41, 0x5F, 0xC3 };
    put32(f2 + 22, 0x300 - (0x200 + 26));
    std::memcpy(P + 0x200, f2, sizeof f2);
    const uint8_t thunk[] = { 0xB9, 0x07, 0, 0, 0, 0xC3 };  // mov ecx,7; ret
    std::memcpy(P + 0x300, thunk, sizeof thunk);
    // f3 @0x400: call thunk; mov rax,rcx; add rax,rdx; ret
    uint8_t f3[] = { 0xE8, 0, 0, 0, 0, 0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3 };
    put32(f3 + 1, 0x300 - (0x400 + 5));
    std::memcpy(P + 0x400, f3, sizeof f3);
    DWORD old;
    VirtualProtect(P, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), P, 0x1000);

    F1 f1p = reinterpret_cast<F1>(P);
    F2 f2p = reinterpret_cast<F2>(P + 0x200);
    F3 f3p = reinterpret_cast<F3>(P + 0x400);
    const uint64_t ctx[2] = { 0, 35 };
    CHECK(f1p(5) == 1006 && f2p(ctx) == 42 && f3p(0, 10) == 17, "unhooked baseline");

    char err[256];
    for (int abs = 0; abs < 2; ++abs) {
        void* orig = nullptr;
        void* h = T->hook_create(P, reinterpret_cast<void*>(&d1), abs ? 0 : 1, &orig, err, sizeof err);
        CHECK(h != nullptr, "f1 create (abs=%d): %s", abs, err);
        if (!h) continue;
        int kind; size_t stolen, plen;
        T->hook_info(h, &kind, &stolen, &plen);
        CHECK(kind == abs && stolen == (abs ? 16u : 7u) && plen == (abs ? 14u : 5u), "f1 info kind %d stolen %zu plen %zu", kind, stolen, plen);
        o1 = reinterpret_cast<F1>(orig);
        uint8_t before[16]; std::memcpy(before, P, 16);
        CHECK(T->hook_enable(h, err, sizeof err) == 1, "f1 enable: %s", err);
        n1 = 0;
        const uint64_t r = f1p(5);
        CHECK(r == 101006 && n1 == 1, "f1 hooked (abs=%d) -> %llu calls %d", abs, static_cast<unsigned long long>(r), n1);
        CHECK(o1(5) == 1006, "f1 via trampoline");
        CHECK(T->hook_disable(h, err, sizeof err) == 1, "f1 disable: %s", err);
        CHECK(std::memcmp(before, P, 16) == 0, "f1 bytes restored");
        CHECK(f1p(5) == 1006, "f1 unhooked again");
        T->hook_destroy(h);
    }
    {
        void* orig = nullptr;
        void* h = T->hook_create(P + 0x200, reinterpret_cast<void*>(&d2), 1, &orig, err, sizeof err);
        CHECK(h != nullptr, "f2 create: %s", err);
        if (h) {
            int kind; size_t stolen, plen;
            T->hook_info(h, &kind, &stolen, &plen);
            CHECK(stolen == 6, "f2 stolen %zu (must stop before the +0x15 call)", stolen);
            o2 = reinterpret_cast<F2>(orig);
            CHECK(T->hook_enable(h, err, sizeof err) == 1, "f2 enable: %s", err);
            n2 = 0;
            CHECK(f2p(ctx) == 1042 && n2 == 1, "f2 hooked");
            CHECK(T->hook_disable(h, err, sizeof err) == 1 && f2p(ctx) == 42, "f2 restored");
            T->hook_destroy(h);
        }
        h = T->hook_create(P + 0x200, reinterpret_cast<void*>(&d2), 0, &orig, err, sizeof err);
        CHECK(h != nullptr, "f2 abs create: %s", err);
        if (h) {
            o2 = reinterpret_cast<F2>(orig);
            CHECK(T->hook_enable(h, err, sizeof err) == 1 && f2p(ctx) == 1042, "f2 abs hooked");
            CHECK(T->hook_disable(h, err, sizeof err) == 1 && f2p(ctx) == 42, "f2 abs restored");
            T->hook_destroy(h);
        }
    }
    {
        void* orig = nullptr;
        void* h = T->hook_create(P + 0x400, reinterpret_cast<void*>(&d3), 1, &orig, err, sizeof err);
        CHECK(h != nullptr, "f3 create: %s", err);
        if (h) {
            o3 = reinterpret_cast<F3>(orig);
            CHECK(T->hook_enable(h, err, sizeof err) == 1, "f3 enable: %s", err);
            n3 = 0;
            CHECK(f3p(0, 10) == 517 && n3 == 1, "f3 hooked (relocated call executes)");
            CHECK(T->hook_disable(h, err, sizeof err) == 1 && f3p(0, 10) == 17, "f3 restored");
            T->hook_destroy(h);
        }
        h = T->hook_create(P + 0x400, reinterpret_cast<void*>(&d3), 0, &orig, err, sizeof err);
        CHECK(h == nullptr && std::strstr(err, "ends") != nullptr, "f3 abs14 must be refused: %s", err);
    }
    // double enable is idempotent, disable without enable is a no-op
    {
        void* orig = nullptr;
        void* h = T->hook_create(P, reinterpret_cast<void*>(&d1), 1, &orig, err, sizeof err);
        if (h) {
            o1 = reinterpret_cast<F1>(orig);
            CHECK(T->hook_disable(h, err, sizeof err) == 1, "disable before enable");
            CHECK(T->hook_enable(h, err, sizeof err) == 1 && T->hook_enable(h, err, sizeof err) == 1, "double enable");
            CHECK(f1p(5) == 101006, "hooked once");
            CHECK(T->hook_disable(h, err, sizeof err) == 1 && f1p(5) == 1006, "restored");
            T->hook_destroy(h);
        }
    }
    VirtualFree(P, 0, MEM_RELEASE);
    std::printf("live hooks: done\n");
}

// ------------------------------------------------------------------ late-load patch safety
// The runtime can be injected while the title's threads run. enable()/disable() suspend every other
// thread and refuse while one is inside the bytes being replaced. Checked here with a thread parked
// (never started, RIP set by SetThreadContext) inside / outside the range, one moved out during the
// retries, and a worker calling the function in a loop while the hook is toggled.
typedef uint64_t (*F4)(uint64_t);
static F4 o4;
static uint64_t d4(uint64_t x) { return o4(x) + 1000; }
static DWORD WINAPI park_fn(void*) { return 0; }  // never runs

static bool set_rip(HANDLE t, uint64_t rip) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(t, &c)) return false;
    c.Rip = rip;
    return SetThreadContext(t, &c) != 0;
}
// Moves the parked thread out of the range once the hook has started its second freeze round,
// i.e. after at least one round saw the thread inside.
struct Mover { HANDLE t; uint64_t rip; void* hook; volatile LONG moved; };
static DWORD WINAPI mover_fn(void* p) {
    auto* m = static_cast<Mover*>(p);
    for (int i = 0; i < 60000; ++i) {
        uint32_t attempts = 0;
        T->hook_freeze_info(m->hook, nullptr, &attempts, nullptr, nullptr);
        if (attempts >= 2) break;
        Sleep(1);
    }
    if (set_rip(m->t, m->rip)) InterlockedExchange(&m->moved, 1);
    return 0;
}
struct Hammer { F4 f; volatile LONG stop; LONG64 calls, hooked, bad; };
static DWORD WINAPI hammer_fn(void* p) {
    auto* h = static_cast<Hammer*>(p);
    for (uint64_t x = 0; !h->stop && x < (1ull << 40); ++x) {
        const uint64_t r = h->f(x);
        ++h->calls;
        if (r == x + 1003) ++h->hooked;
        else if (r != x + 3) ++h->bad;
    }
    return 0;
}

static void test_late_load() {
    // pure range check
    CHECK(T->hook_rip_in_range(0x1000, 0x1000, 6) == 1, "rip == lo is inside");
    CHECK(T->hook_rip_in_range(0x1005, 0x1000, 6) == 1, "rip == lo+n-1 is inside");
    CHECK(T->hook_rip_in_range(0x1006, 0x1000, 6) == 0, "rip == lo+n is outside");
    CHECK(T->hook_rip_in_range(0x0FFF, 0x1000, 6) == 0, "rip == lo-1 is outside");
    CHECK(T->hook_rip_in_range(0x1000, 0x1000, 0) == 0, "empty range contains nothing");
    CHECK(T->hook_rip_in_range(~0ull, ~0ull - 3, 14) == 1, "range ending past 2^64: last address inside");
    CHECK(T->hook_rip_in_range(5, ~0ull - 3, 14) == 0, "range ending past 2^64 does not wrap to low addresses");
    CHECK(T->hook_rip_in_range(0, 0, 1) == 1 && T->hook_rip_in_range(1, 0, 1) == 0, "range at address 0");
    CHECK(T->hook_guarded_len(5, 6) == 6, "rel32 on Run: guard the 6 relocated bytes");
    CHECK(T->hook_guarded_len(14, 14) == 14 && T->hook_guarded_len(5, 5) == 5 && T->hook_guarded_len(14, 16) == 16,
          "guarded length = max(patch, relocated)");

    uint8_t* P = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    CHECK(P != nullptr, "VirtualAlloc");
    if (!P) return;
    std::memset(P, 0xCC, 0x1000);
    const uint8_t f4[] = { 0x48, 0x8D, 0x41, 0x01, 0x48, 0x83, 0xC0, 0x02, 0xC3 };  // lea rax,[rcx+1]; add rax,2; ret
    std::memcpy(P, f4, sizeof f4);
    DWORD old;
    VirtualProtect(P, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), P, 0x1000);
    const F4 f4p = reinterpret_cast<F4>(P);
    const uint64_t base = reinterpret_cast<uint64_t>(P);
    CHECK(f4p(1) == 4, "f4 baseline");

    char err[256] = {};
    void* orig = nullptr;
    void* h = T->hook_create(P, reinterpret_cast<void*>(&d4), 1, &orig, err, sizeof err);
    CHECK(h != nullptr, "f4 create: %s", err);
    DWORD ptid = 0;
    HANDLE parked = CreateThread(nullptr, 0, park_fn, nullptr, CREATE_SUSPENDED, &ptid);
    CHECK(parked != nullptr, "parked thread");
    if (!h || !parked) {
        if (parked) { TerminateThread(parked, 0); CloseHandle(parked); }
        VirtualFree(P, 0, MEM_RELEASE);
        return;
    }
    o4 = reinterpret_cast<F4>(orig);
    int kind; size_t stolen, plen;
    T->hook_info(h, &kind, &stolen, &plen);
    CHECK(kind == 0 && plen == 5 && stolen == 8 && T->hook_guarded_len(plen, stolen) == 8, "f4 rel32: patch %zu stolen %zu", plen, stolen);
    uint8_t before[16];
    std::memcpy(before, P, 16);
    uint32_t threads = 0, attempts = 0, btid = 0;
    uint64_t brip = 0;

    // 1. a thread at an instruction boundary inside the patch (+4): refused, nothing written
    CHECK(set_rip(parked, base + 4), "SetThreadContext +4");
    const ULONGLONG t0 = GetTickCount64();
    CHECK(T->hook_enable(h, err, sizeof err) == 0, "enable must refuse while a thread is at +4");
    const ULONGLONG dt = GetTickCount64() - t0;
    CHECK(std::strstr(err, "inside the patch range") != nullptr && std::strstr(err, "nothing written") != nullptr, "refusal text: %s", err);
    T->hook_freeze_info(h, &threads, &attempts, &btid, &brip);
    CHECK(btid == ptid && brip == base + 4 && attempts >= 2, "blocking tid %u (want %lu) rip %llx attempts %u", btid, ptid,
          static_cast<unsigned long long>(brip), attempts);
    CHECK(dt <= 10000 + 2000, "refusal is time-bounded: %llu ms", static_cast<unsigned long long>(dt));
    CHECK(std::memcmp(before, P, 16) == 0 && f4p(1) == 4, "bytes untouched after the refusal");
    std::printf("late load: refusal after %u attempts in %llu ms: %s\n", attempts, static_cast<unsigned long long>(dt), err);

    // 2. the same thread just past the guarded range (+8): enabled on the first attempt
    CHECK(set_rip(parked, base + 8), "SetThreadContext +8");
    CHECK(T->hook_enable(h, err, sizeof err) == 1, "enable with a thread at +8: %s", err);
    T->hook_freeze_info(h, &threads, &attempts, &btid, &brip);
    CHECK(attempts == 1 && threads >= 1 && btid == 0, "first-attempt freeze: threads %u attempts %u", threads, attempts);
    CHECK(f4p(1) == 1004, "f4 hooked");
    CHECK(T->hook_disable(h, err, sizeof err) == 1 && f4p(1) == 4 && std::memcmp(before, P, 16) == 0, "f4 restored: %s", err);

    // 3. a thread inside the range that leaves during the retries: enabled after more than one attempt
    CHECK(set_rip(parked, base + 4), "SetThreadContext +4 (again)");
    Mover mv{ parked, base + 8, h, 0 };
    HANDLE mt = CreateThread(nullptr, 0, mover_fn, &mv, 0, nullptr);
    CHECK(mt != nullptr, "mover thread");
    CHECK(T->hook_enable(h, err, sizeof err) == 1, "enable after the thread left: %s", err);
    T->hook_freeze_info(h, &threads, &attempts, &btid, &brip);
    if (mt) { WaitForSingleObject(mt, 60000); CloseHandle(mt); }
    CHECK(mv.moved == 1 && attempts >= 2 && attempts < 200, "retried then enabled: attempts %u (moved %ld)", attempts, mv.moved);
    CHECK(f4p(2) == 1005, "f4 hooked after retries");
    CHECK(T->hook_disable(h, err, sizeof err) == 1 && f4p(2) == 5, "f4 restored after retries: %s", err);
    TerminateThread(parked, 0);
    WaitForSingleObject(parked, 5000);
    CloseHandle(parked);

    // 4. a thread executing the function in a loop while the hook is toggled 20 times
    Hammer hm{ f4p, 0, 0, 0, 0 };
    HANDLE ht = CreateThread(nullptr, 0, hammer_fn, &hm, 0, nullptr);
    CHECK(ht != nullptr, "hammer thread");
    int toggles = 0, retried = 0;
    uint32_t max_attempts = 0;
    for (int i = 0; ht && i < 20; ++i) {
        const int on = T->hook_enable(h, err, sizeof err);
        T->hook_freeze_info(h, &threads, &attempts, &btid, &brip);
        if (attempts > 1) ++retried;
        if (attempts > max_attempts) max_attempts = attempts;
        Sleep(1);
        const int off = on ? T->hook_disable(h, err, sizeof err) : 0;
        T->hook_freeze_info(h, &threads, &attempts, &btid, &brip);
        if (attempts > 1) ++retried;
        if (attempts > max_attempts) max_attempts = attempts;
        if (on && off) ++toggles;
        else break;
    }
    if (ht) {
        InterlockedExchange(&hm.stop, 1);
        WaitForSingleObject(ht, 10000);
        CloseHandle(ht);
    }
    CHECK(toggles == 20, "20 enable/disable cycles under load: %d (%s)", toggles, err);
    CHECK(hm.bad == 0 && hm.calls > 0, "worker: %lld calls, %lld through the detour, %lld wrong results", hm.calls, hm.hooked, hm.bad);
    CHECK(std::memcmp(before, P, 16) == 0, "bytes restored after the toggles");
    std::printf("late load: worker made %lld calls (%lld hooked) during 20 toggles; %d writes needed a retry (max %u attempts)\n",
                hm.calls, hm.hooked, retried, max_attempts);
    T->hook_destroy(h);
    VirtualFree(P, 0, MEM_RELEASE);
    std::printf("late load: done\n");
}

// ------------------------------------------------------------------ handler self-id (synthetic)
static void test_selfid() {
    uint8_t h[256];
    std::memset(h, 0xCC, sizeof h);
    // GET_ENTITY_ALPHA shape: push rsi; push rdi; sub rsp,38; cmp [rcx+10],0; mov rsi,rcx; js +0x3b
    const uint8_t pro[] = { 0x56, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x83, 0x79, 0x10, 0x00, 0x48, 0x89, 0xCE, 0x78, 0x3B };
    std::memcpy(h, pro, sizeof pro);
    const uint64_t id = 0xB050D415E44AFE9Dull;
    uint8_t tail[] = { 0x48, 0x8B, 0x06, 0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x89, 0x08, 0xEB, 0xB3 };
    std::memcpy(tail + 5, &id, 8);
    std::memcpy(h + 0x0F + 0x3B, tail, sizeof tail);
    const uint64_t x = (id << 32) | (id >> 32);
    const uint64_t hash = ~x;   // id = rotl64(~hash, 32)
    uint64_t got = 0;
    CHECK(T->verify_selfid(h, h, h + sizeof h, hash, &got) == 1 && got == id, "synthetic self-id ok (state %d)",
          T->verify_selfid(h, h, h + sizeof h, hash, &got));
    CHECK(T->verify_selfid(h, h, h + sizeof h, hash ^ 1, &got) == 5, "wrong hash -> id_mismatch");
    h[6] = 0x90;  // break the cmp
    CHECK(T->verify_selfid(h, h, h + sizeof h, hash, &got) != 1, "broken prologue must fail");
    std::printf("handler self-id: done\n");
}

// ------------------------------------------------------------------ scheduler + host API
static int a_ticks = 0, b_ticks = 0;
static g6_status a_wait_status = G6_E_INTERNAL, a_invoke_status = G6_OK;
static void G6_CALL tick_a(void*) {
    ++a_ticks;
    g6_native_call c{}; c.size = sizeof c;
    a_invoke_status = H->native_invoke_hash(0x06843DA7060A026Bull, &c);
    a_wait_status = H->script_wait(100);
    ++a_ticks;
}
static void G6_CALL tick_b(void*) { ++b_ticks; }

static void test_sched() {
    CHECK(H && H->size == sizeof(g6_host_api) && H->version_major == G6_API_VERSION_MAJOR, "host api table");
    g6_native_handle nh = nullptr;
    CHECK(H->native_resolve(0x06843DA7060A026Bull, &nh) == G6_OK && nh, "resolve SET_ENTITY_COORDS");
    CHECK(H->native_resolve(0x1234ull, &nh) == G6_E_UNKNOWN_NATIVE, "unknown hash");
    g6_native_call c{}; c.size = sizeof c;
    CHECK(H->native_invoke_hash(0x06843DA7060A026Bull, &c) == G6_E_NOT_ON_SCRIPT_THREAD, "invoke off the script thread");
    CHECK(H->script_wait(10) == G6_E_NOT_ON_SCRIPT_THREAD, "wait off a script fiber");
    uintptr_t addr = 1;
    CHECK(H->anchor_address("script.exec.run", &addr) == G6_E_UNSUPPORTED_BUILD && addr == 0, "anchor without a game");
    g6_build_info bi{}; bi.size = sizeof bi;
    CHECK(H->get_build_info(&bi) == G6_E_UNSUPPORTED_BUILD && bi.natives_known == kExpectedNatives, "build info (known %u)", bi.natives_known);

    g6_script_desc da{ sizeof(g6_script_desc), "a", tick_a, nullptr };
    g6_script_desc db{ sizeof(g6_script_desc), "b", tick_b, nullptr };
    g6_script_id ia = 0, ib = 0;
    CHECK(H->script_register(&da, &ia) == G6_OK && H->script_register(&db, &ib) == G6_OK, "register");
    T->sched_tick(0);
    CHECK(a_ticks == 1 && b_ticks == 1, "t=0: a %d b %d", a_ticks, b_ticks);
    CHECK(a_invoke_status == G6_E_UNSUPPORTED_BUILD, "invoke on the script fiber without a game: %d", a_invoke_status);
    T->sched_tick(50);
    CHECK(a_ticks == 1 && b_ticks == 2, "t=50: a %d b %d (a waits)", a_ticks, b_ticks);
    T->sched_tick(100);
    CHECK(a_ticks == 2 && a_wait_status == G6_OK && b_ticks == 3, "t=100: a resumed (%d), wait %d", a_ticks, a_wait_status);
    T->sched_tick(110);
    CHECK(a_ticks == 3, "t=110: a ticks again (%d)", a_ticks);
    CHECK(H->script_unregister(ia) == G6_OK && H->script_unregister(ia) == G6_E_INVALID_ARG, "unregister a");
    T->sched_tick(300);
    T->sched_tick(400);
    CHECK(a_ticks == 3 && b_ticks == 6, "a removed while waiting (a %d, b %d)", a_ticks, b_ticks);
    CHECK(H->script_unregister(ib) == G6_OK, "unregister b");
    T->sched_tick(500);
    CHECK(b_ticks == 6, "b removed");
    std::printf("scheduler: done\n");
}

// ------------------------------------------------------------------ plugin stop event (g6_plugin_stop)
static int g_seq = 0, stop_seq[3], unload_seq[3], stop_calls[3];
static g6_status stop_invoke[3];
static void stop_body(int i) {
    ++stop_calls[i];
    stop_seq[i] = ++g_seq;
    g6_native_call c{}; c.size = sizeof c;
    stop_invoke[i] = H->native_invoke_hash(0x06843DA7060A026Bull, &c);   // which thread is this on?
}
static void G6_CALL stop0(void) { stop_body(0); }
static void G6_CALL stop1(void) { stop_body(1); }
static void G6_CALL unload0(void) { unload_seq[0] = ++g_seq; }
static void G6_CALL unload1(void) { unload_seq[1] = ++g_seq; }
static void G6_CALL unload2(void) { unload_seq[2] = ++g_seq; }

static void reset_stop_counters() {
    g_seq = 0;
    for (int i = 0; i < 3; ++i) { stop_seq[i] = unload_seq[i] = stop_calls[i] = 0; stop_invoke[i] = G6_E_INTERNAL; }
}

static void test_plugin_stop() {
    const g6_plugin_stop_fn stops[3] = { stop0, stop1, nullptr };   // plugin 2 has no stop export (API 0.1)
    const g6_plugin_unload_fn unloads[3] = { unload0, unload1, unload2 };
    // A script tick arrives: the stop event runs in a script fiber (the thread check passes, so the call gets
    // as far as the build check), once per plugin, before any unload.
    reset_stop_counters();
    CHECK(T->plugin_stop_run(stops, unloads, 3, 1, 1000) == 1, "stop event queued as a script");
    CHECK(stop_calls[0] == 1 && stop_calls[1] == 1, "each stop called once (%d, %d)", stop_calls[0], stop_calls[1]);
    CHECK(stop_invoke[0] == G6_E_UNSUPPORTED_BUILD && stop_invoke[1] == G6_E_UNSUPPORTED_BUILD,
          "stop ran on the script fiber: natives pass the thread check (%d, %d)", stop_invoke[0], stop_invoke[1]);
    CHECK(stop_seq[0] < stop_seq[1] && stop_seq[1] < unload_seq[0] && unload_seq[0] < unload_seq[1] &&
          unload_seq[1] < unload_seq[2], "order: stops, then unloads (%d %d | %d %d %d)", stop_seq[0], stop_seq[1],
          unload_seq[0], unload_seq[1], unload_seq[2]);
    // No tick (game paused or exiting): stop still runs once before unload, on this thread, where natives are refused.
    reset_stop_counters();
    CHECK(T->plugin_stop_run(stops, unloads, 3, 0, 2000) == 1, "stop event queued (no tick follows)");
    CHECK(stop_calls[0] == 1 && stop_calls[1] == 1, "fallback: each stop called once (%d, %d)", stop_calls[0], stop_calls[1]);
    CHECK(stop_invoke[0] == G6_E_NOT_ON_SCRIPT_THREAD, "fallback stop is off the script thread (%d)", stop_invoke[0]);
    CHECK(stop_seq[1] < unload_seq[0], "fallback: stops before unloads");
    // Only plugins without the export: nothing is queued, they are just unloaded.
    reset_stop_counters();
    const g6_plugin_stop_fn none[1] = { nullptr };
    CHECK(T->plugin_stop_run(none, unloads + 2, 1, 1, 3000) == 0 && unload_seq[2] == 1, "no stop export: plain unload");
    std::printf("plugin stop event: done\n");
}

// ------------------------------------------------------------------ game clock (synthetic)
static void put_rel(uint8_t* buf, size_t at_insn, size_t insn_len, size_t disp_off, size_t target) {
    put32(buf + at_insn + disp_off, static_cast<int64_t>(target) - static_cast<int64_t>(at_insn + insn_len));
}

static void test_clock_decode() {
    uint8_t b[0x200];
    std::memset(b, 0xCC, sizeof b);
    // GET_GAME_TIMER shape (1.0.2.1 0x143e85810): push rsi; sub rsp,20; cmp [rcx+10],0; mov rsi,rcx; js; call core; cdqe ...
    const uint8_t h[] = { 0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x10, 0x00, 0x48, 0x89, 0xCE, 0x78, 0x14,
                          0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0x4E, 0x08, 0x48, 0x98, 0x48, 0x89, 0x01, 0x48, 0x83, 0xC4, 0x20, 0x5E, 0xC3 };
    std::memcpy(b, h, sizeof h);
    put_rel(b, 14, 5, 1, 0x40);
    const uint8_t core[] = { 0x8B, 0x05, 0, 0, 0, 0, 0xC3 };
    std::memcpy(b + 0x40, core, sizeof core);
    put_rel(b, 0x40, 6, 2, 0x100);
    const uint8_t* tgt = nullptr; const uint8_t* cr = nullptr; char why[160];
    CHECK(T->clock_find_load(b, b, b + sizeof b, 0, &tgt, &cr, why, sizeof why) == 1 && tgt == b + 0x100 && cr == b + 0x40,
          "timer via core: %s", why);
    CHECK(T->clock_find_load(b, b, b + sizeof b, 1, &tgt, &cr, why, sizeof why) == 0, "wrong kind must fail");
    // GET_FRAME_TIME core: vmovss xmm0,[rip+d]; ret
    const uint8_t core2[] = { 0xC5, 0xFA, 0x10, 0x05, 0, 0, 0, 0, 0xC3 };
    std::memcpy(b + 0x40, core2, sizeof core2);
    put_rel(b, 0x40, 8, 4, 0x10c);
    CHECK(T->clock_find_load(b, b, b + sizeof b, 1, &tgt, &cr, why, sizeof why) == 1 && tgt == b + 0x10c, "frame time via core: %s", why);
    // core that is not `load; ret`
    const uint8_t core3[] = { 0x8B, 0x05, 0, 0, 0, 0, 0x90, 0xC3 };
    std::memcpy(b + 0x40, core3, sizeof core3);
    CHECK(T->clock_find_load(b, b, b + sizeof b, 0, &tgt, &cr, why, sizeof why) == 0 && std::strstr(why, "ret"), "load;nop;ret refused: %s", why);
    // 64-bit load is not the u32 timer
    const uint8_t core4[] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3 };
    std::memcpy(b + 0x40, core4, sizeof core4);
    CHECK(T->clock_find_load(b, b, b + sizeof b, 0, &tgt, &cr, why, sizeof why) == 0, "mov rax,[rip] refused");
    // call target outside the code range
    CHECK(T->clock_find_load(b, b, b + 0x30, 0, &tgt, &cr, why, sizeof why) == 0 && std::strstr(why, "outside"), "range: %s", why);
    // inline load in the handler (another build could inline the core)
    uint8_t c[0x80];
    std::memset(c, 0xCC, sizeof c);
    const uint8_t hi[] = { 0x83, 0x79, 0x10, 0x00, 0x78, 0x10, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x49, 0x08, 0x89, 0x01, 0xC3 };
    std::memcpy(c, hi, sizeof hi);
    put_rel(c, 6, 6, 2, 0x70);
    CHECK(T->clock_find_load(c, c, c + sizeof c, 0, &tgt, &cr, why, sizeof why) == 1 && tgt == c + 0x70 && cr == nullptr, "inline: %s", why);
    // a handler that returns before any load
    const uint8_t hr[] = { 0x83, 0x79, 0x10, 0x00, 0xC3 };
    std::memcpy(c, hr, sizeof hr);
    CHECK(T->clock_find_load(c, c, c + sizeof c, 0, &tgt, &cr, why, sizeof why) == 0, "ret before load refused");
    std::printf("clock decode: done\n");
}

static void test_clock_mixer() {
    std::vector<uint32_t> g; std::vector<uint64_t> w;
    uint64_t now = 0; int st = -1; uint32_t nt = 0, na = 0, nr = 0;
    auto run = [&](int has) { return T->clock_mix(has, g.data(), w.data(), g.size(), &now, &st, &nt, &na, &nr); };
    // steady 16 ms frames
    for (int i = 0; i < 200; ++i) { g.push_back(5000 + 16u * i); w.push_back(1000000 + 16ull * i); }
    CHECK(run(1) == 1 && st == 1 && nt == 1 && na == 0, "steady: state %d trusted %u anomalies %u", st, nt, na);
    CHECK(now == 199ull * 16, "steady: output %llu (want %u)", static_cast<unsigned long long>(now), 199u * 16);
    // game paused before validation: stays candidate, follows the wall clock
    g.assign(200, 777u);
    CHECK(run(1) == 1 && st == 0 && now == 199ull * 16, "paused: state %d out %llu", st, static_cast<unsigned long long>(now));
    // paused after trust: output stops with the game
    g.clear(); w.clear();
    for (int i = 0; i < 100; ++i) { g.push_back(16u * i); w.push_back(16ull * i); }
    for (int i = 100; i < 200; ++i) { g.push_back(16u * 99); w.push_back(16ull * i); }
    CHECK(run(1) == 1 && st == 1 && now == 16ull * 99, "pause after trust: out %llu want %u", static_cast<unsigned long long>(now), 16u * 99);
    // u32 wrap is a normal step
    g.clear(); w.clear();
    for (int i = 0; i < 200; ++i) { g.push_back(0xFFFFF000u + 16u * i); w.push_back(16ull * i); }
    CHECK(run(1) == 1 && st == 1 && na == 0 && now == 199ull * 16, "wrap: state %d anomalies %u", st, na);
    // one reset after trust: anomaly, still trusted, output monotonic (run() returns -1 otherwise)
    g.clear(); w.clear();
    for (int i = 0; i < 100; ++i) { g.push_back(100000 + 16u * i); w.push_back(16ull * i); }
    for (int i = 0; i < 100; ++i) { g.push_back(16u * i); w.push_back(1600 + 16ull * i); }
    CHECK(run(1) == 1 && st == 1 && na == 1, "reset after trust: state %d anomalies %u", st, na);
    // repeated backwards steps while validating: rejected, system timer
    g.clear(); w.clear();
    for (int i = 0; i < 200; ++i) { g.push_back((i % 10 == 9) ? 0u : 10000 + 16u * i); w.push_back(16ull * i); }
    CHECK(run(1) == 1 && st == 2 && nr == 1 && nt == 0, "flapping: state %d rejected %u trusted %u", st, nr, nt);
    // implausible jump while validating restarts validation
    g.clear(); w.clear();
    for (int i = 0; i < 20; ++i) { g.push_back(16u * i); w.push_back(16ull * i); }
    g.push_back(16u * 19 + 600000); w.push_back(16ull * 20);
    CHECK(run(1) == 1 && st == 0 && na == 1, "jump: state %d anomalies %u", st, na);
    // no game timer: system timer, each step capped at 1000 ms
    g.assign(3, 0u); w = { 0, 5000, 5500 };
    CHECK(run(0) == 1 && st == 2 && now == 1500, "system only: state %d out %llu", st, static_cast<unsigned long long>(now));
    std::printf("clock mixer: done\n");
}

// ------------------------------------------------------------------ vfs (pure + fake engine)
static void test_vfs_map() {
    char out[256];
    CHECK(T->vfs_map_path("C:\\Games\\GTA6\\mods", "C:\\Games\\GTA6|G:\\", out, sizeof out) == 1 && std::strcmp(out, "G:\\mods") == 0, "map1 '%s'", out);
    CHECK(T->vfs_map_path("c:/games/gta6/mods/", "C:\\Games\\GTA6\\ | G:", out, sizeof out) == 1 && std::strcmp(out, "G:/mods") == 0, "map2 '%s'", out);
    CHECK(T->vfs_map_path("C:\\Games\\GTA6x\\mods", "C:\\Games\\GTA6|G:", out, sizeof out) == 0, "prefix boundary");
    CHECK(T->vfs_map_path("D:\\mods", "C:\\Games|G:", out, sizeof out) == 0, "not under prefix");
    CHECK(T->vfs_map_path("C:\\Games\\mods", "", out, sizeof out) == 0, "empty map");
    CHECK(T->vfs_map_path("C:\\Games\\mods", "C:\\Games", out, sizeof out) == 0, "map without |");
    CHECK(T->vfs_map_path("C:\\Games\\m\xC3\xBC", "C:\\Games|G:", out, sizeof out) == 0, "non-ASCII refused");
    std::string longp = "C:\\Games\\" + std::string(200, 'a');
    CHECK(T->vfs_map_path(longp.c_str(), "C:\\Games|G:", out, sizeof out) == 0, "too long refused");
    std::printf("vfs path map: done\n");
}

struct FakeEntry { const char* prefix; uint16_t flag, len; uint32_t pad; int32_t cap, count; void** devs; };
static_assert(sizeof(FakeEntry) == 0x20, "mount entry is 0x20 bytes");
static void* g_fake_rel_vt[81];
static void* g_fake_par_vt[81];
static void* g_fake_parent[2] = { g_fake_par_vt, nullptr };
static void* g_plat_devs[8]; static void* g_comm_devs[8];
static FakeEntry g_fake_entries[2];
static struct { int32_t cap, count; FakeEntry* entries; } g_fake_tbl;
static uint8_t g_dev_a[0x200], g_dev_b[0x200];

static uint32_t fake_par_getattr(void*, const char* p) { return std::strcmp(p, "G:/mods") == 0 ? 0x10u : 0xFFFFFFFFu; }
static void* fake_ctor(void* d) { auto* p = static_cast<uint8_t*>(d); void* vt = g_fake_rel_vt; std::memcpy(p, &vt, 8); std::memset(p + 8, 0, 8); return d; }
static void* fake_ctor_bad(void* d) { return d; }
static void fake_init(void* d, const char* root, bool ro, void* parent) {
    auto* p = static_cast<uint8_t*>(d);
    std::snprintf(reinterpret_cast<char*>(p + 0x10), 0x100, "%s", root);
    p[0x114] = ro ? 1 : 0;
    void* par = parent ? parent : static_cast<void*>(g_fake_parent);
    std::memcpy(p + 8, &par, 8);
}
static void fake_init_fault(void*, const char*, bool, void*) { *static_cast<volatile int*>(nullptr) = 1; }
static bool fake_mount_as(void* d, const char* mp) {
    for (auto& e : g_fake_entries) {
        if (std::strcmp(e.prefix, mp) != 0) continue;
        if (e.count >= e.cap) return false;
        e.devs[e.count++] = d;
        const int32_t n = static_cast<int32_t>(std::strlen(mp)) - 1;
        std::memcpy(static_cast<uint8_t*>(d) + 0x110, &n, 4);
        return true;
    }
    return false;
}
static bool fake_mount_false(void*, const char*) { return false; }

static void test_vfs_fake() {
    static void* game_dev_vt[81] = {};
    static void* game_dev[2] = { game_dev_vt, nullptr };
    g_fake_par_vt[0x168 / 8] = reinterpret_cast<void*>(&fake_par_getattr);
    g_plat_devs[0] = game_dev; g_plat_devs[1] = game_dev; g_comm_devs[0] = game_dev;
    g_fake_entries[0] = { "platform:/", 0, 10, 0, 8, 2, g_plat_devs };
    g_fake_entries[1] = { "common:/", 0, 8, 0, 8, 1, g_comm_devs };
    g_fake_tbl = { 256, 2, g_fake_entries };
    void* slot = &g_fake_tbl.entries;   // count at slot-4, capacity at slot-8
    char why[400];
    auto mount = [&](void* ctor, void* init, void* ma, const char* root, const char* mp, uint8_t* dev) {
        return T->vfs_mount_fake(ctor, init, ma, g_fake_rel_vt, slot, 0x118, root, mp, dev, why, sizeof why);
    };
    int r = mount(reinterpret_cast<void*>(&fake_ctor), reinterpret_cast<void*>(&fake_init), reinterpret_cast<void*>(&fake_mount_as),
                  "G:/mods", "platform:/", g_dev_a);
    int32_t mlen; std::memcpy(&mlen, g_dev_a + 0x110, 4);
    CHECK(r == 0 && g_fake_entries[0].count == 3 && g_plat_devs[2] == g_dev_a && mlen == 9 && g_dev_a[0x114] == 1 &&
          std::strstr(why, "newest = ours"), "mount platform: r %d count %d why %s", r, g_fake_entries[0].count, why);
    r = mount(reinterpret_cast<void*>(&fake_ctor), reinterpret_cast<void*>(&fake_init), reinterpret_cast<void*>(&fake_mount_as),
              "G:/mods", "update:/", g_dev_b);
    CHECK(r == 1 && std::strstr(why, "not in the mount table"), "absent mount point: r %d %s", r, why);
    r = mount(reinterpret_cast<void*>(&fake_ctor), reinterpret_cast<void*>(&fake_init), reinterpret_cast<void*>(&fake_mount_as),
              "G:/missing", "common:/", g_dev_b);
    CHECK(r == 1 && std::strstr(why, "missing") && g_fake_entries[1].count == 1, "missing root: r %d %s", r, why);
    r = mount(reinterpret_cast<void*>(&fake_ctor), reinterpret_cast<void*>(&fake_init), reinterpret_cast<void*>(&fake_mount_false),
              "G:/mods", "common:/", g_dev_b);
    CHECK(r == 1 && std::strstr(why, "returned false"), "MountAs false: r %d %s", r, why);
    r = mount(reinterpret_cast<void*>(&fake_ctor_bad), reinterpret_cast<void*>(&fake_init), reinterpret_cast<void*>(&fake_mount_as),
              "G:/mods", "common:/", g_dev_b);
    CHECK(r == 1 && std::strstr(why, "vtable"), "ctor without vtable: r %d %s", r, why);
    r = mount(reinterpret_cast<void*>(&fake_ctor), reinterpret_cast<void*>(&fake_init_fault), reinterpret_cast<void*>(&fake_mount_as),
              "G:/mods", "common:/", g_dev_b);
    CHECK(r == 2 && std::strstr(why, "faulted"), "faulting Init: r %d %s", r, why);
    r = mount(reinterpret_cast<void*>(&fake_ctor), reinterpret_cast<void*>(&fake_init), reinterpret_cast<void*>(&fake_mount_as),
              "G:/mods", "common:/", g_dev_b);
    CHECK(r == 0 && g_fake_entries[1].count == 2 && g_comm_devs[1] == g_dev_b, "mount common: r %d %s", r, why);
    g6_status st = H->vfs_add_overlay("C:\\", 0);
    CHECK(st == G6_E_UNSUPPORTED_BUILD, "vfs_add_overlay without a game: %d", static_cast<int>(st));
    std::printf("vfs fake engine: done\n");
}

// ------------------------------------------------------------------ single-player guard
static void test_guard() {
    struct V4 { uint8_t a[4]; int cls; } v4[] = {
        { { 127, 0, 0, 1 }, 0 }, { { 127, 255, 1, 2 }, 0 }, { { 169, 254, 10, 1 }, 1 }, { { 0, 0, 0, 0 }, 2 },
        { { 10, 1, 2, 3 }, 3 }, { { 172, 16, 0, 1 }, 3 }, { { 172, 31, 255, 1 }, 3 }, { { 172, 32, 0, 1 }, 4 },
        { { 192, 168, 1, 1 }, 3 }, { { 100, 64, 0, 1 }, 3 }, { { 8, 8, 8, 8 }, 4 }, { { 52, 1, 2, 3 }, 4 }, { { 169, 253, 0, 1 }, 4 },
    };
    for (const auto& t : v4)
        CHECK(T->guard_classify(0, t.a) == t.cls, "v4 %u.%u.%u.%u: class %d want %d", t.a[0], t.a[1], t.a[2], t.a[3],
              T->guard_classify(0, t.a), t.cls);
    struct V6 { uint8_t a[16]; int cls; } v6[] = {
        { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 0 },
        { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 2 },
        { { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 }, 1 },
        { { 0xfe, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 1 },
        { { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 4 },
        { { 0xfd, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 3 },
        { { 0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 3 },
        { { 0x20, 0x01, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88 }, 4 },
        { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1 }, 0 },     // ::ffff:127.0.0.1
        { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 8, 8, 4, 4 }, 4 },       // ::ffff:8.8.4.4
        { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 0, 9 }, 3 },
    };
    for (size_t i = 0; i < sizeof v6 / sizeof v6[0]; ++i)
        CHECK(T->guard_classify(1, v6[i].a) == v6[i].cls, "v6 case %zu: class %d want %d", i, T->guard_classify(1, v6[i].a), v6[i].cls);
    // decision: only ESTABLISHED (5) and SYN_SENT (3), only non-loopback non-link-local remotes
    for (int c = 0; c <= 4; ++c) {
        const int want = (c >= 2) ? 1 : 0;
        CHECK(T->guard_trips(c, 5) == want && T->guard_trips(c, 3) == want, "class %d ESTABLISHED/SYN_SENT -> %d", c, want);
        CHECK(T->guard_trips(c, 2) == 0 && T->guard_trips(c, 8) == 0 && T->guard_trips(c, 11) == 0, "class %d LISTEN/CLOSE_WAIT/TIME_WAIT ignored", c);
    }
    // latch semantics
    {
        const int ops[] = { 5, 2, 5, 1, 5, 2, 5, 5, 5, 3, 5, 2, 5, 4, 4, 5, 2, 5, 0, 1, 5 };
        const uint64_t t[] = { 0, 1000, 1000, 0, 1000, 2000, 2000, 21999, 22001, 0, 22001, 23000, 23000, 0, 0, 23000, 24000, 24000, 0, 0, 24000 };
        const size_t n = sizeof ops / sizeof ops[0];
        int out[n];
        CHECK(T->guard_gate_run(ops, t, n, out) == 1, "gate script");
        CHECK(out[0] == 0, "closed at start");
        CHECK(out[2] == 0, "a clean scan alone does not open it (no attestation)");
        CHECK(out[4] == 1, "attested + clean scan -> open");
        CHECK(out[6] == 1 && out[7] == 1 && out[8] == 0, "stale after 20 s without a clean scan (%d %d %d)", out[6], out[7], out[8]);
        CHECK(out[10] == 0, "failed scan closes it");
        CHECK(out[12] == 1, "reopens after the next clean scan");
        CHECK(out[13] == 1 && out[14] == 0, "trip reports once");
        CHECK(out[15] == 0 && out[17] == 0 && out[20] == 0, "tripped stays closed after clean scans and re-attestation");
    }
    // live: the test process has no TCP connections
    uint32_t own = 99, trip = 99; unsigned long err = 0;
    CHECK(T->guard_scan_self(&own, &trip, &err) == 1, "GetExtendedTcpTable on the test process failed (%lu)", err);
    CHECK(own == 0 && trip == 0, "test process owns %u TCP row(s), %u tripping (want 0)", own, trip);
    g6_native_call c{}; c.size = sizeof c;
    CHECK(H->native_invoke_hash(0x06843DA7060A026Bull, &c) == G6_E_NOT_ON_SCRIPT_THREAD, "invoke still refused off the script thread");
    std::printf("guard: done (live scan: %u own rows)\n", own);
}

// ------------------------------------------------------------------ dump (optional)
static void test_dump() {
    char dir[1024];
    if (!GetEnvironmentVariableA("G6_DUMP", dir, sizeof dir)) { std::printf("dump: skipped (G6_DUMP not set)\n"); return; }
    char path[1100];
    std::snprintf(path, sizeof path, "%s\\game_scarlett_master.exe", dir);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(f != INVALID_HANDLE_VALUE, "open %s", path);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    std::vector<uint8_t> file(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0; size_t off = 0;
    while (off < file.size() && ReadFile(f, file.data() + off, static_cast<DWORD>(std::min<size_t>(1u << 26, file.size() - off)), &got, nullptr) && got)
        off += got;
    CloseHandle(f);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    const size_t img_size = nt->OptionalHeader.SizeOfImage;
    uint8_t* img = static_cast<uint8_t*>(VirtualAlloc(nullptr, img_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(img != nullptr, "alloc image");
    if (!img) return;
    std::memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sh = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const size_t n = std::min<size_t>(sh[i].SizeOfRawData, sh[i].Misc.VirtualSize);
        if (sh[i].PointerToRawData + n <= file.size() && sh[i].VirtualAddress + n <= img_size)
            std::memcpy(img + sh[i].VirtualAddress, file.data() + sh[i].PointerToRawData, n);
    }
    std::vector<uint8_t>().swap(file);
    char rep[8192];
    CHECK(T->check_image(img, img_size, rep, sizeof rep) == 1, "identity: %s", rep);
    std::printf("dump identity: %s\n", rep);
    const ULONGLONG t0 = GetTickCount64();
    const int ra = T->resolve_anchors(img, img_size, rep, sizeof rep);
    std::printf("dump anchors (%llu ms): %s\n", static_cast<unsigned long long>(GetTickCount64() - t0), rep);
    CHECK(ra == 1, "required anchors on the file image");
    uint32_t ok = 0, bad = 0;
    CHECK(T->verify_handlers(img, img_size, &ok, &bad) == 1 && ok == kExpectedNatives && bad == 0, "handlers ok %u bad %u", ok, bad);
    std::printf("dump handlers: %u verified, %u failed\n", ok, bad);

    // Game clock: decode the real handler bytes (1.0.2.1 values from the disassembly in build_consts.hpp).
    // `sh` pointed into the freed file buffer: take .text from the image's own headers.
    const auto* ish = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        img + reinterpret_cast<const IMAGE_DOS_HEADER*>(img)->e_lfanew));
    const uint8_t* tlo = img + ish[0].VirtualAddress;
    const uint8_t* thi = tlo + ish[0].Misc.VirtualSize;
    const uint8_t* tgt = nullptr; const uint8_t* core = nullptr; char why[200];
    CHECK(T->clock_find_load(img + 0x3e85810, tlo, thi, 0, &tgt, &core, why, sizeof why) == 1 && tgt == img + 0xabbef40 &&
          core == img + 0x5f2ef0, "GET_GAME_TIMER handler: target rva 0x%llx core rva 0x%llx (%s)",
          tgt ? static_cast<long long>(tgt - img) : -1ll, core ? static_cast<long long>(core - img) : -1ll, why);
    CHECK(T->clock_find_load(img + 0x3e85700, tlo, thi, 0, &tgt, &core, why, sizeof why) == 1 && tgt == img + 0xabbef44,
          "GET_FRAME_COUNT handler (%s)", why);
    CHECK(T->clock_find_load(img + 0x3e85740, tlo, thi, 1, &tgt, &core, why, sizeof why) == 1 && tgt == img + 0xabbef4c,
          "GET_FRAME_TIME handler (%s)", why);
    CHECK(T->clock_find_load(img + 0x3e85740, tlo, thi, 0, &tgt, &core, why, sizeof why) == 0, "GET_FRAME_TIME is not a u32 load");
    uint32_t crva = 0; int vok = 0;
    const ULONGLONG t1 = GetTickCount64();
    const int re = T->resolve_engine(img, img_size, rep, sizeof rep, &crva, &vok);
    std::printf("dump engine (%llu ms): %s\n", static_cast<unsigned long long>(GetTickCount64() - t1), rep);
    CHECK(re == 1 && crva == 0xabbef40, "clock block rva 0x%x (want 0xabbef40)", crva);
    // G-016: the re-db anchor world.game_timer (xref chain from the GET_GAME_TIMER handler, generated patterns.hpp)
    // lands on the block the clock decode found.
    const g6::patterns::anchor* ga = g6::patterns::by_name("world.game_timer");
    CHECK(ga && !ga->signature[0] && g6::patterns::chain_by_name("world.game_timer"), "world.game_timer is a chain anchor in patterns.hpp");
    if (ga) {
        const g6::patterns::result gr = g6::patterns::resolve_anchor(img, img_size, tlo, static_cast<size_t>(thi - tlo), *ga);
        CHECK(gr.state == g6::patterns::status::ok && gr.address == img + crva && crva == 0xabbef40,
              "world.game_timer anchor: state %d rva 0x%llx, clock block rva 0x%x", static_cast<int>(gr.state),
              gr.address ? static_cast<long long>(gr.address - img) : -1ll, crva);
    }
    CHECK(vok == 1, "vfs ABI checks on the file image");
    VirtualFree(img, 0, MEM_RELEASE);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // output survives a crash
    SetEnvironmentVariableW(L"G6RT_NO_AUTOSTART", L"1");
    const char* dll = argc > 1 ? argv[1] : "gta6mod_runtime.dll";
    HMODULE m = LoadLibraryA(dll);
    if (!m) { std::printf("cannot load %s (%lu)\n", dll, GetLastError()); return 2; }
    auto get = reinterpret_cast<g6rt_get_test_api_fn>(reinterpret_cast<void*>(GetProcAddress(m, "g6rt_get_test_api")));
    auto host = reinterpret_cast<const g6_host_api* (G6_CALL*)(void)>(reinterpret_cast<void*>(GetProcAddress(m, "g6_get_host_api")));
    if (!get || !host) { std::printf("missing exports\n"); return 2; }
    T = get();
    H = host();
    if (!T || T->size != sizeof(g6rt_test_api)) { std::printf("test api size mismatch\n"); return 2; }
    test_decoder();
    test_scanner();
    test_builder();
    test_live_hooks();
    test_late_load();
    test_selfid();
    test_sched();
    test_plugin_stop();
    test_clock_decode();
    test_clock_mixer();
    test_vfs_map();
    test_vfs_fake();
    test_guard();
    test_dump();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
