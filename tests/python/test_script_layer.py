"""Script layer tests: generated clean names, helpers, Mod, and the five example mods on the mock world.

    python -m unittest discover -s sdk/tests/python -v
"""
import os
import runpy
import sys
import unittest

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(SDK, "python"))
sys.path.insert(0, os.path.join(SDK, "generators"))

try:
    import gen_names  # noqa: E402  (private tree only; the public SDK ships without generators)
except ImportError:
    gen_names = None
needs_gen = unittest.skipIf(gen_names is None, "sdk/generators is not part of this checkout")
from gta6mod import runtime, scenario, script  # noqa: E402
from gta6mod import natives as N  # noqa: E402
from gta6mod.game import (Entity, Mod, Ped, Vector3, Vehicle, clock, models, names,  # noqa: E402
                          player, wait, world)

EXAMPLES = os.path.join(SDK, "examples", "python")


def fresh(**kw):
    """New quiet mock runtime and an empty default scheduler."""
    rt = runtime.use_mock(quiet=True, **kw)
    return rt, script.reset()


class TestCleanNames(unittest.TestCase):
    @needs_gen
    def test_committed_output_is_current(self):
        self.assertEqual(gen_names.main(["--check"]), 0, "run: python -I sdk/generators/gen_names.py")

    def test_observed_hash_wins_over_seed(self):
        self.assertIs(names.player.ped_id, N.PLAYER.PLAYER_PED_ID_0EE0)
        self.assertIs(names.player.ped_id_88a6, N.PLAYER.PLAYER_PED_ID)
        self.assertIs(names.entity.get_coords, N.ENTITY.GET_ENTITY_COORDS_F44D)
        self.assertEqual(names.RAW_TO_CLEAN["ENTITY.GET_ENTITY_COORDS"], "entity.get_coords_0d5a")

    def test_namespace_word_and_underscore_are_dropped(self):
        self.assertIs(names.map.get_waypoint_coords, N.MAP._GET_WAYPOINT_COORDS)
        self.assertIs(names.entity.does_exist, N.ENTITY.DOES_ENTITY_EXIST_1677)
        self.assertIs(names.vehicle.set_fixed, N.VEHICLE.SET_VEHICLE_FIXED)

    def test_every_clean_name_resolves_once(self):
        values = list(names.RAW_TO_CLEAN.values())
        self.assertEqual(len(values), len(set(values)))
        for raw, clean in names.RAW_TO_CLEAN.items():
            ns, fn = clean.split(".")
            rns, rfn = raw.split(".")
            self.assertIs(getattr(getattr(names, ns), fn), getattr(getattr(N, rns), rfn), raw)

    @needs_gen
    def test_rules_on_synthetic_input(self):
        def nat(ns, name, h, status="observed"):
            return {"namespace": ns, "name": name, "hash": h, "status": status}
        table = gen_names.build([
            nat("ENTITY", "GET_ENTITY_COORDS", "0x3FEF770D40960D5A", "unverified"),
            nat("ENTITY", "GET_ENTITY_COORDS_F44D", "0xA86D5F069399F44D"),
            nat("ENTITY", "GET_COORDS", "0x0000000000000001"),       # stripping would collide
            nat("PLAYER", "PLAYER_ID", "0x0000000000000002"),
            nat("PLAYER", "_SET_PLAYER_THING", "0x0000000000000003"),
            nat("PLAYER", "N_00000000000000AB", "0x00000000000000AB"),  # unnamed: skipped
        ])
        clean = {(ns, m["raw"]): m["clean"] for ns, ms in table.items() for m in ms}
        self.assertEqual(clean[("ENTITY", "GET_ENTITY_COORDS_F44D")], "GET_ENTITY_COORDS")
        self.assertEqual(clean[("ENTITY", "GET_ENTITY_COORDS")], "GET_ENTITY_COORDS_0D5A")
        self.assertEqual(clean[("ENTITY", "GET_COORDS")], "GET_COORDS")
        self.assertEqual(clean[("PLAYER", "PLAYER_ID")], "ID")
        self.assertEqual(clean[("PLAYER", "_SET_PLAYER_THING")], "SET_THING")
        self.assertNotIn(("PLAYER", "N_00000000000000AB"), clean)
        self.assertEqual(gen_names.camel("SET_THING"), "setThing")

    @needs_gen
    def test_hash_fragment_collisions_extend_the_suffix(self):
        nat = [{"namespace": "X", "name": "FOO", "hash": f"0x{i:08X}0000ABCD", "status": "observed"}
               for i in range(1, 5)]
        nat.append({"namespace": "X", "name": "GET_X_1", "hash": "0x0000000000000009", "status": "observed"})
        nat.append({"namespace": "X", "name": "GET_X1", "hash": "0x000000000000000A", "status": "observed"})
        ms = gen_names.build(nat)["X"]
        snakes = [gen_names.snake(m["clean"]) for m in ms]
        camels = [gen_names.camel(m["clean"]) for m in ms]
        self.assertEqual(len(set(snakes)), len(ms), snakes)
        self.assertEqual(len(set(camels)), len(ms), camels)


