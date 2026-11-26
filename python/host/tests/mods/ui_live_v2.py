"""Test mod (g6_python_test "ui_reload"), version 2: replaces ui_live_v1.py's frame (same frame name, new module)."""
from gta6mod import ui


@ui.on_frame("live")
def live(frame):
    with ui.window("Live") as w:
        if w:
            ui.text("live version two")
