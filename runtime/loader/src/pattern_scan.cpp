#include "pattern_scan.hpp"

#include <cstring>

namespace g6rt::scan {
namespace {
int hexval(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline std::uint16_t key_at(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
} // namespace

bool parse(const char* sig, Compiled& out) {
    out = Compiled{};
    if (!sig) return false;
    const char* s = sig;
    while (*s) {
        if (*s == ' ') { ++s; continue; }
        if (*s == '?') {
            ++s;
            if (*s == '?') ++s;
            if (*s && *s != ' ') return false;
            out.bytes.push_back(0);
            out.mask.push_back(0);
            continue;
        }
        const int hi = hexval(s[0]);
        const int lo = hi >= 0 ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0) return false;
        if (s[2] && s[2] != ' ') return false;
        out.bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        out.mask.push_back(0xFF);
        s += 2;
    }
    return true;
}

void scan_all(const std::uint8_t* base, std::size_t size, std::vector<Compiled>& pats, std::vector<Hit>& hits) {
    hits.assign(pats.size(), Hit{});
    if (!base || size < 2) return;

    std::vector<std::uint32_t> freq(65536, 0);
    for (std::size_t i = 0; i + 1 < size; ++i) {
        std::uint32_t& f = freq[key_at(base + i)];
        if (f != 0xFFFFFFFFu) ++f;
    }

    constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    std::vector<std::uint32_t> head(65536, kNone);
    std::vector<std::uint32_t> next(pats.size(), kNone);
    for (std::size_t p = 0; p < pats.size(); ++p) {
        Compiled& c = pats[p];
        c.usable = false;
        if (c.bytes.size() < 2) continue;
        std::uint32_t best = kNone;
        for (std::size_t k = 0; k + 1 < c.bytes.size(); ++k) {
            if (!c.mask[k] || !c.mask[k + 1]) continue;
            const std::uint32_t f = freq[key_at(&c.bytes[k])];
            if (best == kNone || f < best) { best = f; c.key_off = k; }
        }
        if (best == kNone) continue;
        c.usable = true;
        const std::uint16_t key = key_at(&c.bytes[c.key_off]);
        next[p] = head[key];
        head[key] = static_cast<std::uint32_t>(p);
    }

    for (std::size_t i = 0; i + 1 < size; ++i) {
        std::uint32_t p = head[key_at(base + i)];
        while (p != kNone) {
            const Compiled& c = pats[p];
            if (i >= c.key_off) {
                const std::size_t start = i - c.key_off;
                const std::size_t n = c.bytes.size();
                if (start + n <= size) {
                    const std::uint8_t* at = base + start;
                    bool eq = true;
                    for (std::size_t k = 0; k < n; ++k) {
                        if ((at[k] & c.mask[k]) != c.bytes[k]) { eq = false; break; }
                    }
                    if (eq) {
                        Hit& h = hits[p];
                        if (h.count == 0) h.first = start;
                        if (h.count < kCountCap) ++h.count;
                    }
                }
            }
            p = next[p];
        }
    }
}

} // namespace g6rt::scan
