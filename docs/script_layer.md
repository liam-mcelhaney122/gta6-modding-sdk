# Script layer plan: clean names and helpers

Branch `feature/sdk-script-layer`. This adds two layers on top of the generated natives, so a mod can say
`player.ped().health = 200` instead of
`N.ENTITY.SET_ENTITY_HEALTH(N.PLAYER.PLAYER_PED_ID_0EE0(), 200, 0, 0, 0)`.

```
mods ─────────────► gta6mod.game  /  @gta6/game        hand-written helpers (this plan, phase 1)
                        │
                        ▼
                    gta6mod.game.names  /  @gta6/names  GENERATED clean names (gen_names.py)
                        │
                        ▼
                    gta6mod.natives  /  @gta6/natives   GENERATED raw wrappers (gen_bindings.py, unchanged)
                        │
                        ▼
                    runtime (mock, C ABI, in-game)      unchanged
```

Nothing below the clean-name layer changes. Status checks (`verified` / `observed` / `unverified`), the
signature strings and the marshalling stay exactly as they are, so a helper is refused by a real backend
for the same reason the raw native would be.

## Problems with the raw surface

1. **Duplicate names with different hashes.** The seed table holds GTA V hashes (`unverified`, not in the
   GTA VI handler table). The real GTA VI handler, matched by exact hash to an RDR2 nativedb name, then gets a
   hash-fragment suffix: `PLAYER_PED_ID` (GTA V guess) versus `PLAYER_PED_ID_0EE0` (observed in 1.0.2.1).
   The plain name is the worse choice, and nothing tells a script author that.
2. **Redundant tokens.** `ENTITY.GET_ENTITY_COORDS`, `PLAYER.GET_PLAYER_WANTED_LEVEL`.
3. **Leading underscores** on community-named natives (`MAP._GET_WAYPOINT_COORDS`).
4. **Filler arguments** (`SET_ENTITY_HEALTH(entity, amount, killer, a5, a6)`), raw out-param tuples, 16-byte
   entity handles as bare ints or buffers, and model streaming loops that every mod rewrites.
5. **No mod structure.** Every mod hand-writes the poll loop, key edge detection, toggles and timers.

## Layer 1: clean names (generated)

`sdk/generators/gen_names.py` reads `sdk/data/generated/natives.index.json` and writes
`sdk/python/gta6mod/game/names.py` and `sdk/luau/lib/names.luau`. The output is plain aliases to the raw
wrappers, so there is no second call path.

Rules, applied in this order:

