"""God mode: a toggle that makes the player invincible, blocks ragdoll and keeps health full.

Offline run (mock world): damage at 496 ms lands, G at 1008 turns god mode on (heals), damage at 2000
is blocked, G at 3008 turns it off, damage at 3504 lands again.

    python -m gta6mod run sdk/examples/python/god_mode.py --ms 4000 --damage 50@496 --press INPUT_DETONATE@1008 --damage 50@2000 --press INPUT_DETONATE@3008 --damage 30@3504

Twin of sdk/luau/examples/god_mode. Behaviour and log lines: sdk/docs/script_layer.md.
"""
from gta6mod.game import Control, Mod, player

# Config: a GTA VI input action (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
KEY_TOGGLE = Control.DETONATE  # GTA V default key G
CHECK_MS = 250

m = Mod("god_mode")
god = m.toggle(KEY_TOGGLE, "god mode")
state = {"ped": None}


def protect(ped) -> None:
    ped.set_invincible(True)
    ped.set_can_ragdoll(False)
    state["ped"] = ped


@god.on_enable
def enable():
    ped = player.ped()
    protect(ped)
    ped.heal()
    m.log("god mode ON")


@god.on_disable
def disable():
    ped = player.ped()
    ped.set_invincible(False)
    ped.set_can_ragdoll(True)
    state["ped"] = None
    m.log("god mode OFF")


@m.every(CHECK_MS, when=god)
def keep_alive():
    ped = player.ped()
    if ped != state["ped"]:  # character switch or respawn: the old ped's flags do not carry over
        protect(ped)
        m.log("new player ped, god mode reapplied")
    max_hp = ped.max_health
    if ped.health < max_hp:
        ped.heal()
        m.log(f"health topped up to {max_hp}")
