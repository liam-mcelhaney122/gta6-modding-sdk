// ui_retained.cpp — retained UI programs replayed on the render thread (ui_retained.h; sdk/docs/python_host.md,
// "Python UI"). No language runtime is linked here: the render thread only ever runs this file and the g6_ui_api.
#define G6_UI_RETAINED_IMPL 1
#include "ui_retained.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace g6uiret {
namespace {

// Program commands (wire values; the Python encoder is gta6mod/runtime/embedded_ui.py).
enum Op : std::uint8_t {
    kBegin = 1, kEnd, kNextPos, kNextSize, kText, kTextColored, kButton, kCheckbox, kSliderInt, kSliderFloat,
    kInputText, kCombo, kSeparator, kSameLine, kTreeNode, kTreePop, kTabBar, kEndTabBar, kTabItem, kEndTabItem,
    kColorEdit, kPlotLines,
};

constexpr std::uint32_t kProgramMagic = 0x50553647u;  // "G6UP"
constexpr std::uint32_t kResultMagic = 0x52553647u;   // "G6UR"
constexpr std::uint32_t kNoString = 0xFFFFFFFFu;
constexpr std::uint32_t kMaxCommands = 65536, kMaxString = 65536, kMaxItems = 4096, kMaxValues = 65536;
constexpr std::size_t kMaxDepth = 64;
constexpr std::size_t kMaxStates = 4096;     // opener states kept per frame (cleared when exceeded)
constexpr std::uint32_t kMaxInputCap = 65537; // input_text buffer bytes incl. the NUL (Python: max_bytes + 1)

struct Cmd {
    std::uint8_t op = 0;
    std::uint8_t closable = 0;   // kBegin: the window has a close button
    std::uint8_t body = 0;       // kTreeNode / kTabBar / kTabItem: the host recorded the contents and the closer
    std::uint8_t has_opt = 0;    // kSliderFloat format / kPlotLines overlay present
    std::uint32_t u = 0;         // window flags, cond, color flags, input buffer size
    std::uint32_t occ = 0;       // n-th command with this (op, label) in the program: with them, the widget's key
    std::uint32_t end = 0;       // openers with a closer: index of the matching closer
    std::int32_t i[3] = {};      // int value, min, max
    float f[8] = {};             // slider value/min/max, rgba, pos/size, plot scale + size
    std::string s;               // label, title or text
    std::string opt;             // format, overlay or input text
    std::vector<std::string> items;
    std::vector<float> values;
};

struct Program {
    std::vector<Cmd> cmds;
};

struct Key {
    std::uint8_t op;
    std::uint32_t occ;
    std::string label;
    bool operator<(const Key& o) const { return std::tie(op, occ, label) < std::tie(o.op, o.occ, o.label); }
};

// What the user produced on the render thread.
struct Value {
    std::int32_t i = 0;
    float f[4] = {};
    std::string s;
    bool delivered = false;  // handed to the host by poll(); dropped at its next submit (the program carries it)
};
struct Event {
    std::uint32_t clicks = 0;  // buttons
    bool closed = false;       // closable windows
};

struct Slot {
    std::mutex mu;
    bool used = false;
    bool closing = false;      // being created or destroyed: frames do nothing
    std::uint32_t gen = 1;
    g6_ui_id ui_id = 0;
    const g6_ui_api* ui = nullptr;
    std::shared_ptr<const Program> program;
    std::map<Key, std::uint8_t> states;   // openers: drawn / open / visible / selected in the latest frame
    std::map<Key, Value> values;          // value widgets: the user's value, also the override while replaying
    std::map<Key, Event> events;          // buttons, closable windows
    std::uint32_t frames = 0;             // frames drawn since the last poll
    bool have_frame = false;
    g6_ui_frame last{};

    void reset() {
        used = false;
        closing = false;
        ui_id = 0;
        ui = nullptr;
        program.reset();
        states.clear();
        values.clear();
        events.clear();
        frames = 0;
        have_frame = false;
        last = g6_ui_frame{};
    }
};

std::atomic<const g6_host_api*> g_host{ nullptr };
std::mutex g_alloc;
Slot g_slots[G6_UI_RETAINED_MAX_FRAMES];

Key key_of(const Cmd& c) { return Key{ c.op, c.occ, c.s }; }

// ------------------------------------------------------------------ program parsing (script thread)
struct Reader {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t at = 0;
    bool ok = true;

