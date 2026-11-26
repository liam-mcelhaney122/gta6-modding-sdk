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
| mod structure | `m = Mod("name")`, `@m.loop`, `@m.every(ms, when=toggle)`, `@m.on_key(key, mods=)`, `@m.on_release(key, mods=, max_ms=)`, `@m.on_hold(key, ms, mods=, repeat_ms=)`, `m.block(*actions, when=)`, `t = m.toggle(key, label, mods=)` with `@t.on_enable` / `@t.on_disable`, `@m.on_stop`, `m.log/warn/error`, `m.stop()` | `local m = game.mod("name")`, `m.loop(fn)`, `m.every(ms, fn, toggle?)`, `m.onKey(key, fn, opts?)`, `m.onRelease(key, fn, opts?)`, `m.onHold(key, ms, fn, opts?)`, `m.block(actions, opts?)`, `t = m.toggle(key, label?, opts?)` with `t.onEnable(fn)` / `t.onDisable(fn)`, `m.onStop(fn)`, `m.log/warn/error`, `m.stop()` |
| timing | `wait(ms)`, `now()`, `log(msg, level, source)` | `game.wait(ms)`, `game.now()`, `game.log(msg, level?, source?)` |
| player | `player.ped()`, `.id()`, `.position()`, `.teleport(pos, heading=None, with_vehicle=True)`, `.vehicle()`, `.wanted_level()`, `.clear_wanted()`, `.max_wanted()`, `.set_max_wanted(n)` | same in camelCase |
| entities | `Entity`, `Ped`, `Vehicle`; properties `exists`, `position`, `heading`, `health`, `max_health`, `is_dead`, `speed`, `forward`, `model`, `visible` (position, heading, health and visible are settable); methods `heal`, `set_invincible`, `freeze`, `set_velocity`, `offset(x, y, z)`, `distance_to`, `delete()`, `release()` | same objects; reads are methods (`ped:health()`), writes are `set…` methods; `ent:delete()`, `ent:release()` |
| peds | `set_can_ragdoll`, `vehicle()`, `in_vehicle()`, `put_into(vehicle, seat=SEAT_DRIVER)` | same |
| vehicles | `Vehicle.spawn(model, pos, heading, timeout_ms)` (generator), `repair()` | `game.Vehicle.spawn(...)` (blocks the calling script with `wait`) |
| streaming | `models.hash_of`, `.is_valid`, `.request(model, timeout_ms)` (generator), `.release(model)` | `game.models.hashOf/isValid/request/release` |
| world | `world.waypoint()`, `.ground_z(x, y)`, `.find_ground(x, y, timeout_ms)` (generator), `.request_collision(pos)`, `.set_time_scale(s)` | same |
| clock | `clock.time()`, `clock.set_time(h, m, s)`, `clock.add(h, m, s)`, `clock.pause(on)` | same |
| input | `controls.just_pressed(action, pad)`, `.pressed`, `.just_released`, `.released`, `.normal`, `.disable`, `.enable`; actions `Control.*`, groups `ControlGroup.*` (`gta6mod.controls`, [reference](api/controls.md)) | same; tables in `@gta6/controls`, re-exported as `game.Control` / `game.ControlGroup` |
| offline | `gta6mod.scenario` (timed mock events), `script.reset()`, `MockBackend(quiet=True).logs` | `g6_luau_runner` flags |

Two language rules:

- **Python** waits with generators, so anything that waits is `yield from helper(...)`. A plain function
  is called once per tick. `Mod.on_key`, `on_release` and `on_hold` handlers may be plain functions or generators.
- **Luau** waits by yielding the script's coroutine, so helpers that wait (`models.request`,
  `Vehicle.spawn`) just block the calling script.

`Mod` rules, the same in both languages:

- One input script per mod polls every binding each frame. A key handler that waits (a Python generator;
  any Luau handler except a toggle flip) runs as its own script `<mod>.keyN`, so it never delays other
  bindings. A binding whose handler is still running does not fire.
- Hotkeys (`gta6mod/game/hotkeys.py`, `@gta6/hotkeys`; the same rules in both): a **press** binding fires
  when the action is just pressed; a **release** binding when it is just released, and with `max_ms` /
  `maxMs` only after a press at most that long (a tap) that the mod saw; a **hold** binding once the action
  has been held `ms` since a press the mod saw, then every `repeat_ms` / `repeatMs` while still held. An
  action already held when the mod starts counts only from its next press. Every binding takes modifier
  actions (`mods`) that must be held at that moment. **The most specific chord wins**: of the bindings of
  one kind on one action that fire on a tick, one whose modifiers are a strict subset of another's is
  dropped, and a press that fired a chord also suppresses release and hold bindings of that action with
  fewer modifiers until it is released (so `SPRINT`+`CONTEXT` is not also a tap of `CONTEXT`). Pressing the
  modifier after the key is not a chord.
- `block` disables its actions (`DISABLE_CONTROL_ACTION`, related actions off) every tick while `when` is
  on (a toggle, a function, or none for always), after the mod's bindings have been read that tick. It stops
  with the mod; nothing is re-enabled, because a disable lasts one frame. That other scripts and the game
  then see the action as disabled for the rest of the frame is GTA V behaviour, not verified on GTA VI.
