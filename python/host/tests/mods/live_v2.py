"""Test mod (g6_python_test hot reload), version 2: replaces live.py while the game runs."""
from gta6mod.game import Mod, wait

m = Mod("live")


@m.loop
def run():
    m.log("version two running")
    yield wait(100000)


@m.on_stop
def bye():
    m.log("version two stopped")
