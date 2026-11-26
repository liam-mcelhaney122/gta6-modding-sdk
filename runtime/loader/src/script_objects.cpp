#include "script_objects.hpp"

#include "gta6mod/script_objects.h"
#include "log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>

namespace g6rt::objects {
namespace {

// Interned objects live for the session, as a script's string literals live as long as its program. Past these
// limits each call gets its own object, freed after the call (logged once).
constexpr std::size_t kMaxInterned = 16384;
constexpr std::size_t kMaxBytes = std::size_t{ 8 } << 20;

struct Store {
    std::mutex mu;
    std::unordered_map<std::string, void*> strings, texts;
    std::size_t bytes = 0;
    std::uint32_t temporaries = 0;
    bool full_logged = false;
};
Store& store() {
    static Store* s = new Store();  // never destroyed: natives may hold pointers until the process ends
    return *s;
}

std::atomic<const void*> g_type{ nullptr };
std::atomic<const void*> g_elem{ nullptr };
std::atomic<bool> g_text{ false };
g6_script_text_vtable g_vtable;
std::once_flag g_vtable_once;

bool in(const std::uint8_t* p, std::size_t n, const std::uint8_t* lo, const std::uint8_t* hi) noexcept {
    return p && lo && hi && p >= lo && p <= hi && static_cast<std::size_t>(hi - p) >= n;
}

std::int32_t rd32(const std::uint8_t* p) noexcept {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void* build(int kind, const char* s, std::size_t len) noexcept {
    const std::size_t n = kind == 0 ? g6_script_string_bytes(len) : g6_script_text_bytes(len);
    void* mem = _aligned_malloc(n, 16);
    if (!mem) return nullptr;
    if (kind == 0) return g6_script_string_init(mem, s, len, g_type.load(), g_elem.load());
    std::call_once(g_vtable_once, [] { g6_script_text_vtable_init(&g_vtable); });
    return g6_script_text_init(mem, s, len, &g_vtable);
}

// Interned object for (kind, text), or a temporary (*temp set) when the store is full; null when out of memory.
void* acquire(int kind, const char* s, std::size_t len, void** temp) noexcept {
    try {
        Store& st = store();
        std::lock_guard<std::mutex> lk(st.mu);
        auto& map = kind == 0 ? st.strings : st.texts;
        std::string key(s, len);
        if (auto it = map.find(key); it != map.end()) return it->second;
        const std::size_t n = kind == 0 ? g6_script_string_bytes(len) : g6_script_text_bytes(len);
        void* obj = build(kind, s, len);
        if (!obj) return nullptr;
        if (st.strings.size() + st.texts.size() < kMaxInterned && st.bytes + n <= kMaxBytes) {
            map.emplace(std::move(key), obj);
            st.bytes += n;
            return obj;
        }
        if (!st.full_logged) {
            st.full_logged = true;
            G6LOG_W("script objects: %zu interned (%zu bytes); new texts now get a per-call object freed after the "
                    "call (a native that keeps the pointer would then dangle)", st.strings.size() + st.texts.size(), st.bytes);
        }
        ++st.temporaries;
        *temp = obj;
        return obj;
    } catch (...) {
        return nullptr;
    }
}

} // namespace

bool literal_op_records(const std::uint8_t* op, const std::uint8_t* lo, const std::uint8_t* hi, const void** type,
                        const void** elem) noexcept {
    if (type) *type = nullptr;
    if (elem) *elem = nullptr;
    if (!in(op, kLiteralOpSpan, lo, hi)) return false;
    static constexpr std::uint8_t lea_t[] = { 0x48, 0x8D, 0x15 }, lea_e[] = { 0x4C, 0x8D, 0x05 };
    static constexpr std::uint8_t st_t[] = { 0x48, 0x89, 0x11 }, st_e[] = { 0x4C, 0x89, 0x41, 0x20 };
    if (std::memcmp(op + kLeaTypeAt, lea_t, 3) || std::memcmp(op + kLeaElemAt, lea_e, 3) ||
        std::memcmp(op + kStoreTypeAt, st_t, 3) || std::memcmp(op + kStoreElemAt, st_e, 4))
        return false;
    const std::uint8_t* t = op + kLeaTypeAt + 7 + rd32(op + kLeaTypeAt + 3);
    const std::uint8_t* e = op + kLeaElemAt + 7 + rd32(op + kLeaElemAt + 3);
    if (type) *type = t;
    if (elem) *elem = e;
    return t != e;
}

bool text_path_ok(const std::uint8_t* get, const std::uint8_t* native_get, const std::uint8_t* lo,
                  const std::uint8_t* hi) noexcept {
    if (!in(get, kTextGetSpan, lo, hi) || !in(native_get, sizeof kNativeGetBytes, lo, hi)) return false;
    static constexpr std::uint8_t rd[] = { 0x48, 0x8B, 0x49, 0x18, 0xE8 };
    if (std::memcmp(get + kTextReadAt, rd, sizeof rd)) return false;
    const std::uint8_t* call_end = get + kTextReadAt + 4 + 5;
    if (call_end + rd32(get + kTextReadAt + 5) != native_get) return false;
    return std::memcmp(native_get, kNativeGetBytes, sizeof kNativeGetBytes) == 0;
}

void configure(const void* string_type, const void* string_elem, bool text_ok) noexcept {
    const bool s = string_type && string_elem;
    g_type.store(s ? string_type : nullptr);
    g_elem.store(s ? string_elem : nullptr);
    g_text.store(text_ok);
}
bool string_ready() noexcept { return g_type.load() && g_elem.load(); }
bool text_ready() noexcept { return g_text.load(); }

Stats stats() noexcept {
    Store& st = store();
    std::lock_guard<std::mutex> lk(st.mu);
    return Stats{ static_cast<std::uint32_t>(st.strings.size()), static_cast<std::uint32_t>(st.texts.size()), st.temporaries,
                  st.bytes };
}

g6_status CallObjects::prepare(g6_native_call* call) noexcept {
    if (!call || call->size < sizeof(g6_native_call)) return G6_OK;  // API < 0.4 caller: no masks
    const std::uint32_t sm = call->string_slots, tm = call->text_slots;
    if (!(sm | tm)) return G6_OK;
    const std::uint32_t argc = call->argc > G6_MAX_NATIVE_ARGS ? G6_MAX_NATIVE_ARGS : call->argc;
    const std::uint32_t valid = argc >= 32 ? 0xFFFFFFFFu : ((1u << argc) - 1u);
    if ((sm & tm) || ((sm | tm) & ~valid)) return G6_E_INVALID_ARG;
    static std::atomic<int> logged{ 0 };
    if ((sm && !string_ready()) || (tm && !text_ready())) {
        if (logged.fetch_add(1) < 5)
            G6LOG_W("script objects: %s argument refused: %s did not resolve or check out on this build",
                    sm && !string_ready() ? "String" : "text",
                    sm && !string_ready() ? "script.string.literal_op / type_record / literal_elem_record"
                                          : "script.text.get / native_get");
        return G6_E_NO_SUCH_ANCHOR;
    }
    for (std::uint32_t i = 0; i < argc; ++i) {
        const std::uint32_t bit = 1u << i;
        if (!((sm | tm) & bit)) continue;
        const char* s = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(call->args[i]));
        if (!s) continue;  // a null object, as scripts pass it
        const std::size_t len = strnlen(s, G6_SSTR_MAX_LENGTH + 1);
        void* temp = nullptr;
        void* obj = len > G6_SSTR_MAX_LENGTH ? nullptr : acquire((sm & bit) ? 0 : 1, s, len, &temp);
        if (!obj) {
            for (std::uint32_t k = 0; k < i; ++k) {  // undo what this call already converted
                if (mask_ & (1u << k)) call->args[k] = saved_[k];
                if (temps_[k]) { _aligned_free(temps_[k]); temps_[k] = nullptr; }
            }
            mask_ = 0;
            return G6_E_INVALID_ARG;
        }
        saved_[i] = call->args[i];
        temps_[i] = temp;
        mask_ |= bit;
        call->args[i] = reinterpret_cast<std::uintptr_t>(obj);
    }
    call_ = call;
    return G6_OK;
}

CallObjects::~CallObjects() {
    for (std::uint32_t i = 0; i < G6_MAX_NATIVE_ARGS; ++i) {
        if (call_ && (mask_ & (1u << i))) call_->args[i] = saved_[i];
        if (temps_[i]) _aligned_free(temps_[i]);
    }
}

} // namespace g6rt::objects
