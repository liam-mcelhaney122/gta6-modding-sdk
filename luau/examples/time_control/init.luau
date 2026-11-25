--!strict
-- time_control: cycle the time scale and move the clock forward
-- (Luau twin of sdk/examples/python/time_control.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\time_control\init.luau 2000 --press 20@496 --press 20@1008 --press 29@1504

local game = require("@gta6/game")

-- Config. GTA VI control ids are unknown: these are GTA V control indices (sdk/docs/script_layer.md).
local KEY_SCALE = game.GTA5Control.MULTIPLAYER_INFO -- Z
local KEY_HOUR = game.GTA5Control.SPECIAL_ABILITY_SECONDARY -- B
local SCALES = { 1.0, 0.5, 0.2 }

local m = game.mod("time_control")
local index = 1

m.onKey(KEY_SCALE, function()
	index = index % #SCALES + 1
	game.world.setTimeScale(SCALES[index])
	m.log(string.format("time scale %.2f", SCALES[index]))
end)

m.onKey(KEY_HOUR, function()
	game.clock.add(1, 0, 0)
	local h, mins = game.clock.time()
	m.log(string.format("clock %02d:%02d", h, mins))
end)

-- Runs when something calls m.stop() (the runtime unload hook is future work).
m.onStop(function()
	if SCALES[index] ~= 1.0 then
		index = 1
		game.world.setTimeScale(1.0)
		m.log("time scale reset to 1.00")
	end
end)
