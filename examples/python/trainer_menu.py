"""Trainer menu: an in-game menu (Insert) with God mode and Never wanted toggles, a Heal button and the player's
coordinates. Twin of sdk/luau/examples/trainer_menu and the C++ plugin g6trainer.

The toggles have no key (``m.toggle(None, ...)``): the menu flips them. The menu function runs where the host
draws (in-game: the render thread), where natives fail, so it only reads ``view`` (kept up to date by a script)
and hands changes to the next script tick with ``ui.defer``.

Offline run (mock world and mock UI): God mode clicked at 496 ms, damage at 1008 is blocked, Never wanted
clicked at 1504, a crime at 2000 is cleared, Heal clicked at 2496.

    python -m gta6mod run sdk/examples/python/trainer_menu.py --ms 3000 --ui-click "God mode@496" --damage 50@1008
        --ui-click "Never wanted@1504" --wanted 3@2000 --ui-click Heal@2496 --print-ui
"""
from gta6mod import ui
from gta6mod.game import Mod, player

m = Mod("trainer_menu")

# god mode (as god_mode.py)
god = m.toggle(None, "god mode")


def protect(ped, on: bool) -> None:
    ped.set_invincible(on)
    ped.set_can_ragdoll(not on)


@god.on_enable
def god_on():
    ped = player.ped()
    protect(ped, True)
    ped.heal()
    m.log("god mode ON")


@god.on_disable
def god_off():
    protect(player.ped(), False)
    m.log("god mode OFF")


# never wanted (as never_wanted.py)
never_wanted = m.toggle(None, "never wanted")
saved = {"max": 0}


@never_wanted.on_enable
def nw_on():
    saved["max"] = player.max_wanted()
    player.set_max_wanted(0)
    player.clear_wanted()
    m.log(f"never wanted ON (max wanted level was {saved['max']})")


@never_wanted.on_disable
def nw_off():
    player.set_max_wanted(saved["max"])
    m.log(f"never wanted OFF (max wanted level restored to {saved['max']})")


# what the menu shows, refreshed by a script (natives run there)
view = {"known": False, "pos": None, "health": 0, "wanted": 0}


@m.every(100)
def refresh():
    ped = player.ped()
    view["pos"] = ped.position
    view["health"] = ped.health
    view["wanted"] = player.wanted_level()
    view["known"] = True
    if never_wanted and view["wanted"] > 0:
        player.clear_wanted()
        m.log(f"cleared wanted level {view['wanted']}")
        view["wanted"] = 0
    if god and ped.health < ped.max_health:
        ped.heal()


def heal():
    player.ped().heal()
    m.log("healed")


@ui.on_frame("trainer_menu")
def menu(frame):
    with ui.window("Trainer", x=40, y=120, auto_resize=True) as w:
        if not w:
            return
        changed, on = ui.checkbox("God mode", bool(god))
        if changed:
            ui.defer(lambda on=on: god.set(on))
        changed, on = ui.checkbox("Never wanted", bool(never_wanted))
        if changed:
            ui.defer(lambda on=on: never_wanted.set(on))
        if ui.button("Heal"):
            ui.defer(heal)
        ui.separator()
        if view["known"]:
            p = view["pos"]
            ui.text(f"Player: {p.x:.1f} {p.y:.1f} {p.z:.1f}")
            ui.text(f"Health: {view['health']}   Wanted level: {view['wanted']}")
        else:
            ui.text("Player: unknown")