- `stop()` first cancels every script of the mod: a suspended handler never resumes (Python closes the
  generator; Luau uses the host's `script.cancel`), and a model request it was waiting on is released.
  Then it turns toggles off and runs `on_stop` handlers. Each callback runs even if an earlier one
  raised; failures are logged as `stop: cleanup failed: <error>` and returned. Called from one of the
  mod's own scripts, that script ends at its next wait.
- `models.request` releases its request again when it times out.
- **Stop event.** The host calls `stop()` itself when it unloads the scripts: Python `Scheduler.stop()` /
  `script.shutdown()` (raised by `python -m gta6mod run` when the run ends), Luau `g6_luau_stop` (the Luau
  plugin's `g6_plugin_stop` export, and `g6_luau_destroy` when it was not raised). Every `Mod` / `game.mod`
  registers its `stop()` there when it is created (`Scheduler.on_stop`, `@gta6/script.onStop`). Stop handlers run
  once each, in registration order, then every script still alive is cancelled. The in-game runtime calls
  `g6_plugin_stop` on the game script thread before `g6_plugin_unload` (`gta6mod.h`, API 0.2), so toggle cleanup
  can still call natives; without a script tick within 1 s it calls it from the shutdown thread, where natives are
  refused.

Entity wrappers hold the 16-byte handle unchanged (Python int, Luau buffer). They never build or modify a
handle themselves. A wrapper whose handle is 0 (`lo == 0`) is falsy in Python (`if not veh:`), and `isValid()` in Luau.
`delete()` (DELETE_ENTITY) and `release()` (SET_ENTITY_AS_NO_LONGER_NEEDED) pass the handle through the in/out
code `X` and store what the game wrote back, so the wrapper turns invalid when the game cleared the handle.

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
- Entities created by a script (vehicles from the create native) are script-owned; the player ped is not.
  `DELETE_ENTITY` / `_PED` / `_VEHICLE` / `_OBJECT` remove a script-owned entity (and take peds out of it) and
  always write a zero handle back. `SET_*_AS_NO_LONGER_NEEDED` clears the script ownership and writes a zero handle
  back only for a script-owned entity; otherwise the handle is left as it was.
  `SET_ENTITY_COORDS` on a vehicle also moves the peds in it. `SET_VEHICLE_FIXED` sets health to max.
- `REQUEST_COLLISION_AT_COORD((x, y, z))` marks the cell `(round(x), round(y))` (half away from zero) loaded 100 ms after the first
  request. `GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD((x, y, z))` returns true, ground 12.0 and normal (0, 0, 1)
  only when that cell is loaded and `z > 12`; otherwise false, 0 and (0, 0, 0).
- Wanted level starts 0, max 5. `SET_MAX_WANTED_LEVEL(n)` stores `n` and lowers the current level to at
  most `n`. `CLEAR_PLAYER_WANTED_LEVEL` sets 0. `PLAYER_ID` and `GET_PLAYER_INDEX` return 0.
- Clock starts 12:00:00 and does not advance by itself. `ADD_TO_CLOCK_TIME` normalises and wraps at 24 h.
  `PAUSE_CLOCK` and `SET_TIME_SCALE` only store their value.
- Controls on group 0 only. A press is just pressed on its tick and held until its release time
  (`MS + HOLD`, or the press tick itself without `+HOLD`); it is just released on the first tick at or
  after that time. `IS_CONTROL_PRESSED` is true while held, `IS_CONTROL_RELEASED` while not held.
  `DISABLE_CONTROL_ACTION` does nothing (tests count its calls).

Scenario flags for `python -m gta6mod run` and `g6_luau_runner` (all `@MS` times in mock game time; an
event fires on the tick whose time equals `MS`, so use multiples of 16):
`--press CTL@MS[+HOLD]` (CTL is an `INPUT_*` name, hashed like `GET_HASH_KEY`, or a number: decimal or `0x` hex; HOLD is how long it stays held, in ms); `--damage N@MS` (0 ≤ N ≤ 2147483647, as for `--wanted`) lowers the player ped's health by N (not below 0) unless it is
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
- **time_control**, actions `INPUT_MULTIPLAYER_INFO` and `INPUT_SPECIAL_ABILITY`. Time scales cycle 1.0 → 0.5 → 0.2 → 1.0.
  `INPUT_MULTIPLAYER_INFO` tapped (released at most 300 ms after the press): set the next scale, log
  `time scale <s %.2f>`. `INPUT_MULTIPLAYER_INFO` held 1000 ms, and on stop: if the scale is not 1.0, set 1.0
  and log `time scale reset to 1.00`. `INPUT_SPECIAL_ABILITY`: `clock.add(1, 0, 0)`, log `clock <HH>:<MM>`.

## Example mods (phase 1 deliverable)

Each mod exists in Python (`sdk/examples/python/<mod>.py`) and Luau (`sdk/luau/examples/<mod>/init.luau`),
with the same behaviour and the same log lines.

| Mod | Actions (GTA VI script keys; GTA V default key in brackets, GTA VI bindings unknown) | Shows |
|---|---|---|
| `god_mode` | `INPUT_DETONATE` (G) toggle | toggles, `every`, reapply when the player ped changes, clean disable |
| `never_wanted` | `INPUT_VEH_HEADLIGHT` (H) toggle | saving and restoring game state, periodic clearing |
| `waypoint_teleport` | `INPUT_VEH_DUCK` (X) teleport | waypoint read, collision request, ground probe with retry and timeout, teleport with vehicle |
| `vehicle_spawner` | `INPUT_CONTEXT_SECONDARY` (Q) spawn | model streaming, offsets, seating the player, auto-repair loop |
| `time_control` | `INPUT_MULTIPLAYER_INFO` (Z) tap: cycle time scale, hold 1 s: reset it; `INPUT_SPECIAL_ABILITY` +1 hour (GTA V's `SPECIAL_ABILITY_SECONDARY` does not exist in GTA VI) | tap and hold on one key, clock and time scale, restore on stop |

## Unknowns (kept visible, not guessed)

| Unknown | Where it shows | How to resolve |
|---|---|---|
| Default bindings of the actions. The action keys are confirmed statically (`findings/phase5/31_controls.md`; every example action is in the build's action table), but which button or key triggers each one in GTA VI is not known, and no press has been logged in game. | every mod's `KEY_*` | Log `IS_CONTROL_JUST_PRESSED` for the example actions in game (after G-002), or decode the control mapping data files (PSO structures that use `rage__InputType`). |
| Model names. `adder` is a GTA V name, kept as a placeholder. No story-script model native (`REQUEST_MODEL`, `HAS_MODEL_LOADED`, `IS_MODEL_*`, the vehicle-model natives) takes a constant model hash: 0 constants at their 290 call sites in the block-local scan, and report 29's data-flow walk found none of 113 model arguments constant (`findings/phase5/30_script_layer_evidence.md`). | `vehicle_spawner` | A live trace of the model argument (`REQUEST_MODEL` / `IS_MODEL_IN_CDIMAGE`) while the game spawns a vehicle, or the data files the scripts load models from; then `IS_MODEL_IN_CDIMAGE` live. |
| `CREATE_VEHICLE` exists only as a GTA V seed hash (`unverified`). No handler of the 6,829 has its GTA VI shape (model, packed position, heading float, 16-byte handle return), and no call site passes it a constant model (`findings/phase5/30_script_layer_evidence.md`). | `Vehicle.spawn` | Trace the script runtime's spawn path (class methods, report 26) or a live call; until then `Vehicle.spawn` keeps the seed native, which real backends refuse. |
| Driver seat `-1` (`SEAT_DRIVER`) is statically confirmed: the `SET_PED_INTO_VEHICLE` handler maps seat `s >= -1` to internal seat `s + 1` (`-2` = any free seat), and scripts pass `-1` at 197 of the 203 sites that seat the player ped (`findings/phase5/30_script_layer_evidence.md`). | `Ped.put_into` | One logged seating in game. |
| Player id: resolved statically. `player.id()` calls `GET_PLAYER_INDEX` (`0x144501320`), which returns 0 and feeds 3,113 direct 1-slot player arguments (`findings/phase5/30_script_layer_evidence.md`). The wanted-level natives take the player **ped** (every resolved site passes both slots of one 16-byte native result, mostly `PLAYER_PED_ID`), so `player.wanted_level()` / `clear_wanted()`, which pass `id()`, are wrong in game until those natives are typed as taking a `Ped`. | `player.wanted_level`, `player.clear_wanted` | Type `GET/CLEAR_PLAYER_WANTED_LEVEL` as `Ped` in re-db (outside this layer), then pass `player.ped()`. |
| Health scale (`200` ped, `1000` vehicle in the mock). | mocks only | The helpers use `max_health`, not constants. |

## Gaps the layer does not paper over

- **In/out entity pointers** (resolved, G-084). The in/out code `X` exists for the eight natives whose handlers
  read and write the pointer (`native-signatures.md`), and `delete()` / `release()` use it.
- **Status gating.** Nearly everything is `observed`, which real backends refuse without
  `allow_unverified`. The helpers do not bypass that.
- **Unload hook** (resolved, G-086): the stop event above. It is offline-tested only (mocks, `plugin_check`,
  the runtime self-test with fake plugins); no unload has run in game.
- **No on-screen text yet.** No notification or text-draw native is named in 1.0.2.1, so feedback goes to
  `log`.

## Phases

1. **This branch:** `gen_names.py`, the Python and Luau helper layers, mock world, scenario flags, the five
   examples, tests (Python unittest per mod, runner checks per Luau mod, stale-output test for names).
2. In/out pointer signature code and `delete()` / `release()` helpers (done, G-084).
3. Model names from the decompiled scripts. (Control actions: done, `gta6mod.controls` replaced `GTA5Control`; bindings still unknown.)
4. `Mod.stop()` driven by the runtime on unload (done, G-086); per-mod config files.
5. Docs: generate a names reference page (`gen_docs.py`) next to the raw native pages.
