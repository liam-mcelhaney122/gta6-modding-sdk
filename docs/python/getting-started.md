# Get started with Python

Run a Python mod in the offline mock world and simulate a control press. Commands below run in PowerShell from the repository root.

## Prerequisites

Install Python 3.10 or later. The mock example requires neither a game dump nor a compiled DLL. Make the package available for this shell:

```powershell
$env:PYTHONPATH = "$PWD\sdk\python"
```

Alternatively, install the SDK into an environment with `python -m pip install -e .\sdk`. The package has no mandatory third-party dependencies. Optional extras are listed in `sdk/pyproject.toml`.

## Run an example

```powershell
python -m gta6mod run sdk\examples\python\god_mode.py --ms 2000 --press 47@496
```

The output includes a `god mode ON` log and a final summary of native calls, live scripts and errors. The control number is a GTA V placeholder, not a confirmed GTA VI control. Mock events fire on matching ticks; the default frame interval is 16 ms, so use multiples of 16 for event times.

## Create a mod

Save the following as `hello_mod.py` in the repository root:

```python
from gta6mod.game import GTA5Control, Mod, player

mod = Mod("hello")

@mod.on_key(GTA5Control.DETONATE)
def show_position():
    pos = player.position()
    mod.log(f"position: {pos.x:.1f}, {pos.y:.1f}, {pos.z:.1f}")
```

Run `python -m gta6mod run .\hello_mod.py --ms 1000 --press 47@496`. The mock starts the player at `(-1000, -2000, 15)`, so the log shows that position. The runner selects a backend before loading the file; its decorators register scripts on the default scheduler.

## Wait without blocking

A waiting handler must be a generator. Yield a `wait` request or delegate to a helper that waits:

```python
from gta6mod.game import GTA5Control, Mod, player, wait, world

mod = Mod("waypoint")

@mod.on_key(GTA5Control.VEH_DUCK)
def teleport():
    point = world.waypoint()
    if point is None:
        mod.warn("No waypoint set")
        return
    z = yield from world.find_ground(point.x, point.y)
    if z is None:
        mod.warn("Ground not found")
        return
    player.teleport((point.x, point.y, z + 1))
    yield wait(0)
```

For a runnable scenario, use `sdk/examples/python/waypoint_teleport.py` and the command in its docstring. Waiting key handlers run independently; repeat presses for the same binding are ignored until the handler finishes.

## Next steps

Read the [Python API reference](reference.md), explore the [examples](../../examples/python), and consult the [generated native reference](../api/index.md) for raw calls. The mock's synthetic world and accepted native calls do not prove live-game compatibility.
