/*
 * gta6mod_mock — offline implementation of the gta6mod.h C ABI.
 *
 * Purpose: let the Python (ctypes) and Luau hosts, and C/C++ plugins, be built
 * and tested against the real ABI before any in-game runtime exists. Natives
 * are fake handlers registered through g6_mock_register_native(); the game
 * clock advances through g6_mock_step().
 *
 * The real runtime will replace exactly this file's responsibilities:
 *   native_resolve/invoke  -> game's own native resolver + script-thread call
 *   script_register/tick   -> hooked script-thread tick, fibers for wait()
 *   anchor_address         -> address manifest (exe SHA-256 -> RVAs) + pattern fallback
 *   vfs_add_overlay        -> fiDevice / file-layer hooks (mods/ overlay)
 *   ui                     -> g6ui.dll: Dear ImGui on the host's swap chain (here: a recorder, "Mock UI")
 */
#define G6_BUILDING_RUNTIME 1
#include "gta6mod/gta6mod.h"
#include "gta6mod/script_objects.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_NATIVE_SLOTS 1024u   /* power of two */
#define MOCK_MAX_SCRIPTS  64u

typedef struct mock_native {
    uint64_t          hash;      /* 0 = empty slot */
    g6_mock_native_fn fn;
    void*             user;
} mock_native;

typedef struct mock_script {
    int               used;
    g6_script_desc    desc;
    char              name[64];
} mock_script;

static mock_native g_natives[MOCK_NATIVE_SLOTS];
static uint32_t    g_native_count;
static mock_script g_scripts[MOCK_MAX_SCRIPTS];
static uint32_t    g_clock_ms;
static int         g_in_tick;
static int         g_ui_in_frame;   /* inside g6_mock_ui_frame: natives are refused, as on the render thread */

static mock_native* find_slot(uint64_t hash, int for_insert) {
    uint32_t i = (uint32_t)(hash ^ (hash >> 32)) & (MOCK_NATIVE_SLOTS - 1u);
    for (uint32_t n = 0; n < MOCK_NATIVE_SLOTS; ++n, i = (i + 1u) & (MOCK_NATIVE_SLOTS - 1u)) {
        if (g_natives[i].hash == hash) return &g_natives[i];
        if (g_natives[i].hash == 0) return for_insert ? &g_natives[i] : NULL;
    }
    return NULL;
}

/* --------------------------------------------------------------- host API */
static g6_status G6_CALL host_get_build_info(g6_build_info* out) {
    if (!out || out->size < sizeof(g6_build_info)) return G6_E_INVALID_ARG;
    memset(out->exe_sha256, 0, sizeof out->exe_sha256);
    out->game_version     = "1.0.2.1";
    out->platform         = "mock";
    out->natives_known    = g_native_count;
    out->natives_verified = 0;
    return G6_OK;
}

static void G6_CALL host_log(g6_log_level level, const char* plugin, const char* message) {
    static const char* names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    fprintf(stderr, "[%s] %s: %s\n", (unsigned)level < 4u ? names[level] : "?", plugin ? plugin : "?", message ? message : "");
}

static g6_status G6_CALL host_native_resolve(uint64_t hash, g6_native_handle* out) {
    if (!out) return G6_E_INVALID_ARG;
    mock_native* n = find_slot(hash, 0);
    if (!n) { *out = NULL; return G6_E_UNKNOWN_NATIVE; }
    *out = (g6_native_handle)n;
    return G6_OK;
}

/* Script objects (g6_native_call.string_slots / text_slots, G-120): the same layouts as the runtime builds
   (gta6mod/script_objects.h) over two mock records, so a mock handler reads them as the game's handlers do.
   Each object lives until the call returns (the runtime keeps one per distinct text; the mock has no natives
   that keep the pointer). The two exported records are what a mock handler finds at +0x00 / +0x20. */
