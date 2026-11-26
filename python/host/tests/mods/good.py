"""Test mod (g6_python_test): keeps running while other mods fail; logs every 10th tick and on stop."""
from gta6mod.game import Mod, player, wait

m = Mod("good")
state = {"ticks": 0}


@m.loop
def run():
    while True:
        state["ticks"] += 1
        player.ped().health  # a native call every tick
        if state["ticks"] % 10 == 0:
            m.log(f"good tick {state['ticks']}")
        yield wait(0)


@m.on_stop
def bye():
    m.log(f"good stopped after {state['ticks']} ticks")