class TestScenario(unittest.TestCase):
    def test_rejects_out_of_range_amounts(self):
        for kind, spec in (("damage", "-5@0"), ("damage", "2147483648@0"), ("wanted", "-1@0")):
            with self.assertRaises(ValueError):
                scenario.parse(kind, spec)
        self.assertEqual(scenario.parse("damage", "2147483647@16").value, 2147483647)


class TestHelpers(unittest.TestCase):
    def setUp(self):
        self.rt, self.sched = fresh()
        self.mock = self.rt.backend

    def run_gen(self, gen, ms=2000):
        """Drive one helper generator to completion on the mock clock; return its value."""
        box = {}

        def runner():
            box["v"] = yield from gen
        self.sched.add("t", runner)
        scenario.drive(self.sched, self.mock, ms)
        self.sched.remove("t")
        return box["v"]

    def test_entity_wrapper(self):
        self.assertFalse(Entity(0))
        ped = player.ped()
        self.assertIsInstance(ped, Ped)
        self.assertTrue(ped.exists)
        self.assertEqual(ped.position, Vector3(-1000.0, -2000.0, 15.0))
        ped.position = (1.0, 2.0, 3.0)
        ped.heading = 90.0
        self.assertEqual(self.mock.player.pos, Vector3(1.0, 2.0, 3.0))
        ahead = ped.offset(0.0, 5.0, 0.0)  # heading 90 faces -x
        self.assertAlmostEqual(ahead.x, -4.0, places=4)
        self.assertAlmostEqual(ahead.y, 2.0, places=4)
        ped.health = 50
        self.assertEqual(ped.health, 50)
        ped.heal()
        self.assertEqual(ped.health, ped.max_health)
        ped.set_invincible(True)
        self.assertTrue(self.mock.player.invincible)

    def test_vehicle_spawn_and_seat(self):
        veh = self.run_gen(Vehicle.spawn("adder", Vector3(5, 6, 7), 10.0))
        self.assertIsInstance(veh, Vehicle)
        self.assertEqual(veh.max_health, 1000)
        player.ped().put_into(veh)
        self.assertEqual(player.vehicle(), veh)
        player.teleport(Vector3(10, 20, 30))  # moves the vehicle and its rider
        self.assertEqual(veh.position, Vector3(10, 20, 30))
        self.assertEqual(player.position(), Vector3(10, 20, 30))

    def test_spawn_times_out(self):
        self.rt, self.sched = fresh(model_load_ms=10_000)
        self.mock = self.rt.backend
        self.assertIsNone(self.run_gen(Vehicle.spawn("adder", Vector3(), timeout_ms=200), ms=1000))
        self.assertFalse([e for e in self.mock.world.entities.values() if e.kind == "vehicle"])

    def test_find_ground(self):
        self.assertIsNone(world.ground_z(1.0, 1.0))  # collision not loaded yet
        self.assertEqual(self.run_gen(world.find_ground(1.0, 1.0)), 12.0)
        self.assertIsNone(self.run_gen(world.find_ground(2.0, 2.0, 300, probe_z=5.0)))  # below ground

    def test_clock_and_wanted(self):
        clock.add(13, 30, 0)
        self.assertEqual(clock.time(), (1, 30, 0))  # wraps at 24 h
        self.mock.set_wanted(4)
        self.assertEqual(player.wanted_level(), 4)
        player.set_max_wanted(2)
        self.assertEqual(player.wanted_level(), 2)
        player.clear_wanted()
        self.assertEqual(player.wanted_level(), 0)

    def test_wanted_helpers_pass_the_player_ped(self):
        """G-090: GET/CLEAR_PLAYER_WANTED_LEVEL get the player ped's two handle slots, not the player index."""
        from gta6mod.runtime.marshal import entity_slots
        get, clear = 0xABC532F9098BFD9D, 0x4E4B996C928C7AA6
        self.assertEqual((N.BY_HASH[get][2], N.BY_HASH[clear][2]), ("e>i", "e>n"))
        self.mock.set_wanted(3)
        self.assertEqual(player.wanted_level(), 3)
        player.clear_wanted()
        ped = entity_slots(self.mock.player_ped)
        sent = [(h, s) for h, s in self.mock.calls if h in (get, clear)]
        self.assertEqual(sent, [(get, ped), (clear, ped)])
        self.assertEqual(self.mock.world.wanted_level, 0)

    def test_wanted_natives_ignore_a_non_player_argument(self):
        """The mock follows the game: a null or non-player ped reads 0 and clears nothing (as the old id() call did)."""
        self.mock.set_wanted(2)
        self.assertEqual(names.player.get_wanted_level(0), 0)            # player index 0 as a handle: null ped
        names.player.clear_wanted_level(0)
        self.assertEqual(self.mock.world.wanted_level, 2)
        self.assertEqual(names.player.get_wanted_level(self.mock.player_ped), 2)

    def test_models_hash(self):
        self.assertEqual(models.hash_of("adder"), 0xB779A091)
        self.assertEqual(models.hash_of(0x1_B779A091), 0xB779A091)

    def test_player_id_uses_the_observed_index_native(self):
        """G-082: player.id() calls GET_PLAYER_INDEX (observed handler 0x144501320), not the GTA V seed."""
        self.assertEqual(player.id(), 0)
        self.assertEqual(self.mock.calls[-1][0], 0x47E385B0D957C8D4)
        self.assertEqual(N.BY_HASH[0x47E385B0D957C8D4][3], "observed")

    def test_release_and_delete_clear_the_handle(self):
        """G-084: the in/out Entity* natives get the handle and the game's write-back replaces it."""
        w = self.mock.world
        a = self.run_gen(Vehicle.spawn("lifetime_a", Vector3(1, 2, 3)))
        keep_a = a.handle
        a.release()
        self.assertFalse(a)  # handle cleared
        self.assertIn(keep_a, w.entities)  # still in the world, no longer the script's
        self.assertFalse(w.entities[keep_a].script_owned)
        b = self.run_gen(Vehicle.spawn("lifetime_b", Vector3(4, 5, 6)))
        keep_b = b.handle
        b.delete()
        self.assertFalse(b)
        self.assertNotIn(keep_b, w.entities)
        again = Vehicle(keep_a)
        again.delete()  # not the script's any more: the handle is cleared, the entity stays
        self.assertFalse(again)
        self.assertIn(keep_a, w.entities)
        ped = player.ped()
        ped.release()  # the player ped is not script-created: the game keeps the handle
        self.assertEqual(ped.handle, self.mock.player_ped)
        ped.delete()
        self.assertFalse(ped)
        self.assertIn(self.mock.player_ped, w.entities)
        delete = 0x4CD38C78BD19A497  # DELETE_ENTITY: one pointer slot, 16-byte aligned scratch is not required
        self.assertEqual(len([s for h, s in self.mock.calls if h == delete][0]), 1)


