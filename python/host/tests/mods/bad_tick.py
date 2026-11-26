"""Test mod (g6_python_test): a script raises after 100 ms; the host disables the mod and runs its cleanup."""
from gta6mod.game import Mod, wait

m = Mod("bad_tick")


@m.loop
def run():
    yield wait(100)
    raise ValueError("broken in a tick")


@m.on_stop
def cleanup():
    m.log("bad_tick cleanup ran")
