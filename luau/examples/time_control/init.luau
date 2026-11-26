--!strict
-- time_control: tap to cycle the time scale, hold to reset it, and move the clock forward
-- (Luau twin of sdk/examples/python/time_control.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\time_control\init.luau 3000 --press INPUT_MULTIPLAYER_INFO@496 --press INPUT_MULTIPLAYER_INFO@1008 --press INPUT_SPECIAL_ABILITY@1504 --press INPUT_MULTIPLAYER_INFO@1600+1200

local game = require("@gta6/game")

-- Config: GTA VI input actions (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
local KEY_SCALE = game.Control.MULTIPLAYER_INFO -- GTA V default key Z
local KEY_HOUR = game.Control.SPECIAL_ABILITY -- GTA V SPECIAL_ABILITY_SECONDARY (B) does not exist in GTA VI
local TAP_MS = 300 -- a press up to this long is a tap
local RESET_HOLD_MS = 1000
local SCALES = { 1.0, 0.5, 0.2 }

local m = game.mod("time_control")
local index = 1

local function resetScale()
	if SCALES[index] ~= 1.0 then
		index = 1
		game.world.setTimeScale(1.0)
		m.log("time scale reset to 1.00")
	end
end

m.onRelease(KEY_SCALE, function()
	index = index % #SCALES + 1
	game.world.setTimeScale(SCALES[index])
	m.log(string.format("time scale %.2f", SCALES[index]))
end, { maxMs = TAP_MS })

m.onHold(KEY_SCALE, RESET_HOLD_MS, resetScale)

m.onKey(KEY_HOUR, function()
	game.clock.add(1, 0, 0)
	local h, mins = game.clock.time()
	m.log(string.format("clock %02d:%02d", h, mins))
end)

-- Also runs when something calls m.stop() (the runtime unload hook is future work).
m.onStop(resetScale)