class TestMod(unittest.TestCase):
    def setUp(self):
        self.rt, self.sched = fresh()
        self.mock = self.rt.backend

    def test_toggle_every_key_and_stop(self):
        m = Mod("t")
        t = m.toggle(47)
        seen = []
        t.on_enable(lambda: seen.append("on"))
        t.on_disable(lambda: seen.append("off"))
        ticks = []
        m.every(100, when=t)(lambda: ticks.append(self.mock.now_ms()))

        @m.on_key(52)
        def slow():
            seen.append("key")
            yield wait(200)
            seen.append("key done")

        events = [scenario.parse("press", "47@96"), scenario.parse("press", "52@400"),
                  scenario.parse("press", "47@800")]
        scenario.drive(self.sched, self.mock, 1000, events)
        self.assertEqual(seen, ["on", "key", "key done", "off"])
        self.assertTrue(ticks and all(96 <= x < 900 for x in ticks))
        t.set(True)
        m.stop()
        self.assertEqual(seen[-1], "off")
        self.assertEqual(self.sched.scripts, [])
        m.stop()  # idempotent

    def test_waiting_handler_does_not_block_other_keys(self):
        m = Mod("t")
        flips = []
        t = m.toggle(47)
        t.on_enable(lambda: flips.append(self.mock.now_ms()))
        runs = []

        @m.on_key(52)
        def slow():
            runs.append(self.mock.now_ms())
            yield wait(500)

        events = [scenario.parse("press", "52@96"), scenario.parse("press", "47@208"),
                  scenario.parse("press", "52@304"),  # ignored: the first slow() is still running
                  scenario.parse("press", "52@800")]
        scenario.drive(self.sched, self.mock, 1000, events)
        self.assertEqual(flips, [208])
        self.assertEqual(runs, [96, 800])

    def test_stop_cancels_first_and_runs_every_cleanup(self):
        m = Mod("t")
        a, b = m.toggle(47), m.toggle(52)
        cleaned = []

        @a.on_disable
        def boom():
            raise RuntimeError("boom")
        b.on_disable(lambda: cleaned.append("b"))
        m.on_stop(lambda: cleaned.append("stop"))
        m.every(16)(lambda: cleaned.append("tick"))
        a.set(True)
        b.set(True)
        errors = m.stop()
        self.assertEqual([str(e) for e in errors], ["boom"])
        self.assertEqual(cleaned, ["b", "stop"])
        self.assertFalse(a.on or b.on)
        self.assertEqual(self.sched.scripts, [])
        self.assertIn("stop: cleanup failed: RuntimeError('boom')", [msg for _, _, msg in self.mock.logs])
        scenario.drive(self.sched, self.mock, 100)
        self.assertNotIn("tick", cleaned)

    def test_host_stop_event_stops_every_mod(self):
        """G-086: the scheduler's stop event (host unload) runs Mod.stop: toggles off, scripts cancelled."""
        m = Mod("a")
        t = m.toggle(47)
        log = []
        t.on_disable(lambda: log.append("a off"))
        m.every(16)(lambda: log.append("tick"))

        @m.on_key(52)
        def waiting():
            yield wait(10_000)
            log.append("resumed")

        m2 = Mod("b")
        m2.on_stop(lambda: log.append("b stop"))
        scenario.drive(self.sched, self.mock, 64, [scenario.parse("press", "47@0"), scenario.parse("press", "52@16")])
        self.assertTrue(t.on)
        log.clear()
        self.assertEqual(script.shutdown(), [])
        self.assertEqual(log, ["a off", "b stop"])
        self.assertTrue(m.stopped and m2.stopped and not t.on)
        self.assertTrue(all(s.done for s in self.sched.scripts))
        scenario.drive(self.sched, self.mock, 20_000)
        self.assertEqual(log, ["a off", "b stop"])  # nothing ran again, the waiting handler never resumed
        self.assertEqual(script.shutdown(), [])  # raised once

    def test_host_stop_event_reports_failures(self):
        m = Mod("a")
        t = m.toggle(47)

        @t.on_disable
        def boom():
            raise RuntimeError("boom")
        self.sched.on_stop(lambda: 1 / 0)
        ran = []
        self.sched.on_stop(lambda: ran.append(1))
        t.set(True)
        errors = self.sched.stop()
        self.assertEqual(sorted(type(e).__name__ for e in errors), ["RuntimeError", "ZeroDivisionError"])
        self.assertEqual(ran, [1])  # later handlers still ran
        msgs = [msg for _, _, msg in self.mock.logs]
        self.assertIn("stop: cleanup failed: RuntimeError('boom')", msgs)
        self.assertTrue(any(msg.startswith("stop: handler failed: ZeroDivisionError") for msg in msgs))

    def test_stop_while_handler_waits_for_model(self):
        self.rt, self.sched = fresh(model_load_ms=500)
        self.mock = self.rt.backend
        m = Mod("t")
        made = []

        @m.on_key(52)
        def spawn():
            veh = yield from Vehicle.spawn("adder", Vector3(), timeout_ms=2000)
            made.append(veh)

        scenario.drive(self.sched, self.mock, 160, [scenario.parse("press", "52@0")])
        m.stop()
        scenario.drive(self.sched, self.mock, 1000)
        self.assertEqual(made, [])
        self.assertFalse([e for e in self.mock.world.entities.values() if e.kind == "vehicle"])
        release = 0x4AD96EF928BD4F9A  # SET_MODEL_AS_NO_LONGER_NEEDED_4F9A
        self.assertEqual(sum(1 for h, _ in self.mock.calls if h == release), 1)


