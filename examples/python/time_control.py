"""Time control: Z cycles the time scale (1.0, 0.5, 0.2) and B moves the clock forward one hour.
Stopping the mod puts the time scale back to 1.0.

Offline run (mock world): Z at 496 and 1008 (0.50, then 0.20), B at 1504 (12:00 -> 13:00).

    python -m gta6mod run sdk/examples/python/time_control.py --ms 2000 --press 20@496 --press 20@1008 --press 29@1504

Twin of sdk/luau/examples/time_control. Behaviour and log lines: sdk/docs/script_layer.md.
"""
from gta6mod.game import GTA5Control, Mod, clock, world

# Config. GTA VI control ids are unknown; these are GTA V control indices (sdk/docs/script_layer.md).
KEY_TIME_SCALE = GTA5Control.MULTIPLAYER_INFO  # 20, Z
KEY_HOUR = GTA5Control.SPECIAL_ABILITY_SECONDARY  # 29, B
SCALES = (1.0, 0.5, 0.2)

m = Mod("time_control")
state = {"i": 0}


@m.on_key(KEY_TIME_SCALE)
def next_scale():
    state["i"] = (state["i"] + 1) % len(SCALES)
    scale = SCALES[state["i"]]
    world.set_time_scale(scale)
    m.log(f"time scale {scale:.2f}")


@m.on_key(KEY_HOUR)
def add_hour():
    clock.add(1, 0, 0)
    h, mi, _s = clock.time()
    m.log(f"clock {h:02d}:{mi:02d}")


@m.on_stop
def restore():
    if SCALES[state["i"]] != 1.0:
        world.set_time_scale(1.0)
        state["i"] = 0
        m.log("time scale reset to 1.00")
