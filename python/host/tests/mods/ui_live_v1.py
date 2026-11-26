"""Test mod (g6_python_test "ui_reload"), version 1: a UI frame that hot reload replaces with ui_live_v2.py."""
from gta6mod import ui

ui.on_frame("live", lambda frame: ui.text("live v1"))