    template <class T> T get() {
        T v{};
        if (!ok || n - at < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p + at, sizeof v);
        at += sizeof v;
        return v;
    }
    float flt() {
        const float v = get<float>();
        if (!std::isfinite(v)) ok = false;
        return v;
    }
    std::string str() {
        bool present = false;
        std::string s = opt_str(present);
        if (!present) ok = false;
        return s;
    }
    std::string opt_str(bool& present) {
        const std::uint32_t len = get<std::uint32_t>();
        present = ok && len != kNoString;
        if (!present) return {};
        if (len > kMaxString || n - at < len) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        if (s.find('\0') != std::string::npos) ok = false;  // C strings end at the first NUL
        return s;
    }
};

bool parse(const void* data, std::size_t size, Program& out) {
    if (!data || size < 12 || size > G6_UI_RETAINED_MAX_PROGRAM) return false;
    Reader r{ static_cast<const std::uint8_t*>(data), size };
    if (r.get<std::uint32_t>() != kProgramMagic || r.get<std::uint16_t>() != G6_UI_RETAINED_VERSION) return false;
    r.get<std::uint16_t>();
    const std::uint32_t count = r.get<std::uint32_t>();
    if (!r.ok || count > kMaxCommands) return false;
    out.cmds.resize(count);
    std::vector<std::pair<char, std::uint32_t>> stack;
    std::map<std::pair<std::uint8_t, std::string>, std::uint32_t> seen;
    for (std::uint32_t idx = 0; idx < count && r.ok; ++idx) {
        Cmd& c = out.cmds[idx];
        c.op = r.get<std::uint8_t>();
        auto close = [&](char kind) {
            if (stack.empty() || stack.back().first != kind) { r.ok = false; return; }
            out.cmds[stack.back().second].end = idx;
            stack.pop_back();
        };
        switch (c.op) {
        case kBegin:
            c.s = r.str();
            c.closable = static_cast<std::uint8_t>(r.get<std::uint8_t>() ? 1 : 0);
            c.u = r.get<std::uint32_t>();
            stack.emplace_back('W', idx);
            break;
        case kEnd: close('W'); break;
        case kNextPos:
        case kNextSize:
            c.f[0] = r.flt();
            c.f[1] = r.flt();
            c.u = r.get<std::uint32_t>();
            if (c.u > G6_UI_COND_FIRST_USE) r.ok = false;
            break;
        case kText: c.s = r.str(); break;
        case kTextColored:
            for (int k = 0; k < 4; ++k) c.f[k] = r.flt();
            c.s = r.str();
            break;
        case kButton: c.s = r.str(); break;
        case kCheckbox:
            c.s = r.str();
            c.i[0] = r.get<std::int32_t>() ? 1 : 0;
            break;
        case kSliderInt:
            c.s = r.str();
            for (int k = 0; k < 3; ++k) c.i[k] = r.get<std::int32_t>();
            break;
        case kSliderFloat: {
            c.s = r.str();
            for (int k = 0; k < 3; ++k) c.f[k] = r.flt();
            bool present = false;
            c.opt = r.opt_str(present);
            c.has_opt = static_cast<std::uint8_t>(present ? 1 : 0);
            break;
        }
        case kInputText:
            c.s = r.str();
            c.opt = r.str();
            c.u = r.get<std::uint32_t>();
            if (c.u < 2 || c.u > kMaxInputCap) r.ok = false;
            break;
        case kCombo: {
            c.s = r.str();
            c.i[0] = r.get<std::int32_t>();
            const std::uint32_t n = r.get<std::uint32_t>();
            if (n > kMaxItems) { r.ok = false; break; }
            c.items.reserve(n);
            for (std::uint32_t k = 0; k < n && r.ok; ++k) c.items.push_back(r.str());
            break;
        }
        case kSeparator:
        case kSameLine: break;
        case kTreeNode:
        case kTabBar:
        case kTabItem:
            c.s = r.str();
            c.body = static_cast<std::uint8_t>(r.get<std::uint8_t>() ? 1 : 0);
            if (c.op == kTabItem && (stack.empty() || stack.back().first != 'B')) r.ok = false;  // only inside a tab bar
            if (c.body) stack.emplace_back(c.op == kTreeNode ? 'T' : c.op == kTabBar ? 'B' : 'I', idx);
            break;
        case kTreePop: close('T'); break;
        case kEndTabBar: close('B'); break;
        case kEndTabItem: close('I'); break;
        case kColorEdit:
            c.s = r.str();
            for (int k = 0; k < 4; ++k) c.f[k] = r.flt();
            c.u = r.get<std::uint32_t>();
            break;
        case kPlotLines: {
            c.s = r.str();
            const std::uint32_t n = r.get<std::uint32_t>();
            if (n > kMaxValues) { r.ok = false; break; }
            c.values.resize(n);
            for (std::uint32_t k = 0; k < n && r.ok; ++k) c.values[k] = r.flt();
            bool present = false;
            c.opt = r.opt_str(present);
            c.has_opt = static_cast<std::uint8_t>(present ? 1 : 0);
            for (int k = 0; k < 4; ++k) c.f[k] = r.flt();
            break;
        }
        default: r.ok = false; break;
        }
        if (stack.size() > kMaxDepth) r.ok = false;
        if (r.ok) c.occ = seen[{ c.op, c.s }]++;
    }
    return r.ok && r.at == size && stack.empty();
}

// ------------------------------------------------------------------ replay (render thread)
struct Replay {
    const g6_ui_api* ui;
    const Program& p;
    const std::map<Key, Value>& overrides;
    std::vector<std::pair<Key, std::uint8_t>> states;
    std::vector<std::pair<Key, Value>> values;
    std::vector<Key> clicks, closed;

