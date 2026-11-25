"""Vehicle spawner: press Q to spawn a vehicle 5 m in front of you and get in; the vehicle you are in is
repaired once a second whenever it is damaged.

Offline run (mock world): Q at 496 spawns and seats the player; damage at 2000 hits the vehicle and the
next check repairs it.

    python -m gta6mod run sdk/examples/python/vehicle_spawner.py --ms 3000 --press 52@496 --damage 150@2000

Twin of sdk/luau/examples/vehicle_spawner. Behaviour and log lines: sdk/docs/script_layer.md.
CREATE_VEHICLE has only a GTA V seed hash (unverified) in 1.0.2.1; see the unknowns table there.
"""
from gta6mod.game import GTA5Control, Mod, Vehicle, player

# Config. GTA VI control ids are unknown; this is a GTA V control index (sdk/docs/script_layer.md).
KEY_SPAWN = GTA5Control.CONTEXT_SECONDARY  # 52, Q
MODEL = "adder"  # a GTA V model name; GTA VI model names are not confirmed
SPAWN_AHEAD = 5.0  # metres in front of the player
REPAIR_CHECK_MS = 1000

m = Mod("vehicle_spawner")


@m.on_key(KEY_SPAWN)
def spawn():
    ped = player.ped()
    veh = yield from Vehicle.spawn(MODEL, ped.offset(0.0, SPAWN_AHEAD, 0.0), ped.heading)
    if veh is None:
        m.warn(f"could not spawn {MODEL}")
        return
    ped.put_into(veh)
    m.log(f"spawned {MODEL} and seated the player")


@m.every(REPAIR_CHECK_MS)
def auto_repair():
    veh = player.vehicle()
    if veh is None:
        return
    before = veh.health
    if before < veh.max_health:
        veh.repair()
        m.log(f"repaired vehicle ({before} -> {veh.health})")
