"""Test mod (g6_python_test hot reload), version 1 (copied as live.py)."""
from gta6mod.game import Mod, wait

m = Mod("live")


@m.loop
def run():
    m.log("v1 running")
    yield wait(100000)


@m.on_stop
def bye():
    m.log("v1 stopped")