G6_EXPORT const uint64_t g6_mock_string_type_record[0x88 / 8] = { 0x5354524953545950ull };
G6_EXPORT const uint64_t g6_mock_string_elem_record[0x88 / 8] = { 0x4D454C4553545249ull };
static g6_script_text_vtable g_text_vtable;

static g6_status convert_objects(g6_native_call* call, void** temps) {
    if (call->size < sizeof(g6_native_call)) return G6_OK;  /* API 0.3 caller: no masks */
    uint32_t sm = call->string_slots, tm = call->text_slots;
    uint32_t valid = call->argc >= 32u ? 0xFFFFFFFFu : ((1u << call->argc) - 1u);
    if ((sm & tm) || ((sm | tm) & ~valid)) return G6_E_INVALID_ARG;
    if (!g_text_vtable.get) g6_script_text_vtable_init(&g_text_vtable);
    for (uint32_t i = 0; i < call->argc; ++i) {
        uint32_t bit = 1u << i;
        const char* s = (const char*)(uintptr_t)call->args[i];
        if (!((sm | tm) & bit) || !s) continue;
        size_t len = strlen(s);
        if (len > G6_SSTR_MAX_LENGTH) return G6_E_INVALID_ARG;
        size_t n = (sm & bit) ? g6_script_string_bytes(len) : g6_script_text_bytes(len);
        void* mem = calloc(1, n);
        if (!mem) return G6_E_INTERNAL;
        temps[i] = mem;
        if (sm & bit) g6_script_string_init(mem, s, len, g6_mock_string_type_record, g6_mock_string_elem_record);
        else g6_script_text_init(mem, s, len, &g_text_vtable);
        call->args[i] = (uint64_t)(uintptr_t)mem;
    }
    return G6_OK;
}

static g6_status G6_CALL host_native_invoke(g6_native_handle h, g6_native_call* call) {
    if (!h || !call || call->size < G6_NATIVE_CALL_V3_SIZE) return G6_E_INVALID_ARG;
    if (call->argc > G6_MAX_NATIVE_ARGS) return G6_E_TOO_MANY_ARGS;
    if (g_ui_in_frame) return G6_E_NOT_ON_SCRIPT_THREAD;  /* frame callbacks run on the render thread in-game */
    mock_native* n = (mock_native*)h;
    void* temps[G6_MAX_NATIVE_ARGS] = { 0 };
    uint64_t saved[G6_MAX_NATIVE_ARGS];
    memcpy(saved, call->args, sizeof saved);
    g6_status st = convert_objects(call, temps);
    if (st == G6_OK) {
        memset(call->ret, 0, sizeof call->ret);
        n->fn(call, n->user);
    }
    for (uint32_t i = 0; i < G6_MAX_NATIVE_ARGS; ++i) {
        if (temps[i]) { call->args[i] = saved[i]; free(temps[i]); }
    }
    return st;
}

static g6_status G6_CALL host_native_invoke_hash(uint64_t hash, g6_native_call* call) {
    g6_native_handle h;
    g6_status s = host_native_resolve(hash, &h);
    return s == G6_OK ? host_native_invoke(h, call) : s;
}

static g6_status G6_CALL host_script_register(const g6_script_desc* desc, g6_script_id* out) {
    if (!desc || desc->size < sizeof(g6_script_desc) || !desc->tick || !desc->name) return G6_E_INVALID_ARG;
    for (uint32_t i = 0; i < MOCK_MAX_SCRIPTS; ++i) {
        if (!g_scripts[i].used) {
            g_scripts[i].used = 1;
            g_scripts[i].desc = *desc;
            size_t k = 0;
            for (; k + 1 < sizeof g_scripts[i].name && desc->name[k]; ++k) g_scripts[i].name[k] = desc->name[k];
            g_scripts[i].name[k] = 0;
            g_scripts[i].desc.name = g_scripts[i].name;
            if (out) *out = i + 1u;
            return G6_OK;
        }
    }
    return G6_E_INTERNAL;
}

