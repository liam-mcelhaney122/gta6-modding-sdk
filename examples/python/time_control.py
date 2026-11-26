"""Time control: tap Z to cycle the time scale (1.0, 0.5, 0.2), hold Z for a second to reset it to 1.0, and
press B to move the clock forward one hour. Stopping the mod also puts the time scale back to 1.0.

Offline run (mock world): Z taps at 496 and 1008 (0.50, then 0.20), B at 1504 (12:00 -> 13:00), Z held from
1600 for 1200 ms (reset at 2608; the release is no tap because the press was too long).

    python -m gta6mod run sdk/examples/python/time_control.py --ms 3000 --press INPUT_MULTIPLAYER_INFO@496 --press INPUT_MULTIPLAYER_INFO@1008 --press INPUT_SPECIAL_ABILITY@1504 --press INPUT_MULTIPLAYER_INFO@1600+1200

Twin of sdk/luau/examples/time_control. Behaviour and log lines: sdk/docs/script_layer.md.
"""
from gta6mod.game import Control, Mod, clock, world

# Config: GTA VI input actions (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
KEY_TIME_SCALE = Control.MULTIPLAYER_INFO  # GTA V default key Z
KEY_HOUR = Control.SPECIAL_ABILITY  # GTA V SPECIAL_ABILITY_SECONDARY (B) does not exist in GTA VI
TAP_MS = 300  # a press up to this long is a tap
RESET_HOLD_MS = 1000
SCALES = (1.0, 0.5, 0.2)

m = Mod("time_control")
state = {"i": 0}


@m.on_release(KEY_TIME_SCALE, max_ms=TAP_MS)
def next_scale():
    state["i"] = (state["i"] + 1) % len(SCALES)
    scale = SCALES[state["i"]]
    world.set_time_scale(scale)
    m.log(f"time scale {scale:.2f}")


@m.on_hold(KEY_TIME_SCALE, RESET_HOLD_MS)
def reset_scale():
    if SCALES[state["i"]] != 1.0:
        world.set_time_scale(1.0)
        state["i"] = 0
        m.log("time scale reset to 1.00")


@m.on_key(KEY_HOUR)
def add_hour():
    clock.add(1, 0, 0)
    h, mi, _s = clock.time()
    m.log(f"clock {h:02d}:{mi:02d}")


m.on_stop(reset_scale)
