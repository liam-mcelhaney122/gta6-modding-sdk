"""gta6mod.game: the script layer (clean names + helpers). Design: sdk/docs/script_layer.md.

    from gta6mod.game import Mod, player, world, clock, controls, models, Vehicle, Control, wait

Layers:
  ``names``     GENERATED clean aliases of the raw natives (``names.entity.get_coords``)
  helpers       ``player``, ``world``, ``clock``, ``controls``, ``models``, ``Entity`` / ``Ped`` / ``Vehicle``
  ``Mod``       loops, timers, hotkeys (press, release, hold, chords), blocked actions and toggles

Helpers call natives through ``names`` only, so status checks and marshalling are the raw wrappers' own.
"""
from ..controls import Control, ControlGroup
from ..joaat import joaat
from ..types import Vector3
from . import names
from .core import log, now, wait
from .entities import SEAT_DRIVER, Entity, Ped, Vehicle, models
from .mod import Mod, Toggle
from .world import clock, controls, player, world

__all__ = [
    "names", "joaat", "Vector3", "log", "now", "wait",
    "Entity", "Ped", "Vehicle", "SEAT_DRIVER", "models",
    "Mod", "Toggle", "Control", "ControlGroup", "clock", "controls", "player", "world",
]
