--!strict
-- vehicle_spawner: spawn a vehicle in front of the player, seat them in it, and keep the vehicle they
-- are in repaired (Luau twin of sdk/examples/python/vehicle_spawner.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\vehicle_spawner\init.luau 3000 --press 52@496 --damage 150@2000

local game = require("@gta6/game")

-- Config. GTA VI control ids are unknown: this is a GTA V control index (sdk/docs/script_layer.md).
local KEY_SPAWN = game.GTA5Control.CONTEXT_SECONDARY -- Q
-- A GTA V model name; GTA VI model names are unknown (sdk/docs/script_layer.md).
local MODEL = "adder"
local REPAIR_MS = 1000

local m = game.mod("vehicle_spawner")

m.onKey(KEY_SPAWN, function()
	local ped = game.player.ped()
	local veh = game.Vehicle.spawn(MODEL, ped:offset(0, 5, 0), ped:heading())
	if veh == nil then
		m.warn(`could not spawn {MODEL}`)
		return
	end
	ped:putInto(veh)
	m.log(`spawned {MODEL} and seated the player`)
end)

m.every(REPAIR_MS, function()
	local veh = game.player.vehicle()
	if veh == nil then
		return
	end
	local before = veh:health()
	if before < veh:maxHealth() then
		veh:repair()
		m.log(`repaired vehicle ({before} -> {veh:health()})`)
	end
end)
