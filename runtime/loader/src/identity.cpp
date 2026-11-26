#include "identity.hpp"

#include "manifest_1_0_2_1.inc"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace g6rt::identity {
namespace {

struct Img {
    const std::uint8_t* b;
    std::size_t n;
    template <class T> const T* at(std::size_t rva, std::size_t count = 1) const noexcept {
        if (rva > n || count > (n - rva) / sizeof(T)) return nullptr;
        return reinterpret_cast<const T*>(b + rva);
    }
    const char* cstr(std::size_t rva, std::size_t maxlen) const noexcept {
        if (rva >= n) return nullptr;
        const std::size_t lim = (n - rva) < maxlen ? (n - rva) : maxlen;
        const void* z = std::memchr(b + rva, 0, lim);
        return z ? reinterpret_cast<const char*>(b + rva) : nullptr;
    }
};

const IMAGE_NT_HEADERS64* nt_headers(const Img& im) noexcept {
    const auto* dos = im.at<IMAGE_DOS_HEADER>(0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return nullptr;
    const auto* nt = im.at<IMAGE_NT_HEADERS64>(static_cast<std::size_t>(dos->e_lfanew));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    return nt;
}

bool readable(const void* p, std::size_t len) noexcept {
    const std::uint8_t* a = static_cast<const std::uint8_t*>(p);
    const std::uint8_t* end = a + len;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(a, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        const DWORD x = mbi.Protect & 0xFF;
        if (!(x == PAGE_READONLY || x == PAGE_READWRITE || x == PAGE_WRITECOPY || x == PAGE_EXECUTE_READ ||
              x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY)) return false;
        a = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

void append(char* buf, std::size_t cap, const char* fmt, ...) noexcept {
    const std::size_t used = std::strlen(buf);
    if (used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
}

bool hash_init(const wchar_t* alg, BCRYPT_ALG_HANDLE& a, BCRYPT_HASH_HANDLE& h) noexcept {
    a = nullptr; h = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) < 0) return false;
    if (BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(a, 0); a = nullptr; return false; }
    return true;
}
void hash_close(BCRYPT_ALG_HANDLE a, BCRYPT_HASH_HANDLE h) noexcept {
    if (h) BCryptDestroyHash(h);
    if (a) BCryptCloseAlgorithmProvider(a, 0);
}
void to_hex(const std::uint8_t* d, std::size_t n, char* out) noexcept {
    static const char k[] = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) { out[2 * i] = k[d[i] >> 4]; out[2 * i + 1] = k[d[i] & 15]; }
    out[2 * n] = 0;
}

} // namespace

void check_headers(const std::uint8_t* base, std::size_t size, Report& r) noexcept {
    namespace m = g6rt::manifest;
    r.headers_ok = false;
    r.exports_ok = false;
    r.detail[0] = 0;
    const Img im{ base, size };
    const auto* nt = nt_headers(im);
    if (!nt) { append(r.detail, sizeof r.detail, "no PE32+ header; "); return; }
    const auto& fh = nt->FileHeader;
    const auto& oh = nt->OptionalHeader;
    r.characteristics = fh.Characteristics;
    r.entry_rva = oh.AddressOfEntryPoint;
    r.import_dir_rva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    r.tls_dir_rva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;
    r.header_image_base = oh.ImageBase;

    bool ok = true;
    if (fh.Machine != m::kMachine) { ok = false; append(r.detail, sizeof r.detail, "Machine 0x%x; ", fh.Machine); }
    if (fh.TimeDateStamp != m::kTimeDateStamp) { ok = false; append(r.detail, sizeof r.detail, "TimeDateStamp %u != %u; ", fh.TimeDateStamp, m::kTimeDateStamp); }
    if (m::kSizeOfImage == 0) { ok = false; append(r.detail, sizeof r.detail, "SizeOfImage pin UNSET (regenerate with --exe); "); }
    else if (oh.SizeOfImage != m::kSizeOfImage) { ok = false; append(r.detail, sizeof r.detail, "SizeOfImage 0x%x != 0x%x; ", oh.SizeOfImage, m::kSizeOfImage); }
    constexpr std::size_t nsec = sizeof(m::kSections) / sizeof(m::kSections[0]);
    if (fh.NumberOfSections != nsec) {
        ok = false;
        append(r.detail, sizeof r.detail, "NumberOfSections %u != %zu; ", fh.NumberOfSections, nsec);
    } else {
        const std::size_t sec_rva = reinterpret_cast<const std::uint8_t*>(IMAGE_FIRST_SECTION(nt)) - base;
        const auto* sh = im.at<IMAGE_SECTION_HEADER>(sec_rva, nsec);
        if (!sh) { ok = false; append(r.detail, sizeof r.detail, "section table out of range; "); }
        for (std::size_t i = 0; sh && i < nsec; ++i) {
            char name[9] = {};
            std::memcpy(name, sh[i].Name, 8);
            if (std::strcmp(name, m::kSections[i].name) != 0 || sh[i].VirtualAddress != m::kSections[i].rva ||
                sh[i].Misc.VirtualSize != m::kSections[i].vsize || sh[i].SizeOfRawData != m::kSections[i].raw_size) {
                ok = false;
                append(r.detail, sizeof r.detail, "section %zu '%s' rva 0x%x vsize 0x%x differs; ", i, name,
                       sh[i].VirtualAddress, sh[i].Misc.VirtualSize);
            }
        }
    }
    r.headers_ok = ok;

    // exports: exactly the pinned set
    const auto& dd = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* ed = dd.VirtualAddress ? im.at<IMAGE_EXPORT_DIRECTORY>(dd.VirtualAddress) : nullptr;
    constexpr std::size_t nexp = sizeof(m::kExports) / sizeof(m::kExports[0]);
    bool eok = ed && ed->NumberOfNames == nexp;
    if (eok) {
        const auto* names = im.at<DWORD>(ed->AddressOfNames, ed->NumberOfNames);
        const auto* ords = im.at<WORD>(ed->AddressOfNameOrdinals, ed->NumberOfNames);
        const auto* funcs = im.at<DWORD>(ed->AddressOfFunctions, ed->NumberOfFunctions);
        eok = names && ords && funcs;
        for (std::size_t i = 0; eok && i < nexp; ++i) {
            const char* s = im.cstr(names[i], 256);
            const WORD o = ords[i];
            if (!s || o >= ed->NumberOfFunctions || std::strcmp(s, m::kExports[i].name) != 0 || funcs[o] != m::kExports[i].rva) {
                eok = false;
                append(r.detail, sizeof r.detail, "export %zu '%s' differs; ", i, s ? s : "?");
            }
        }
    } else {
        append(r.detail, sizeof r.detail, "export directory missing or count differs; ");
    }
    r.exports_ok = eok;
}

void check_imphash(const std::uint8_t* base, std::size_t size, Report& r) noexcept {
    namespace m = g6rt::manifest;
    r.imphash = Tri::unavailable;
    r.imphash_hex[0] = 0;
    if (m::kOrigImportDirRva == 0) return;
    const Img im{ base, size };
    try {
        std::string s;
        s.reserve(16384);
        std::size_t rva = m::kOrigImportDirRva;
        bool first = true;
        for (int d = 0; d < 512; ++d, rva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const auto* id = im.at<IMAGE_IMPORT_DESCRIPTOR>(rva);
            if (!id) return;
            if (id->OriginalFirstThunk == 0 && id->Name == 0 && id->FirstThunk == 0) break;
            const char* dll = im.cstr(id->Name, 256);
            if (!dll) return;
            std::string lib(dll);
            for (char& c : lib) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            const std::string dll_lower = lib;
            const std::size_t dot = lib.rfind('.');
            if (dot != std::string::npos) {
                const std::string ext = lib.substr(dot + 1);
                if (ext == "ocx" || ext == "sys" || ext == "dll") lib.resize(dot);
            }
            std::size_t t = id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk;
            for (int k = 0; k < 8192; ++k, t += 8) {
                const auto* v = im.at<std::uint64_t>(t);
                if (!v) return;
                if (*v == 0) break;
                std::string fn;
                if (*v & IMAGE_ORDINAL_FLAG64) {
                    const unsigned ord = static_cast<unsigned>(*v & 0xFFFF);
                    for (const auto& on : m::kOrdinalNames) {
                        if (on.ordinal == ord && dll_lower == on.dll_lower) { fn = on.name; break; }
                    }
                    if (fn.empty()) { char b[16]; std::snprintf(b, sizeof b, "ord%u", ord); fn = b; }
                } else {
                    const char* nm = im.cstr(static_cast<std::size_t>((*v & 0x7FFFFFFFull) + 2), 512);
                    if (!nm) return;
                    fn = nm;
                }
                for (char& c : fn) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                if (!first) s += ',';
                first = false;
                s += lib;
                s += '.';
                s += fn;
            }
        }
        if (!md5_hex(s.data(), s.size(), r.imphash_hex)) return;
        r.imphash = std::strcmp(r.imphash_hex, m::kImphashMd5Hex) == 0 ? Tri::match : Tri::mismatch;
    } catch (...) {
        r.imphash = Tri::unavailable;
    }
}

bool readable_range(const void* p, std::size_t len) noexcept { return readable(p, len); }

bool md5_hex(const void* data, std::size_t n, char out_hex[33]) noexcept {
    BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE h;
    if (!hash_init(BCRYPT_MD5_ALGORITHM, a, h)) return false;
    std::uint8_t d[16];
    const bool ok = BCryptHashData(h, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0) >= 0 &&
                    BCryptFinishHash(h, d, sizeof d, 0) >= 0;
    hash_close(a, h);
    if (ok) to_hex(d, sizeof d, out_hex);
    return ok;
}

bool sha256_file(const wchar_t* path, char out_hex[65]) noexcept {
    out_hex[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    constexpr DWORD kChunk = 4u << 20;
    void* buf = VirtualAlloc(nullptr, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    BCRYPT_ALG_HANDLE a = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = buf && hash_init(BCRYPT_SHA256_ALGORITHM, a, h);
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(f, buf, kChunk, &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        if (BCryptHashData(h, static_cast<PUCHAR>(buf), got, 0) < 0) ok = false;
    }
    std::uint8_t d[32];
    if (ok) ok = BCryptFinishHash(h, d, sizeof d, 0) >= 0;
    hash_close(a, h);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(f);
    if (ok) to_hex(d, sizeof d, out_hex);
    return ok;
}

bool find_game_module(GameModule& out) noexcept {
    out = GameModule{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&find_game_module), &self);
    auto probe = [&](std::uint8_t* b) -> bool {
        if (!readable(b, 0x1000)) return false;
        const Img hdr{ b, 0x1000 };
        const auto* nt = nt_headers(hdr);
        if (!nt) return false;
        const std::size_t sz = nt->OptionalHeader.SizeOfImage;
        const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dd.VirtualAddress || dd.VirtualAddress >= sz || !readable(b + dd.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return false;
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(b + dd.VirtualAddress);
        if (ed->NumberOfNames > 64 || ed->AddressOfNames >= sz || !readable(b + ed->AddressOfNames, 4 * ed->NumberOfNames)) return false;
        const DWORD* names = reinterpret_cast<const DWORD*>(b + ed->AddressOfNames);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
            if (names[i] >= sz || !readable(b + names[i], 32)) continue;
            if (std::strncmp(reinterpret_cast<const char*>(b + names[i]), "__XMemAllocationHooks__", 24) == 0) {
                out.base = b;
                out.size = sz;
                out.export_seen = true;
                return true;
            }
        }
        return false;
    };

    HMODULE mods[1024];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        const DWORD n = needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024;
        for (DWORD i = 0; i < n; ++i) {
            if (mods[i] == self) continue;
            if (probe(reinterpret_cast<std::uint8_t*>(mods[i]))) {
                out.from_loader_list = true;
                GetModuleFileNameW(mods[i], out.path, static_cast<DWORD>(sizeof out.path / sizeof out.path[0]));
                return true;
            }
        }
    }
    // Fallback: allocation bases that look like a mapped PE (manual-map hosts).
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::uint8_t* a = static_cast<std::uint8_t*>(si.lpMinimumApplicationAddress);
    std::uint8_t* const end = static_cast<std::uint8_t*>(si.lpMaximumApplicationAddress);
    for (int guard = 0; guard < 2000000 && a < end; ++guard) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && mbi.BaseAddress == mbi.AllocationBase && (mbi.Type == MEM_IMAGE || mbi.Type == MEM_PRIVATE) &&
            mbi.BaseAddress != self) {
            if (probe(static_cast<std::uint8_t*>(mbi.BaseAddress))) {
                K32GetMappedFileNameW(GetCurrentProcess(), mbi.BaseAddress, out.path, static_cast<DWORD>(sizeof out.path / sizeof out.path[0]));
                return true;
            }
        }
        a = static_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return false;
}

} // namespace g6rt::identity
