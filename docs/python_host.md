# In-game Python host (`g6_python.dll`)

`g6_python.dll` is a `gta6mod_runtime` plugin that embeds CPython 3.12 and runs `gta6mod` Python mods inside
the game process, the way `g6_luau.dll` runs Luau mods. The mods are the same files that run offline against
the mock world (`python -m gta6mod run`); only the backend changes (`EmbeddedBackend` instead of
`MockBackend` / `CAbiBackend`).

**Status: verified offline only.** The plugin has run against the mock runtime (`gta6mod_mock.dll`) and the
shared C mock world in `g6_python_test.exe`. It has not run inside the game. "What needs a live check" at the
end lists what the offline test cannot show.

## Files

```
sdk/python/host/g6_python.cpp            the plugin: config, interpreter start, _g6 module, watchdog, tick/stop/unload
sdk/python/host/g6_python.ini            default config (copied to build\python\ once, never over a user's copy)
sdk/python/host/build.cmd                clang-cl build of the plugin and its test; "test" runs the test
sdk/python/host/tests/g6_python_test.cpp offline test: the plugin against gta6mod_mock.dll + runtime/src/mock_world.cpp
sdk/python/host/tests/mods/*.py          test-only mods (isolation, environment, hot reload)
sdk/python/gta6mod/runtime/embedded.py   EmbeddedBackend: Backend over _g6
sdk/python/gta6mod/runtime/host.py       ModHost: mod discovery, per-mod namespaces, failure isolation, stop, reload
sdk/python/gta6mod/runtime/embedded_ui.py EmbeddedUi: gta6mod.ui recorded on the script thread ("Python UI")
sdk/runtime/src/ui_retained.{h,cpp}      the replay on the render thread; in gta6mod_runtime.dll and g6_python_test.exe
sdk/tests/python/test_python_host.py     the Python half (host.py, embedded.py) with a fake _g6 over MockBackend
sdk/tests/python/test_embedded_ui.py     EmbeddedUi against a Python copy of ui_retained.cpp, transcript parity
sdk/tests/ui/*.ui.txt                    mock-UI transcripts of the Luau runs (parity across hosts)
sdk/runtime/src/mock_world.*             the C mock world and mock-UI driver, shared with g6_luau_runner (same semantics as runtime/mock.py)
```

## Install layout

```
<runtime folder>\gta6mod_runtime.dll         [runtime] mode=full, [guard] offline_attested=1
<runtime folder>\plugins\g6_python.dll
<runtime folder>\plugins\g6_python.ini       [python] enable=1
<runtime folder>\plugins\python\             CPython 3.12: the embeddable zip unpacked, or set home= to an install
<runtime folder>\plugins\python_sdk\gta6mod\ a copy of sdk\python\gta6mod
<runtime folder>\plugins\mods\python\*.py    the mods (sdk\examples\python\*.py are ready to copy)
```

All paths in `g6_python.ini` may be absolute; relative ones are relative to the plugin's folder.

