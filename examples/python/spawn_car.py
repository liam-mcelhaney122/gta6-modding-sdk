"""Example Python mod (same behaviour as sdk/luau/examples/spawn_car).

Run offline against the mock world:
    python -m gta6mod run sdk/examples/python/spawn_car.py --ms 5000 --press INPUT_CONTEXT@1008

All natives used here are UNVERIFIED for GTA VI (GTA V hashes). The mock
backend accepts them; a real backend needs allow_unverified until re-db
verifies them.
"""
from gta6mod import joaat, natives as N, script, runtime

CONTROL_CONTEXT = joaat("INPUT_CONTEXT")  # GTA VI action hash (sdk/docs/api/controls.md)


def spawn_vehicle(model_name: str):
    model = joaat(model_name)
    N.STREAMING.REQUEST_MODEL(model)
    deadline = runtime.current().now_ms() + 2000
    while not N.STREAMING.HAS_MODEL_LOADED(model):
        if runtime.current().now_ms() > deadline:
            runtime.current().log("WARN", "spawn_car", f"model {model_name} did not load")
            return
        yield script.wait(0)
    ped = N.PLAYER.PLAYER_PED_ID()
    pos = N.ENTITY.GET_ENTITY_COORDS(ped, True)
    veh = N.VEHICLE.CREATE_VEHICLE(model, pos.x + 3, pos.y, pos.z, 0.0, False, False, False)
    N.STREAMING.SET_MODEL_AS_NO_LONGER_NEEDED(model)
    runtime.current().log("INFO", "spawn_car", f"spawned {model_name} as handle {veh:#x}")


@script.register("spawn_car")
def main():
    while True:
        if N.PAD.IS_CONTROL_JUST_PRESSED(0, CONTROL_CONTEXT):
            yield from spawn_vehicle("adder")
        yield script.wait(0)


@script.register("where_am_i")
def where_am_i():
    while True:
        p = N.ENTITY.GET_ENTITY_COORDS(N.PLAYER.PLAYER_PED_ID(), True)
        runtime.current().log("INFO", "where_am_i", f"player at {p.x:.1f} {p.y:.1f} {p.z:.1f}")
        yield script.wait(2000)