static g6_status G6_CALL host_script_unregister(g6_script_id id) {
    if (id == 0 || id > MOCK_MAX_SCRIPTS || !g_scripts[id - 1].used) return G6_E_INVALID_ARG;
    g_scripts[id - 1].used = 0;
    return G6_OK;
}

static g6_status G6_CALL host_script_wait(uint32_t ms) {
    (void)ms;
    /* The real runtime runs each script on its own fiber and switches back to
       the game here. The mock has no fibers: language hosts (Python/Luau)
       implement wait() with their own coroutines on top of tick(). */
    return G6_E_NOT_IMPLEMENTED;
}

static g6_status G6_CALL host_anchor_address(const char* name, uintptr_t* out) {
    (void)name;
    if (out) *out = 0;
    return G6_E_NO_SUCH_ANCHOR; /* no game image in the mock */
}

static g6_status G6_CALL host_vfs_add_overlay(const char* mods_root, int32_t priority) {
    (void)mods_root; (void)priority;
    return G6_E_NOT_IMPLEMENTED;
}

static uint64_t G6_CALL host_game_time_ms(void) { return g_clock_ms; }

/* ------------------------------------------------------------ mock UI
 * A recorder for the g6_ui_api: each widget appends one line to the frame's transcript and returns
 * what a user would have produced through queued interactions (g6_mock_ui_click / g6_mock_ui_set).
 * Tree nodes start closed and stay as the last click left them; the first tab of a bar is selected
 * until another tab is clicked; windows are always open (a click on a window title with an `open`
 * pointer closes it). Unbalanced begin/end, tree, tab bar or tab item calls are counted as errors. */
#define MOCK_UI_MAX_FRAMES   64u
#define MOCK_UI_MAX_ACTIONS  64u
#define MOCK_UI_MAX_STATE    64u
#define MOCK_UI_LABEL        128u
#define MOCK_UI_TRANSCRIPT   65536u

typedef struct mock_ui_entry {
    int              used;
    g6_ui_id         id;
    g6_ui_frame_desc desc;
    char             name[64];
} mock_ui_entry;

typedef struct mock_ui_action {
    int  set;                      /* 0 = click, 1 = set */
    char label[MOCK_UI_LABEL];
    char value[256];
} mock_ui_action;

typedef struct mock_ui_kv { int used; char key[MOCK_UI_LABEL]; char value[MOCK_UI_LABEL]; } mock_ui_kv;

static mock_ui_entry  g_ui[MOCK_UI_MAX_FRAMES];
static g6_ui_id       g_ui_next_id = 1;
static mock_ui_action g_ui_q[MOCK_UI_MAX_ACTIONS];
static uint32_t       g_ui_nq;
static char           g_ui_tr[MOCK_UI_TRANSCRIPT];
static size_t         g_ui_trlen;
static int            g_ui_menu_open = 1;
static int32_t        g_ui_errors;
static uint64_t       g_ui_frame_no;
static int            g_ui_depth;                     /* transcript indent */
static int            g_ui_windows, g_ui_trees, g_ui_bars, g_ui_tabs;
static mock_ui_kv     g_ui_open_trees[MOCK_UI_MAX_STATE];   /* key = label, value unused */
static mock_ui_kv     g_ui_selected[MOCK_UI_MAX_STATE];     /* key = tab bar id, value = selected tab label */
static char           g_ui_bar[MOCK_UI_LABEL];              /* innermost tab bar id */

