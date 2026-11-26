"""Test mod (g6_python_test): fails while loading."""
from gta6mod.game import Mod

m = Mod("bad_load")
raise RuntimeError("broken at import")
