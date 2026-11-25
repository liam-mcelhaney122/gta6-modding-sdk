// One-pass multi-pattern scanner for the signatures in gta6mod/patterns.hpp.
// Pure functions over a byte range; the caller decides which range is safe to read.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g6rt::scan {

struct Compiled {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;   // 0xFF = must match, 0x00 = wildcard
    std::size_t key_off = 0;          // offset of the 2-byte key inside the pattern
    bool usable = false;              // false: empty/unsigned or no concrete byte pair
};

struct Hit {
    std::uint32_t count = 0;          // saturates at kCountCap
    std::size_t first = 0;            // offset of the first match from the range start
};

inline constexpr std::uint32_t kCountCap = 1000;

// "48 8B ?? 05 ? ..." -> bytes/mask. Returns false on a malformed signature.
bool parse(const char* signature, Compiled& out);

// Resolve every pattern over [base, base+size) with a single matching pass.
// A 2-byte histogram of the range picks the rarest concrete byte pair of each
// pattern as its bucket key.
void scan_all(const std::uint8_t* base, std::size_t size, std::vector<Compiled>& pats, std::vector<Hit>& hits);

} // namespace g6rt::scan
