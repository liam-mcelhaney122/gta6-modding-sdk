"""Never wanted: a toggle that caps the wanted level at 0 and clears any level that still appears.
Turning it off restores the max wanted level the game had before.

Offline run (mock world): H at 496 turns it on, a crime at 1504 raises the level to 3 and the next
check clears it, H at 3008 turns it off.

    python -m gta6mod run sdk/examples/python/never_wanted.py --ms 4000 --press 74@496 --wanted 3@1504 --press 74@3008

Twin of sdk/luau/examples/never_wanted. Behaviour and log lines: sdk/docs/script_layer.md.
"""
from gta6mod.game import GTA5Control, Mod, player

# Config. GTA VI control ids are unknown; this is a GTA V control index (sdk/docs/script_layer.md).
KEY_TOGGLE = GTA5Control.VEH_HEADLIGHT  # 74, H
CHECK_MS = 500

m = Mod("never_wanted")
never = m.toggle(KEY_TOGGLE, "never wanted")
saved = {"max": None}


@never.on_enable
def enable():
    saved["max"] = player.max_wanted()
    player.set_max_wanted(0)
    player.clear_wanted()
    m.log(f"never wanted ON (max wanted level was {saved['max']})")


@never.on_disable
def disable():
    player.set_max_wanted(saved["max"])
    m.log(f"never wanted OFF (max wanted level restored to {saved['max']})")


@m.every(CHECK_MS, when=never)
def clear():
    level = player.wanted_level()
    if level > 0:
        player.clear_wanted()
        m.log(f"cleared wanted level {level}")
