--!strict
-- waypoint_teleport: teleport the player (and the vehicle they are in) to the map waypoint, on the ground
-- (Luau twin of sdk/examples/python/waypoint_teleport.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\waypoint_teleport\init.luau 3000 --press INPUT_VEH_DUCK@496 --waypoint 250,-400,0@1008 --press INPUT_VEH_DUCK@1504

local game = require("@gta6/game")

-- Config: a GTA VI input action (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
local KEY_TELEPORT = game.Control.VEH_DUCK -- GTA V default key X
local GROUND_TIMEOUT_MS = 1500

local m = game.mod("waypoint_teleport")

m.onKey(KEY_TELEPORT, function()
	local wp = game.world.waypoint()
	if wp == nil then
		m.warn("no waypoint set")
		return
	end
	local z = game.world.findGround(wp.x, wp.y, GROUND_TIMEOUT_MS)
	if z == nil then
		m.warn(string.format("no ground found at %.1f %.1f; not teleporting", wp.x, wp.y))
		return
	end
	game.player.teleport(vector.create(wp.x, wp.y, z + 1))
	m.log(string.format("teleported to %.1f %.1f %.1f", wp.x, wp.y, z + 1))
end)
