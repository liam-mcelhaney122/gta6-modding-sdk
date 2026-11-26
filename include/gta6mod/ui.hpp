// gta6mod/ui.hpp: C++ helpers over the g6_ui_api C ABI (gta6mod.h, "UI layer"). Header-only, no ImGui needed.
//
//   g6::ui::Ui ui(host_api);                          // null table -> every call is a no-op
//   g6::ui::Frame menu(ui, "trainer", [&](const g6_ui_frame& f) {
//       if (auto w = ui.window("Trainer")) {          // end() is called by the guard
//           ui.checkbox("God mode", god);             // bool& in/out, returns true when changed
//           if (ui.button("Heal")) heal_requested = true;
//       }
//   });                                               // unregistered by ~Frame
//
// Frame callbacks run on the render thread (natives are refused there): share state with script ticks
// through std::atomic or a mutex. See gta6mod/ui_imgui.hpp to draw with ImGui directly.
#pragma once
#include "gta6mod/gta6mod.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

namespace g6::ui {

class Ui {
public:
    Ui() = default;
    explicit Ui(const g6_host_api* host) : api_(g6_ui_get(host)) {}
    explicit Ui(const g6_ui_api* api) : api_(api) {}
    const g6_ui_api* api() const noexcept { return api_; }
    explicit operator bool() const noexcept { return api_ != nullptr; }

    // Scoped begin/end pairs: the destructor calls the matching end only when ImGui requires it.
    class Scope {
    public:
        Scope(void (G6_CALL* end)(void), bool open, bool always_end) noexcept : end_(end), open_(open), call_(always_end || open) {}
        Scope(Scope&& o) noexcept : end_(o.end_), open_(o.open_), call_(o.call_) { o.call_ = false; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { if (call_ && end_) end_(); }
        explicit operator bool() const noexcept { return open_; }
    private:
        void (G6_CALL* end_)(void);
        bool open_;
        bool call_;
    };

    Scope window(const char* title, bool* open = nullptr, std::uint32_t flags = 0) const {
        if (!api_) return Scope(nullptr, false, false);
        std::int32_t o = open ? (*open ? 1 : 0) : 1;
        const bool r = api_->begin(title, open ? &o : nullptr, flags) != 0;
        if (open) *open = o != 0;
        return Scope(api_->end, r, true);   // End() after every Begin()
    }
    Scope tree(const char* label) const {
        if (!api_) return Scope(nullptr, false, false);
        return Scope(api_->tree_pop, api_->tree_node(label) != 0, false);
    }
    Scope tab_bar(const char* id) const {
        if (!api_) return Scope(nullptr, false, false);
        return Scope(api_->end_tab_bar, api_->begin_tab_bar(id) != 0, false);
    }
    Scope tab(const char* label) const {
        if (!api_) return Scope(nullptr, false, false);
        return Scope(api_->end_tab_item, api_->begin_tab_item(label) != 0, false);
    }

    void next_window_pos(float x, float y, bool first_use_only = true) const {
        if (api_) api_->set_next_window_pos(x, y, first_use_only ? G6_UI_COND_FIRST_USE : G6_UI_COND_ALWAYS);
    }
    void next_window_size(float w, float h, bool first_use_only = true) const {
        if (api_) api_->set_next_window_size(w, h, first_use_only ? G6_UI_COND_FIRST_USE : G6_UI_COND_ALWAYS);
    }
    void text(const char* s) const { if (api_) api_->text(s); }
    void text(const std::string& s) const { text(s.c_str()); }
    void text_colored(float r, float g, float b, float a, const char* s) const { if (api_) api_->text_colored(r, g, b, a, s); }
    bool button(const char* label) const { return api_ && api_->button(label) != 0; }
    bool checkbox(const char* label, bool& v) const {
        if (!api_) return false;
        std::int32_t x = v ? 1 : 0;
        const bool c = api_->checkbox(label, &x) != 0;
        v = x != 0;
        return c;
    }
    bool slider(const char* label, int& v, int lo, int hi) const {
        if (!api_) return false;
        std::int32_t x = v;
        const bool c = api_->slider_int(label, &x, lo, hi) != 0;
        v = x;
        return c;
    }
    bool slider(const char* label, float& v, float lo, float hi, const char* format = nullptr) const {
        return api_ && api_->slider_float(label, &v, lo, hi, format) != 0;
    }
    template <std::size_t N> bool input_text(const char* label, char (&buf)[N]) const {
        return api_ && api_->input_text(label, buf, N) != 0;
    }
    bool combo(const char* label, int& current, const char* const* items, int count) const {
        if (!api_) return false;
        std::int32_t x = current;
        const bool c = api_->combo(label, &x, items, count) != 0;
        current = x;
        return c;
    }
    void separator() const { if (api_) api_->separator(); }
    void same_line() const { if (api_) api_->same_line(); }
    bool color_edit(const char* label, float (&rgba)[4], bool alpha = true) const {
        return api_ && api_->color_edit(label, rgba, alpha ? 0u : G6_UI_COLOR_NO_ALPHA) != 0;
    }
    void plot_lines(const char* label, const float* v, int n, const char* overlay = nullptr, float lo = 0.f, float hi = 0.f,
                    float w = 0.f, float h = 0.f) const {
        if (api_) api_->plot_lines(label, v, n, overlay, lo, hi, w, h);
    }

private:
    const g6_ui_api* api_ = nullptr;
};

// RAII frame registration. The callable runs on the render thread; it must outlive nothing but this object.
class Frame {
public:
    using Fn = std::function<void(const g6_ui_frame&)>;
    Frame() = default;
    Frame(const Ui& ui, const char* name, Fn fn, std::uint32_t flags = 0) { reset(ui, name, std::move(fn), flags); }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame() { reset(); }

    // G6_OK, or why it was not registered (G6_E_NOT_IMPLEMENTED: overlay off; G6_E_INVALID_ARG: no UI table).
    g6_status reset(const Ui& ui, const char* name, Fn fn, std::uint32_t flags = 0) {
        reset();
        if (!ui) return status_ = G6_E_INVALID_ARG;
        api_ = ui.api();
        fn_ = std::move(fn);
        const g6_ui_frame_desc d{ sizeof(g6_ui_frame_desc), flags, name, &Frame::thunk, this };
        status_ = api_->frame_register(&d, &id_);
        if (status_ != G6_OK) { id_ = 0; api_ = nullptr; }
        return status_;
    }
    void reset() {
        if (api_ && id_) api_->frame_unregister(id_);   // waits for a frame in progress
        api_ = nullptr;
        id_ = 0;
    }
    g6_status status() const noexcept { return status_; }
    bool registered() const noexcept { return id_ != 0; }

private:
    static void G6_CALL thunk(const g6_ui_frame* f, void* user) {
        auto* self = static_cast<Frame*>(user);
        if (self && self->fn_ && f) self->fn_(*f);
    }
    const g6_ui_api* api_ = nullptr;
    g6_ui_id id_ = 0;
    g6_status status_ = G6_E_INVALID_ARG;
    Fn fn_;
};

}  // namespace g6::ui
