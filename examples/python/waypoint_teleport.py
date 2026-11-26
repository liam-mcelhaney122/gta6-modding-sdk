"""Waypoint teleport: press X to teleport to the map waypoint (with your vehicle, if you are in one).

The waypoint's z is not trustworthy, so the mod requests collision there and probes for the ground until
it is found or the timeout passes. It never teleports blind.

Offline run (mock world): X at 496 with no waypoint warns; the waypoint is set at 1008; X at 1504
teleports once collision has loaded (100 ms in the mock).

    python -m gta6mod run sdk/examples/python/waypoint_teleport.py --ms 3000 --press INPUT_VEH_DUCK@496 --waypoint 250,-400,0@1008 --press INPUT_VEH_DUCK@1504

Twin of sdk/luau/examples/waypoint_teleport. Behaviour and log lines: sdk/docs/script_layer.md.
"""
from gta6mod.game import Control, Mod, Vector3, player, world

# Config: a GTA VI input action (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
KEY_TELEPORT = Control.VEH_DUCK  # GTA V default key X
GROUND_TIMEOUT_MS = 1500
LIFT = 1.0  # metres above the ground

m = Mod("waypoint_teleport")


@m.on_key(KEY_TELEPORT)
def teleport():
    wp = world.waypoint()
    if wp is None:
        m.warn("no waypoint set")
        return
    z = yield from world.find_ground(wp.x, wp.y, GROUND_TIMEOUT_MS)
    if z is None:
        m.warn(f"no ground found at {wp.x:.1f} {wp.y:.1f}; not teleporting")
        return
    dest = Vector3(wp.x, wp.y, z + LIFT)
    player.teleport(dest)
    m.log(f"teleported to {dest.x:.1f} {dest.y:.1f} {dest.z:.1f}")