| Step | Rule | Example |
|---|---|---|
| skip | unnamed natives (`N_<hash>`, or a name that is only a hash such as `_0x0CB3…`) stay raw-only | `N.RAW.N_…` |
| group | the same base name in one namespace (after dropping a leading `_` and a `_XXXX` suffix that equals the hash's last 4 hex digits) is one group | `GET_ENTITY_COORDS`, `GET_ENTITY_COORDS_F44D` |
| pick | the group's best member gets the clean name: highest status (`verified` > `observed` > `unverified`), then no suffix, then lowest hash | `F44D` (observed) wins over the seed |
| others | every other member keeps a clean name with `_<last 4 hex of its hash>` | `get_coords_0d5a` |
| strip | drop the namespace word when it is the first token or directly follows the first token | `GET_ENTITY_COORDS` → `get_coords`, `PLAYER_PED_ID` → `ped_id` |
| keep | do not strip when that empties the name, makes it start with a digit, makes it a keyword, or collides | `PED.IS_PED_RAGDOLL` → `is_ragdoll`, collision → full name |
| case | namespace lower case; Python names `snake_case`, Luau names `camelCase` | `entity.get_coords` / `entity.getCoords` |

`names.RAW_TO_CLEAN` (Python) maps `"ENTITY.GET_ENTITY_COORDS_F44D"` to `"entity.get_coords"` for search and
docs. The generator is deterministic, and a test fails when the committed output is stale.

## Layer 2: helpers (hand-written)

`sdk/python/gta6mod/game/` and `sdk/luau/lib/game.luau`. Every helper calls the clean names, never a hash,
so a re-db update that verifies or renames a native changes nothing here.

| Area | Python | Luau |
|---|---|---|
| mod structure | `m = Mod("name")`, `@m.loop`, `@m.every(ms, when=toggle)`, `@m.on_key(key)`, `t = m.toggle(key, label)` with `@t.on_enable` / `@t.on_disable`, `@m.on_stop`, `m.log/warn/error`, `m.stop()` | `local m = game.mod("name")`, `m.loop(fn)`, `m.every(ms, fn, toggle?)`, `m.onKey(key, fn)`, `t = m.toggle(key, label?)` with `t.onEnable(fn)` / `t.onDisable(fn)`, `m.onStop(fn)`, `m.log/warn/error`, `m.stop()` |
| timing | `wait(ms)`, `now()`, `log(msg, level, source)` | `game.wait(ms)`, `game.now()`, `game.log(msg, level?, source?)` |
| player | `player.ped()`, `.id()`, `.position()`, `.teleport(pos, heading=None, with_vehicle=True)`, `.vehicle()`, `.wanted_level()`, `.clear_wanted()`, `.max_wanted()`, `.set_max_wanted(n)` | same in camelCase |
| entities | `Entity`, `Ped`, `Vehicle`; properties `exists`, `position`, `heading`, `health`, `max_health`, `is_dead`, `speed`, `forward`, `model`, `visible` (position, heading, health and visible are settable); methods `heal`, `set_invincible`, `freeze`, `set_velocity`, `offset(x, y, z)`, `distance_to` | same objects; reads are methods (`ped:health()`), writes are `set…` methods |
| peds | `set_can_ragdoll`, `vehicle()`, `in_vehicle()`, `put_into(vehicle, seat=SEAT_DRIVER)` | same |
| vehicles | `Vehicle.spawn(model, pos, heading, timeout_ms)` (generator), `repair()` | `game.Vehicle.spawn(...)` (blocks the calling script with `wait`) |
| streaming | `models.hash_of`, `.is_valid`, `.request(model, timeout_ms)` (generator), `.release(model)` | `game.models.hashOf/isValid/request/release` |
| world | `world.waypoint()`, `.ground_z(x, y)`, `.find_ground(x, y, timeout_ms)` (generator), `.request_collision(pos)`, `.set_time_scale(s)` | same |
| clock | `clock.time()`, `clock.set_time(h, m, s)`, `clock.add(h, m, s)`, `clock.pause(on)` | same |
| input | `controls.just_pressed(action)`, `.pressed`, `.just_released`, `.disable` | same |
| offline | `gta6mod.scenario` (timed mock events), `script.reset()`, `MockBackend(quiet=True).logs` | `g6_luau_runner` flags |

Two language rules:

- **Python** waits with generators, so anything that waits is `yield from helper(...)`. A plain function
  is called once per tick. `Mod.on_key` handlers may be plain functions or generators.
- **Luau** waits by yielding the script's coroutine, so helpers that wait (`models.request`,
  `Vehicle.spawn`) just block the calling script.

`Mod` rules, the same in both languages:

- One input script per mod polls every binding each frame. A key handler that waits (a Python generator;
  any Luau handler except a toggle flip) runs as its own script `<mod>.keyN`, so it never delays other
  bindings. A press on a binding whose handler is still running is ignored.
