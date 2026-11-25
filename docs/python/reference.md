# Python API reference

Import helpers from `gta6mod.game`. Calls that reach natives require an active runtime. The command-line runner selects one before loading the mod; standalone code can call `gta6mod.runtime.use_mock()` first.

## Mod and Toggle

`Mod(name, scheduler=None)` registers scripts on the supplied scheduler or the default scheduler. Give each mod a distinct name to avoid script-name collisions.

| Member | Description |
|---|---|
| `@mod.loop` | Register a plain function for every tick, or a generator function that controls its own waits. |
| `@mod.every(ms, when=None)` | Call a plain or generator handler repeatedly, waiting `ms` after each call. An optional `Toggle` gates calls. |
| `@mod.on_key(key, pad=0)` | Call a handler on a just-pressed event. A waiting handler runs in a separate script; further presses for that binding are ignored while it runs. |
| `mod.toggle(key, label=None, pad=0)` | Bind a key and return a `Toggle`, initially off. |
| `@mod.on_stop` | Register a plain cleanup callback. |
| `mod.log(message)`, `warn(message)`, `error(message)` | Log with the mod name as source. |
| `mod.stop()` | Cancel scripts, turn off active toggles and run cleanup callbacks. Returns a list of cleanup exceptions; repeated calls return an empty list. |

A `Toggle` exposes `on`, `set(on)`, `flip()`, `@toggle.on_enable`, and `@toggle.on_disable`; its truth value reflects `on`. Toggle and stop callbacks must be plain functions. `stop()` closes suspended generators so their `finally` blocks can run. There is currently no automatic unload hook: arrange to call `stop()` when appropriate.

## Timing and scheduling

`wait(ms=0)` returns a scheduler request; use `yield wait(ms)` inside a generator. Negative values are clamped to zero. `now()` returns backend game time in milliseconds. `log(message, level="INFO", source="mod")` writes to the active backend.

For lower-level scheduling, `gta6mod.script.register(name)` decorates a function, `scheduler()` returns the default `Scheduler`, and `reset()` replaces it. `Scheduler.add(name, fn)`, `remove(name)`, and `tick()` manage scripts. `script.run_for(ms, frame_ms=16, sched=None)` drives a backend with a controllable clock, such as the mock. Script exceptions are logged and stop that script; other scripts continue.

## Player and entities

`player.ped()` returns a `Ped`; `player.id()` returns the player index. `player.position()` returns a `Vector3`; `player.vehicle()` returns a `Vehicle` or `None`. `player.teleport(pos, heading=None, with_vehicle=True)` moves the vehicle when applicable. Wanted-level methods are `wanted_level()`, `clear_wanted()`, `max_wanted()`, and `set_max_wanted(level)`. Player ID uses an unverified seed native.

`Entity(handle)`, `Ped(handle)` and `Vehicle(handle)` wrap integer script handles. A wrapper is falsy if the low 64 bits of its handle are zero; use `entity.exists` to query whether a nonzero handle exists. `Ped` and `Vehicle` inherit `Entity`.

| Entity member | Access / behavior |
|---|---|
| `handle` | Underlying integer handle. |
| `position`, `heading`, `health`, `visible` | Read and write properties. Position accepts a `Vector3` or a three-value iterable. |
| `exists`, `max_health`, `is_dead`, `speed`, `forward`, `model` | Read-only properties. |
| `heal()` | Set health to maximum health. |
| `set_invincible(on=True)`, `freeze(on=True)`, `set_velocity(v)` | Change entity state. |
| `offset(x=0, y=0, z=0)` | Return a world position from local right, forward and up offsets. |
| `distance_to(other)` | Distance to an entity or position. |

`Ped` adds `set_can_ragdoll(on)`, `in_vehicle()`, `vehicle()` and `put_into(vehicle, seat=SEAT_DRIVER)`. The default driver seat value is unverified for GTA VI. `Vehicle.repair()` repairs a vehicle. `Vehicle.spawn(model, position, heading=0.0, timeout_ms=2000)` is a generator: use `vehicle = yield from Vehicle.spawn(...)`; it returns a `Vehicle` or `None`. Its create-vehicle native is unverified.

`Vector3` holds `x`, `y`, and `z`. `models.hash_of(model)` hashes a name or converts an integer to a 32-bit hash; `models.is_valid(model)` checks it. `yield from models.request(model, timeout_ms=2000)` returns a boolean and releases the request on timeout or cancellation. On success call `models.release(model)` when finished. The spawn helper handles release after creation.

## World, clock and controls

| Member | Description |
|---|---|
| `world.waypoint()` | Return a `Vector3` or `None`; waypoint height may need a ground probe. |
| `world.request_collision(pos)` | Request collision at a position. |
| `world.ground_z(x, y, probe_z=1000.0)` | Return ground height or `None`. |
| `world.find_ground(x, y, timeout_ms=1500, probe_z=1000.0)` | Generator that requests collision and retries; returns height or `None`. Use `yield from`. |
| `world.set_time_scale(scale)` | Set the time scale. |
| `clock.time()` | Return `(hours, minutes, seconds)`. |
| `clock.set_time(hours, minutes=0, seconds=0)`, `clock.add(hours=0, minutes=0, seconds=0)`, `clock.pause(on=True)` | Set, advance or pause the clock. |
| `controls.just_pressed(action, pad=0)`, `pressed(action, pad=0)`, `just_released(action, pad=0)` | Query control state. |
| `controls.disable(action, pad=0)` | Disable the action for the current frame; call each tick to keep it disabled. |

`GTA5Control` values are GTA V placeholders, not confirmed GTA VI action identifiers. Mock control and world behavior is described in the [script layer design](../script_layer.md#mock-world-offline).

## Runtime and native calls

`runtime.use_mock(**kwargs)` selects the pure Python mock. `runtime.use_cabi(dll_path, allow_unverified=False)` selects a C ABI backend; `runtime.current()` returns the active runtime or raises `RuntimeError` if none is selected. `Runtime.call_count`, `build_info()`, and `now_ms()` expose runtime information. The mock accepts unverified calls. Other backends reject statuses other than `verified` unless explicitly configured with `allow_unverified=True`; this opt-in does not validate a signature or make a call safe.

Generated raw wrappers are available through `from gta6mod import natives as N`; generated clean aliases are available as `gta6mod.game.names`. `names.RAW_TO_CLEAN` maps raw names to clean names. Consult the [native index](../api/index.md) for exact signatures and status, and the [marshalling contract](../native-signatures.md) for out parameters and slot layouts. The helpers retain the same status checks as raw wrappers. In/out entity pointer helpers are not currently provided.

## Command-line reference

Run `python -m gta6mod --help` or append `--help` to a subcommand. `run MOD` defaults to the mock and 5000 ms. It accepts `--ms`, `--backend mock` or `--backend cabi:PATH`, and `--allow-unverified`. Repeat mock scenario flags `--press CONTROL@MS`, `--damage N@MS`, `--wanted N@MS`, and `--waypoint X,Y,Z@MS` to schedule events. Unsupported scenario events are ignored with a notice. The command returns a nonzero status if a script errors. Other commands include `info`, `rpf9`, `rds5`, `joaat`, `anchors`, `patterns`, and `natives`.