| key | default | meaning |
|---|---|---|
| `enable` | `0` | `0`: `g6_plugin_load` logs one line, registers nothing, and `python312.dll` is never loaded |
| `home` | `python` | CPython 3.12: `python312.dll` plus `Lib\` + `DLLs\` (an install) or `python312.zip` (embeddable) |
| `sdk_path` | `python_sdk` | folder that contains the `gta6mod` package |
| `mods_dir` | `mods\python` | every `*.py` here is a mod; names starting with `_` are skipped |
| `extra_paths` | empty | more `sys.path` entries, `;`-separated |
| `allow_unverified` | `0` | let mods call natives that re-db has not marked verified (today: all of them, see below) |
| `hot_reload`, `reload_poll_ms` | `0`, `1000` | reload a mod when its file changes; load new files; unload deleted ones |
| `tick_budget_ms`, `load_budget_ms` | `2000`, `10000` | watchdog budgets in wall-clock ms; `tick_budget_ms=0` turns it off |
| `guard_retry_ms` | `5000` | how often a start blocked by the single-player guard is retried |
| `write_bytecode` | `1` | `__pycache__` for the SDK; mods are always compiled from source |

A bad value (for example `tick_budget_ms=5`) makes `g6_plugin_load` log the key and return
`G6_E_INVALID_ARG`; nothing is registered.

`allow_unverified`: `Runtime.call` refuses every native whose generated status is not `verified` unless the
runtime allows unverified natives. Every 1.0.2.1 native is `observed` today, so mods that call natives need
`allow_unverified=1` in game (the Luau host's `G6_ALLOW_UNVERIFIED=1` is the same switch). The mock runtime
(build platform `mock`) accepts them by itself. The runtime's own handler self-id check is separate and
always applies.

## Embedding: what and why

**Decision: link `python312.lib`, delay-load `python312.dll` from `[python] home`, and start an isolated
interpreter with an explicit module search path.** The embeddable distribution is supported as one possible
`home`, not required.

- *Delay-load* (`/DELAYLOAD:python312.dll`): the runtime can load `g6_python.dll` on a machine without
  Python, and with `enable=0` Python is never touched. Before the first Python call the plugin
  `LoadLibraryExW`'s `<home>\python312.dll` (`LOAD_WITH_ALTERED_SEARCH_PATH`, so its `vcruntime140.dll` and
  the `.pyd` files' dependencies come from `home`), and the delay-load helper binds to that module. If a
  different `python312.dll` is already loaded in the process, the plugin refuses to start (the delay-load
  would bind to the other one). The linker refuses to delay-load **data** imports, so the plugin uses no
  Python data symbol (`Py_None`, `PyExc_*`, type objects): `None` comes from `Py_BuildValue("")`, builtin
  exceptions from the builtins dict.
- *Isolated config*: `PyPreConfig_InitIsolatedConfig` (with UTF-8 mode) and `PyConfig_InitIsolatedConfig`:
  no `PYTHON*` environment variables, no user site, `safe_path`. On top of that `site_import=0` (no
  `site-packages` of the install, so nothing the user `pip install`ed is visible), no signal handlers (the
  game's console/signal handling is left alone), no `faulthandler`, `home` set (which also disables `._pth`
  discovery), `sys.executable` = `<home>\python.exe` (never the game exe, so `subprocess`/`multiprocessing`
  cannot relaunch the game), and `module_search_paths_set=1` with exactly:
  `<home>\python312.zip` (if present), `<home>\Lib`, `<home>\DLLs` (if present), `<home>`, `sdk_path`,
  `extra_paths`.
- *Why not only the embeddable zip next to the plugin*: it would work, but tying the plugin to one folder
  layout buys nothing; the same search-path rules cover both, and the offline test runs against a normal
  CPython 3.12 install (`%LOCALAPPDATA%\Programs\Python\Python312`) in exactly the same isolated mode.
- The build needs the 3.12 headers and `libs\python312.lib`: `G6_PYTHON_HOME`, else the default install
  path above. The ABI is the full (not limited) 3.12 API; `Py_GetVersion()` must start with `3.12.`.

## Threading and the tick model

The runtime calls a plugin's registered tick on the game script thread, inside a runtime fiber, once per
frame (README "Architecture" step 8). The Python host follows the Luau host:

1. `g6_plugin_load` (runtime init thread) reads the ini, validates `home`/`sdk_path`, and registers one
   script, `python-host`. **No Python runs here.**
2. **First tick**: the guard probe (below), then `Py_InitializeFromConfig` on the script thread, the
   watchdog thread, and `gta6mod.runtime.host.boot(...)`, which installs `EmbeddedBackend`, routes
   `sys.stdout`/`sys.stderr` to the runtime log (`stdout` / `stderr` sources) and loads every mod. That
   frame also pays for the first import of `gta6mod.natives` (2.5 MB of source; `__pycache__` makes later
   starts faster). The cost of this frame in game is **not measured**.
3. **Every tick**: `PyGILState_Ensure` → `ModHost.tick()` → `PyGILState_Release`. The GIL is held only
   inside the tick (and the stop/unload entries); between frames no Python runs. If a stop/unload entry
   on another thread holds the plugin's lock, the tick skips that frame instead of blocking the game.
4. A mod is a set of `gta6mod.script` scripts (generators): `yield wait(ms)` suspends the generator until
   `game_time_ms` has advanced `ms` (the runtime clock, which pauses with the game). The host never calls
   `g6_host_api.script_wait`; the plugin's tick always returns to the runtime, so no Python frame is ever
   suspended across a runtime fiber switch.
5. If the runtime ticks the plugin on a different OS thread than the one that initialised Python, it still
   works (`PyGILState_Ensure` creates a thread state for that thread); the plugin logs it once.

## Natives: `_g6` and `EmbeddedBackend`

The plugin registers the built-in module `_g6` with `PyImport_AppendInittab` before initialisation.
`EmbeddedBackend.invoke(hash, slots)` calls `_g6.invoke`, which fills a `g6_native_call`, resolves the hash
once (`native_resolve`, cached per hash) and calls `native_invoke`. Packing and unpacking are
`runtime/marshal.py`'s (`Runtime.call`), identical for every backend; out-param buffers are still ctypes
objects whose addresses go into slots, but no native call goes through ctypes.

| `_g6` | |
|---|---|
| `invoke(hash, slots) -> (status, (r0, r1, r2, r3))` | at most 32 slots; ints are masked to 64 bits |
| `log(level, source, message)` | `g6_host_api.log`, level 0..3 |
| `now_ms()` | `g6_host_api.game_time_ms` |
| `build_info()` | `(status, version, platform, sha256_hex, natives_known, natives_verified)` |
| `ui_proc(name)` | address of a `g6_ui_*` export of the runtime module, else 0 (`EmbeddedUi` binds `g6_ui_retained_*`, "Python UI") |
| `on_script_thread()` | true inside a plugin tick/stop/unload entry on this thread |
| `watch(on[, budget_ms])`, `set_budget_exception(cls)` | watchdog control, used by `host.py` |

A status other than 0 raises `NativeError`; `G6_E_ONLINE_SESSION` raises its subclass `GuardClosed`.
A native called from a thread that is not inside a plugin entry (a thread a mod started) returns
`G6_E_NOT_ON_SCRIPT_THREAD` from `_g6` itself and never reaches the runtime. Mods should not start threads:
such a thread runs Python whenever the script thread is not in a tick, which is outside the "GIL only in
the tick" model.

## Mods

- **Discovery**: `mods_dir\*.py`, sorted by name; `_*.py` and `.*` are skipped. One file = one mod, named
  after its stem.
- **Namespaces**: each mod is its own module `g6mods.<stem>` in `sys.modules`, compiled from source on every
  load (`compile` + `exec`, never a `.pyc`: a `.pyc` is keyed by whole-second mtime and size, so a quick edit
  could run stale code). The `gta6mod` SDK is shared.
- **Schedulers**: each mod has its own `script.Scheduler`. Its load and every tick run inside
  `script.using(<its scheduler>)`, so `@script.register` and `Mod(...)` register there, and two mods may use
  the same script names.
- **Failures**: a mod whose top-level code raises, one of whose scripts raises, which raises a
  `BaseException` (`SystemExit`, ...), or which overruns the watchdog is **disabled**: the traceback is
  logged at ERROR, its stop handlers run (`Mod.stop`: toggles off, `on_stop`), its scripts are cancelled,
  and every other mod keeps running. A disabled mod stays disabled until it is reloaded (hot reload, or a
  plugin reload).
- **Guard**: a native refused with `G6_E_ONLINE_SESSION` pauses **every** mod (the runtime's guard latches
  until restart, so every later native would be refused too). Paused mods' stop handlers are skipped at the
  stop event, because their natives would only fail; their scripts are cancelled.
- **Stop event**: `g6_plugin_stop` (runtime: from a script fiber while ticks still run, so natives work)
  calls `ModHost.stop()`: every loaded mod's stop handlers run once. `g6_plugin_unload` raises it first if
  the runtime did not, then drops every mod module.
- **Hot reload** (`hot_reload=1`): every `reload_poll_ms` the host compares each file's (mtime, size); a
  changed mod gets its stop event and is loaded again in a fresh module and scheduler; new files load;
  deleted files are stopped and dropped.
- `print()` goes to the runtime log (source `stdout`, `stderr` at WARN).

Isolation is about failures and names, not security: a mod is ordinary Python with the whole interpreter,
`ctypes` and the file system available.

## Watchdog

`host.py` arms the watchdog around each mod operation (`_g6.watch(True, budget)` / `watch(False)`): one
mod's load (`load_budget_ms`), tick or stop (`tick_budget_ms`). A watchdog thread checks every 20 ms; when
an armed operation has run past its budget of wall time, it takes the GIL (the busy thread drops it within
the 5 ms switch interval at its next bytecode boundary) and, if the same operation is still armed, sets
`TickBudgetExceeded` as an asynchronous exception on the thread that armed it
(`PyThreadState_SetAsyncExc`). `TickBudgetExceeded` is a `BaseException`, so a mod's `except Exception`
does not swallow it; the host disables that mod. It fires again after another budget if the mod still runs.

- Not `Py_AddPendingCall`: in 3.12 a pending call added from a non-main thread does not set the main
  thread's eval breaker, so it only runs after the next GIL hand-over. That was observed in the offline
  test (a spinning mod was never interrupted) before the switch to the async exception.
- A mod stuck inside one C call that never releases the GIL (a huge `sum(range(...))`) cannot be
  interrupted; the exception lands when the call returns to bytecode. A C call that releases the GIL
  (`time.sleep`) is interrupted when it returns.
- The watchdog thread holds a Python thread state only for the moment it sets the exception.
- The SDK is imported before any mod is armed, so its first-import cost never counts against a mod.

## Single-player guard

The plugin is gated like every plugin: the runtime refuses natives with `G6_E_ONLINE_SESSION` while its
guard is closed (README "Single-player guard"). In addition, Python does not even start while the guard is
closed: on each tick before the start, the plugin calls `GET_GAME_TIMER` (observed hash
`0x4F67E8ECA7D3F667`, read-only) through `native_invoke_hash`. `G6_E_ONLINE_SESSION` logs one WARN and
retries after `guard_retry_ms`; any other status (including an unverified handler) lets Python start, and
the per-native refusal still applies afterwards.

## Lifetime, cleanup and unloading

- One cleanup path: a failed start and `g6_plugin_unload` both go through `release_python_side`, which
  (with the GIL) calls `ModHost.shutdown()` when the host ran, drops the plugin's references, clears the
  native-handle cache and stops the watchdog thread. Unload unregisters the tick first, so no tick runs
  during or after it. A failed start leaves the (no-op) tick registered until unload: the plugin does not
  unregister its own script from inside its tick fiber.
- **The interpreter is never finalised, and `python312.dll` and `g6_python.dll` are pinned** once Python
  starts (`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN)`). Reasons: `Py_FinalizeEx` joins every
  non-daemon thread a mod started (a hang inside the game's shutdown); extension modules (`_ctypes`, ...) do
  not survive finalise/re-initialise reliably; the interpreter keeps pointers into `g6_python.dll` (the
  `_g6` method table); and a Python frame (a suspended mod generator, a mod thread) may still reference
  either module. `FreeLibrary` on either while any of that exists would crash later, so neither is ever
  unloaded. The runtime itself never frees a loaded plugin; the pin also covers a host that would.
- A later `g6_plugin_load` in the same process reuses the interpreter ("reusing the interpreter" in the log)
  and boots a fresh `ModHost`. If `Py_InitializeFromConfig` failed once, the plugin does not retry in that
  process.

## Python UI (`gta6mod.ui` in game)

The overlay (`g6ui.dll`, runtime/loader/README.md "UI overlay") calls frame callbacks on the game's **render
thread**, inside its ImGui frame, from its Present hook. Python must not run there: Python runs only on the script
thread with the GIL, inside the plugin's tick, and the watchdog, the guard pause and the "GIL only in the tick"
model above all depend on that.

**Decision: retained programs.** A Python frame function never runs when the overlay draws. Once per script tick,
after the mod's scripts, `gta6mod.ui` runs it on the script thread; every widget call is *recorded* into a
*program* (the command and the value the mod passed) and answered from what the user did in the frames drawn since
the previous tick. The runtime's C++ (`sdk/runtime/src/ui_retained.{h,cpp}`, linked into `gta6mod_runtime.dll`)
replays the latest program on the render thread, every frame, through the same `g6_ui_api` C plugins use, and
stores the results for the next tick.

The alternative, frame callbacks that take the GIL on the render thread with a short timeout (the Luau host's VM
lock waits at most 4 ms), was rejected: CPython has no timed GIL acquire (`PyGILState_Ensure` blocks without a
limit, and the GIL may be held by a mod thread or the watchdog, not only by the tick), a frame would stall Present
for as long as Python runs, the watchdog does not cover the render thread, and any Python object touched there
would need its own locking against the tick. The retained model needs none of that: the render thread runs only
C++ and never waits for Python; the script thread never waits for a frame.

```
script thread (plugin tick, GIL held)                  render thread (Present hook, overlay lock held)
  ModHost.tick -> mod scheduler.tick                      g6ui -> retained frame callback (ui_retained.cpp)
    scripts ...                                              lock slot: take program pointer + overrides
    after_tick: gta6mod.ui._pump                             unlock
      poll()   -> results since the last tick                replay commands through g6_ui_api (ImGui)
      frame function: widgets record + answer                lock slot: merge clicks / values / states
      submit() -> new program (pointer swap)                 unlock
      ui.defer work queued by the frame runs now