static void ui_line(const char* fmt, ...) {
    if (g_ui_trlen + 1 >= sizeof g_ui_tr) return;
    for (int i = 0; i < g_ui_depth && g_ui_trlen + 3 < sizeof g_ui_tr; ++i) { g_ui_tr[g_ui_trlen++] = ' '; g_ui_tr[g_ui_trlen++] = ' '; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_ui_tr + g_ui_trlen, sizeof g_ui_tr - g_ui_trlen, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    g_ui_trlen += (size_t)n < sizeof g_ui_tr - g_ui_trlen ? (size_t)n : sizeof g_ui_tr - g_ui_trlen - 1;
    if (g_ui_trlen + 1 < sizeof g_ui_tr) { g_ui_tr[g_ui_trlen++] = '\n'; g_ui_tr[g_ui_trlen] = 0; }
}

static void ui_copy(char* dst, size_t cap, const char* src) {
    size_t k = 0;
    if (!cap) return;
    for (; src && src[k] && k + 1 < cap; ++k) dst[k] = src[k];
    dst[k] = 0;
}

/* Removes and returns the first queued interaction for `label` (0 = none). */
static int ui_take(const char* label, mock_ui_action* out) {
    if (!label) return 0;
    for (uint32_t i = 0; i < g_ui_nq; ++i) {
        if (strcmp(g_ui_q[i].label, label) != 0) continue;
        *out = g_ui_q[i];
        memmove(&g_ui_q[i], &g_ui_q[i + 1], (g_ui_nq - i - 1) * sizeof g_ui_q[0]);
        --g_ui_nq;
        return 1;
    }
    return 0;
}

static mock_ui_kv* ui_kv(mock_ui_kv* tab, const char* key, int create) {
    for (uint32_t i = 0; i < MOCK_UI_MAX_STATE; ++i)
        if (tab[i].used && strcmp(tab[i].key, key) == 0) return &tab[i];
    if (!create) return NULL;
    for (uint32_t i = 0; i < MOCK_UI_MAX_STATE; ++i)
        if (!tab[i].used) { tab[i].used = 1; ui_copy(tab[i].key, sizeof tab[i].key, key); tab[i].value[0] = 0; return &tab[i]; }
    return NULL;
}

static const char* ui_s(const char* s) { return s ? s : "(null)"; }

static int32_t G6_CALL ui_begin(const char* title, int32_t* open, uint32_t flags) {
    (void)flags;
    if (!g_ui_in_frame) return 0;
    mock_ui_action a;
    if (open && ui_take(title, &a) && !a.set) *open = 0;
    ui_line("begin %s", ui_s(title));
    ++g_ui_windows;
    ++g_ui_depth;
    return 1;
}
static void G6_CALL ui_end(void) {
    if (!g_ui_in_frame) return;
    if (g_ui_windows <= 0) { ui_line("error: end without begin"); ++g_ui_errors; return; }
    --g_ui_windows;
    if (g_ui_depth > 0) --g_ui_depth;
    ui_line("end");
}
static void G6_CALL ui_next_pos(float x, float y, uint32_t cond) {
    if (g_ui_in_frame) ui_line("next_window_pos %.0f %.0f%s", (double)x, (double)y, cond == G6_UI_COND_FIRST_USE ? " (first use)" : "");
}
static void G6_CALL ui_next_size(float w, float h, uint32_t cond) {
    if (g_ui_in_frame) ui_line("next_window_size %.0f %.0f%s", (double)w, (double)h, cond == G6_UI_COND_FIRST_USE ? " (first use)" : "");
}
static void G6_CALL ui_text(const char* text) { if (g_ui_in_frame) ui_line("text %s", ui_s(text)); }
static void G6_CALL ui_text_colored(float r, float g, float b, float a, const char* text) {
    (void)r; (void)g; (void)b; (void)a;
    if (g_ui_in_frame) ui_line("text %s", ui_s(text));
}
static int32_t G6_CALL ui_button(const char* label) {
    if (!g_ui_in_frame) return 0;
    mock_ui_action a;
    const int clicked = ui_take(label, &a) && !a.set;
    ui_line("button %s%s", ui_s(label), clicked ? " (clicked)" : "");
    return clicked;
}
static int32_t G6_CALL ui_checkbox(const char* label, int32_t* value) {
    if (!g_ui_in_frame || !value) return 0;
    mock_ui_action a;
    int changed = 0;
    if (ui_take(label, &a)) {
        const int32_t v = a.set ? (atoi(a.value) != 0) : !*value;
        changed = v != *value;
        *value = v;
    }
    ui_line("checkbox %s = %d", ui_s(label), *value ? 1 : 0);
    return changed;
}
static int32_t G6_CALL ui_slider_int(const char* label, int32_t* value, int32_t min, int32_t max) {
    if (!g_ui_in_frame || !value) return 0;
    mock_ui_action a;
    int changed = 0;
    if (ui_take(label, &a) && a.set) {
        long v = strtol(a.value, NULL, 0);
        if (v < min) v = min;
        if (v > max) v = max;
        changed = (int32_t)v != *value;
        *value = (int32_t)v;
    }
    ui_line("slider_int %s = %d", ui_s(label), *value);
    return changed;
}
static int32_t G6_CALL ui_slider_float(const char* label, float* value, float min, float max, const char* format) {
    (void)format;
    if (!g_ui_in_frame || !value) return 0;
    mock_ui_action a;
    int changed = 0;
    if (ui_take(label, &a) && a.set) {
        float v = (float)strtod(a.value, NULL);
        if (v < min) v = min;
        if (v > max) v = max;
        changed = v != *value;
        *value = v;
    }
    ui_line("slider_float %s = %.3f", ui_s(label), (double)*value);
    return changed;
}
static int32_t G6_CALL ui_input_text(const char* label, char* buf, size_t cap) {
    if (!g_ui_in_frame || !buf || cap == 0) return 0;
    mock_ui_action a;
    int changed = 0;
    buf[cap - 1] = 0;
    if (ui_take(label, &a) && a.set) {
        changed = strncmp(buf, a.value, cap - 1) != 0;
        ui_copy(buf, cap, a.value);
    }
    ui_line("input_text %s = %s", ui_s(label), buf);
    return changed;
}
static int32_t G6_CALL ui_combo(const char* label, int32_t* current, const char* const* items, int32_t count) {
    if (!g_ui_in_frame || !current || (!items && count > 0) || count < 0) return 0;
    mock_ui_action a;
    int changed = 0;
    if (ui_take(label, &a) && a.set) {
        char* end = NULL;
        long v = strtol(a.value, &end, 0);
        if (!end || *end || end == a.value) {  /* not a number: an item's text */
            v = -1;
            for (int32_t i = 0; i < count; ++i) if (items[i] && strcmp(items[i], a.value) == 0) { v = i; break; }
        }
        if (v >= 0 && v < count) { changed = (int32_t)v != *current; *current = (int32_t)v; }
    }
    ui_line("combo %s = %d (%s)", ui_s(label), *current,
            *current >= 0 && *current < count && items[*current] ? items[*current] : "");
    return changed;
}
static void G6_CALL ui_separator(void) { if (g_ui_in_frame) ui_line("separator"); }
static void G6_CALL ui_same_line(void) { if (g_ui_in_frame) ui_line("same_line"); }
static int32_t G6_CALL ui_tree_node(const char* label) {
    if (!g_ui_in_frame || !label) return 0;
    mock_ui_action a;
    mock_ui_kv* kv = ui_kv(g_ui_open_trees, label, 0);
    if (ui_take(label, &a)) {
        const int want = a.set ? atoi(a.value) != 0 : kv == NULL;   /* click flips, set "1"/"0" */
        if (!want && kv) { kv->used = 0; kv = NULL; }
        else if (want && !kv) kv = ui_kv(g_ui_open_trees, label, 1);
    }
    const int open = kv != NULL;
    ui_line("tree_node %s%s", label, open ? " (open)" : "");
    if (open) { ++g_ui_trees; ++g_ui_depth; }
    return open;
}
static void G6_CALL ui_tree_pop(void) {
    if (!g_ui_in_frame) return;
    if (g_ui_trees <= 0) { ui_line("error: tree_pop without an open tree_node"); ++g_ui_errors; return; }
    --g_ui_trees;
    if (g_ui_depth > 0) --g_ui_depth;
    ui_line("tree_pop");
}
static int32_t G6_CALL ui_begin_tab_bar(const char* id) {
    if (!g_ui_in_frame || !id) return 0;
    ui_line("tab_bar %s", id);
    ui_copy(g_ui_bar, sizeof g_ui_bar, id);
    ++g_ui_bars;
    ++g_ui_depth;
    return 1;
}
static void G6_CALL ui_end_tab_bar(void) {
    if (!g_ui_in_frame) return;
    if (g_ui_bars <= 0) { ui_line("error: end_tab_bar without begin_tab_bar"); ++g_ui_errors; return; }
    --g_ui_bars;
    g_ui_bar[0] = 0;
    if (g_ui_depth > 0) --g_ui_depth;
    ui_line("end_tab_bar");
}
static int32_t G6_CALL ui_begin_tab_item(const char* label) {
    if (!g_ui_in_frame || !label) return 0;
    if (g_ui_bars <= 0) { ui_line("error: tab_item outside a tab bar"); ++g_ui_errors; return 0; }
    mock_ui_kv* sel = ui_kv(g_ui_selected, g_ui_bar, 1);
    mock_ui_action a;
    if (sel && ui_take(label, &a) && !a.set) ui_copy(sel->value, sizeof sel->value, label);
    if (sel && !sel->value[0]) ui_copy(sel->value, sizeof sel->value, label);   /* first tab of the bar */
    const int selected = sel && strcmp(sel->value, label) == 0;
    ui_line("tab_item %s%s", label, selected ? " (selected)" : "");
    if (selected) { ++g_ui_tabs; ++g_ui_depth; }
    return selected;
}
static void G6_CALL ui_end_tab_item(void) {
    if (!g_ui_in_frame) return;
    if (g_ui_tabs <= 0) { ui_line("error: end_tab_item without a selected tab_item"); ++g_ui_errors; return; }
    --g_ui_tabs;
    if (g_ui_depth > 0) --g_ui_depth;
    ui_line("end_tab_item");
}
static int32_t G6_CALL ui_color_edit(const char* label, float rgba[4], uint32_t flags) {
    if (!g_ui_in_frame || !rgba) return 0;
    mock_ui_action a;
    int changed = 0;
    if (ui_take(label, &a) && a.set) {
        float v[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        char* p = a.value;
        for (int i = 0; i < 4 && *p; ++i) {
            v[i] = (float)strtod(p, &p);
            if (*p == ',') ++p;
        }
        if (flags & G6_UI_COLOR_NO_ALPHA) v[3] = rgba[3];
        changed = memcmp(v, rgba, sizeof v) != 0;
        memcpy(rgba, v, sizeof v);
    }
    ui_line("color_edit %s = %.3f %.3f %.3f %.3f", ui_s(label), (double)rgba[0], (double)rgba[1], (double)rgba[2], (double)rgba[3]);
    return changed;
}
static void G6_CALL ui_plot_lines(const char* label, const float* values, int32_t count, const char* overlay,
                                  float scale_min, float scale_max, float width, float height) {
    (void)scale_min; (void)scale_max; (void)width; (void)height;
    if (!g_ui_in_frame) return;
    ui_line("plot_lines %s (%d values%s%s)", ui_s(label), values ? count : 0, overlay ? ", " : "", overlay ? overlay : "");
}

static g6_status G6_CALL ui_frame_register(const g6_ui_frame_desc* desc, g6_ui_id* out) {
    if (!desc || desc->size < sizeof(g6_ui_frame_desc) || !desc->draw) return G6_E_INVALID_ARG;
    for (uint32_t i = 0; i < MOCK_UI_MAX_FRAMES; ++i) {
        if (g_ui[i].used) continue;
        g_ui[i].used = 1;
        g_ui[i].id = g_ui_next_id++;
        g_ui[i].desc = *desc;
        ui_copy(g_ui[i].name, sizeof g_ui[i].name, desc->name ? desc->name : "ui");
        g_ui[i].desc.name = g_ui[i].name;
        if (out) *out = g_ui[i].id;
        return G6_OK;
    }
    return G6_E_INTERNAL;
}
static g6_status G6_CALL ui_frame_unregister(g6_ui_id id) {
    for (uint32_t i = 0; i < MOCK_UI_MAX_FRAMES; ++i)
        if (g_ui[i].used && g_ui[i].id == id) { g_ui[i].used = 0; return G6_OK; }
    return G6_E_INVALID_ARG;
}
static g6_status G6_CALL ui_imgui_info(g6_ui_imgui_info* out) {
    if (!out || out->size < sizeof(g6_ui_imgui_info)) return G6_E_INVALID_ARG;
    return G6_E_NOT_IMPLEMENTED;  /* the mock has no ImGui */
}

static const g6_ui_api g_ui_api = {
    sizeof(g6_ui_api), G6_UI_ABI_MAJOR, G6_UI_ABI_MINOR,
    ui_frame_register, ui_frame_unregister, ui_imgui_info,
    ui_begin, ui_end, ui_next_pos, ui_next_size, ui_text, ui_text_colored, ui_button, ui_checkbox,
    ui_slider_int, ui_slider_float, ui_input_text, ui_combo, ui_separator, ui_same_line,
    ui_tree_node, ui_tree_pop, ui_begin_tab_bar, ui_end_tab_bar, ui_begin_tab_item, ui_end_tab_item,
    ui_color_edit, ui_plot_lines,
};

static const g6_ui_api* G6_CALL host_ui(void) { return &g_ui_api; }

static const g6_host_api g_api = {
    sizeof(g6_host_api), G6_API_VERSION_MAJOR, G6_API_VERSION_MINOR,
    host_get_build_info, host_log,
    host_native_resolve, host_native_invoke, host_native_invoke_hash,
    host_script_register, host_script_unregister, host_script_wait,
    host_anchor_address, host_vfs_add_overlay,
    host_game_time_ms,
    host_ui,
};

G6_EXPORT int32_t G6_CALL g6_mock_ui_frame(void) {
    if (g_ui_in_frame || g_in_tick) return -1;
    g_ui_trlen = 0;
    g_ui_tr[0] = 0;
    int32_t ran = 0;
    g6_ui_frame fr;
    memset(&fr, 0, sizeof fr);
    fr.size = sizeof fr;
    fr.menu_open = g_ui_menu_open ? 1u : 0u;
    fr.display_w = 1920.f;
    fr.display_h = 1080.f;
    fr.delta_s = 1.f / 60.f;
    fr.frame = g_ui_frame_no++;
    g_ui_in_frame = 1;
    for (uint32_t i = 0; i < MOCK_UI_MAX_FRAMES; ++i) {
        if (!g_ui[i].used || (!g_ui_menu_open && !(g_ui[i].desc.flags & G6_UI_FRAME_ALWAYS))) continue;
        const g6_ui_frame_desc d = g_ui[i].desc;   /* the callback may unregister itself */
        g_ui_depth = g_ui_windows = g_ui_trees = g_ui_bars = g_ui_tabs = 0;
        ui_line("frame %s", d.name);
        ++g_ui_depth;
        d.draw(&fr, d.user);
        ++ran;
        if (g_ui_windows || g_ui_trees || g_ui_bars || g_ui_tabs) {
            char msg[200];
            snprintf(msg, sizeof msg, "ui frame '%s' left %d window(s), %d tree node(s), %d tab bar(s), %d tab item(s) open",
                     d.name, g_ui_windows, g_ui_trees, g_ui_bars, g_ui_tabs);
            g_ui_depth = 1;
            ui_line("error: %s", msg);
            ++g_ui_errors;
            host_log(G6_LOG_ERROR, "mock-ui", msg);
        }
    }
    g_ui_depth = 0;
    g_ui_in_frame = 0;
    return ran;
}

G6_EXPORT const char* G6_CALL g6_mock_ui_transcript(void) { return g_ui_tr; }

static g6_status ui_queue(int set, const char* label, const char* value) {
    if (!label || !*label || strlen(label) >= MOCK_UI_LABEL || (value && strlen(value) >= 256)) return G6_E_INVALID_ARG;
    if (g_ui_nq >= MOCK_UI_MAX_ACTIONS) return G6_E_INTERNAL;
    mock_ui_action* a = &g_ui_q[g_ui_nq++];
    a->set = set;
    ui_copy(a->label, sizeof a->label, label);
    ui_copy(a->value, sizeof a->value, value ? value : "");
    return G6_OK;
}
G6_EXPORT g6_status G6_CALL g6_mock_ui_click(const char* label) { return ui_queue(0, label, NULL); }
G6_EXPORT g6_status G6_CALL g6_mock_ui_set(const char* label, const char* value) {
    return value ? ui_queue(1, label, value) : G6_E_INVALID_ARG;
}
G6_EXPORT void G6_CALL g6_mock_ui_set_menu_open(int32_t open) { g_ui_menu_open = open != 0; }
G6_EXPORT int32_t G6_CALL g6_mock_ui_pending(void) { return (int32_t)g_ui_nq; }
G6_EXPORT int32_t G6_CALL g6_mock_ui_errors(void) { return g_ui_errors; }
G6_EXPORT int32_t G6_CALL g6_mock_ui_registered(void) {
    int32_t n = 0;
    for (uint32_t i = 0; i < MOCK_UI_MAX_FRAMES; ++i) n += g_ui[i].used ? 1 : 0;
    return n;
}

/* ---------------------------------------------------------- DLL exports */
G6_EXPORT const g6_host_api* G6_CALL g6_get_host_api(void) { return &g_api; }

G6_EXPORT const char* G6_CALL g6_status_str(g6_status s) {
    switch (s) {
    case G6_OK: return "ok";
    case G6_E_INVALID_ARG: return "invalid argument";
    case G6_E_UNKNOWN_NATIVE: return "unknown native";
    case G6_E_UNVERIFIED_NATIVE: return "unverified native";
    case G6_E_TOO_MANY_ARGS: return "too many arguments";
    case G6_E_NOT_ON_SCRIPT_THREAD: return "not on script thread";
    case G6_E_UNSUPPORTED_BUILD: return "unsupported game build";
    case G6_E_ONLINE_SESSION: return "online session (refused)";
    case G6_E_NOT_IMPLEMENTED: return "not implemented";
    case G6_E_NO_SUCH_ANCHOR: return "no such anchor";
    default: return "internal error";
    }
}

G6_EXPORT g6_status G6_CALL g6_mock_register_native(uint64_t hash, g6_mock_native_fn fn, void* user) {
    if (hash == 0 || !fn) return G6_E_INVALID_ARG;
    mock_native* n = find_slot(hash, 1);
    if (!n) return G6_E_INTERNAL;
    if (n->hash == 0) { n->hash = hash; ++g_native_count; }
    n->fn = fn;
    n->user = user;
    return G6_OK;
}

G6_EXPORT g6_status G6_CALL g6_mock_step(uint32_t elapsed_ms) {
    if (g_in_tick) return G6_E_INVALID_ARG;
    g_in_tick = 1;
    for (uint32_t i = 0; i < MOCK_MAX_SCRIPTS; ++i)
        if (g_scripts[i].used) g_scripts[i].desc.tick(g_scripts[i].desc.user);
    g_clock_ms += elapsed_ms;
    g_in_tick = 0;
    return G6_OK;
}

/* Built-in fake GET_GAME_TIMER so the DLL is useful with zero setup. */
static void G6_CALL mock_get_game_timer(g6_native_call* c, void* user) { (void)user; c->ret[0] = g_clock_ms; }

#if defined(_WIN32)
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) g6_mock_register_native(0x9CD27B0045628463ull, mock_get_game_timer, NULL);
    return TRUE;
}
#endif
