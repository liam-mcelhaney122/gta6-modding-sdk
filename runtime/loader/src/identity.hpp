// Build identity of the mapped game image (design §6, adapted to a relocated, host-patched image).
#pragma once
#include <cstddef>
#include <cstdint>

namespace g6rt::identity {

enum class Tri : int { unavailable = -1, mismatch = 0, match = 1 };

struct Report {
    bool headers_ok = false;          // Machine, TimeDateStamp, SizeOfImage, section table
    bool exports_ok = false;          // exactly the pinned export set at the pinned RVAs
    Tri imphash = Tri::unavailable;   // pefile-compatible imphash of the ORIGINAL import descriptors
    char imphash_hex[33] = {};
    Tri module_file_sha = Tri::unavailable;  // SHA-256 of the file the module was mapped from
    char module_file_sha_hex[65] = {};
    Tri exe_path_sha = Tri::unavailable;     // SHA-256 of [runtime] exe_path, if configured
    char exe_path_sha_hex[65] = {};
    char detail[512] = {};            // first header/export mismatch, human readable
    // informational: fields the host is documented to patch (research/03 §2.1)
    std::uint16_t characteristics = 0;
    std::uint32_t entry_rva = 0;
    std::uint32_t import_dir_rva = 0;
    std::uint32_t tls_dir_rva = 0;
    std::uint64_t header_image_base = 0;
};

// Bounds-checked header + export check over an image laid out at its RVAs (mapped image,
// or a file mapped section-by-section for the offline test).
void check_headers(const std::uint8_t* base, std::size_t size, Report& r) noexcept;
void check_imphash(const std::uint8_t* base, std::size_t size, Report& r) noexcept;

bool sha256_file(const wchar_t* path, char out_hex[65]) noexcept;
// Every page of [p, p+len) committed and readable (no guard/noaccess).
bool readable_range(const void* p, std::size_t len) noexcept;
bool md5_hex(const void* data, std::size_t n, char out_hex[33]) noexcept;

struct GameModule {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    wchar_t path[1024] = {};
    bool from_loader_list = false;
    bool export_seen = false;          // a module exporting __XMemAllocationHooks__ was found
};

// Locate the title image: a module exporting __XMemAllocationHooks__ (loader list first,
// then MEM_IMAGE/private allocations), never GetModuleHandle(nullptr) (that is the host).
bool find_game_module(GameModule& out) noexcept;

} // namespace g6rt::identity