class TestExampleMods(unittest.TestCase):
    """The five examples, with the scenarios from their docstrings (same as the Luau runner checks)."""

    def run_mod(self, name, ms, **events):
        rt, sched = fresh()
        evs = [scenario.parse(kind, spec) for kind, specs in events.items() for spec in specs]
        ns = runpy.run_path(os.path.join(EXAMPLES, f"{name}.py"), run_name="__gta6mod__")
        scenario.drive(sched, rt.backend, ms, evs)
        self.assertEqual([s.name for s in sched.scripts if s.error], [])
        return rt.backend, [f"[{lvl}] {src}: {msg}" for lvl, src, msg in rt.backend.logs], ns

    def test_god_mode(self):
        mock, logs, _ = self.run_mod("god_mode", 4000, damage=["50@496", "50@2000", "30@3504"],
                                     press=["INPUT_DETONATE@1008", "INPUT_DETONATE@3008"])
        self.assertEqual(logs, ["[INFO] god_mode: god mode ON", "[INFO] god_mode: god mode OFF"])
        self.assertEqual(mock.player.health, 170)  # 200 -50, healed, blocked, off, -30
        self.assertFalse(mock.player.invincible)
        self.assertTrue(mock.player.can_ragdoll)

    def test_god_mode_tops_up_and_follows_new_ped(self):
        rt, sched = fresh()
        runpy.run_path(os.path.join(EXAMPLES, "god_mode.py"), run_name="__gta6mod__")
        mock = rt.backend
        scenario.drive(sched, mock, 304, [scenario.parse("press", "INPUT_DETONATE@0")])
        mock.player.invincible = False  # something outside the mod clears the flag
        mock.damage_player(80)
        new_ped = mock.world.spawn("ped", 1, Vector3())
        mock.player_ped = new_ped  # character switch
        mock.world.entities[new_ped].health = 10
        scenario.drive(sched, mock, 304)
        msgs = [m for _, _, m in mock.logs]
        self.assertIn("new player ped, god mode reapplied", msgs)
        self.assertIn("health topped up to 200", msgs)
        self.assertTrue(mock.world.entities[new_ped].invincible)

    def test_never_wanted(self):
        mock, logs, _ = self.run_mod("never_wanted", 4000, press=["INPUT_VEH_HEADLIGHT@496", "INPUT_VEH_HEADLIGHT@3008"], wanted=["3@1504"])
        self.assertEqual(logs, [
            "[INFO] never_wanted: never wanted ON (max wanted level was 5)",
            "[INFO] never_wanted: cleared wanted level 3",
            "[INFO] never_wanted: never wanted OFF (max wanted level restored to 5)",
        ])
        self.assertEqual((mock.world.wanted_level, mock.world.max_wanted_level), (0, 5))

    def test_waypoint_teleport(self):
        mock, logs, _ = self.run_mod("waypoint_teleport", 3000, press=["INPUT_VEH_DUCK@496", "INPUT_VEH_DUCK@1504"],
                                     waypoint=["250,-400,0@1008"])
        self.assertEqual(logs, ["[WARN] waypoint_teleport: no waypoint set",
                                "[INFO] waypoint_teleport: teleported to 250.0 -400.0 13.0"])
        self.assertEqual(mock.player.pos, Vector3(250.0, -400.0, 13.0))

    def test_vehicle_spawner(self):
        mock, logs, _ = self.run_mod("vehicle_spawner", 3000, press=["INPUT_CONTEXT_SECONDARY@496"], damage=["150@2000"])
        self.assertEqual(logs, ["[INFO] vehicle_spawner: spawned adder and seated the player",
                                "[INFO] vehicle_spawner: repaired vehicle (850 -> 1000)"])
        veh = mock.world.entities[mock.player.vehicle]
        self.assertEqual((veh.kind, veh.health), ("vehicle", 1000))
        self.assertAlmostEqual(veh.pos.y, -1995.0, places=4)  # 5 m ahead at heading 0 (+y)
        self.assertEqual(mock.player.health, 50)  # the same damage hit the driver, who survives

    def test_time_control(self):
        # taps cycle the scale; a 1200 ms press is no tap but resets after 1000 ms held (at 2608)
        mock, logs, ns = self.run_mod("time_control", 3000, press=[
            "INPUT_MULTIPLAYER_INFO@496", "INPUT_MULTIPLAYER_INFO@1008", "INPUT_SPECIAL_ABILITY@1504",
            "INPUT_MULTIPLAYER_INFO@1600+1200"])
        self.assertEqual(logs, ["[INFO] time_control: time scale 0.50",
                                "[INFO] time_control: time scale 0.20",
                                "[INFO] time_control: clock 13:00",
                                "[INFO] time_control: time scale reset to 1.00"])
        self.assertEqual(mock.world.time_scale, 1.0)
        ns["m"].stop()
        self.assertEqual(len(mock.logs), len(logs))  # already 1.0: stop logs nothing

    def test_time_control_stop_resets(self):
        mock, logs, ns = self.run_mod("time_control", 600, press=["INPUT_MULTIPLAYER_INFO@96"])
        self.assertAlmostEqual(mock.world.time_scale, 0.5, places=6)
        ns["m"].stop()
        self.assertEqual(mock.world.time_scale, 1.0)
        self.assertEqual(mock.logs[-1][2], "time scale reset to 1.00")


if __name__ == "__main__":
    unittest.main()
