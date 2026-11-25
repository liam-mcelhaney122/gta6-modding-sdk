--!strict
-- waypoint_teleport: teleport the player (and the vehicle they are in) to the map waypoint, on the ground
-- (Luau twin of sdk/examples/python/waypoint_teleport.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\waypoint_teleport\init.luau 3000 --press 73@496 --waypoint 250,-400,0@1008 --press 73@1504

local game = require("@gta6/game")

-- Config. GTA VI control ids are unknown: this is a GTA V control index (sdk/docs/script_layer.md).
local KEY_TELEPORT = game.GTA5Control.VEH_DUCK -- X
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