```

How it shows to a mod (the same frame function works with `MockUi` and `CAbiUi`; `test_embedded_ui.py` and
`g6_python_test` check the rules):

- An interaction reaches the frame function on the **next tick**, not in the frame where it happened: a click is
  `True` once, an edited value is returned as `(True, value)`, and `ui.defer` work it queues runs in that same tick,
  after the frame function. So a click is applied on the same tick as in the Luau host (whose frame runs between
  ticks and whose deferred work runs on the next tick).
- The value a widget returns is what the program records, so the frame drawn after the tick shows it even when the
  mod applies it only through `ui.defer`. Until the program carries a value the user produced, the replay keeps
  drawing the user's value (a dragged slider does not jump back between ticks).
- A tree node or tab that opens shows its contents **one frame later** (the program recorded none for it); one that
  closes skips them at once. ImGui's closers are always called exactly when the opener returned 1, whatever the
  program recorded.
- While no frame is drawn (menu hidden, `[ui] overlay=0`, guard closed), the frame function does not run (the last
  program is still current); the first frame after the menu opens shows the last program.
- Natives are refused inside a frame function (`G6_E_NOT_ON_SCRIPT_THREAD` from `EmbeddedBackend`), although it
  runs on the script thread here: the rule is the same in every host, so a mod behaves the same everywhere.
- A frame function that raises is logged; ten in a row remove it (as offline). Unbalanced calls are closed at the
  end of the frame function and logged; the program the runtime gets is always balanced.
- Frames and deferred work belong to the mod (its scheduler): a mod's stop event, failure or hot reload removes only
  its frames, and two mods may use the same frame name.

**Exports and binding.** `gta6mod_runtime.dll` exports (C ABI, cdecl):

| export | |
|---|---|
| `uint32_t g6_ui_retained_version(void)` | wire format version, 1 |
| `g6_status g6_ui_retained_create(const char* name, uint32_t flags, uint32_t* handle)` | registers one frame with the runtime's `g6_ui_api` (from the plugin's tick, so the runtime tags it with `g6_python` and drops it if the plugin unloads without destroy); `G6_E_NOT_IMPLEMENTED` with the overlay off |
| `g6_status g6_ui_retained_destroy(uint32_t handle)` | unregisters (waits for a frame in progress) and frees the slot |
| `g6_status g6_ui_retained_submit(uint32_t handle, const void* program, size_t size)` | validates and publishes a program; a malformed one is refused (`G6_E_INVALID_ARG`) and the previous stays |
| `g6_status g6_ui_retained_poll(uint32_t handle, void* out, size_t cap, size_t* size)` | results since the last poll; too small a buffer consumes nothing and returns the size needed |

`EmbeddedBackend` binds them through `ui_proc` (only `g6_ui_*` names resolve), with ctypes `CFUNCTYPE` (the GIL is
released during each call), and sets `backend.ui = EmbeddedUi` when all five exist and the version matches; else
`backend.ui` is `None` and `ui.available()` is false. At most 64 retained frames per process.

**Wire format (version 1, little-endian).** Strings are `u32 length` + UTF-8 bytes without NUL (an optional string
uses length `0xFFFFFFFF` for "none"; at most 65,536 bytes); floats are finite `f32`.

- Program: `u32 'G6UP' (0x50553647)`, `u16 version`, `u16 0`, `u32 count`, then `count` commands, each `u8 op` and
  its fields: 1 `begin` (str title, u8 closable, u32 flags), 2 `end`, 3 `next_window_pos` / 4 `next_window_size`
  (f32, f32, u32 cond), 5 `text` (str), 6 `text_colored` (f32 x4, str), 7 `button` (str), 8 `checkbox` (str, i32),
  9 `slider_int` (str, i32 value, min, max), 10 `slider_float` (str, f32 value, min, max, optional str format),
  11 `input_text` (str label, str text, u32 buffer bytes 2..65537), 12 `combo` (str, i32, u32 n, n x str),
  13 `separator`, 14 `same_line`, 15 `tree_node` / 17 `tab_bar` / 19 `tab_item` (str, u8 contents recorded),
  16 `tree_pop`, 18 `end_tab_bar`, 20 `end_tab_item`, 21 `color_edit` (str, f32 x4, u32 flags), 22 `plot_lines`
  (str, u32 n, n x f32, optional str overlay, f32 scale min, max, width, height). Every `begin` has its `end`; an
  opener with "contents recorded" has its closer; tab items only inside a tab bar; at most 64 levels, 65,536
  commands, 8 MiB.
- Widget key: `(op, n, label)`, `n` counting earlier commands with the same op and label in the program.
- Results: `u32 'G6UR'`, `u16 version`, `u16 0`, `u32 frames drawn since the last poll`, `u32 a frame was ever
  drawn`, `u32 menu_open`, `f32 width, height, delta`, `u64 frame`; then `u32 n` states (key, `u8` drawn / open /
  visible / selected in the latest frame); `u32 n` new values (key, then `i32` for checkbox / slider_int / combo,
  `f32` for slider_float, str for input_text, `f32 x4` for color_edit); `u32 n` events (key, `u32` clicks, `u8`
  window closed). Keys are `u8 op`, `u32 n`, str label.

Offline the same model runs in two places: `test_embedded_ui.py` replays programs with a Python copy of the C++ rules
(`FakeRetained`) into the Python `MockUi`, and `g6_python_test` links `ui_retained.cpp` into the test exe (which owns
the host table there, as the runtime does in game) and draws the mock UI from a second thread after every tick.
Transcript parity: `sdk/tests/ui/trainer_menu.ui.txt` and `ui_widgets.ui.txt` are the Luau runs' mock-UI
transcripts (`g6_luau_runner --ui-log`); the Luau runs, the Python mock and the in-game host must reproduce them,
the in-game host with the one allowed difference above (the tree opened at 96 ms in `ui_widgets`).

## Tests

```powershell
sdk\python\host\build.cmd test                         # plugin + g6_python_test (17 scenarios)
.\sdk\runtime\loader\build.ps1 -Test -Low              # the whole runtime tree, g6_python_test included
python -I -m unittest discover -s sdk/tests/python     # includes test_python_host.py and test_embedded_ui.py
```

CMake: `-DG6_WITH_PYTHON_HOST=ON [-DG6_PYTHON_HOME=<CPython 3.12>]` adds `g6_python`, `g6_python_test` and the
`g6_python_test` ctest (configured and run with MSVC 14.52 + Ninja on 2026-11-26).

`g6_python_test.exe <g6_python.dll> <sdk> <python home> <work>` runs each scenario in its own process (an
interpreter per process, since it is never finalised), each with `<work>\<name>\plugins\` holding a copy of
the plugin, its ini and its mods:

| scenario | checks |
|---|---|
| `god_mode`, `never_wanted`, `waypoint_teleport`, `vehicle_spawner`, `time_control` | the example's log lines equal `test_script_layer.TestExampleMods` (same events, same mock world), world state, no ERROR |
| `stop_event` | `g6_plugin_stop` at 1504 ms turns god mode off; later damage lands; one stop summary |
| `isolation` | load error, script error and a spinning mod (watchdog, 300 ms) are each disabled with their cleanup run; the good mod ticks to 100 and its stop handler runs |
| `environment` | from inside: `sys.flags.isolated`, `no_site`, no `site-packages`, `_g6` built in, `sys.executable`, natives from a mod thread refused with status 5, 33 slots refused, `print` reaches the log, the UI hook |
| `disabled` | `enable=0`: nothing registered, `python312.dll` not loaded |
| `guard_closed` | guard closed from the start: Python never starts, one WARN |
| `guard_trip` | guard closes at 1008 ms: every mod paused (not disabled), stop handlers skipped |
| `reload` | hot reload replaces a mod (old stop handler runs, new code runs), then unload + load in one process reuses the interpreter |
| `bad_config` | `tick_budget_ms=5` is refused with `G6_E_INVALID_ARG`, nothing registered |
| `ui_trainer` | `examples/trainer_menu.py` with the Luau run's events and clicks: log lines, world state, every mock-UI frame equal to `sdk/tests/ui/trainer_menu.ui.txt`; frames drawn on a second thread; the only draw callback lives in the test exe (no Python, no ctypes thunk on the render thread); nothing left registered |
| `ui_widgets` | `tests/ui_widgets.py` (twin of the Luau `ui_widgets` scenario): every widget changed by mock interactions reaches the mod; every frame equal to `ui_widgets.ui.txt` except the tree contents at 96 ms (one frame later) |
| `ui_off` | the host table's UI refuses frames (`[ui] overlay=0`): the trainer loads and runs without its menu, one INFO line |
| `ui_reload` | a UI mod hot reloaded next to the trainer: only its frame is replaced (3 registrations), both drawn afterwards |

Every scenario also checks that loading `g6_python.dll` does not load `python312.dll` (delay-load) and that
no tick is registered after unload.

## What needs a live check

- That the runtime loads `g6_python.dll` from `plugins\` and its tick runs on the bound script thread
  (`mode=full`), and how long the first tick (interpreter start + SDK import) stalls the frame.
- CPython on the runtime's script fiber (`CreateFiberEx`, reserve `[script] fiber_stack_kb`, default 1024):
  half of `python.exe`'s 2 MB main-thread stack. Python-to-Python calls do not use the C stack in 3.11+, but
  C-level recursion (deep `repr`, C callbacks, imports) does, and the recursion limits count calls, not bytes.
  Interpreter start and the five examples ran on a normal thread offline; on the fiber it is not measured.
  If a stack overflow shows, raise `fiber_stack_kb`.
- Whether the game ever runs the bound script thread's update on different OS threads (the plugin logs the
  first switch).
- Native behaviour itself: every example native is `observed`, not verified (needs `allow_unverified=1`), and
  the mock world is not GTA VI.
- The watchdog's async exception during a native call: it lands after the native returns (the GIL is held
  across the call), so a native that hangs the game is not interruptible.
- The Python UI in game (`[ui] overlay=1`, `mode=full`): that `g6_ui_retained_create` from the plugin's tick
  registers with g6ui (tagged with `g6_python`), that the replay draws the trainer menu on the real swap chain and a
  click reaches the mod on the next tick, the cost of a recorded frame per tick (one poll, one frame function, one
  submit per frame function) and of the replay per frame, and that destroy from the script thread does not stall
  against the render thread (it waits for a frame in progress, as `frame_unregister` does for C plugins). The
  offline tests drive the mock UI, not ImGui; ImGui widgets that keep state across frames (an InputText being
  typed in, a slider being dragged) are only modelled by the mock's set interactions.
