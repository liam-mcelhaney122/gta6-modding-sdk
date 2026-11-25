"""gta6mod — Python side of the GTA VI modding SDK (scaffold).

Two halves:

* Offline tooling (works today, needs only the user's own dump):
  ``gta6mod.formats`` (RPF9 / RDS5 headers), ``gta6mod.pe``, ``gta6mod.pattern``
  (IDA-style signatures), ``gta6mod.anchors`` (re-db anchors, verified against an
  exe), ``gta6mod.joaat``.

* Scripting API (same surface as the Luau API):
  ``gta6mod.runtime`` (backends: in-process mock today, C-ABI runtime later),
  ``gta6mod.natives`` (generated typed wrappers), ``gta6mod.script`` (script
  registration + cooperative ``wait``).

Nothing here writes, copies or redistributes game data.
"""

from .joaat import joaat

__all__ = ["joaat", "__version__", "GAME_BUILD", "API_VERSION"]
__version__ = "0.1.0"
GAME_BUILD = "1.0.2.1"
API_VERSION = (0, 1)  # must track G6_API_VERSION_MAJOR/MINOR in gta6mod.h