    const Value* override_of(const Cmd& c) const {
        const auto it = overrides.find(key_of(c));
        return it == overrides.end() ? nullptr : &it->second;
    }
    Value& changed(const Cmd& c) {
        values.emplace_back(key_of(c), Value{});
        return values.back().second;
    }

    void run(std::size_t i, std::size_t stop) {
        while (i < stop) {
            const Cmd& c = p.cmds[i];
            switch (c.op) {
            case kBegin: {
                std::int32_t open = 1;
                const std::int32_t drawn = ui->begin(c.s.c_str(), c.closable ? &open : nullptr, c.u);
                states.emplace_back(key_of(c), static_cast<std::uint8_t>(drawn ? 1 : 0));
                if (c.closable && open == 0) closed.push_back(key_of(c));
                if (drawn) run(i + 1, c.end);
                ui->end();
                i = c.end + 1;
                continue;
            }
            case kTreeNode:
            case kTabBar:
            case kTabItem: {
                const std::int32_t open = c.op == kTreeNode ? ui->tree_node(c.s.c_str())
                                        : c.op == kTabBar  ? ui->begin_tab_bar(c.s.c_str())
                                                           : ui->begin_tab_item(c.s.c_str());
                states.emplace_back(key_of(c), static_cast<std::uint8_t>(open ? 1 : 0));
                // The host drew the contents only when the previous frame said open; a node that opened this frame
                // shows its contents from the next program on, one that closed skips them. Either way the closer
                // ImGui needs is called exactly when the opener returned 1.
                if (open && c.body) run(i + 1, c.end);
                if (open) {
                    if (c.op == kTreeNode) ui->tree_pop();
                    else if (c.op == kTabBar) ui->end_tab_bar();
                    else ui->end_tab_item();
                }
                i = c.body ? c.end + 1 : i + 1;
                continue;
            }
            case kNextPos: ui->set_next_window_pos(c.f[0], c.f[1], c.u); break;
            case kNextSize: ui->set_next_window_size(c.f[0], c.f[1], c.u); break;
            case kText: ui->text(c.s.c_str()); break;
            case kTextColored: ui->text_colored(c.f[0], c.f[1], c.f[2], c.f[3], c.s.c_str()); break;
            case kButton:
                if (ui->button(c.s.c_str())) clicks.push_back(key_of(c));
                break;
            case kCheckbox: {
                std::int32_t v = c.i[0];
                if (const Value* o = override_of(c)) v = o->i;
                if (ui->checkbox(c.s.c_str(), &v)) changed(c).i = v ? 1 : 0;
                break;
            }
            case kSliderInt: {
                std::int32_t v = c.i[0];
                if (const Value* o = override_of(c)) v = o->i;
                if (ui->slider_int(c.s.c_str(), &v, c.i[1], c.i[2])) changed(c).i = v;
                break;
            }
            case kSliderFloat: {
                float v = c.f[0];
                if (const Value* o = override_of(c)) v = o->f[0];
                if (ui->slider_float(c.s.c_str(), &v, c.f[1], c.f[2], c.has_opt ? c.opt.c_str() : nullptr))
                    changed(c).f[0] = v;
                break;
            }
            case kInputText: {
                const Value* o = override_of(c);
                const std::string& text = o ? o->s : c.opt;
                std::vector<char> buf(c.u, '\0');
                std::memcpy(buf.data(), text.data(), text.size() < c.u - 1 ? text.size() : c.u - 1);
                if (ui->input_text(c.s.c_str(), buf.data(), buf.size())) {
                    buf.back() = '\0';
                    changed(c).s = buf.data();
                }
                break;
            }
            case kCombo: {
                std::vector<const char*> items;
                items.reserve(c.items.size());
                for (const std::string& it : c.items) items.push_back(it.c_str());
                std::int32_t cur = c.i[0];
                if (const Value* o = override_of(c)) cur = o->i;
                if (ui->combo(c.s.c_str(), &cur, items.empty() ? nullptr : items.data(), static_cast<std::int32_t>(items.size())))
                    changed(c).i = cur;
                break;
            }
            case kSeparator: ui->separator(); break;
            case kSameLine: ui->same_line(); break;
            case kColorEdit: {
                float rgba[4] = { c.f[0], c.f[1], c.f[2], c.f[3] };
                if (const Value* o = override_of(c)) std::memcpy(rgba, o->f, sizeof rgba);
                if (ui->color_edit(c.s.c_str(), rgba, c.u)) std::memcpy(changed(c).f, rgba, sizeof rgba);
                break;
            }
            case kPlotLines:
                ui->plot_lines(c.s.c_str(), c.values.empty() ? nullptr : c.values.data(), static_cast<std::int32_t>(c.values.size()),
                               c.has_opt ? c.opt.c_str() : nullptr, c.f[0], c.f[1], c.f[2], c.f[3]);
                break;
            default: break;  // closers are reached only through their opener
            }
            ++i;
        }
    }
};

void G6_CALL draw(const g6_ui_frame* frame, void* user) {
    Slot* s = static_cast<Slot*>(user);
    std::shared_ptr<const Program> program;
    std::map<Key, Value> overrides;
    const g6_ui_api* ui = nullptr;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        if (!s->used || s->closing) return;
        program = s->program;
        overrides = s->values;
        ui = s->ui;
    }
    static const Program kEmpty;
    Replay r{ ui, program ? *program : kEmpty, overrides, {}, {}, {}, {} };
    if (program && ui) r.run(0, program->cmds.size());
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->used || s->closing) return;
    if (s->states.size() + r.states.size() > kMaxStates) s->states.clear();
    for (auto& [k, v] : r.states) s->states[k] = v;
    for (auto& [k, v] : r.values) s->values[k] = std::move(v);   // delivered = false: the host has not seen it
    for (const Key& k : r.clicks) ++s->events[k].clicks;
    for (const Key& k : r.closed) s->events[k].closed = true;
    ++s->frames;
    s->have_frame = true;
    if (frame && frame->size >= sizeof(g6_ui_frame)) s->last = *frame;
}

