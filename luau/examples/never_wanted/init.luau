--!strict
-- never_wanted: keep the wanted level at 0 while on, restore the max wanted level when off
-- (Luau twin of sdk/examples/python/never_wanted.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\never_wanted\init.luau 4000 --press 74@496 --wanted 3@1504 --press 74@3008

local game = require("@gta6/game")

-- Config. GTA VI control ids are unknown: this is a GTA V control index (sdk/docs/script_layer.md).
local KEY_TOGGLE = game.GTA5Control.VEH_HEADLIGHT -- H
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
