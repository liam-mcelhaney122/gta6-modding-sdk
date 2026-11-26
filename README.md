# gta6-modding-sdk

A single-player modding SDK for **GTA VI build 1.0.2.1** (Xbox Series dump): a stable C ABI, generated C, C++, Python and Luau bindings, an in-process runtime DLL, an API reference and a PSO schema. Every address and layout was verified offline against your own copy of the game executable.

![build 1.0.2.1](https://img.shields.io/badge/game%20build-1.0.2.1-2a78d6)
![single-player only](https://img.shields.io/badge/scope-single--player%20only-52514e)
![offline](https://img.shields.io/badge/verified-offline%20%2F%20static-52514e)
![in-game](https://img.shields.io/badge/in--game-read--only%20under%20DexNom-b26b00)
![license](https://img.shields.io/badge/license-not%20yet%20chosen-898781)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/overview-dark.svg">
  <img alt="Pipeline: a private research pipeline produces the generated data in this repository, which feeds the C ABI, C++, Python and Luau bindings and the runtime DLL. Tiles: 6,829 native handlers, 412 engine anchors, 896 struct fields, 19,662 PSO structures, 414/414 runtime self-tests passed offline, 56 native namespace pages." src="docs/assets/overview-light.svg">
</picture>

> [!IMPORTANT]
> **Status: built and self-tested offline; first in-game runs were read-only.** Mods can be written and unit-tested today against the mock runtime. The runtime DLL has been loaded into the running game under the [DexNom/VIctory](https://github.com/DexNom/VIctory) host: it passed its identity and anchor checks, installed the script-thread tick hook, ticked a plugin on the script thread and read the player's handle, coordinates, heading and health through natives. Nothing that **writes** game state, the UI overlay, the embedded Python host and the script-string natives has run in game yet.

## What works today

| Piece | Status |
|---|---|
| C ABI (`include/gta6mod/gta6mod.h`) | Stable host function table and plugin entry points, size-versioned; implemented by the mock runtime |
| Natives | **6,829 handlers** with typed wrappers in C, C++, Python and Luau; 1,903 carry a name, the rest are exposed by hash (`N.RAW.N_<hash>`); 13 seed-only entries are marked `unverified` and refused by real backends unless you opt in |
| Engine anchors (`include/gta6mod/patterns.hpp`) | **412 anchors**: 394 byte signatures unique in the 1.0.2.1 exe, 18 resolved through xref chains |
| Types (`classes.hpp`, `offsets.hpp`) | 89 engine types, 896 fields, with `static_assert` offsets |
| PSO schema (`data/generated/pso/`) | 19,662 structures, 79,222 members and 2,679 enums as JSON Schema (draft 2020-12), plus an XSD for the 2,014 structures with a known name |
| Python package (`python/gta6mod`) | Mock backend and a ctypes backend over the C ABI; PE reader, signature scanner, pattern resolver and RPF9/RDS5 header readers that work on your own dump |
| Luau host (`luau/`) | Plugin DLL, embedding API and an offline runner; the example mods run against the mock world |
| Script layer (`gta6mod.game`, `@gta6/game`) | Clean native names, helpers (player, entities, vehicles, world, clock, input) and a `Mod` structure with loops, timers, toggles and hotkeys; six example mods in Python and Luau with identical log lines, tested on the mocks only ([design](docs/script_layer.md)) |
| Input actions (`gta6mod.controls`, `@gta6/controls`, `controls.h`) | The build's **858 input actions** as script keys (`joaat("INPUT_<NAME>")`, the RDR2 scheme), 629 of them named, and the three control groups ([reference](docs/api/controls.md)). Which button triggers each action is not known yet |
| Runtime DLL (`runtime/loader`) | Builds with clang-cl (`/W4 /WX`); self-test **414/414** offline against the exe file; loaded in game under DexNom (read-only stages: identity, anchors, tick hook, plugin tick, read natives); `g6inject` loads it into a running host |
| UI overlay (`runtime/loader/ui`, `gta6mod.ui`, `@gta6/ui`) | Dear ImGui 1.91.9b drawn on the host's D3D12 swap chain through the `g6_ui_api` C ABI, with Python and Luau bindings and trainer-menu examples; tested offline on a D3D12 test window and the mocks; **not yet run in game** |
| Embedded Python host (`g6_python`) | Optional plugin that embeds CPython 3.12 (isolated) and runs Python mods on the script thread with per-mod isolation, a watchdog and hot reload ([design](docs/python_host.md)); tested against the mock runtime; **not yet run in game** |
| API reference (`docs/api`) | One page per native namespace, plus types, anchors, the native ABI and the PSO type table |

## Architecture

The SDK has three layers:

1. **Platform and runtime.** `runtime/loader` builds `gta6mod_runtime.dll`. It checks the executable's identity, resolves anchors by signature, checks each native handler statically, installs the script-thread tick hook and keeps a single-player guard. `runtime/src/mock_backend.c` implements the same C ABI offline. The design is in [`docs/runtime_design.md`](docs/runtime_design.md).
2. **Generated bindings.** The native wrappers, `patterns.hpp`, the type headers, the address manifest, the PSO schema and the API pages are generated files. Do not edit them by hand.
3. **Stable public API.** Mods use `gta6mod.h` (C), `natives.hpp` (C++), the `gta6mod` Python package or the Luau library. This layer is meant to stay the same across game builds; only the generated layer changes.

The generated files come from a private evidence database, where every address, type and name has its provenance and a confidence level. The research tooling and that database are not part of this repository.

## Quick start

All commands run from the repository root in PowerShell. `<DUMP>` is the folder of your own dump, the one that holds `game_scarlett_master.exe`.

### Python SDK

```powershell
$env:PYTHONPATH = "$PWD\python"          # or: pip install -e .
python -m gta6mod info "<DUMP>"                                              # exe build check and archive headers
python -m gta6mod patterns check --exe "<DUMP>\game_scarlett_master.exe"     # every pattern unique at its RVA
python -m gta6mod anchors verify --exe "<DUMP>\game_scarlett_master.exe"     # the seed anchors
python -m gta6mod run examples\python\spawn_car.py --ms 5000 --press INPUT_CONTEXT@1008 # example mod on the mock world
```

`patterns check` is the gate to run after every game update: each pattern must match exactly once, at its recorded RVA.

### Write a mod

A mod binds GTA VI input actions (`Control.*`) and runs on the mock world offline. Press, release (tap),
hold and chord bindings follow one set of rules in Python and Luau ([script layer](docs/script_layer.md)):

```python
from gta6mod.game import Control, Mod, player, world

m = Mod("demo")
god = m.toggle(Control.DETONATE, "god mode")          # press toggles
god.on_enable(lambda: player.ped().set_invincible(True))
god.on_disable(lambda: player.ped().set_invincible(False))

@m.on_release(Control.MULTIPLAYER_INFO, max_ms=300)   # tap
def slow_motion():
    world.set_time_scale(0.5)

@m.on_hold(Control.MULTIPLAYER_INFO, 1000)            # hold for 1 s
def normal_speed():
    world.set_time_scale(1.0)

@m.on_key(Control.CONTEXT, mods=Control.SPRINT)       # SPRINT + CONTEXT; the most specific chord wins
def chord():
    m.log("chord")

m.block(Control.ATTACK, when=god)                     # disabled every tick while the toggle is on
```

```powershell
python -m gta6mod run .\demo.py --ms 2000 --press INPUT_DETONATE@96 --press INPUT_MULTIPLAYER_INFO@496 --press INPUT_MULTIPLAYER_INFO@800+1100
```

`--press NAME@MS+HOLD` keeps the action held for `HOLD` ms in the mock. The action keys are confirmed from the
executable and the game's own scripts; which button triggers each action in game is not known yet.

### Tests

```powershell
python -m unittest discover -s tests\python -v
$env:G6_DUMP = "<DUMP>"; python -m unittest discover -s tests\python -v   # adds the dump-gated checks
```

### Runtime DLL

Needs LLVM's `clang-cl` (`C:\Program Files\LLVM\bin` or `PATH`) and the MSVC Build Tools with a Windows SDK.

```powershell
runtime\loader\build.cmd test                              # build, then run the offline self-test
runtime\loader\build.ps1 -Test -Dump "<DUMP>"              # also runs the checks against your exe file
```

### Luau host

`luau\build.cmd` clones Luau at the pinned tag (0.650) into `build\_deps` on first use and builds the plugin, the offline runner and the tests into `build\luau`.

```powershell
luau\build.ps1 -Test
build\luau\g6_luau_runner.exe luau\lib luau\examples\spawn_car\init.luau 5000 --press INPUT_CONTEXT@1008
```

### CMake (all targets)

```powershell
cmake -S . -B build-cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
      -DG6_WITH_RUNTIME_LOADER=ON -DG6_WITH_LUAU=ON
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

The CMake build and `ctest` were verified on an exported copy of this repository (CMake 4.4.5, Ninja 1.13.2; clang-cl 23.1.6, and MSVC `cl` 19.52 from a VS x64 developer prompt without the two `-DCMAKE_*_COMPILER` options). With `G6_WITH_LUAU=ON` and no `build\_deps\luau-0.650` checkout, configuring clones Luau 0.650 from GitHub, so it needs git. Set `$env:G6_DUMP = "<DUMP>"` before `ctest` to add the runtime self-test's checks against your exe file.

## Repository layout

| Path | Contents |
|---|---|
| `include/gta6mod/` | `gta6mod.h` (C ABI); generated `natives.h`, `natives.hpp`, `patterns.hpp`, `classes.hpp`, `offsets.hpp` |
| `python/gta6mod/` | Python package: runtime backends, generated natives, PE, patterns, anchors, format headers, CLI |
| `luau/` | Luau host (plugin, embedding API, runner), generated `natives.luau`, script layer (`lib/game.luau`, `hotkeys.luau`, `controls.luau`), examples and tests |
| `runtime/loader/` | Runtime DLL sources, self-test and build scripts ([README](runtime/loader/README.md)) |
| `runtime/src/` | Mock implementation of the C ABI |
| `data/generated/` | Natives index, pattern index, address manifest, types index, PSO schema |
| `data/seed/` | Seed natives and anchors |
| `third_party/imgui/` | Dear ImGui v1.91.9b (MIT, `LICENSE.txt`), vendored unmodified for the overlay |
| `docs/` | [Runtime design](docs/runtime_design.md), [script layer](docs/script_layer.md), [Python host](docs/python_host.md), [API reference](docs/api/index.md), [input actions](docs/api/controls.md), [native signatures](docs/native-signatures.md), [Python guide](docs/python/index.md) |
| `examples/`, `tests/` | Example Python mods; Python unit tests and C/C++ header checks |

## Guardrails and scope

- **Single-player and offline only.** NETWORK natives are always refused. Every other native, and the `mods/` overlay, stays refused while the single-player guard is closed. Nothing here touches online services, anti-cheat or networking code.
- **No game content.** This repository contains no game files, keys, decrypted data or assets. The tools read only the files you point them at.
- **Bring your own dump.** You need your own legally obtained copy of the game. Identity checks refuse any other executable build.

## Disclaimer

This is an independent project. It is not affiliated with, endorsed by or sponsored by Rockstar Games or Take-Two Interactive. Grand Theft Auto and related names are trademarks of their respective owners.

## License

License: not yet chosen; all rights reserved until a license is added.