- `stop()` first cancels every script of the mod: a suspended handler never resumes (Python closes the
  generator; Luau uses the host's `script.cancel`), and a model request it was waiting on is released.
  Then it turns toggles off and runs `on_stop` handlers. Each callback runs even if an earlier one
  raised; failures are logged as `stop: cleanup failed: <error>` and returned. Called from one of the
  mod's own scripts, that script ends at its next wait.
- `models.request` releases its request again when it times out.

Entity wrappers hold the 16-byte handle unchanged (Python int, Luau buffer). They never build or modify a
handle. A wrapper whose handle is 0 (`lo == 0`) is falsy in Python (`if not veh:`), and `isValid()` in Luau.

## Mock world (offline)

The mods run offline in both mocks, which implement the same fake semantics:

- the **observed** hash of every seed native the mocks already implemented, next to the old seed hash;
- invincibility, health and max health, ragdoll and freeze flags, velocity, model, forward vector and
  entity offsets;
- wanted level and max wanted level (default 5);
- a waypoint, a collision request that loads after 100 ms, and flat ground at z = 12 that the ground probe
  finds only once collision is loaded;
- peds in vehicles (seat ignored), vehicle repair, vehicle health 1000;
- a game clock starting at 12:00:00, `ADD_TO_CLOCK_TIME`, `PAUSE_CLOCK`, and the time scale.

Mock semantics, identical in `runtime/mock.py` and `luau/host/g6_luau_runner.cpp`:

- Player ped handle `0x100` (hi = 0) at (-1000, -2000, 15), heading 0, health 200, max 200. New
  vehicles get the next handle, health 1000, max 1000. Every entity starts visible, not invincible,
  ragdoll allowed, not frozen, velocity 0.
- Forward vector for heading `h` degrees: `(-sin h, cos h, 0)`; right: `(cos h, sin h, 0)`.
  `GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(e, (x, y, z))` = `pos + right·x + forward·y + (0,0,z)`.
- Vector parameters arrive in the GTA VI layout their signature gives (`native-signatures.md`): the packed
  `w` form (two slots) for `SET_ENTITY_COORDS`, `SET_ENTITY_VELOCITY`, `GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS`,
  `REQUEST_COLLISION_AT_COORD` and `GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD`; three float slots only for the seed
  `CREATE_VEHICLE`, whose layout is unconfirmed. A `Vector3*` out-param must be 16-byte aligned; the mocks refuse
  (Python) or log an error for (Luau runner) a misaligned one.
- `SET_ENTITY_HEALTH` clamps to `[0, max]`. `IS_ENTITY_DEAD` is `health <= 0`. `GET_ENTITY_SPEED` is the
  length of the velocity. `IS_MODEL_VALID` is true for any non-zero hash.
- `SET_PED_INTO_VEHICLE` records the ped's vehicle and moves the ped to the vehicle's position.
  `SET_ENTITY_COORDS` on a vehicle also moves the peds in it. `SET_VEHICLE_FIXED` sets health to max.
- `REQUEST_COLLISION_AT_COORD((x, y, z))` marks the cell `(round(x), round(y))` (half away from zero) loaded 100 ms after the first
  request. `GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD((x, y, z))` returns true, ground 12.0 and normal (0, 0, 1)
  only when that cell is loaded and `z > 12`; otherwise false, 0 and (0, 0, 0).
- Wanted level starts 0, max 5. `SET_MAX_WANTED_LEVEL(n)` stores `n` and lowers the current level to at
  most `n`. `CLEAR_PLAYER_WANTED_LEVEL` sets 0. `PLAYER_ID` returns 0.
- Clock starts 12:00:00 and does not advance by itself. `ADD_TO_CLOCK_TIME` normalises and wraps at 24 h.
  `PAUSE_CLOCK` and `SET_TIME_SCALE` only store their value.
- `IS_CONTROL_PRESSED` is the same as just pressed (a mock press lasts one tick); `IS_CONTROL_JUST_RELEASED`
  is false; `DISABLE_CONTROL_ACTION` does nothing.

Scenario flags for `python -m gta6mod run` and `g6_luau_runner` (all `@MS` times in mock game time; an
event fires on the tick whose time equals `MS`, so use multiples of 16):
`--press CTL@MS`; `--damage N@MS` (0 ≤ N ≤ 2147483647, as for `--wanted`) lowers the player ped's health by N (not below 0) unless it is
invincible, and the same for the vehicle the ped is in; `--wanted N@MS` sets the wanted level to N (a crime:
the mock does not apply the max here); `--waypoint X,Y,Z@MS` sets the waypoint.

### Behaviour and log lines

Logs use the mod name as the source (`[INFO] god_mode: god mode ON`). Floats use `%.1f` / `%.2f`.

- **god_mode**, key 47 toggle.
  On: `set_invincible(true)`, `set_can_ragdoll(false)`, `heal()`, remember the ped, log `god mode ON`.
  Every 250 ms while on: if `player.ped()` differs from the remembered ped, reapply invincible and
  ragdoll to the new ped, remember it and log `new player ped, god mode reapplied`; then if
  `health < max_health`, `heal()` and log `health topped up to <max>`.
  Off: `set_invincible(false)`, `set_can_ragdoll(true)` on the current ped, log `god mode OFF`.
- **never_wanted**, key 74 toggle.
  On: save `max_wanted()`, `set_max_wanted(0)`, `clear_wanted()`, log
  `never wanted ON (max wanted level was <saved>)`.
  Every 500 ms while on: if `wanted_level() > 0`, `clear_wanted()` and log `cleared wanted level <n>`.
  Off: `set_max_wanted(saved)`, log `never wanted OFF (max wanted level restored to <saved>)`.
- **waypoint_teleport**, key 73.
  No waypoint: warn `no waypoint set`. Otherwise `find_ground(x, y, 1500)`; on failure warn
  `no ground found at <x> <y>; not teleporting`; on success `teleport((x, y, z + 1))` (moves the vehicle
  when the player is in one) and log `teleported to <x> <y> <z+1>`.
- **vehicle_spawner**, key 52; model `adder`.
  Spawn at `ped.offset(0, 5, 0)` with the ped's heading; on failure warn `could not spawn adder`; on
  success `put_into(veh)` and log `spawned adder and seated the player`.
  Every 1000 ms: if the player is in a vehicle with `health < max_health`, repair it and log
  `repaired vehicle (<before> -> <after>)`.
- **time_control**, keys 20 and 29. Time scales cycle 1.0 → 0.5 → 0.2 → 1.0.
  Key 20: set the next scale, log `time scale <s %.2f>`. Key 29: `clock.add(1, 0, 0)`, log `clock <HH>:<MM>`.
  On stop: if the scale is not 1.0, set 1.0 and log `time scale reset to 1.00`.

## Example mods (phase 1 deliverable)

Each mod exists in Python (`sdk/examples/python/<mod>.py`) and Luau (`sdk/luau/examples/<mod>/init.luau`),
with the same behaviour and the same log lines.

| Mod | Keys (GTA V control index, see below) | Shows |
|---|---|---|
| `god_mode` | 47 (G) toggle | toggles, `every`, reapply when the player ped changes, clean disable |
| `never_wanted` | 74 (H) toggle | saving and restoring game state, periodic clearing |
| `waypoint_teleport` | 73 (X) teleport | waypoint read, collision request, ground probe with retry and timeout, teleport with vehicle |
| `vehicle_spawner` | 52 (Q) spawn | model streaming, offsets, seating the player, auto-repair loop |
| `time_control` | 20 (Z) cycle time scale, 29 (B) +1 hour | clock and time scale, restore on stop |

## Unknowns (kept visible, not guessed)

| Unknown | Where it shows | How to resolve |
|---|---|---|
| Control ids. GTA VI's `IS_CONTROL_JUST_PRESSED(int, Hash action)` signature comes from RDR2, where the action is a hash. The examples use GTA V control **indices** as config values (`GTA5Control`). | every mod's `KEY_*` | Decompile `script.rpf` with `tools/phase4/decomp`, list the constant second arguments at `IS_CONTROL_JUST_PRESSED` / `IS_CONTROL_PRESSED` call sites, and match them to prompts and menus; confirm in game with a logging mod. |
| Model names. `adder` is a GTA V name. | `vehicle_spawner` | Pick a model hash referenced by a vehicle-spawning script (`CREATE_VEHICLE` call sites in the decompiled scripts) and check it with `IS_MODEL_IN_CDIMAGE`. |
| `CREATE_VEHICLE` and `PLAYER_ID` exist only as GTA V seed hashes (`unverified`). No observed handler is named yet. | `Vehicle.spawn`, `player.id()` | Find the handler by behaviour: script call sites with a model hash, coords and heading for the vehicle, and the native whose return feeds every `player`-int argument for the player id. Then add the names to re-db. |
| Seat index convention (-1 driver) for `SET_PED_INTO_VEHICLE`. | `Ped.put_into` | Read the seat argument at `SET_PED_INTO_VEHICLE` call sites in story scripts. |
| Health scale (`200` ped, `1000` vehicle in the mock). | mocks only | The helpers use `max_health`, not constants. |

## Gaps the layer does not paper over

- **In/out entity pointers.** `DELETE_ENTITY(Entity*)` and `SET_*_AS_NO_LONGER_NEEDED(Entity*)` are
  generated with the out-only code `E`, so the caller cannot pass the handle in. There is no `delete()` or
  `release()` helper until the signature grammar gets an in/out code (proposal: `X`, a 16-byte scratch
  buffer filled from the argument and read back). That is a change to `gen_bindings`, `marshal.py`,
  `g6_luau.cpp` and `natives.h`, and it needs its own tests.
- **Status gating.** Nearly everything is `observed`, which real backends refuse without
  `allow_unverified`. The helpers do not bypass that.
- **Unload hook.** The scheduler has no stop event, so `Mod.stop()` (which runs toggle `on_disable`
  handlers and removes the mod's scripts) is called by the mod or a test. The in-game runtime should call
  it on unload; that belongs to the runtime work, not this layer.
- **No on-screen text yet.** No notification or text-draw native is named in 1.0.2.1, so feedback goes to
  `log`.

## Phases

1. **This branch:** `gen_names.py`, the Python and Luau helper layers, mock world, scenario flags, the five
   examples, tests (Python unittest per mod, runner checks per Luau mod, stale-output test for names).
2. In/out pointer signature code and `delete()` / `release()` helpers.
3. Control ids and model names from the decompiled scripts, replacing `GTA5Control`.
4. `Mod.stop()` driven by the runtime on unload; per-mod config files.
5. Docs: generate a names reference page (`gen_docs.py`) next to the raw native pages.