// ------------------------------------------------------------------ poll results (script thread)
struct Writer {
    std::string b;
    template <class T> void put(T v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void str(const std::string& s) {
        put(static_cast<std::uint32_t>(s.size()));
        b += s;
    }
    void key(const Key& k) {
        put(k.op);
        put(k.occ);
        str(k.label);
    }
};

std::string results(const Slot& s) {
    Writer w;
    w.put(kResultMagic);
    w.put(static_cast<std::uint16_t>(G6_UI_RETAINED_VERSION));
    w.put(static_cast<std::uint16_t>(0));
    w.put(s.frames);
    w.put(static_cast<std::uint32_t>(s.have_frame ? 1 : 0));
    w.put(s.last.menu_open);
    w.put(s.last.display_w);
    w.put(s.last.display_h);
    w.put(s.last.delta_s);
    w.put(s.last.frame);
    w.put(static_cast<std::uint32_t>(s.states.size()));
    for (const auto& [k, v] : s.states) {
        w.key(k);
        w.put(v);
    }
    std::uint32_t fresh = 0;
    for (const auto& kv : s.values) fresh += kv.second.delivered ? 0u : 1u;
    w.put(fresh);
    for (const auto& [k, v] : s.values) {
        if (v.delivered) continue;
        w.key(k);
        switch (k.op) {
        case kSliderFloat: w.put(v.f[0]); break;
        case kInputText: w.str(v.s); break;
        case kColorEdit: for (float f : v.f) w.put(f); break;
        default: w.put(v.i); break;   // checkbox, slider_int, combo
        }
    }
    w.put(static_cast<std::uint32_t>(s.events.size()));
    for (const auto& [k, e] : s.events) {
        w.key(k);
        w.put(e.clicks);
        w.put(static_cast<std::uint8_t>(e.closed ? 1 : 0));
    }
    return std::move(w.b);
}

Slot* find(std::uint32_t handle) {
    const std::uint32_t idx = handle & 0xFFu;
    if (idx >= G6_UI_RETAINED_MAX_FRAMES) return nullptr;
    return &g_slots[idx];
}
bool live(const Slot& s, std::uint32_t handle) { return s.used && !s.closing && (s.gen & 0xFFFFFFu) == (handle >> 8); }

} // namespace

