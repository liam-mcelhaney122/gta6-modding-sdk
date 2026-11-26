"""Test mod (g6_python_test): never yields after 200 ms; the watchdog must interrupt it.
`except Exception` does not catch TickBudgetExceeded (a BaseException)."""
from gta6mod.game import Mod, wait

m = Mod("runaway")


@m.loop
def run():
    yield wait(200)
    m.log("runaway: spinning")
    while True:
        try:
            pass
        except Exception:  # noqa: BLE001 - on purpose: must not swallow the watchdog
            pass


@m.on_stop
def cleanup():
    m.log("runaway cleanup ran")
