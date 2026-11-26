--!strict
-- never_wanted: keep the wanted level at 0 while on, restore the max wanted level when off
-- (Luau twin of sdk/examples/python/never_wanted.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\never_wanted\init.luau 4000 --press INPUT_VEH_HEADLIGHT@496 --wanted 3@1504 --press INPUT_VEH_HEADLIGHT@3008

local game = require("@gta6/game")

-- Config: a GTA VI input action (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
local KEY_TOGGLE = game.Control.VEH_HEADLIGHT -- GTA V default key H
local CHECK_MS = 500

local m = game.mod("never_wanted")
local toggle = m.toggle(KEY_TOGGLE)
local savedMax = 0

toggle.onEnable(function()
	savedMax = game.player.maxWanted()
	game.player.setMaxWanted(0)
	game.player.clearWanted()
	m.log(`never wanted ON (max wanted level was {savedMax})`)
end)

toggle.onDisable(function()
	game.player.setMaxWanted(savedMax)
	m.log(`never wanted OFF (max wanted level restored to {savedMax})`)
end)

m.every(CHECK_MS, function()
	local level = game.player.wantedLevel()
	if level > 0 then
		game.player.clearWanted()
		m.log(`cleared wanted level {level}`)
	end
end, toggle)