void bind(const g6_host_api* host) noexcept { g_host.store(host); }

unsigned live_frames() noexcept {
    unsigned n = 0;
    for (Slot& s : g_slots) {
        std::lock_guard<std::mutex> lk(s.mu);
        n += s.used ? 1u : 0u;
    }
    return n;
}

} // namespace g6uiret

using namespace g6uiret;

extern "C" {

G6_UI_RETAINED_EXPORT uint32_t G6_CALL g6_ui_retained_version(void) { return G6_UI_RETAINED_VERSION; }

G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_create(const char* name, uint32_t flags, uint32_t* out_handle) {
    if (!name || !out_handle || (flags & ~G6_UI_FRAME_ALWAYS) != 0) return G6_E_INVALID_ARG;
    *out_handle = 0;
    const g6_host_api* host = g_host.load();
    const g6_ui_api* ui = host ? g6_ui_get(host) : nullptr;
    if (!ui) return G6_E_NOT_IMPLEMENTED;
    std::lock_guard<std::mutex> alloc(g_alloc);
    Slot* s = nullptr;
    std::uint32_t idx = 0;
    for (; idx < G6_UI_RETAINED_MAX_FRAMES; ++idx) {
        std::lock_guard<std::mutex> lk(g_slots[idx].mu);
        if (!g_slots[idx].used) {
            s = &g_slots[idx];
            s->reset();
            s->used = true;
            s->closing = true;   // no frame does anything until the registration is complete
            s->ui = ui;
            break;
        }
    }
    if (!s) return G6_E_INTERNAL;
    const g6_ui_frame_desc d{ sizeof(g6_ui_frame_desc), flags, name, draw, s };
    g6_ui_id id = 0;
    const g6_status st = ui->frame_register(&d, &id);
    std::lock_guard<std::mutex> lk(s->mu);
    if (st != G6_OK) {
        s->reset();
        ++s->gen;
        return st;
    }
    s->ui_id = id;
    s->closing = false;
    *out_handle = ((s->gen & 0xFFFFFFu) << 8) | idx;
    return G6_OK;
}

G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_destroy(uint32_t handle) {
    Slot* s = find(handle);
    if (!s) return G6_E_INVALID_ARG;
    g6_ui_id id = 0;
    const g6_ui_api* ui = nullptr;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        if (!live(*s, handle)) return G6_E_INVALID_ARG;
        s->closing = true;
        id = s->ui_id;
        ui = s->ui;
    }
    // Waits for a frame in progress. G6_E_INVALID_ARG when the runtime already dropped the frame with its plugin.
    if (ui) ui->frame_unregister(id);
    std::lock_guard<std::mutex> lk(s->mu);
    s->reset();
    ++s->gen;
    return G6_OK;
}

G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_submit(uint32_t handle, const void* program, size_t size) {
    Slot* s = find(handle);
    if (!s) return G6_E_INVALID_ARG;
    auto p = std::make_shared<Program>();
    if (!parse(program, size, *p)) return G6_E_INVALID_ARG;
    std::shared_ptr<const Program> old;
    std::lock_guard<std::mutex> lk(s->mu);
    if (!live(*s, handle)) return G6_E_INVALID_ARG;
    old = std::move(s->program);   // freed after the lock is released (a frame may still hold its own reference)
    s->program = std::move(p);
    for (auto it = s->values.begin(); it != s->values.end();) {
        if (it->second.delivered) it = s->values.erase(it);
        else ++it;
    }
    return G6_OK;
}

G6_UI_RETAINED_EXPORT g6_status G6_CALL g6_ui_retained_poll(uint32_t handle, void* out, size_t cap, size_t* out_size) {
    Slot* s = find(handle);
    if (!s || !out_size) return G6_E_INVALID_ARG;
    std::lock_guard<std::mutex> lk(s->mu);
    if (!live(*s, handle)) return G6_E_INVALID_ARG;
    const std::string r = results(*s);
    *out_size = r.size();
    if (!out || cap < r.size()) return G6_E_INVALID_ARG;
    std::memcpy(out, r.data(), r.size());
    for (auto& kv : s->values) kv.second.delivered = true;
    s->events.clear();
    s->frames = 0;
    return G6_OK;
}

} // extern "C"
