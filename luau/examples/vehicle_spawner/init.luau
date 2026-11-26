--!strict
-- vehicle_spawner: spawn a vehicle in front of the player, seat them in it, and keep the vehicle they
-- are in repaired (Luau twin of sdk/examples/python/vehicle_spawner.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\vehicle_spawner\init.luau 3000 --press INPUT_CONTEXT_SECONDARY@496 --damage 150@2000

local game = require("@gta6/game")

-- Config: a GTA VI input action (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
local KEY_SPAWN = game.Control.CONTEXT_SECONDARY -- GTA V default key Q
-- A GTA V model name, kept as a placeholder: no GTA VI vehicle model hash has call-site evidence
-- (findings/phase5/30_script_layer_evidence.md, G-081: no story-script model native takes a constant model hash).
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
