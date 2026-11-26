--!strict
-- trainer_menu: an in-game menu (Insert) with God mode and Never wanted toggles, a Heal button and the
-- player's coordinates (twin of sdk/examples/python/trainer_menu.py and the C++ plugin g6trainer).
-- The toggles have no key (m.toggle(nil, ...)): the menu flips them; examples/god_mode and never_wanted bind keys.
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\trainer_menu\init.luau 3000 --ui-click "God mode@496"
--   --damage 50@1008 --ui-click "Never wanted@1504" --wanted 3@2000 --ui-click Heal@2496 --expect-clean
--   --expect-log "god mode ON" --expect-log "never wanted ON (max wanted level was 5)" --expect-log "cleared wanted level 3"
--   --expect-ui "text Player: -1000.0 -2000.0 15.0" --expect-ui "checkbox God mode = 1"
--
-- The menu function runs where the host draws (in-game: the render thread), where natives fail. It only
-- reads locals the mod's scripts keep up to date and hands changes to the next tick with ui.defer.

local game = require("@gta6/game")
local ui = require("@gta6/ui")

local m = game.mod("trainer_menu")

-- god mode (as examples/god_mode)
local god = m.toggle(nil, "god mode")
local function protect(ped: any, on: boolean)
	ped:setInvincible(on)
	ped:setCanRagdoll(not on)
end
god.onEnable(function()
	local ped = game.player.ped()
	protect(ped, true)
	ped:heal()
	m.log("god mode ON")
end)
god.onDisable(function()
	protect(game.player.ped(), false)
	m.log("god mode OFF")
end)

-- never wanted (as examples/never_wanted)
local neverWanted = m.toggle(nil, "never wanted")
local savedMax = 0
neverWanted.onEnable(function()
	savedMax = game.player.maxWanted()
	game.player.setMaxWanted(0)
	game.player.clearWanted()
	m.log(`never wanted ON (max wanted level was {savedMax})`)
end)
neverWanted.onDisable(function()
	game.player.setMaxWanted(savedMax)
	m.log(`never wanted OFF (max wanted level restored to {savedMax})`)
end)

-- what the menu shows, refreshed by a script (natives run there)
local view = { x = 0, y = 0, z = 0, wanted = 0, health = 0, known = false }
m.every(100, function()
	local ped = game.player.ped()
	local p = ped:position()
	view.x, view.y, view.z = p.x, p.y, p.z
	view.health = ped:health()
	view.wanted = game.player.wantedLevel()
	view.known = true
	if neverWanted.isOn() and view.wanted > 0 then
		game.player.clearWanted()
		m.log(`cleared wanted level {view.wanted}`)
		view.wanted = 0
	end
	if god.isOn() and view.health < ped:maxHealth() then
		ped:heal()
	end
end)

ui.onFrame("trainer_menu", function()
	ui.window("Trainer", function()
		-- each deferred function captures its own local (a shared `on` would be overwritten before the tick)
		local godOn, godChanged = ui.checkbox("God mode", god.isOn())
		if godChanged then
			ui.defer(function()
				god.set(godOn)
			end)
		end
		local nwOn, nwChanged = ui.checkbox("Never wanted", neverWanted.isOn())
		if nwChanged then
			ui.defer(function()
				neverWanted.set(nwOn)
			end)
		end
		if ui.button("Heal") then
			ui.defer(function()
				game.player.ped():heal()
				m.log("healed")
			end)
		end
		ui.separator()
		if view.known then
			ui.text(string.format("Player: %.1f %.1f %.1f", view.x, view.y, view.z))
			ui.text(`Health: {view.health}   Wanted level: {view.wanted}`)
		else
			ui.text("Player: unknown")
		end
	end, { x = 40, y = 120, autoResize = true })
end)
