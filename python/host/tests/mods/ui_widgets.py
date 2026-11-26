"""Test mod (g6_python_test "ui_widgets", test_embedded_ui.py): every gta6mod.ui widget driven through mock
interactions. Twin of sdk/luau/tests/scenarios/ui_widgets.luau, with the same flags:

    --ui-click Heal@96 --ui-click "God mode@96" --ui-set Wanted=4@96 --ui-set Speed=2.5@96 --ui-set Name=Jason@96
    --ui-set Weather=Fog@96 --ui-click Advanced@96 --ui-click Second@96 --ui-set Tint=0.1,0.2,0.3,0.4@96
    --ui-menu 0@496 --ui-menu 1@800

Logs "PASS ui_widgets" at 1200 ms, or "FAIL ui_widgets: ..." with what did not hold.
"""
from gta6mod import ui
from gta6mod.game import Mod, wait

m = Mod("ui_widgets")
WEATHERS = ["Clear", "Rain", "Fog"]
state = {"heals": 0, "god": False, "wanted": 0, "speed": 1.0, "name": "Lucia", "weather": 0, "advanced_open": False,
         "tab": "", "tint": (1.0, 1.0, 1.0, 1.0), "changes": 0, "hidden_frames": 0, "menu_frames": 0}


@ui.on_frame("widgets")
def widgets(frame):
    if not frame.menu_open:
        state["hidden_frames"] += 1  # must not happen: this frame is not always=True
        return
    state["menu_frames"] += 1
    with ui.window("Widgets") as w:
        if not w:
            return
        if ui.button("Heal"):
            state["heals"] += 1
        changed, state["god"] = ui.checkbox("God mode", state["god"])
        if changed:
            state["changes"] += 1
        _, state["wanted"] = ui.slider_int("Wanted", state["wanted"], 0, 5)
        _, state["speed"] = ui.slider_float("Speed", state["speed"], 0, 10)
        _, state["name"] = ui.input_text("Name", state["name"])
        _, state["weather"] = ui.combo("Weather", state["weather"], WEATHERS)
        with ui.tree("Advanced") as t:
            if t:
                state["advanced_open"] = True
                ui.text("inside")
        with ui.tab_bar("tabs") as bar:
            if bar:
                with ui.tab("First") as a:
                    if a:
                        state["tab"] = "First"
                with ui.tab("Second") as b:
                    if b:
                        state["tab"] = "Second"
        _, state["tint"] = ui.color_edit("Tint", state["tint"])


@m.loop
def check():
    yield wait(1200)
    s = state
    problems = [what for ok, what in (
        (s["heals"] == 1, f"button clicked once ({s['heals']})"),
        (s["god"] is True and s["changes"] == 1, "checkbox flipped once"),
        (s["wanted"] == 4, f"slider_int set ({s['wanted']})"),
        (abs(s["speed"] - 2.5) < 1e-6, f"slider_float set ({s['speed']})"),
        (s["name"] == "Jason", f"input_text set ({s['name']})"),
        (s["weather"] == 2, f"combo set by item text ({s['weather']})"),
        (s["advanced_open"], "tree opened by a click"),
        (s["tab"] == "Second", f"tab selected by a click ({s['tab']})"),
        (abs(s["tint"][0] - 0.1) < 1e-6 and abs(s["tint"][3] - 0.4) < 1e-6, f"color_edit set ({s['tint']})"),
        (s["hidden_frames"] == 0, "menu frames do not run while the menu is hidden"),
        (40 < s["menu_frames"] < 75, f"menu frames ({s['menu_frames']}): hidden from 496 to 800 ms"),
    ) if not ok]
    m.log("PASS ui_widgets" if not problems else "FAIL ui_widgets: " + "; ".join(problems))
