// mock_world — the fake GTA VI world behind the offline C hosts (g6_luau_runner, g6_python_test).
//
// Registers fake natives in gta6mod_mock.dll (g6_mock_register_native) with the same semantics as the
// Python mock world (sdk/python/gta6mod/runtime/mock.py; the contract is "Mock world" in
// sdk/docs/script_layer.md), so a Luau mod, its Python twin run by the Python mock, and the same Python
// mod run by the in-process Python host (g6_python.dll) all see one world:
// player ped 0x100 at (-1000, -2000, 15), spawned handles from 0x101, models load 50 ms after REQUEST_MODEL,
// CREATE_VEHICLE returns 0 until then, a pressed control is held for its +HOLD ms (one tick by default),
// flat ground at z = 12 once collision is loaded (100 ms after the first request for a cell), wanted
// level 0 / max 5, clock 12:00:00. Each native is registered under its seed (GTA V) hash and, where
// re-db has one, the observed GTA VI hash.
//
// Driving a run: install() once, then per frame fire() the events due at now_ms() (and fire_ui() the UI
// interactions due), run the scripts, g6_mock_step(16), draw_ui_frame(), end_tick(). Not thread-safe (the mock
// is single-threaded).
//
// Mock UI driver (both hosts): gta6mod_mock.dll records the g6_ui_api calls ("Mock UI" in
// sdk/docs/script_layer.md); after every tick the host draws one UI frame with draw_ui_frame(). Interactions
// (--ui-click/--ui-set/--ui-menu) are queued on the tick whose time equals their MS, before the scripts run, so
// the frame drawn after that tick consumes them. UiLog keeps every drawn frame's transcript, run-length encoded
// by mock time, for transcript parity checks between the Luau host, the in-game Python host and the Python mock
// (sdk/tests/ui/*.ui.txt).
#pragma once

#include "gta6mod/gta6mod.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace g6mock {

enum class Ev { Press, Damage, Wanted, Waypoint };
struct Event {
    std::uint64_t at;     // mock game ms; fires on the tick whose time equals it
    Ev            kind;
    std::int32_t  n;      // press: control hash; damage/wanted: amount
    float         x, y, z;
    std::uint64_t hold;   // press: ms held after `at` (0 = that tick only)
};

// Reset the world, spawn the player and register every fake native. `mock` is gta6mod_mock.dll's own table
// (its game_time_ms is the world clock). Returns false when a registration failed.
bool install(const g6_host_api* mock);

// Parse one scenario option: kind is "press" | "damage" | "wanted" | "waypoint", spec the value after the
// flag ("INPUT_CONTEXT@1008+200", "50@496", "250,-400,0@1008"). The rules are the runner's and
// gta6mod/scenario.py's. On failure `err` says why.
bool parse_event(const char* kind, const char* spec, Event& out, std::string& err);

void fire(const Event& ev);
// After g6_mock_step: clear just-pressed/just-released, release held controls whose time has come.
void end_tick();

std::uint64_t now_ms();
// Errors the world itself detected (a misaligned Vector3* out-param).
int  world_errors();
std::size_t entity_count();

struct Player {
    bool          exists;
    std::int32_t  health, max_health;
    bool          invincible, can_ragdoll;
    float         x, y, z;
    std::uint64_t vehicle;          // 0 = on foot
    std::int32_t  vehicle_health;   // when in a vehicle
};
Player player();
std::int32_t wanted_level();
std::int32_t max_wanted_level();
float        time_scale();
std::int64_t clock_seconds();       // since midnight
bool         clock_paused();

// One "world: ..." summary line (the runner's format).
void print_summary(std::FILE* out);

// ---------------------------------------------------------------- mock UI driver
enum class UiEv { Click, Set, Menu };
struct UiEvent {
    std::uint64_t at;     // mock game ms (the tick whose time equals it)
    UiEv          kind;
    std::string   label;  // click/set: widget label; menu: "0" or "1"
    std::string   value;  // set: the value
};

// Parse one UI option: kind is "ui-click" | "ui-set" | "ui-menu", spec the value after the flag
// ("God mode@496", "Wanted=4@96", "0@496"). The rules are the runner's and gta6mod/scenario.py's.
bool parse_ui_event(const char* kind, const char* spec, UiEvent& out, std::string& err);
// Queue the interaction in gta6mod_mock.dll's mock UI (g6_mock_ui_click / _set / _set_menu_open).
void fire_ui(const UiEvent& ev);

// Every drawn frame's transcript, keyed by the mock time of the tick before it. text() is the run-length encoded
// form ("@ 0-480 ms" headers, each followed by the transcript shared by those consecutive frames; a tick that drew
// no frame ends a run), the format of sdk/tests/ui/*.ui.txt.
class UiLog {
public:
    void add(std::uint64_t ms, const std::string& transcript);
    void gap() { if (!runs_.empty()) runs_.back().closed = true; }   // a tick drew no frame
    bool contains(const std::string& text) const;   // some frame's transcript contains `text`
    const std::string& last() const { return last_; }
    std::size_t frames() const { return frames_; }
    std::string text() const;
private:
    struct Run { std::uint64_t first, last; std::string transcript; bool closed; };
    std::vector<Run> runs_;
    std::string      last_;
    std::size_t      frames_ = 0;
};

// Draw one mock UI frame (g6_mock_ui_frame) and add its transcript to `log` when a callback ran. `ms` is the
// tick's time (now_ms() before g6_mock_step). Returns the number of frame callbacks run.
int draw_ui_frame(std::uint64_t ms, UiLog& log);
// What --expect-clean checks about the mock UI: unbalanced calls, interactions nothing consumed, frames left
// registered (`check_registered` = false skips the last one, for checks made while a host is still loaded).
// Empty = clean; otherwise one line per problem.
std::string ui_problems(bool check_registered);

// A transcript log file (sdk/tests/ui/*.ui.txt) as UiLog::text() would write it: line endings normalised (CRLF
// reads as LF) and the leading '#' comment lines (how the file was made) removed. ok = false: unreadable.
std::string read_ui_log(const std::string& path, bool& ok);
// Compare `got` (UiLog::text()) with the file at `path`. Empty = equal; otherwise the first differing line of
// each side, with its line number.
std::string compare_ui_log(const std::string& got, const std::string& path);

} // namespace g6mock
